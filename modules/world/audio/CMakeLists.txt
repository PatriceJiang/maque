# maque::audio —— 音频（分层解耦）
#
#   maque_audio_core : Mixer + 解码器（纯 C++，无 SDL / VFS）
#   maque_audio      : C ABI 门面 + 设备后端（null；SDL2 存在时加 backend_sdl）
#
# 换后端（miniaudio/SoLoud/平台原生）= 新增一个 AudioDevice 实现并在这里加一个源文件，
# 核心/解码器/C ABI/runtime 不动。
add_library(maque_audio_core STATIC
    src/mixer.cpp
    src/decode_wav.cpp
    src/decode_ogg.cpp
    src/decode_mp3.cpp
    "${CMAKE_SOURCE_DIR}/third_party/stb_vorbis/stb_vorbis.c"   # public domain（C）
)
target_include_directories(maque_audio_core PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/stb_vorbis"
    "${CMAKE_SOURCE_DIR}/third_party/dr_mp3")
add_library(maque::audio_core ALIAS maque_audio_core)
target_include_directories(maque_audio_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_audio_core PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

add_library(maque_audio STATIC
    src/audio.cpp
    src/backend_null.cpp
)
add_library(maque::audio ALIAS maque_audio)
target_include_directories(maque_audio PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MAQUE_GEN_DIR}")   # DSL 生成的 abi/audio.h（docs/21）
target_link_libraries(maque_audio PUBLIC maque_audio_core maque::tracy)
set_target_properties(maque_audio PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(NOT MAQUE_WASM AND NOT EMSCRIPTEN AND NOT MAQUE_HEADLESS_STUBS)
    if(TARGET SDL2::SDL2)
        set(_maque_audio_sdl2 SDL2::SDL2)
    elseif(TARGET SDL2)
        set(_maque_audio_sdl2 SDL2)
    else()
        find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
        find_library(SDL2_LIBRARY NAMES SDL2)
        if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
            set(_maque_audio_sdl2 "${SDL2_LIBRARY}")
        endif()
    endif()
endif()
if(_maque_audio_sdl2)
    target_sources(maque_audio PRIVATE src/backend_sdl.cpp)
    target_compile_definitions(maque_audio PUBLIC MAQUE_HAVE_SDL_AUDIO=1)
    if(SDL2_INCLUDE_DIR)
        target_include_directories(maque_audio PRIVATE "${SDL2_INCLUDE_DIR}")
    endif()
    target_link_libraries(maque_audio PRIVATE ${_maque_audio_sdl2})
    message(STATUS "maque::audio: SDL2 audio 后端已启用")
else()
    message(STATUS "maque::audio: 未找到 SDL2 → 仅 null 后端")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_audio_test tests/audio_test.cpp)
    maque_deploy_asan(maque_audio_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_audio_test PRIVATE maque::audio)
    target_compile_definitions(maque_audio_test PRIVATE
        MAQUE_AUDIO_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/data/audio")
    add_test(NAME audio COMMAND maque_audio_test)
endif()
