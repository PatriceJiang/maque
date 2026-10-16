# maque::reflect —— 反射运行时（加载描述符、按字段访问与遍历）
add_library(maque_reflect STATIC src/reflect.cpp)
add_library(maque::reflect ALIAS maque_reflect)
target_include_directories(maque_reflect PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_reflect PUBLIC maque::schema)
set_target_properties(maque_reflect PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

# 单元测试（独立：只链接 maque::reflect）
if(MAQUE_BUILD_TESTS)
    add_executable(maque_reflect_test tests/reflect_test.cpp)
maque_deploy_asan(maque_reflect_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_reflect_test PRIVATE maque::reflect)
    add_test(NAME reflect COMMAND maque_reflect_test)
endif()
target_include_directories(maque_reflect PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_reflect_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
