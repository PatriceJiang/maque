# maque::handle —— 世代槽表（句柄 {index,generation}）
add_library(maque_handle STATIC src/handle.cpp)
add_library(maque::handle ALIAS maque_handle)
target_include_directories(maque_handle PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_handle PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_handle_test tests/handle_test.cpp)
maque_deploy_asan(maque_handle_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_handle_test PRIVATE maque::handle)
    target_include_directories(maque_handle_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
    add_test(NAME handle COMMAND maque_handle_test)
endif()
target_include_directories(maque_handle PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
