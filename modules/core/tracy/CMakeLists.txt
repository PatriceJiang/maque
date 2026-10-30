# maque::tracy —— Tracy 性能分析封装（docs/29）。
#
# 两种构建形态：
#   · MAQUE_TRACY=ON（默认）：静态库 src/tracy.cpp + vendored Tracy client（third_party/tracy）；
#     各模块 `target_link_libraries(<mod> PRIVATE maque::tracy)` 即获得封装宏与彩色 zone。
#   · MAQUE_TRACY=OFF：INTERFACE 目标，只提供头（宏全部展开为 no-op）→ 无第三方依赖、零开销。
#
# 子开关（docs/29 §10；只在 MAQUE_TRACY=ON 时生效，OFF 时全部按 0 导出）：
#   · MAQUE_TRACY_MEMORY —— 内存记账（script/node/texture/gpu/font；默认 ON，开销 = 计数器）
#   · MAQUE_TRACY_ALLOC  —— 全局 operator new/delete 拦截（额外覆盖 heap 域；默认 OFF：
#                            每块 +16B 头 + 每分配两次原子操作，属于"性能剖析构建"选项）
#   · MAQUE_TRACY_LOCKS  —— 锁追踪（MQ_TRACY_LOCKABLE；默认 ON，未使用则零成本）
#
# 公共宏头：core/tracy/mq_tracy.h（各模块只 include 它，不直接碰 Tracy）。
if(MAQUE_TRACY)
    set(MAQUE_TRACY_SOURCES src/tracy.cpp)
    if(MAQUE_TRACY_ALLOC AND NOT MAQUE_TRACY_MEMORY)
        message(WARNING "maque: MAQUE_TRACY_ALLOC 需要 MAQUE_TRACY_MEMORY —— 已关闭 ALLOC")
        set(MAQUE_TRACY_ALLOC OFF)
    endif()
    if(MAQUE_TRACY_ALLOC AND MSVC AND MAQUE_SANITIZE MATCHES "address")
        # clang-cl 的 Windows ASan 运行时**自己定义了 operator new**（不能像 Linux 那样靠 malloc 拦截），
        # 与我们的全局替换产生 lld-link "operator new ... was replaced"。两者只能二选一。
        message(WARNING "maque: Windows ASan 自带 operator new 替换，与 MAQUE_TRACY_ALLOC 不兼容 "
                        "→ 已关闭 ALLOC（要测它请用不带 ASan 的构建）")
        set(MAQUE_TRACY_ALLOC OFF)
    endif()
    if(MAQUE_TRACY_ALLOC)
        list(APPEND MAQUE_TRACY_SOURCES src/mem_alloc.cpp)
    endif()

    add_library(maque_tracy STATIC ${MAQUE_TRACY_SOURCES})
    add_library(maque::tracy ALIAS maque_tracy)
    target_include_directories(maque_tracy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(maque_tracy PUBLIC maque::tracy_client)
    # 开关 → 0/1 编译宏（配置期定值，避免 genex 在各工具链上的差异）。
    if(MAQUE_TRACY_MEMORY)
        set(MAQUE_TRACY_MEMORY_DEF 1)
    else()
        set(MAQUE_TRACY_MEMORY_DEF 0)
    endif()
    if(MAQUE_TRACY_ALLOC)
        set(MAQUE_TRACY_ALLOC_DEF 1)
    else()
        set(MAQUE_TRACY_ALLOC_DEF 0)
    endif()
    if(MAQUE_TRACY_LOCKS)
        set(MAQUE_TRACY_LOCKS_DEF 1)
    else()
        set(MAQUE_TRACY_LOCKS_DEF 0)
    endif()
    target_compile_definitions(maque_tracy PUBLIC
        MQ_TRACY_ENABLED=1
        MQ_TRACY_MEMORY_ENABLED=${MAQUE_TRACY_MEMORY_DEF}
        MQ_TRACY_ALLOC_ENABLED=${MAQUE_TRACY_ALLOC_DEF}
        MQ_TRACY_LOCKS_ENABLED=${MAQUE_TRACY_LOCKS_DEF})
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
    target_compile_definitions(maque_tracy INTERFACE
        MQ_TRACY_ENABLED=0
        MQ_TRACY_MEMORY_ENABLED=0
        MQ_TRACY_ALLOC_ENABLED=0
        MQ_TRACY_LOCKS_ENABLED=0)
endif()
