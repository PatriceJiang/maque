# maque_inspector —— 独立进程的游戏检查器（plans/17；复用控制面 JSON-RPC）
add_executable(maque_inspector
    src/main.cpp
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp")
find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
find_library(SDL2_LIBRARY NAMES SDL2)
if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
    message(WARNING "maque: SDL2 not found; inspector 关闭")
    return()
endif()
target_include_directories(maque_inspector PRIVATE
    src "${SDL2_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends")
target_link_libraries(maque_inspector PRIVATE "${SDL2_LIBRARY}")
# client.h 的 TCP 端点在 Windows 走 Winsock2（winsock2.h/ws2tcpip.h），需显式链接 ws2_32；
# Linux 用 POSIX socket，无需额外库。
if(WIN32)
    target_link_libraries(maque_inspector PRIVATE ws2_32)
endif()
set_target_properties(maque_inspector PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
maque_deploy_asan(maque_inspector)   # Windows：ASan 运行时 DLL 放到 exe 旁（inspector 不加载渲染插件）

# 无显示环境必需：ImGui 后端只用 SDL2（不碰 GL/EGL）
if(MAQUE_BUILD_TESTS)
    # 无头冒烟：连不上时返回 2；连上后跑只读方法并打印响应
    add_test(NAME inspector_headless COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/inspector_e2e.sh"
             $<TARGET_FILE:maque_inspector> $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/sokoban/sokoban.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/inspector_e2e")
endif()
