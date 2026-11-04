# maque::runtime -- scene runtime and embeddable host API.
# runtime.cpp 拆分的内部头（不对外）：类型 + Engine/Scene 声明。
# 只有 modules/runtime 的 .cpp 包含；公开面仍是 include/runtime/*.h（schema 生成的 shim）。
add_library(maque_runtime STATIC
    src/runtime.cpp src/gltf_model.cpp
    src/runtime_common.h src/runtime_scene.h src/runtime_engine.h
    # 宿主 API（schema api_list 的 310 个实现）按前缀分域
    src/api/system.cpp src/api/node.cpp src/api/ui.cpp src/api/scene.cpp
    src/api/spine.cpp src/api/imgui.cpp src/api/physics.cpp src/api/nav.cpp
    src/api/audio.cpp src/api/storage.cpp
    # 控制面（schema control_list 的 57 个方法）按域
    src/control/system.cpp src/control/scene.cpp src/control/world.cpp
    src/control/render.cpp src/control/storage.cpp src/control/audio.cpp
    src/control/input.cpp)
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
