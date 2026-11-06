# maque_add_native_script(<target> SOURCE <file> [OUTPUT_DIR <dir>] [CXX_STANDARD <std>])
#
# 把一个 C/C++ 源文件编成 native 脚本 guest（MODULE：.so/.dll/.dylib）。
# guest 只依赖生成头 + guest SDK，不链接 maque_runtime/maque_schema：
#   - maque_guest_api.h / abi/script_guest.h 由 schema codegen 生成；
#   - maque/script.hpp 由 codegen 生成；
#   - 运行时由宿主经 mq_host_load/query 加载，API 表在 create 时注入。
#
# 注意：该函数只负责“编译成 guest”。把源路径映射到产物的工作由
# native/<target>/manifest.json 完成（构建系统/打包器生成，见 plans/26-cpp-scripts.md）。
#
# 外部工程可直接 include 本文件（需先设置 MAQUE_GEN_DIR 指向 maque 的 generated/，
# 或把本仓库作为子目录 add_subdirectory）；本函数会按平台补 MQ_TARGET_KEY。
function(maque_add_native_script target)
    cmake_parse_arguments(ARG "" "SOURCE;OUTPUT_DIR;CXX_STANDARD" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCE AND NOT ARG_SOURCES)
        message(FATAL_ERROR "maque_add_native_script(${target}): SOURCE is required")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if(NOT ARG_CXX_STANDARD)
        set(ARG_CXX_STANDARD 17)   # guest SDK 不要求 C++23
    endif()

    if(DEFINED MAQUE_ENGINE_ROOT)
        set(_maque_root "${MAQUE_ENGINE_ROOT}")
    elseif(DEFINED CMAKE_CURRENT_FUNCTION_LIST_DIR)
        set(_maque_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    else()
        set(_maque_root "${CMAKE_CURRENT_LIST_DIR}/..")
    endif()
    if(NOT DEFINED MAQUE_GEN_DIR OR NOT EXISTS "${MAQUE_GEN_DIR}/maque_guest_api.h")
        set(MAQUE_GEN_DIR "${_maque_root}/modules/core/data/generated")
    endif()

    set(_sources ${ARG_SOURCE} ${ARG_SOURCES})
    add_library(${target} MODULE ${_sources})
    target_include_directories(${target} PRIVATE
        "${MAQUE_GEN_DIR}"
        "${_maque_root}/modules/core/module/include"
        "${_maque_root}/modules/script/include")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${ARG_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
        PREFIX ""
        OUTPUT_NAME "${target}"
        LIBRARY_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIR}")

    # 宿主/外部 SDK：只有没有被全局 compile definitions 覆盖时才补 target 宏。
    if(NOT MAQUE_GUEST_GLOBAL_TARGET_DEFINED)
        if(NOT DEFINED MAQUE_TARGET_KEY)
            if(WIN32)
                set(MAQUE_TARGET_KEY "windows-x86_64")
            elseif(ANDROID)
                set(MAQUE_TARGET_KEY "android-${ANDROID_ABI}")
            elseif(EMSCRIPTEN OR MAQUE_WASM)
                set(MAQUE_TARGET_KEY "wasm32")
            elseif(APPLE)
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                    set(MAQUE_TARGET_KEY "macos-arm64")
                else()
                    set(MAQUE_TARGET_KEY "macos-x86_64")
                endif()
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
                    set(MAQUE_TARGET_KEY "linux-aarch64")
                else()
                    set(MAQUE_TARGET_KEY "linux-x86_64")
                endif()
            else()
                set(MAQUE_TARGET_KEY "unknown")
            endif()
        endif()
        target_compile_definitions(${target} PRIVATE MQ_TARGET_KEY="${MAQUE_TARGET_KEY}")
        if(DEFINED MAQUE_ENGINE_VERSION)
            target_compile_definitions(${target} PRIVATE MQ_ENGINE_VERSION="${MAQUE_ENGINE_VERSION}")
        endif()
    endif()

    # Windows + ASan：guest 由宿主 dlopen，ASan 运行时 DLL 必须放到 guest 旁。
    if(COMMAND maque_deploy_asan)
        maque_deploy_asan(${target})
    endif()
endfunction()

# maque_add_native_script_static(<target> SOURCE <file> KEY <registry-key> ID <symbol-id> [CXX_STANDARD <std>])
#
# 把 guest 编成 **OBJECT 库**并生成一个静态注册 TU（全局构造期调用
# mq_script_guest_register_static(KEY, mq_guest_vt_ID())）。调用方把
# $<TARGET_OBJECTS:<target>> 加进宿主可执行文件即可；运行时 manifest 用
#   "kind": "static", "key": "<registry-key>"
# 解析，不产生/加载 .so。
#
# 约束：guest 源文件必须用 MAQUE_SCRIPT_STATIC(Class, ID) 导出唯一符号
# （MAQUE_NATIVE_STATIC 由本函数定义）；ID 必须是合法 C 标识符。
# 调用方宿主需链接 maque::script（或 maque::runtime），否则注册函数未定义。
function(maque_add_native_script_static target)
    cmake_parse_arguments(ARG "" "SOURCE;KEY;ID;CXX_STANDARD" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCE AND NOT ARG_SOURCES)
        message(FATAL_ERROR "maque_add_native_script_static(${target}): SOURCE is required")
    endif()
    if(NOT ARG_KEY)
        set(ARG_KEY "${ARG_SOURCE}")
    endif()
    if(NOT ARG_ID)
        set(ARG_ID "${target}")
    endif()
    if(NOT ARG_ID MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "maque_add_native_script_static(${target}): ID '${ARG_ID}' is not a C identifier")
    endif()
    if(NOT ARG_CXX_STANDARD)
        set(ARG_CXX_STANDARD 17)
    endif()

    if(DEFINED MAQUE_ENGINE_ROOT)
        set(_maque_root "${MAQUE_ENGINE_ROOT}")
    elseif(DEFINED CMAKE_CURRENT_FUNCTION_LIST_DIR)
        set(_maque_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    else()
        set(_maque_root "${CMAKE_CURRENT_LIST_DIR}/..")
    endif()
    if(NOT DEFINED MAQUE_GEN_DIR OR NOT EXISTS "${MAQUE_GEN_DIR}/maque_guest_api.h")
        set(MAQUE_GEN_DIR "${_maque_root}/modules/core/data/generated")
    endif()

    set(_sources ${ARG_SOURCE} ${ARG_SOURCES})
    add_library(${target} OBJECT ${_sources})
    target_include_directories(${target} PRIVATE
        "${MAQUE_GEN_DIR}"
        "${_maque_root}/modules/core/module/include"
        "${_maque_root}/modules/script/include")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${ARG_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${target} PRIVATE MAQUE_NATIVE_STATIC=1)

    if(NOT MAQUE_GUEST_GLOBAL_TARGET_DEFINED)
        if(NOT DEFINED MAQUE_TARGET_KEY)
            if(WIN32)
                set(MAQUE_TARGET_KEY "windows-x86_64")
            elseif(ANDROID)
                set(MAQUE_TARGET_KEY "android-${ANDROID_ABI}")
            elseif(EMSCRIPTEN OR MAQUE_WASM)
                set(MAQUE_TARGET_KEY "wasm32")
            elseif(APPLE)
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                    set(MAQUE_TARGET_KEY "macos-arm64")
                else()
                    set(MAQUE_TARGET_KEY "macos-x86_64")
                endif()
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
                    set(MAQUE_TARGET_KEY "linux-aarch64")
                else()
                    set(MAQUE_TARGET_KEY "linux-x86_64")
                endif()
            else()
                set(MAQUE_TARGET_KEY "unknown")
            endif()
        endif()
        target_compile_definitions(${target} PRIVATE MQ_TARGET_KEY="${MAQUE_TARGET_KEY}")
        if(DEFINED MAQUE_ENGINE_VERSION)
            target_compile_definitions(${target} PRIVATE MQ_ENGINE_VERSION="${MAQUE_ENGINE_VERSION}")
        endif()
    endif()

    set(_registry "${CMAKE_CURRENT_BINARY_DIR}/${target}_static_registry.cpp")
    set(_key "${ARG_KEY}")
    string(REPLACE "\\" "\\\\" _key "${_key}")
    string(REPLACE "\"" "\\\"" _key "${_key}")
    file(GENERATE OUTPUT "${_registry}" CONTENT
"// generated by maque_add_native_script_static(${target}) -- do not edit
#include \"abi/script_guest.h\"

extern \"C\" const mq_script_guest_vt* mq_guest_vt_${ARG_ID}(void);

namespace {
struct StaticGuestRegistrar {
    StaticGuestRegistrar() {
        (void)mq_script_guest_register_static(\"${_key}\", mq_guest_vt_${ARG_ID}());
    }
};
StaticGuestRegistrar g_static_guest_${ARG_ID}_registrar;
}
")
    target_sources(${target} PRIVATE "${_registry}")
endfunction()
