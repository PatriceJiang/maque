# maque::net —— 网络（libwebsockets）：WebSocket 客户端底座
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LWS QUIET libwebsockets)
endif()

if(LWS_FOUND)
    add_library(maque_net STATIC src/net.cpp)
    add_library(maque::net ALIAS maque_net)
    target_include_directories(maque_net PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include" ${LWS_INCLUDE_DIRS})
    target_link_directories(maque_net PUBLIC ${LWS_LIBRARY_DIRS})
    target_link_libraries(maque_net PUBLIC ${LWS_LIBRARIES})
    set_target_properties(maque_net PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

    if(MAQUE_BUILD_TESTS)
        add_executable(maque_net_test tests/net_test.cpp)
maque_deploy_asan(maque_net_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
        target_link_libraries(maque_net_test PRIVATE maque::net)
        add_test(NAME net COMMAND maque_net_test)
        target_include_directories(maque_net_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
    endif()
    target_include_directories(maque_net PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
else()
    # 注意：这两条 include 必须在 target 存在时才调用 —— 放在 else 之外会让
    # configure 直接失败（"Cannot specify include directories for target ... which is not
    # built by this project"），缺 libwebsockets 的平台上连配置都过不去。
    message(WARNING "maque: libwebsockets not found; net 模块关闭")
endif()
