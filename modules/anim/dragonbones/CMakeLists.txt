# maque::dragonbones —— DragonBones 5.x 2D 骨骼运行时（docs/36）
#
# 分层（与 spine2d 同一条原则）：本模块只做 **解析 + 求值 + 绘制数据导出**；
# 不依赖 gfx/image —— 图集页的读取/解码/上传由引擎侧完成，经回调回填句柄与像素尺寸。
add_library(maque_dragonbones STATIC src/dragonbones.cpp)
add_library(maque::dragonbones ALIAS maque_dragonbones)

target_include_directories(maque_dragonbones PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# JSON DOM 走 persist 模块的 C ABI（mq_json_*）：跨模块只暴露 C ABI（docs/02）。
target_link_libraries(maque_dragonbones PUBLIC maque::persist)
set_target_properties(maque_dragonbones PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_dragonbones_test tests/dragonbones_test.cpp)
    target_link_libraries(maque_dragonbones_test PRIVATE maque::dragonbones)
    target_include_directories(maque_dragonbones_test PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    maque_deploy_asan(maque_dragonbones_test)
    add_test(NAME dragonbones COMMAND maque_dragonbones_test "${CMAKE_SOURCE_DIR}/tests/data/dragonbones")
endif()
