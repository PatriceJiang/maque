# maque_inspector -- standalone SDL2/ImGui inspector (native only).
find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
find_library(SDL2_LIBRARY NAMES SDL2)
if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
    message(WARNING "maque: SDL2 not found; inspector disabled")
    return()
endif()

add_executable(maque_inspector
    src/main.cpp
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp")
target_include_directories(maque_inspector PRIVATE
    src "${SDL2_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends")
target_link_libraries(maque_inspector PRIVATE "${SDL2_LIBRARY}")
if(WIN32)
    target_link_libraries(maque_inspector PRIVATE ws2_32)
endif()
set_target_properties(maque_inspector PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
maque_deploy_asan(maque_inspector)

if(MAQUE_BUILD_TESTS)
    add_test(NAME inspector_headless COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/inspector_e2e.sh"
             $<TARGET_FILE:maque_inspector> $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/sokoban/sokoban.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/inspector_e2e")
endif()
