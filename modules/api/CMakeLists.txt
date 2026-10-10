# maque::api —— 引擎 API 实现（匹配生成的 maque_api.h 声明）
add_library(maque_api STATIC src/api.cpp)
add_library(maque::api ALIAS maque_api)
target_include_directories(maque_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_api PUBLIC maque::schema maque::script)
set_target_properties(maque_api PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
