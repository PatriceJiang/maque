# maque::spine2d —— Spine 2D 官方运行时（spine-cpp）的 C ABI 封装
#
# 分层：本模块只依赖 spine-cpp + 标准库；**不依赖** gfx/image。
#   它产出"绘制数据"（顶点/UV/颜色/索引 + 图集页号），纹理解码与 GPU 上传由引擎侧做，
#   完成后通过 mq_spine_data_page_texture 回填句柄（见 schema/world/anim.yaml 的注释）。
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/spine-cpp" "${CMAKE_BINARY_DIR}/third_party/spine-cpp")

# 宿主扩展点（spine::getDefaultExtension）**必须无条件进最终链接**：
# spine-cpp 的 Extension.cpp 引用它，而它是本模块提供的；若放进 maque_spine2d.a，
# GNU ld 单趟扫描（先扫本归档、后扫 spine-cpp）不会回扫 → undefined reference。
# 放进 OBJECT 库并 PUBLIC 传播 → 每个消费者（player/host/测试）都会带上这一份。
# 宿主扩展点（spine::getDefaultExtension）实现放在 spine2d.cpp 里 —— 它**必然被消费者拉入**
# （都引用 mq_spine_*）；单独成 TU 进 .a 会被 GNU ld 的单趟扫描漏掉（undefined reference）。
add_library(maque_spine2d STATIC src/spine2d.cpp)
add_library(maque::spine2d ALIAS maque_spine2d)

target_include_directories(maque_spine2d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_spine2d PUBLIC Spine::spine-cpp)
set_target_properties(maque_spine2d PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_spine2d PUBLIC maque::tracy)   # Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时是空接口）

if(MAQUE_BUILD_TESTS)
    add_executable(maque_spine2d_test tests/spine2d_test.cpp)
    target_link_libraries(maque_spine2d_test PRIVATE maque::spine2d)
    target_include_directories(maque_spine2d_test PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    maque_deploy_asan(maque_spine2d_test)
    add_test(NAME spine2d COMMAND maque_spine2d_test "${CMAKE_SOURCE_DIR}/tests/data/spine")
endif()
