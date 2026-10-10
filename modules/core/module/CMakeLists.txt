# maque::module —— 模块宿主（.so 加载 / mq_module_query / registry）
add_library(maque_module STATIC src/host.cpp)
add_library(maque::module ALIAS maque_module)
target_include_directories(maque_module PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_module PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(maque_module PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    # 测试用模块 .so（纯 C，仅导出 mq_module_query）
    add_library(maque_test_module MODULE tests/test_module.c)
    target_include_directories(maque_test_module PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set_target_properties(maque_test_module PROPERTIES PREFIX "" OUTPUT_NAME "maque_test_module")

    add_executable(maque_host_test tests/host_test.cpp)
    target_include_directories(maque_host_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(maque_host_test PRIVATE maque::module)
    add_dependencies(maque_host_test maque_test_module)
    add_test(NAME module COMMAND maque_host_test "$<TARGET_FILE:maque_test_module>")
endif()
