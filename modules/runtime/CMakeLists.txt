# maque::runtime —— 引擎运行时（"浏览器"）：加载并运行场景资源
add_library(maque_runtime STATIC src/runtime.cpp)
add_library(maque::runtime ALIAS maque_runtime)
target_include_directories(maque_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_runtime PUBLIC maque::persist maque::script maque::module maque::handle maque::vfs maque::image maque::genbind maque::api maque::observability)
# 渲染插件路径（默认；可用环境变量 MAQUE_RENDERER 覆盖）
target_compile_definitions(maque_runtime PRIVATE MQ_RENDERER_DEFAULT="$<TARGET_FILE:maque_gfx>")
set_target_properties(maque_runtime PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
