# samples/tank —— 俯视坦克对战独立游戏工程（samples/tank）
#
# 解耦约定（这就是本工程存在的意义）：
#   · 宿主只链接 maque::runtime，**不链接任何游戏相关模块**。
#   · 玩法全在 tank.scn + tank.js（资源），改玩法不需要重编译引擎。
#   · 素材在 art/（程序化生成，见 tools/make_tank_art.py），路径相对场景目录解析。

if(MAQUE_BUILD_TESTS)
    # 冒烟：headless 跑 60 帧，确认场景/脚本/素材都能加载且不崩
    add_test(NAME tank_smoke COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             smoke $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank.scn")
    set_tests_properties(tank_smoke PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 玩法：autoplay 900 帧（开火→命中→扣血→击毁→计分）+ 同一输入两次运行 world_trail 一致
    add_test(NAME tank_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             play $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank.scn")
    set_tests_properties(tank_e2e PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 判定不许失效：MAQUE_TANK_KILL_AT 强制瞄准开火一次 → 必须扣血、必须击毁、必须加分
    add_test(NAME tank_kill COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
             kill $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank.scn")
    set_tests_properties(tank_kill PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")
endif()
