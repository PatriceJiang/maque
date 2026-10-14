
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

# DSL 生成物目录（**入库**：生成的公开接口是仓库产物，docs/21）
#   - 单一真相 = modules/core/data/schema/（目录，入口 schema.yaml；一文件一模块 + import 依赖）
#   - 生成：`cmake --build build --target maque_codegen`（构建时自动重生成）
#   - 漂移校验：ctest -R codegen_drift（重新生成到临时目录并逐字节比对）
set(MAQUE_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modules/core/data/generated"
    CACHE PATH "Directory holding codegen outputs (tracked)")

# 生成头（含各模块 abi/<mod>.h 的 shim 目标）对全项目可见：配置期已生成，无需逐目标接线。
include_directories("${MAQUE_GEN_DIR}")

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
add_subdirectory(modules/input/simulator)
add_subdirectory(modules/data/persist)
add_subdirectory(modules/image)
add_subdirectory(modules/world/physics)
add_subdirectory(modules/world/nav)
add_subdirectory(modules/ui/canvas)
add_subdirectory(modules/render/text)
add_subdirectory(modules/render/gfx)
add_subdirectory(modules/api)
add_subdirectory(modules/runtime)
add_subdirectory(samples/player)
add_subdirectory(samples/inspector)
add_subdirectory(samples/host)

