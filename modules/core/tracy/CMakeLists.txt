# maque::tracy —— Tracy 性能分析封装（docs/29）。
#
# 两种构建形态：
#   · MAQUE_TRACY=ON（默认）：静态库 src/tracy.cpp + vendored Tracy client（third_party/tracy）；
#     各模块 `target_link_libraries(<mod> PRIVATE maque::tracy)` 即获得封装宏与彩色 zone。
#   · MAQUE_TRACY=OFF：INTERFACE 目标，只提供头（宏全部展开为 no-op）→ 无第三方依赖、零开销。
#
# 公共宏头：core/tracy/mq_tracy.h（各模块只 include 它，不直接碰 Tracy）。
if(MAQUE_TRACY)
    add_library(maque_tracy STATIC src/tracy.cpp)
    add_library(maque::tracy ALIAS maque_tracy)
    target_include_directories(maque_tracy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(maque_tracy PUBLIC maque::tracy_client)
    target_compile_definitions(maque_tracy PUBLIC MQ_TRACY_ENABLED=1)
    target_compile_features(maque_tracy PUBLIC cxx_std_23)

    if(MAQUE_BUILD_TESTS)
        add_executable(maque_tracy_test tests/tracy_test.cpp)
        maque_deploy_asan(maque_tracy_test)
        target_link_libraries(maque_tracy_test PRIVATE maque::tracy)
        add_test(NAME tracy COMMAND maque_tracy_test)
    endif()
else()
    add_library(maque_tracy INTERFACE)
    add_library(maque::tracy ALIAS maque_tracy)
    target_include_directories(maque_tracy INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()
