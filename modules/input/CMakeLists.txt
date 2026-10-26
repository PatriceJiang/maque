# maque::input —— 输入系统（docs/19）：确定性事件流 + 派发 + 录制/回放
add_library(maque_input STATIC src/input.cpp)
add_library(maque::input ALIAS maque_input)
target_include_directories(maque_input PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_input PUBLIC maque::platform maque::persist)
set_target_properties(maque_input PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_input PUBLIC maque::tracy)   # Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时是空接口）

if(MAQUE_BUILD_TESTS)
    add_executable(maque_input_test tests/input_test.cpp)
maque_deploy_asan(maque_input_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_input_test PRIVATE maque::input)
    add_test(NAME input COMMAND maque_input_test)
endif()

