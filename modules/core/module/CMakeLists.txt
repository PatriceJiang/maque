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
    # 该测试模块是纯 C 的 .dll/.so，只用来验证 host 的加载/查询/卸载路径（内容极简，
    # 不含引擎内存逻辑）。开着 ASan 时**显式关掉插桩**：
    #   · clang-cl 下 DLL 被插桩会要求 ASan 运行时以 DLL 形式链接，实测报
    #     "lld-link: undefined symbol: __asan_init"；
    #   · 它的用途与内存安全无关，不值得为此引入运行时依赖。
    # 注意 maque_gfx 插件**不**这么做 —— 渲染插件必须插桩（越界要能被抓到）。
    if(MAQUE_SANITIZE)
        if(MSVC)
            target_compile_options(maque_test_module PRIVATE -fno-sanitize=address)
            target_link_options(maque_test_module PRIVATE -fno-sanitize=address)
        else()
            target_compile_options(maque_test_module PRIVATE -fno-sanitize=${MAQUE_SANITIZE})
            target_link_options(maque_test_module PRIVATE -fno-sanitize=${MAQUE_SANITIZE})
        endif()
    endif()

    add_executable(maque_host_test tests/host_test.cpp)
maque_deploy_asan(maque_host_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_include_directories(maque_host_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(maque_host_test PRIVATE maque::module)
    add_dependencies(maque_host_test maque_test_module)
    add_test(NAME module COMMAND maque_host_test "$<TARGET_FILE:maque_test_module>")
endif()
target_include_directories(maque_module PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_test_module PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
