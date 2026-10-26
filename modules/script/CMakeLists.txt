# quickjs (quickjs-ng) 作为静态库 —— third_party 源码
set(QJS_DIR "${CMAKE_SOURCE_DIR}/third_party/quickjs-ng")
# lua 5.4 作为可选脚本后端（docs/14：统一后端 ABI）
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/lua" "${CMAKE_BINARY_DIR}/third_party/lua")
# Lua 后端：verdored 官方源码（third_party/lua）始终编入 → 后端可用性由构建决定
add_library(quickjs STATIC
    "${QJS_DIR}/dtoa.c"
    "${QJS_DIR}/libregexp.c"
    "${QJS_DIR}/libunicode.c"
    "${QJS_DIR}/quickjs.c")
target_include_directories(quickjs PUBLIC "${QJS_DIR}")
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
# `m`（libm）只在类 UNIX 存在；Windows 上会被解析成 `m.lib` → "could not open 'm.lib'"
#（数学函数在 CRT 里）。_GNU_SOURCE 在 MSVC 下无意义但无害（glibc 特性开关）。
if(UNIX)
    target_link_libraries(quickjs PUBLIC m)
endif()
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# maque::script —— quickjs 封装（C ABI，docs/14）
add_library(maque_script STATIC
    src/script.cpp src/bus.cpp        # 公共 ABI 派发层（docs/14 统一后端 ABI）
    src/js_backend.cpp    # quickjs-ng 后端
    src/lua_backend.cpp   # lua 5.4 后端（沙箱 + 确定性 GC）
)
add_library(maque::script ALIAS maque_script)
target_include_directories(maque_script PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_script PUBLIC quickjs maque::lua)
# Lua 后端可用（vendored 源码始终编入；如需裁剪：-DMAQUE_ENABLE_LUA=OFF）
option(MAQUE_ENABLE_LUA "编入 Lua 脚本后端" ON)
if(MAQUE_ENABLE_LUA)
    target_compile_definitions(maque_script PRIVATE MAQUE_HAVE_LUA=1)
endif()
target_compile_definitions(maque_script PRIVATE _GNU_SOURCE)
set_target_properties(maque_script PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_script PUBLIC maque::tracy)   # Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时是空接口）

if(MAQUE_BUILD_TESTS)
    add_executable(maque_lua_test tests/lua_test.cpp)
maque_deploy_asan(maque_lua_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_lua_test PRIVATE maque::script)
    add_test(NAME lua COMMAND maque_lua_test)

    add_executable(maque_script_test tests/script_test.cpp)
maque_deploy_asan(maque_script_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_script_test PRIVATE maque::script)
    add_test(NAME script COMMAND maque_script_test)
endif()

