
cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Maque CXX C ASM)

option(MAQUE_BUILD_TESTS "Build maque unit tests" ON)

enable_testing()

add_subdirectory(modules/core/data)
add_subdirectory(modules/core/reflect)
add_subdirectory(modules/core/handle)
add_subdirectory(modules/core/module)
add_subdirectory(modules/math)
add_subdirectory(modules/render/gfx)
add_subdirectory(samples/ball_demo)
