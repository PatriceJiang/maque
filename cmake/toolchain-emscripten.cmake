# Compatibility wrapper for callers that explicitly pass this file as CMAKE_TOOLCHAIN_FILE.
# Prefer: emcmake cmake -S . -B build-wasm -DMAQUE_WASM=ON

if(NOT DEFINED ENV{EMSDK})
    message(FATAL_ERROR "EMSDK is not set. Activate emsdk, then use emcmake with -DMAQUE_WASM=ON.")
endif()

file(TO_CMAKE_PATH "$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
     _maque_emscripten_toolchain)
if(NOT EXISTS "${_maque_emscripten_toolchain}")
    message(FATAL_ERROR "Emscripten SDK toolchain not found: ${_maque_emscripten_toolchain}")
endif()
include("${_maque_emscripten_toolchain}")
set(MAQUE_WASM ON CACHE BOOL "Build the WASM/browser host profile" FORCE)
