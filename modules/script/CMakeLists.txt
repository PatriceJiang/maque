# quickjs (quickjs-ng) 作为静态库 —— third_party 源码
set(QJS_DIR "${CMAKE_SOURCE_DIR}/third_party/quickjs-ng")
# Lua 后端可裁剪；关闭时仍编译 lua_backend.cpp 的 unsupported stub，公共 ABI 不变。
option(MAQUE_ENABLE_LUA "编入 Lua 脚本后端" ON)
if(MAQUE_ENABLE_LUA)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/lua" "${CMAKE_BINARY_DIR}/third_party/lua")
endif()
add_library(quickjs STATIC
    "${QJS_DIR}/dtoa.c"
    "${QJS_DIR}/libregexp.c"
    "${QJS_DIR}/libunicode.c"
    "${QJS_DIR}/quickjs.c")
target_include_directories(quickjs PUBLIC "${QJS_DIR}")
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
# quickjs-ng 用文字宏 EMSCRIPTEN（不是编译器自带的 __EMSCRIPTEN__）选择 Web 路径。
if(MAQUE_WASM OR EMSCRIPTEN)
    target_compile_definitions(quickjs PRIVATE EMSCRIPTEN=1)
endif()
# `m`（libm）只在类 UNIX 存在；Windows 上数学函数在 CRT 里。
if(UNIX)
    target_link_libraries(quickjs PUBLIC m)
endif()
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# maque::script —— quickjs 封装（C ABI，docs/14）
add_library(maque_script STATIC
    src/script.cpp src/bus.cpp
    src/js_backend.cpp
    src/lua_backend.cpp)
add_library(maque::script ALIAS maque_script)
target_include_directories(maque_script PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_script PUBLIC quickjs)
if(MAQUE_ENABLE_LUA)
    target_link_libraries(maque_script PUBLIC maque::lua)
    target_compile_definitions(maque_script PRIVATE MAQUE_HAVE_LUA=1)
endif()
target_compile_definitions(maque_script PRIVATE _GNU_SOURCE)
set_target_properties(maque_script PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_script PUBLIC maque::tracy)

if(MAQUE_BUILD_TESTS)
    if(MAQUE_ENABLE_LUA)
        add_executable(maque_lua_test tests/lua_test.cpp)
        maque_deploy_asan(maque_lua_test)
        target_link_libraries(maque_lua_test PRIVATE maque::script)
        add_test(NAME lua COMMAND maque_lua_test)
    endif()

    add_executable(maque_script_test tests/script_test.cpp)
    maque_deploy_asan(maque_script_test)
    target_link_libraries(maque_script_test PRIVATE maque::script)
    add_test(NAME script COMMAND maque_script_test)
endif()
