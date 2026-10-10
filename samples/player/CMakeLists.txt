# maque player —— 通用宿主（无游戏逻辑）
add_executable(maque_player main.cpp)
target_link_libraries(maque_player PRIVATE maque::runtime)

if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME player_run
        COMMAND maque_player "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
                             "${CMAKE_CURRENT_BINARY_DIR}/frames")
endif()

# 确定性：同场景跑两次，world_trail 必须一致（docs/10）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME determinism COMMAND bash -c
      "P=$<TARGET_FILE:maque_player>; S=${CMAKE_SOURCE_DIR}/scenes/ball.scn; \
       A=$($<TARGET_FILE:maque_player> ${CMAKE_SOURCE_DIR}/scenes/ball.scn 2>/dev/null | grep -oE 'world_trail=0x[0-9a-f]+'); \
       B=$($<TARGET_FILE:maque_player> ${CMAKE_SOURCE_DIR}/scenes/ball.scn 2>/dev/null | grep -oE 'world_trail=0x[0-9a-f]+'); \
       test -n \"$A\" && test \"$A\" = \"$B\"")
endif()
