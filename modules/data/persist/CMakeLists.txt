# maque::persist —— 持久化/反序列化（反射驱动的文本 JSON + 二进制 codec/快照池）
add_library(maque_persist STATIC src/persist.cpp src/persist_binary.cpp)
add_library(maque::persist ALIAS maque_persist)
target_include_directories(maque_persist PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_persist PUBLIC maque::reflect)
set_target_properties(maque_persist PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_persist_test tests/persist_test.cpp)
    target_link_libraries(maque_persist_test PRIVATE maque::persist)
    add_test(NAME persist COMMAND maque_persist_test)

    add_executable(maque_snapshot_test tests/snapshot_test.cpp)
    target_link_libraries(maque_snapshot_test PRIVATE maque::persist)
    add_test(NAME snapshot COMMAND maque_snapshot_test)
endif()
target_include_directories(maque_persist PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_persist_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_snapshot_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
