# maque::control —— 控制平面（JSON-RPC 帧；方法由宿主注册）
add_library(maque_control STATIC src/control.cpp src/server.cpp)
add_library(maque::control ALIAS maque_control)
target_include_directories(maque_control PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_control PUBLIC maque::persist)
set_target_properties(maque_control PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(maque_control PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）

# server.cpp 的 TCP 控制端点在 Windows 走 Winsock2（winsock2.h/ws2tcpip.h），
# 需要显式链接 ws2_32；Linux 用 POSIX socket，无需额外库。
if(WIN32)
    target_link_libraries(maque_control PUBLIC ws2_32)
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_control_test tests/control_test.cpp)
maque_deploy_asan(maque_control_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_control_test PRIVATE maque::control)
    target_include_directories(maque_control_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
    add_test(NAME control COMMAND maque_control_test)
endif()
