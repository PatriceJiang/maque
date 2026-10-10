# maque::api —— 引擎 API 实现（匹配生成的 maque_api.h 声明）
add_library(maque_api STATIC src/api.cpp)
add_library(maque::api ALIAS maque_api)
target_include_directories(maque_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_api PUBLIC maque::schema maque::script)
set_target_properties(maque_api PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

# 生成的脚本绑定 TU（从 schema.yaml api_list 生成；调用引擎 API）
get_property(MAQUE_GEN_DIR GLOBAL PROPERTY MAQUE_GEN_DIR)
if(MAQUE_GEN_DIR AND TARGET maque_codegen)
    set(_bind "${MAQUE_GEN_DIR}/maque_bindings.cpp")
    set_source_files_properties("${_bind}" PROPERTIES GENERATED TRUE)
    add_library(maque_genbind STATIC "${_bind}")
    add_library(maque::genbind ALIAS maque_genbind)
    add_dependencies(maque_genbind maque_codegen)
    target_include_directories(maque_genbind PRIVATE "${MAQUE_GEN_DIR}")
    target_link_libraries(maque_genbind PUBLIC maque::script maque::api)
    set_target_properties(maque_genbind PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endif()
