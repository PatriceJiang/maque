# maque::gfx —— WebGPU 后端（实现 maque::render::Renderer）
set(WGPU_NATIVE_ROOT "${CMAKE_SOURCE_DIR}/third_party/wgpu-native"
    CACHE PATH "wgpu-native SDK root (include/webgpu + lib)")

find_library(WGPU_NATIVE_LIB NAMES wgpu_native
    PATHS "${WGPU_NATIVE_ROOT}/lib" NO_DEFAULT_PATH)

if(WGPU_NATIVE_LIB)
    add_library(maque_gfx STATIC src/gfx.cpp)
    add_library(maque::gfx ALIAS maque_gfx)
    target_include_directories(maque_gfx PUBLIC "${WGPU_NATIVE_ROOT}/include")
    target_link_libraries(maque_gfx PUBLIC maque::render ${WGPU_NATIVE_LIB})
    target_compile_features(maque_gfx PUBLIC cxx_std_23)
    set_target_properties(maque_gfx PROPERTIES
        INSTALL_RPATH "${WGPU_NATIVE_ROOT}/lib"
        BUILD_RPATH "${WGPU_NATIVE_ROOT}/lib")

    if(MAQUE_BUILD_TESTS)
        add_executable(maque_gfx_test tests/gfx_test.cpp)
        target_link_libraries(maque_gfx_test PRIVATE maque::gfx)
        set_target_properties(maque_gfx_test PROPERTIES BUILD_RPATH "${WGPU_NATIVE_ROOT}/lib")
        add_test(NAME gfx COMMAND maque_gfx_test)
    endif()
else()
    message(WARNING "maque: 未找到 wgpu-native (${WGPU_NATIVE_ROOT}/lib)；maque::gfx 关闭")
endif()
