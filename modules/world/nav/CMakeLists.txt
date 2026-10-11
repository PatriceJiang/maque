# maque::nav —— 确定性 2D 网格寻路（A* + 视线平滑 + 追踪）
add_library(maque_nav STATIC src/nav.cpp)
add_library(maque::nav ALIAS maque_nav)
target_include_directories(maque_nav PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_nav PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_options(maque_nav PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_nav_test tests/nav_test.cpp)
    target_link_libraries(maque_nav_test PRIVATE maque::nav)
    add_test(NAME nav COMMAND maque_nav_test)
endif()

# 模块 C ABI 头由 DSL 生成（schema.yaml 的 module_abi 段，docs/21）
get_property(_maque_gen GLOBAL PROPERTY MAQUE_GEN_DIR)
if(_maque_gen AND TARGET maque_codegen)
    target_include_directories(maque_nav PUBLIC "${_maque_gen}")
    add_dependencies(maque_nav maque_codegen)
endif()
