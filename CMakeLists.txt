
cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Maque CXX C ASM)

option(MAQUE_BUILD_TESTS "Build maque unit tests" ON)

enable_testing()

# script 需先于 data（data 生成绑定 TU 依赖 script）
add_subdirectory(modules/script)
add_subdirectory(modules/core/data)
add_subdirectory(modules/core/reflect)
add_subdirectory(modules/core/handle)
add_subdirectory(modules/core/module)
add_subdirectory(modules/math)
add_subdirectory(modules/io/vfs)
add_subdirectory(modules/net)
add_subdirectory(modules/platform)
add_subdirectory(modules/data/persist)
add_subdirectory(modules/image)
add_subdirectory(modules/render/gfx)
add_subdirectory(modules/api)
add_subdirectory(modules/runtime)
add_subdirectory(samples/player)
