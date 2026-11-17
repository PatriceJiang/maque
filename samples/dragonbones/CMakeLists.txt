# samples/dragonbones —— DragonBones 2D 素材演示工程（docs/36）
#
# 解耦约定（与 samples/spine 同一套）：
#   · 宿主只链接 maque::runtime，**不链接 dragonbones/gfx/image 任何模块**（骨架与图集都是资源）。
#   · 骨架/图集/动画切换/HUD 全在 mecha.scn + dragonbones.js 里。
#   · 素材在 art/（DragonBones 官方 demo 素材，授权见 art/LICENSE-dragonbones-art.txt）。

if(MAQUE_BUILD_TESTS)
    # 冒烟：headless 跑若干帧，确认骨架/图集/页纹理都能装配、绘制数据非空、渲染不崩
    add_test(NAME dragonbones_smoke COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/dragonbones_e2e.sh"
             smoke $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/mecha.scn")
    set_tests_properties(dragonbones_smoke PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 切换链路：MAQUE_SIM 注入 ↑/↓/空格 → 断言日志里的当前动画名按注入顺序变化 + 像素自证
    add_test(NAME dragonbones_switch COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/dragonbones_e2e.sh"
             switch $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/mecha.scn")
    set_tests_properties(dragonbones_switch PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 确定性：同一场景跑两次（固定帧数 + 定格时间）→ world_trail 与截图逐字节一致
    add_test(NAME dragonbones_determinism COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/dragonbones_e2e.sh"
             determinism $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/mecha.scn")
    set_tests_properties(dragonbones_determinism PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")
endif()
