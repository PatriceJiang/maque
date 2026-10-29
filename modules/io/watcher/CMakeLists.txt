# maque::watcher —— 资源热重载监视（帧边界轮询，docs/08/14）
add_library(maque_watcher STATIC src/watcher.cpp)
add_library(maque::watcher ALIAS maque_watcher)
target_include_directories(maque_watcher PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_watcher PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_watcher_test tests/watcher_test.cpp)
maque_deploy_asan(maque_watcher_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_watcher_test PRIVATE maque::watcher)
    target_include_directories(maque_watcher_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
    add_test(NAME watcher COMMAND maque_watcher_test)
endif()
target_include_directories(maque_watcher PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
