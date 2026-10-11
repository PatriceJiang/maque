
cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Maque CXX C ASM)

option(MAQUE_BUILD_TESTS "Build maque unit tests" ON)

# 确定性 D8（docs/10 §2）：关闭 FMA 收缩 —— 浮点结果不随编译器/优化级别变化。
# 符号可见性 R8（docs/02）：默认 hidden，跨模块导出只经 MQ_MODULE_EXPORT / extern "C"。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-ffp-contract=off)
endif()
if(NOT MSVC)
    add_compile_options(-fvisibility=hidden)
    add_link_options(-fvisibility=hidden)
endif()

# WASM/浏览器目标（P3，docs/plan-wasm.md）：由 cmake/toolchain-emscripten.cmake 置位。
# WASM 下渲染插件（wgpu-native .so）与原生网络后端不可用，由宿主 JS 侧承担（docs/05 §3）。
if(EMSCRIPTEN)
    message(STATUS "maque: WASM/浏览器目标（主循环归宿主；见 docs/plan-wasm.md）")
    set(MAQUE_WASM ON)
endif()

enable_testing()

# script 需先于 data（data 生成绑定 TU 依赖 script）
add_subdirectory(modules/script)
add_subdirectory(modules/core/data)
add_subdirectory(modules/core/reflect)
add_subdirectory(modules/core/handle)
add_subdirectory(modules/core/module)
add_subdirectory(modules/math)
add_subdirectory(modules/observability)
add_subdirectory(modules/control)
add_subdirectory(modules/io/vfs)
add_subdirectory(modules/io/watcher)
add_subdirectory(modules/asset)
add_subdirectory(modules/net)
add_subdirectory(modules/platform)
add_subdirectory(modules/input)
add_subdirectory(modules/data/persist)
add_subdirectory(modules/image)
add_subdirectory(modules/world/physics)
add_subdirectory(modules/world/nav)
add_subdirectory(modules/render/gfx)
add_subdirectory(modules/api)
add_subdirectory(modules/runtime)
add_subdirectory(samples/player)
add_subdirectory(samples/host)

# 声明了 module_abi 的模块：公开接口头由 DSL 生成 →
#   ① 生成目录进入 include 路径（原手写头是 #include "abi/<mod>.h" 的 shim）
#   ② 建立对 codegen 的构建依赖（保证生成先于编译）
# 集中在此处接线：script 等模块必须先于 core/data 加入（生成绑定 TU 依赖 script）。
if(TARGET maque_codegen)
    get_property(_maque_gen GLOBAL PROPERTY MAQUE_GEN_DIR)
    if(_maque_gen)
        foreach(_t maque_script maque_platform maque_input maque_vfs maque_asset maque_physics maque_nav
              maque_handle maque_module maque_reflect maque_observability
              maque_persist maque_watcher maque_image maque_net)
            if(TARGET ${_t})
                target_include_directories(${_t} PUBLIC "${_maque_gen}")
                add_dependencies(${_t} maque_codegen)
            endif()
        endforeach()
    endif()
endif()
