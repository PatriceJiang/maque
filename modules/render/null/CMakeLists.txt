# maque::renderer_null -- deterministic headless renderer for WASM P0 and tests.
add_library(maque_renderer_null STATIC src/null_renderer.cpp)
add_library(maque::renderer_null ALIAS maque_renderer_null)
target_include_directories(maque_renderer_null PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/modules/core/module/include"
    "${MAQUE_GEN_DIR}")
set_target_properties(maque_renderer_null PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_renderer_null_test tests/null_renderer_test.cpp)
    target_link_libraries(maque_renderer_null_test PRIVATE maque::renderer_null)
    maque_deploy_asan(maque_renderer_null_test)
    add_test(NAME renderer_null COMMAND maque_renderer_null_test)
endif()
