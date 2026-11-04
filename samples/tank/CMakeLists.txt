# samples/tank —— FC《坦克大战》复刻（资源工程，无自己的 main）。
#
# 约束（与仓库其它 sample 一致）：
#   · 宿主只链接 maque::runtime，游戏逻辑全在 .scn/.lua/.prefab.json/art；
#   · 场景通过 "引擎即浏览器、游戏即资源" 的路径加载，改玩法不重新编译引擎。
#
# 四个 CTest 分别守：
#   tank_smoke   标题点击 → 动态 stage 场景 → 关卡 play
#   tank_play    autoplay 长跑 + 两次 world_trail 确定性
#   tank_rules   砖块/基地/6 道具/第 3 关
#   tank_zorder  2D z_index 回归（docs/bugs/013）

if(MAQUE_BUILD_TESTS)
    add_test(NAME tank_smoke COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             smoke $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank.scn")
    add_test(NAME tank_play COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             play $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank.scn")
    add_test(NAME tank_rules COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             rules $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank.scn")
    add_test(NAME tank_zorder COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             zorder $<TARGET_FILE:maque_player>)
    set_tests_properties(tank_smoke tank_play tank_rules tank_zorder PROPERTIES
                         SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")
endif()
