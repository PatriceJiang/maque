# third_party/lua — 官方 Lua 5.4.8 静态库（脚本后端用；不含解释器/编译器 main）
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(maque_lua STATIC ${LUA_SOURCES})
add_library(maque::lua ALIAS maque_lua)
target_include_directories(maque_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(maque_lua PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# 只暴露需要的 C 标准库能力：不启用 readline/dlopen（沙箱化脚本运行时不加载动态库）。
#
# 平台差异（踩过）：
#   · LUA_USE_POSIX 让 Lua 走 `_setjmp/_longjmp`（ldo.c:63）并 include <sys/wait.h>/
#     <unistd.h>（lauxlib.c/loslib.c）—— Windows 三者都没有，编译直接失败。
#   · Windows 由 luaconf.h 自己定义 LUA_USE_WINDOWS（→ LUA_DL_DLL + LUA_USE_C89），
#     不要叠加 LUA_USE_POSIX。
if(WIN32)
    # Windows：保持 Lua 默认的 ISO C longjmp/setjmp 路径（luaconf.h 处理其余差异）。
    # Lua 用到 strerror/_popen 等，关掉 MSVC 的弃用告警噪音（本仓库对第三方源码不设告警策略）。
    target_compile_definitions(maque_lua PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    target_compile_definitions(maque_lua PRIVATE LUA_USE_POSIX)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(maque_lua PRIVATE LUA_USE_LINUX)
endif()

# `m`（libm）与 `dl`（libdl）只在类 UNIX 存在。
# Windows 上 `m` 会被解析成 `m.lib` → "could not open 'm.lib'"（数学函数在 CRT 里），
# 而 `dl` 已被上文的 LUA_USE_DLOPEN 排除（Windows 走 LUA_USE_WINDOWS 的 LoadLibrary 路径）。
if(UNIX AND NOT APPLE)
    target_link_libraries(maque_lua PUBLIC m ${CMAKE_DL_LIBS})
endif()

# 第三方源码：不参与本项目警告策略（MSVC 用 /w，GCC/Clang 用 -w）
if(MSVC)
    target_compile_options(maque_lua PRIVATE /w)
else()
    target_compile_options(maque_lua PRIVATE -w)
endif()
