# maque_host —— 嵌入宿主示例：宿主拥有主循环（P3，docs/05）
add_executable(maque_host main.cpp)
target_link_libraries(maque_host PRIVATE maque::runtime)
maque_deploy_plugin_deps(maque_host)   # Windows：ASan 运行时 + wgpu_native.dll 放到 exe 旁

# 宿主驱动 + 多实例确定性（同一场景两个独立实例，逐帧校验和一致）
if(MAQUE_BUILD_TESTS AND (TARGET maque_gfx OR MAQUE_HEADLESS_STUBS))
    if(TARGET maque_gfx)
        add_test(NAME host_tick COMMAND maque_host "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn" 30)
    endif()
    add_test(NAME host_step COMMAND maque_host "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn" 30 --step-only)
    add_test(NAME host_multi_instance COMMAND maque_host "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn" 30 --two-instances --step-only)
    # 该用例验证的是“两个独立实例逐帧 checksum 一致”，与窗口/交换链无关；
    # 双窗口在部分 Mesa/RADV 驱动上退出时会踩 wgpu-native swapchain teardown（与本用例无关）。
    # 用无窗口路径跑，既稳定又仍覆盖两实例并行的确定性。
    set_tests_properties(host_multi_instance PROPERTIES
        ENVIRONMENT "MAQUE_WINDOW=0x0;MAQUE_PRESENT=off")
endif()
