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
        target_link_libraries(maque_net_test PRIVATE maque::net)
        add_test(NAME net COMMAND maque_net_test)
    endif()
else()
    message(WARNING "maque: libwebsockets not found; net 模块关闭")
endif()
target_include_directories(maque_net PUBLIC "${CMAKE_BINARY_DIR}/modules/core/data/generated")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_net_test PUBLIC "${CMAKE_BINARY_DIR}/modules/core/data/generated")   # DSL 生成的 ABI 头（docs/21）
