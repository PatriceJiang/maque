# maque::observability —— 结构化事件 + 指标（per-instance）
add_library(maque_observability STATIC src/obs.cpp)
add_library(maque::observability ALIAS maque_observability)
target_include_directories(maque_observability PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_observability PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
if(MAQUE_BUILD_TESTS)
    add_executable(maque_obs_test tests/obs_test.cpp)
maque_deploy_asan(maque_obs_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_obs_test PRIVATE maque::observability)
    target_include_directories(maque_obs_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
    add_test(NAME obs COMMAND maque_obs_test)
endif()
target_include_directories(maque_observability PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
