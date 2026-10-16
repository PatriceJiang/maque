# maque::spine2d —— Spine 2D 官方运行时（spine-cpp）的 C ABI 封装
#
# 分层：本模块只依赖 spine-cpp + 标准库；**不依赖** gfx/image。
#   它产出"绘制数据"（顶点/UV/颜色/索引 + 图集页号），纹理解码与 GPU 上传由引擎侧做，
#   完成后通过 mq_spine_data_page_texture 回填句柄（见 schema/world/anim.yaml 的注释）。
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/spine-cpp" "${CMAKE_BINARY_DIR}/third_party/spine-cpp")

add_library(maque_spine2d STATIC
    src/spine2d.cpp
)
add_library(maque::spine2d ALIAS maque_spine2d)

target_include_directories(maque_spine2d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_spine2d PUBLIC Spine::spine-cpp)
set_target_properties(maque_spine2d PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_spine2d_test tests/spine2d_test.cpp)
    target_link_libraries(maque_spine2d_test PRIVATE maque::spine2d)
    target_include_directories(maque_spine2d_test PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    maque_deploy_asan(maque_spine2d_test)
    add_test(NAME spine2d COMMAND maque_spine2d_test "${CMAKE_SOURCE_DIR}/tests/data/spine")
endif()
