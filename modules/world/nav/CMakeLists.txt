# maque::nav —— 确定性 2D 网格寻路（A* + 视线平滑 + 追踪）
add_library(maque_nav STATIC src/nav.cpp)
add_library(maque::nav ALIAS maque_nav)
target_include_directories(maque_nav PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_nav PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_options(maque_nav PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_nav_test tests/nav_test.cpp)
maque_deploy_asan(maque_nav_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_nav_test PRIVATE maque::nav)
    add_test(NAME nav COMMAND maque_nav_test)
endif()

