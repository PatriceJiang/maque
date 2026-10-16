# maque_spine —— Spine 2D 素材演示工程（samples/spine）
#
# 解耦约定（与 samples/flappy 同一套）：
#   · 宿主只链接 maque::runtime，**不链接 spine2d/gfx/image 任何模块**（骨架与图集都是资源）。
#   · 骨架/图集/动画切换/HUD 全在 spine.scn、dragon.scn + spine.js 里。
#   · 素材在 art/（官方 spine-runtimes 4.2 样例的副本，随附 LICENSE）。
add_executable(maque_spine main.cpp)
target_link_libraries(maque_spine PRIVATE maque::runtime)
maque_deploy_plugin_deps(maque_spine)   # Windows：ASan 运行时 + wgpu_native.dll 放到 exe 旁

if(MAQUE_BUILD_TESTS)
    # 冒烟：headless 跑若干帧，确认骨架/图集/页纹理都能装配、渲染不崩
    add_test(NAME spine_smoke COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/spine_e2e.sh"
             smoke $<TARGET_FILE:maque_spine> "${CMAKE_CURRENT_SOURCE_DIR}/spine.scn")
    set_tests_properties(spine_smoke PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 切换链路：MAQUE_SIM 注入 ↑/↓/空格（原始键码）→ 断言日志里的当前动画名按注入顺序变化
    add_test(NAME spine_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/spine_e2e.sh"
             play $<TARGET_FILE:maque_spine> "${CMAKE_CURRENT_SOURCE_DIR}/spine.scn")
    set_tests_properties(spine_e2e PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")

    # 多页图集（dragon，5 页）：装配 + 逐页纹理句柄 + 多批次渲染
    add_test(NAME spine_dragon COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/spine_e2e.sh"
             dragon $<TARGET_FILE:maque_spine> "${CMAKE_CURRENT_SOURCE_DIR}/dragon.scn")
    set_tests_properties(spine_dragon PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP")
endif()
