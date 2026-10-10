# maque::data —— schema 驱动的反射类型生成 (docs/06, SCHEMA.md)
#
# 用 parser.js 从 schema.yaml 生成：
#   generated/maque_types.h   C++ 类型
#   generated/maque_desc.h/.cpp C ABI 描述符
#   generated/maque_schema.json
# 生成的 C++ 类型与描述符编译为静态库 maque::schema，供各模块使用。

find_program(NODE_EXECUTABLE node)

set(MAQUE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MAQUE_SCHEMA   "${CMAKE_CURRENT_SOURCE_DIR}/schema.yaml")
set(MAQUE_PARSER   "${CMAKE_CURRENT_SOURCE_DIR}/parser.js")

set(MAQUE_GEN_FILES
    "${MAQUE_GEN_DIR}/maque_types.h"
    "${MAQUE_GEN_DIR}/maque_desc.h"
    "${MAQUE_GEN_DIR}/maque_desc.cpp"
    "${MAQUE_GEN_DIR}/maque_schema.json"
    "${MAQUE_GEN_DIR}/maque_api.h"
    "${MAQUE_GEN_DIR}/maque_bindings.cpp")

if(NODE_EXECUTABLE)
    add_custom_command(
        OUTPUT  ${MAQUE_GEN_FILES}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MAQUE_GEN_DIR}"
        COMMAND "${NODE_EXECUTABLE}" "${MAQUE_PARSER}"
                --schema "${MAQUE_SCHEMA}" --out "${MAQUE_GEN_DIR}"
        # 在源目录运行以便找到 node_modules 与 yaml 依赖（首次需 `npm install`）
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS "${MAQUE_SCHEMA}" "${MAQUE_PARSER}"
        COMMENT "maque: 从 schema.yaml 生成反射代码"
        VERBATIM)

    add_library(maque_schema STATIC "${MAQUE_GEN_DIR}/maque_desc.cpp")
    add_library(maque::schema ALIAS maque_schema)
    target_include_directories(maque_schema PUBLIC "${MAQUE_GEN_DIR}")
    set_target_properties(maque_schema PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

    # 便捷：`ninja maque_codegen` 强制重生成
    add_custom_target(maque_codegen DEPENDS ${MAQUE_GEN_FILES})

    # 生成的脚本绑定 TU（调用引擎 API；链接 script，符号在最终可执行文件解析）
    if(TARGET maque::script)
        add_library(maque_genbind STATIC "${MAQUE_GEN_DIR}/maque_bindings.cpp")
        add_library(maque::genbind ALIAS maque_genbind)
        target_include_directories(maque_genbind PUBLIC "${MAQUE_GEN_DIR}")
        target_link_libraries(maque_genbind PUBLIC maque::script)
        set_target_properties(maque_genbind PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    endif()

    # 兼容旧的 INTERFACE 目标名（暴露生成目录）
    add_library(maque_data INTERFACE)
    add_library(maque::data ALIAS maque_data)
    target_include_directories(maque_data INTERFACE "${MAQUE_GEN_DIR}")
    target_link_libraries(maque_data INTERFACE maque::schema)
else()
    message(WARNING "maque: 未找到 node，跳过反射代码生成（maque::schema 不可用）")
endif()
