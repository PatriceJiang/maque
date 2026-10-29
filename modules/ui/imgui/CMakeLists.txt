# maque::imgui -- platform-independent command export plus an optional SDL backend.
set(_imgui "${CMAKE_SOURCE_DIR}/third_party/imgui")

if(MAQUE_WASM OR EMSCRIPTEN OR MAQUE_HEADLESS_STUBS)
    set(MAQUE_IMGUI_BACKEND_SOURCES src/backend_stub.cpp)
else()
    if(TARGET SDL2::SDL2)
        set(_imgui_sdl2_lib SDL2::SDL2)
    elseif(TARGET SDL2)
        set(_imgui_sdl2_lib SDL2)
    else()
        find_path(_imgui_sdl2_inc SDL.h PATH_SUFFIXES SDL2)
        find_library(_imgui_sdl2_lib NAMES SDL2)
    endif()
    if(_imgui_sdl2_lib AND (TARGET ${_imgui_sdl2_lib} OR _imgui_sdl2_inc))
        set(MAQUE_IMGUI_BACKEND_SOURCES
            src/backend_sdl.cpp
            "${_imgui}/backends/imgui_impl_sdlrenderer2.cpp")
    else()
        set(MAQUE_IMGUI_BACKEND_SOURCES src/backend_stub.cpp)
        message(WARNING "maque: SDL2 not found; ImGui SDL backend disabled")
    endif()
endif()

add_library(maque_imgui STATIC
    src/imgui.cpp
    src/widgets.cpp
    ${MAQUE_IMGUI_BACKEND_SOURCES}
    "${_imgui}/imgui.cpp"
    "${_imgui}/imgui_draw.cpp"
    "${_imgui}/imgui_tables.cpp"
    "${_imgui}/imgui_widgets.cpp"
    "${_imgui}/imgui_demo.cpp")
add_library(maque::imgui ALIAS maque_imgui)

target_include_directories(maque_imgui
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC "${_imgui}" "${_imgui}/backends")
if(_imgui_sdl2_inc)
    target_include_directories(maque_imgui PUBLIC "${_imgui_sdl2_inc}")
endif()
if(_imgui_sdl2_lib AND NOT MAQUE_WASM)
    target_link_libraries(maque_imgui PUBLIC ${_imgui_sdl2_lib})
endif()

set_target_properties(maque_imgui PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_imgui PUBLIC maque::tracy)
if(MAQUE_WASM OR EMSCRIPTEN OR MAQUE_HEADLESS_STUBS)
    target_compile_definitions(maque_imgui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS=1)
endif()

target_compile_options(maque_imgui PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNUCXX>:-Wno-everything>
    $<$<CXX_COMPILER_ID:MSVC>:/W0>)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_imgui_test tests/imgui_test.cpp)
    maque_deploy_asan(maque_imgui_test)
    target_link_libraries(maque_imgui_test PRIVATE maque::imgui)
    add_test(NAME imgui COMMAND maque_imgui_test)
endif()
