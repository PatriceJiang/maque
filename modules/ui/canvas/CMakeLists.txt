# maque::canvas —— 分辨率策略（画布适配，纯函数）
add_library(maque_canvas STATIC src/canvas.cpp)
add_library(maque::canvas ALIAS maque_canvas)
target_include_directories(maque_canvas PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_canvas PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_canvas_test tests/canvas_test.cpp)
    target_link_libraries(maque_canvas_test PRIVATE maque::canvas)
    add_test(NAME canvas COMMAND maque_canvas_test)
endif()
