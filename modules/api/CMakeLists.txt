# maque::api —— 引擎 API 实现（匹配生成的 maque_api.h 声明）
add_library(maque_api STATIC src/api.cpp)
target_include_directories(maque_api PUBLIC "${MAQUE_GEN_DIR}")   # maque_bind.h（DSL 生成，docs/21）
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

# 生成的控制面注册表 + 类型化 MCP schema（docs/21：控制面方法也由 DSL 生成）
if(MAQUE_GEN_DIR AND TARGET maque_codegen)
    set(_ctrl "${MAQUE_GEN_DIR}/maque_control_gen.cpp")
    set_source_files_properties("${_ctrl}" PROPERTIES GENERATED TRUE)
    add_library(maque_gencontrol STATIC "${_ctrl}")
    add_library(maque::gencontrol ALIAS maque_gencontrol)
    add_dependencies(maque_gencontrol maque_codegen)
    target_include_directories(maque_gencontrol PRIVATE "${MAQUE_GEN_DIR}")
    target_link_libraries(maque_gencontrol PUBLIC maque::control)
    set_target_properties(maque_gencontrol PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

    # 生成的脚本事件路由（docs/14 §4、docs/19 §8）
    set(_ev "${MAQUE_GEN_DIR}/maque_events_gen.cpp")
    set_source_files_properties("${_ev}" PROPERTIES GENERATED TRUE)
    add_library(maque_genevents STATIC "${_ev}")
    add_library(maque::genevents ALIAS maque_genevents)
    add_dependencies(maque_genevents maque_codegen)
    target_include_directories(maque_genevents PRIVATE "${MAQUE_GEN_DIR}")
    target_link_libraries(maque_genevents PUBLIC maque::script maque::input)
    set_target_properties(maque_genevents PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endif()
