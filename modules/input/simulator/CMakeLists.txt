# maque::sim —— 输入模拟器（帧索引场景 → 确定性输入事件流，docs/19）
add_library(maque_sim STATIC src/simulator.cpp)
add_library(maque::sim ALIAS maque_sim)
target_include_directories(maque_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_sim PUBLIC maque::persist)   # mq_json 解析
set_target_properties(maque_sim PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_sim_test tests/sim_test.cpp)
maque_deploy_asan(maque_sim_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_sim_test PRIVATE maque::sim)
    add_test(NAME sim COMMAND maque_sim_test)
endif()
