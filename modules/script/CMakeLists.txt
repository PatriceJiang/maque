# quickjs (quickjs-ng) 作为静态库 —— third_party 源码
set(QJS_DIR "${CMAKE_SOURCE_DIR}/third_party/quickjs-ng")
add_library(quickjs STATIC
    "${QJS_DIR}/dtoa.c"
    "${QJS_DIR}/libregexp.c"
    "${QJS_DIR}/libunicode.c"
    "${QJS_DIR}/quickjs.c")
target_include_directories(quickjs PUBLIC "${QJS_DIR}")
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
target_link_libraries(quickjs PUBLIC m)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# maque::script —— quickjs 封装（C ABI，docs/14）
add_library(maque_script STATIC src/script.cpp)
add_library(maque::script ALIAS maque_script)
target_include_directories(maque_script PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_script PUBLIC quickjs)
target_compile_definitions(maque_script PRIVATE _GNU_SOURCE)
set_target_properties(maque_script PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_script_test tests/script_test.cpp)
    target_link_libraries(maque_script_test PRIVATE maque::script)
    add_test(NAME script COMMAND maque_script_test)
endif()

