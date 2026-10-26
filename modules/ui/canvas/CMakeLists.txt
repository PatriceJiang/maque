# maque::canvas —— 分辨率策略（画布适配，纯函数）
add_library(maque_canvas STATIC src/canvas.cpp)
add_library(maque::canvas ALIAS maque_canvas)
target_include_directories(maque_canvas PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_canvas PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_canvas PUBLIC maque::tracy)   # Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时是空接口）

if(MAQUE_BUILD_TESTS)
    add_executable(maque_canvas_test tests/canvas_test.cpp)
maque_deploy_asan(maque_canvas_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_canvas_test PRIVATE maque::canvas)
    add_test(NAME canvas COMMAND maque_canvas_test)
endif()
