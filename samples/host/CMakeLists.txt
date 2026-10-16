# maque_host —— 嵌入宿主示例：宿主拥有主循环（P3，docs/05）
add_executable(maque_host main.cpp)
target_link_libraries(maque_host PRIVATE maque::runtime)
maque_deploy_plugin_deps(maque_host)   # Windows：ASan 运行时 + wgpu_native.dll 放到 exe 旁

# 宿主驱动 + 多实例确定性（同一场景两个独立实例，逐帧校验和一致）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME host_tick COMMAND maque_host "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn" 30)
    add_test(NAME host_multi_instance COMMAND maque_host "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn" 30 --two-instances)
endif()
