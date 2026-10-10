# maque::runtime —— 引擎运行时（"浏览器"）：加载并运行场景资源
add_library(maque_runtime STATIC src/runtime.cpp)
add_library(maque::runtime ALIAS maque_runtime)
target_include_directories(maque_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_runtime PUBLIC maque::persist maque::script maque::gfx)
set_target_properties(maque_runtime PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
