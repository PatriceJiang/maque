# third_party/lua — 官方 Lua 5.4.8 静态库（脚本后端用；不含解释器/编译器 main）
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(maque_lua STATIC ${LUA_SOURCES})
add_library(maque::lua ALIAS maque_lua)
target_include_directories(maque_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(maque_lua PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
# 只暴露需要的 C 标准库能力：不启用 readline/dlopen（沙箱化脚本运行时不加载动态库）
target_compile_definitions(maque_lua PRIVATE LUA_USE_POSIX)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(maque_lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(maque_lua PUBLIC m dl)
endif()
target_compile_options(maque_lua PRIVATE -w)   # 第三方源码：不参与本项目警告策略
