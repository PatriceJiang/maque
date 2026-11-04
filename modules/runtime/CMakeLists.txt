# maque::runtime -- scene runtime and embeddable host API.
# runtime.cpp 鎷嗗垎鐨勫唴閮ㄥご锛堜笉瀵瑰锛夛細绫诲瀷 + Engine/Scene 澹版槑銆?
# 鍙湁 modules/runtime 鐨?.cpp 鍖呭惈锛涘叕寮€闈粛鏄?include/runtime/*.h锛坰chema 鐢熸垚鐨?shim锛夈€?
add_library(maque_runtime STATIC
    src/runtime.cpp src/gltf_model.cpp
    src/runtime_common.h src/runtime_scene.h src/runtime_engine.h
    # 瀹夸富 API锛坰chema api_list 鐨?310 涓疄鐜帮級鎸夊墠缂€鍒嗗煙
    src/api/system.cpp src/api/node.cpp src/api/ui.cpp src/api/scene.cpp
    src/api/spine.cpp src/api/imgui.cpp src/api/physics.cpp src/api/nav.cpp
    src/api/audio.cpp src/api/storage.cpp
    # 鎺у埗闈紙schema control_list 鐨?57 涓柟娉曪級鎸夊煙
    src/control/system.cpp src/control/scene.cpp src/control/world.cpp
    src/control/render.cpp src/control/storage.cpp src/control/audio.cpp
    src/control/input.cpp
    # 资源（纹理/环境/glTF 装配与动画采样）
    src/assets/texture.cpp src/assets/env.cpp src/assets/gltf.cpp src/assets/gltf_anim.cpp
    # 特性层（Spine / 引擎侧 ImGui 面板 / F4 选项）
    src/anim/spine.cpp
    src/ui/debug_panel.cpp src/ui/engine_options.cpp)
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
