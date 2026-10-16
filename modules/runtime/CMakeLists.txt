# maque::runtime —— 引擎运行时（"浏览器"）：加载并运行场景资源
add_library(maque_runtime STATIC src/runtime.cpp)
add_library(maque::runtime ALIAS maque_runtime)
target_include_directories(maque_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_runtime PUBLIC maque::canvas maque::sim maque::text maque::persist maque::script maque::module maque::handle maque::vfs maque::storage maque::image maque::genbind maque::api maque::gencontrol maque::genevents maque::observability maque::control maque::platform maque::input maque::watcher maque::physics maque::nav maque::spine2d)
# 渲染插件路径（默认；可用环境变量 MAQUE_RENDERER 覆盖）
target_compile_definitions(maque_runtime PRIVATE MQ_RENDERER_DEFAULT="$<TARGET_FILE:maque_gfx>")
set_target_properties(maque_runtime PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(maque_runtime PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
