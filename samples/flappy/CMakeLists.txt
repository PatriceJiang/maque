# samples/flappy —— Flappy Bird 独立游戏工程（samples/flappy）
#
# 解耦约定（这就是本工程存在的意义）：
#   · 宿主只链接 maque::runtime，**不链接任何游戏相关模块**。
#   · 玩法全在 flappy.scn + flappy.js（资源），改玩法不需要重编译引擎。
#   · 素材在 art/（程序化生成，见 tools/make_flappy_art.py），路径相对场景目录解析。

if(MAQUE_BUILD_TESTS)
    # 冒烟：headless 跑若干帧，确认场景/脚本/素材都能加载且不崩
    add_test(NAME flappy_smoke COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/flappy_e2e.sh"
             smoke $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/flappy.scn")
    set_tests_properties(flappy_smoke PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 玩法：确定性回放（同一串输入 → 同一分数/同一死亡帧）+ 撞管判死 + 计分
    add_test(NAME flappy_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/flappy_e2e.sh"
             play $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/flappy.scn")
    set_tests_properties(flappy_e2e PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")
endif()
