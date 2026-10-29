# maque::runtime -- scene runtime and embeddable host API.
add_library(maque_runtime STATIC src/runtime.cpp src/gltf_model.cpp)
add_library(maque::runtime ALIAS maque_runtime)
target_include_directories(maque_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(MAQUE_RUNTIME_DEPS
    maque::canvas maque::sim maque::text maque::persist maque::script
    maque::handle maque::vfs maque::storage maque::image maque::genbind maque::api
    maque::gencontrol maque::genevents maque::observability maque::control
    maque::platform maque::input maque::watcher maque::physics maque::nav
    maque::spine2d maque::imgui maque::tracy maque::audio)
if(MAQUE_WASM OR EMSCRIPTEN OR MAQUE_HEADLESS_STUBS)
    list(APPEND MAQUE_RUNTIME_DEPS maque::renderer_null)
    target_compile_definitions(maque_runtime PRIVATE MQ_RENDERER_STATIC_NULL=1)
elseif(MAQUE_ANDROID)
    list(APPEND MAQUE_RUNTIME_DEPS maque::gfx maque::module)
    target_compile_definitions(maque_runtime PRIVATE MQ_RENDERER_STATIC_GFX=1)
else()
    list(APPEND MAQUE_RUNTIME_DEPS maque::module)
    if(TARGET maque_gfx)
        target_compile_definitions(maque_runtime PRIVATE MQ_RENDERER_DEFAULT="$<TARGET_FILE:maque_gfx>")
    endif()
endif()
target_link_libraries(maque_runtime PUBLIC ${MAQUE_RUNTIME_DEPS})
if(MAQUE_ANDROID)
    target_link_libraries(maque_runtime PUBLIC z)
endif()
set_target_properties(maque_runtime PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(maque_runtime PUBLIC "${MAQUE_GEN_DIR}")
