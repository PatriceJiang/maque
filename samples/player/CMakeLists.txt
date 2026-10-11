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

# 控制平面（headless 驱动）：AI 可查询指标/场景/校验和
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME control_plane COMMAND bash -c
      "MAQUE_CONTROL='{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Perf.get\"}' $<TARGET_FILE:maque_player> ${CMAKE_SOURCE_DIR}/scenes/ball.scn 2>/dev/null | grep -q '\"metrics\"'")
endif()

# 输入/UI/快照/回放端到端（docs/10 §3/§5、docs/19 §9）：注入 → 分发 → 生成 → 回放一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME ui_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/ui_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ui.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/ui_e2e")
endif()

# 热重载端到端（docs/08/14）：运行中改脚本 → 帧边界重载 → 世界演化与基线逐帧一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME hot_reload_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/hot_reload_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ui.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/hot_e2e")
endif()

# 动画/粒子端到端（docs/15 §2/§4、docs/10 §5）：关键帧采样 + 粒子发射 + 确定性 + 回滚保真
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME anim_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/anim_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/anim_e2e")
endif()

# 最小编辑器端到端（M9、docs/11 §3/§4）：编辑 → 落盘 → 重载 → 往返一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME editor_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/editor_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/editor_e2e")
endif()

# Prefab 实例化与 bake（docs/12 §5）：展开 → 落盘自包含 → bake 幂等 → 不再重复展开
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME prefab_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/prefab_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/prefab/prefab.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/prefab_e2e")
endif()

# 崩溃/异常上下文（docs/09 §5、docs/10 §7）：脚本异常 → 结构化报告（seed/帧号/检查点/重放）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME crash_context_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/crash_context_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim"
             "${CMAKE_CURRENT_BINARY_DIR}/crash_e2e")
endif()

# 导航端到端（docs/15 导航）：A* 路径 → 逐帧追踪 → 到达目标；两跑确定性
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME nav_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/nav_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/nav/nav.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/nav_e2e")
endif()
