# maque::data —— schema 驱动的反射类型生成 (docs/06, SCHEMA.md)
#
# 用 parser.js 从 schema.yaml 生成：
#   generated/maque_types.h   C++ 类型
#   generated/maque_desc.h/.cpp C ABI 描述符
#   generated/maque_schema.json
# 生成的 C++ 类型与描述符编译为静态库 maque::schema，供各模块使用。

find_program(NODE_EXECUTABLE node)

# 生成到**源码树**（入库）：生成物是公开接口的交付物，不是临时构建产物（docs/21）
set(MAQUE_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
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
    "${MAQUE_GEN_DIR}/abi/math.h"
    "${MAQUE_GEN_DIR}/API.md"
    "${MAQUE_GEN_DIR}/TYPES.md")

if(NODE_EXECUTABLE)
    # 生成物**入库**（源码树）→ 在**配置期**生成：任何编译单元都能直接包含生成头，
    # 不需要逐目标建立构建依赖（schema/parser 变更会触发重新配置，见 CMAKE_CONFIGURE_DEPENDS）。
    execute_process(
        COMMAND "${NODE_EXECUTABLE}" "${MAQUE_PARSER}" --schema "${MAQUE_SCHEMA}" --out "${MAQUE_GEN_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE MAQUE_CODEGEN_RC
        OUTPUT_QUIET)
    if(NOT MAQUE_CODEGEN_RC EQUAL 0)
        message(FATAL_ERROR "maque: codegen 失败（node parser.js 返回 ${MAQUE_CODEGEN_RC}）")
    endif()

    # schema/parser 变化 → 自动重新配置（从而重新生成）
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${MAQUE_SCHEMA}" "${MAQUE_PARSER}")

    add_library(maque_schema STATIC "${MAQUE_GEN_DIR}/maque_desc.cpp")
    add_library(maque::schema ALIAS maque_schema)
    target_include_directories(maque_schema PUBLIC "${MAQUE_GEN_DIR}")
    set_target_properties(maque_schema PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

    # 便捷：`cmake --build build --target maque_codegen` 手动重生成
    add_custom_target(maque_codegen
        COMMAND "${NODE_EXECUTABLE}" "${MAQUE_PARSER}" --schema "${MAQUE_SCHEMA}" --out "${MAQUE_GEN_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "maque: 从 schema.yaml 生成（类型/描述符/API/绑定/控制面/事件/ABI/文档）"
        VERBATIM)

    # 漂移校验：重新生成到临时目录并与入库版本逐字节比对
    add_test(NAME codegen_drift
             COMMAND "${CMAKE_COMMAND}" -E env
                     "MAQUE_SCHEMA=${MAQUE_SCHEMA}" "MAQUE_PARSER=${MAQUE_PARSER}"
                     "MAQUE_TRACKED=${MAQUE_GEN_DIR}"
                     bash "${CMAKE_SOURCE_DIR}/tests/codegen_drift.sh")

    set_property(GLOBAL PROPERTY MAQUE_GEN_DIR "${MAQUE_GEN_DIR}")

    add_library(maque_data INTERFACE)
    add_library(maque::data ALIAS maque_data)
    target_include_directories(maque_data INTERFACE "${MAQUE_GEN_DIR}")
    target_link_libraries(maque_data INTERFACE maque::schema)
else()
    message(WARNING "maque: 未找到 node，跳过反射代码生成（maque::schema 不可用）")
endif()
