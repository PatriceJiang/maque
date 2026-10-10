# M1 · 下落弹跳球 demo（无头逻辑 + 软件渲染）
add_library(ball_demo STATIC
    src/ball_world.cpp)
add_library(maque::ball_demo ALIAS ball_demo)
target_include_directories(ball_demo PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ball_demo PUBLIC maque::math maque::handle)
set_target_properties(ball_demo PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

add_executable(ball_demo_run main.cpp)
target_link_libraries(ball_demo_run PRIVATE ball_demo)
if(TARGET maque::gfx)
    target_link_libraries(ball_demo_run PRIVATE maque::gfx)
    target_link_libraries(ball_demo_run PRIVATE maque::persist)
    target_link_libraries(ball_demo_run PRIVATE maque::script)
else()
    message(WARNING "ball_demo_run: 无 maque::gfx（WebGPU 不可用）")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(ball_demo_test tests/ball_world_test.cpp)
    target_link_libraries(ball_demo_test PRIVATE ball_demo)
    add_test(NAME ball_demo COMMAND ball_demo_test)

    # 端到端：加载 .scn → 反序列化 → 运行 → 退出（需 WebGPU）
    if(TARGET maque::gfx)
        add_test(NAME ball_demo_run
            COMMAND ball_demo_run "${CMAKE_SOURCE_DIR}/scenes/ball_demo.scn"
                                  "${CMAKE_SOURCE_DIR}/scenes/ball_demo.js"
                                  "${CMAKE_CURRENT_BINARY_DIR}/run_frames")
    endif()
endif()
