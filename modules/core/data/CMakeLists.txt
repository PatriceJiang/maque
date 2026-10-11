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
    "${MAQUE_GEN_DIR}/maque_bindings.cpp"
    "${MAQUE_GEN_DIR}/maque_ifaces.h"
    "${MAQUE_GEN_DIR}/maque_bind.h"
    "${MAQUE_GEN_DIR}/maque_control.h"
    "${MAQUE_GEN_DIR}/maque_control_gen.cpp"
    "${MAQUE_GEN_DIR}/maque_events.h"
    "${MAQUE_GEN_DIR}/maque_events_gen.cpp"
    "${MAQUE_GEN_DIR}/abi/vfs.h"
    "${MAQUE_GEN_DIR}/abi/asset.h"
    "${MAQUE_GEN_DIR}/abi/nav.h"
    "${MAQUE_GEN_DIR}/abi/physics.h"
    "${MAQUE_GEN_DIR}/abi/platform.h"
    "${MAQUE_GEN_DIR}/abi/input.h"
    "${MAQUE_GEN_DIR}/abi/script.h"
    "${MAQUE_GEN_DIR}/abi/handle.h"
    "${MAQUE_GEN_DIR}/abi/module.h"
    "${MAQUE_GEN_DIR}/abi/reflect.h"
    "${MAQUE_GEN_DIR}/abi/obs.h"
    "${MAQUE_GEN_DIR}/abi/persist.h"
    "${MAQUE_GEN_DIR}/abi/persist_binary.h"
    "${MAQUE_GEN_DIR}/abi/watcher.h"
    "${MAQUE_GEN_DIR}/abi/image.h"
    "${MAQUE_GEN_DIR}/abi/net.h"
    "${MAQUE_GEN_DIR}/abi/control.h"
    "${MAQUE_GEN_DIR}/abi/host_scene.h"
    "${MAQUE_GEN_DIR}/abi/runtime.h"
    "${MAQUE_GEN_DIR}/abi/math.h")

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

    # 供下游（如 api 模块）引用生成目录与文件
    set_property(GLOBAL PROPERTY MAQUE_GEN_DIR "${MAQUE_GEN_DIR}")

    # 兼容旧的 INTERFACE 目标名（暴露生成目录）
    add_library(maque_data INTERFACE)
    add_library(maque::data ALIAS maque_data)
    target_include_directories(maque_data INTERFACE "${MAQUE_GEN_DIR}")
    target_link_libraries(maque_data INTERFACE maque::schema)
else()
    message(WARNING "maque: 未找到 node，跳过反射代码生成（maque::schema 不可用）")
endif()
