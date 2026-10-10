# maque::gfx —— WebGPU 渲染插件（.so，实现 DSL 生成的 mq_renderer_v1）
set(WGPU_NATIVE_ROOT "${CMAKE_SOURCE_DIR}/third_party/wgpu-native"
    CACHE PATH "wgpu-native SDK root (include/webgpu + lib)")
find_library(WGPU_NATIVE_LIB NAMES wgpu_native
    PATHS "${WGPU_NATIVE_ROOT}/lib" NO_DEFAULT_PATH)

if(WGPU_NATIVE_LIB)
    add_library(maque_gfx MODULE src/gfx.cpp)
    target_include_directories(maque_gfx PRIVATE "${WGPU_NATIVE_ROOT}/include")
    target_link_libraries(maque_gfx PRIVATE ${WGPU_NATIVE_LIB} maque::module maque::schema)
    set_target_properties(maque_gfx PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        PREFIX "" OUTPUT_NAME "maque_gfx"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        BUILD_RPATH "${WGPU_NATIVE_ROOT}/lib")

    if(MAQUE_BUILD_TESTS)
        add_executable(maque_gfx_test tests/gfx_test.cpp)
        target_link_libraries(maque_gfx_test PRIVATE maque::module maque::schema)
        target_compile_definitions(maque_gfx_test PRIVATE MQ_GFX_PATH="$<TARGET_FILE:maque_gfx>")
        add_dependencies(maque_gfx_test maque_gfx)
        add_test(NAME gfx COMMAND maque_gfx_test)
    endif()
else()
    message(WARNING "maque: 未找到 wgpu-native；maque::gfx 关闭")
endif()
