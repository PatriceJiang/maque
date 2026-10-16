# maque::gfx —— WebGPU 渲染插件（.so，实现 DSL 生成的 mq_renderer_v1）
set(WGPU_NATIVE_ROOT "${CMAKE_SOURCE_DIR}/third_party/wgpu-native"
    CACHE PATH "wgpu-native SDK root (include/webgpu + lib)")
find_library(WGPU_NATIVE_LIB NAMES wgpu_native
    PATHS "${WGPU_NATIVE_ROOT}/lib" NO_DEFAULT_PATH)

if(WGPU_NATIVE_LIB)
    # 内建 shader 打包流水线（assets/builtin/shaders/<name>/*.wgsl → 压缩源码，可按 shader 剔除）
    include("${CMAKE_SOURCE_DIR}/cmake/shader_pack.cmake")
    maque_builtin_shaders("${CMAKE_SOURCE_DIR}/assets/builtin/shaders"
                          "${CMAKE_BINARY_DIR}/generated/builtin_shaders"
                          BUILTIN_SHADER_SOURCES BUILTIN_SHADER_DIR)

    add_library(maque_gfx MODULE src/gfx.cpp)
    target_include_directories(maque_gfx PRIVATE "${WGPU_NATIVE_ROOT}/include")
    target_link_libraries(maque_gfx PRIVATE ${WGPU_NATIVE_LIB} maque::module maque::schema maque::builtin_shaders)
    set_target_properties(maque_gfx PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        PREFIX "" OUTPUT_NAME "maque_gfx"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        BUILD_RPATH "${WGPU_NATIVE_ROOT}/lib")
    # Windows：MODULE 走 RUNTIME 目录；显式指到构建根，与 Linux 的 .so 位置保持一致
    #（MQ_RENDERER_DEFAULT 注入的是 $<TARGET_FILE:maque_gfx> 绝对路径，故两者都可用）。
    if(WIN32)
        set_target_properties(maque_gfx PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif()
    # ASan 开着时把运行时 DLL 放到插件旁：插件由宿主 dlopen，其 ASan 引用同样要解析。
    maque_deploy_asan(maque_gfx)

    # wgpu-native 运行时 DLL（wgpu_native.dll）：不在 PATH 上，插件加载时会报错，
    # 必须复制到插件（及宿主）旁边 —— Windows 的 DLL 搜索优先看加载者所在目录。
    if(WIN32)
        set(WGPU_NATIVE_DLL "${WGPU_NATIVE_ROOT}/lib/wgpu_native.dll")
        if(EXISTS "${WGPU_NATIVE_DLL}")
            add_custom_command(TARGET maque_gfx POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${WGPU_NATIVE_DLL}" "$<TARGET_FILE_DIR:maque_gfx>"
                VERBATIM
                COMMENT "maque: 部署 wgpu_native.dll → maque_gfx")
        else()
            message(WARNING "maque: 未找到 ${WGPU_NATIVE_DLL}；Windows 上插件运行时会缺 DLL"
                            "（跑 tools/win/fetch-deps.ps1 获取）")
        endif()
    endif()

    if(MAQUE_BUILD_TESTS)
        add_executable(maque_gfx_test tests/gfx_test.cpp)
        target_link_libraries(maque_gfx_test PRIVATE maque::module maque::schema)
        target_compile_definitions(maque_gfx_test PRIVATE MQ_GFX_PATH="$<TARGET_FILE:maque_gfx>")
        add_dependencies(maque_gfx_test maque_gfx)
        # Windows 上测试要**真正 dlopen 插件**，因此 exe 旁必须同时具备：
        #   ASan 运行时（maque_deploy_asan）与插件依赖的 wgpu_native.dll
        #（加载器不会去"插件自己所在目录"找它的依赖，只看 exe 目录 / PATH / 工作目录）。
        # 缺任何一个的表现都是 mq_host_load 失败 → 测试静默 SKIP (no plugin)，覆盖为零。
        maque_deploy_plugin_deps(maque_gfx_test)
        add_test(NAME gfx COMMAND maque_gfx_test)

        # 文本像素级精确（pixel 2 pixel）：矩形图元 + nearest + 像素对齐 vs 字体位图逐像素比对
        add_executable(maque_text_pixel_test tests/text_pixel_test.cpp)
        target_link_libraries(maque_text_pixel_test PRIVATE maque::module maque::schema maque::text)
        target_compile_definitions(maque_text_pixel_test PRIVATE MQ_GFX_PATH="$<TARGET_FILE:maque_gfx>")
        add_dependencies(maque_text_pixel_test maque_gfx)
        maque_deploy_plugin_deps(maque_text_pixel_test)
        add_test(NAME text_pixel COMMAND maque_text_pixel_test)

        # 内建 shader 打包：注册表 / 解压缓存 / 与资产逐字节一致（资产 ↔ 打包的漂移检查）
        add_executable(maque_builtin_shader_test tests/builtin_shader_test.cpp)
        target_link_libraries(maque_builtin_shader_test PRIVATE maque::builtin_shaders)
        target_compile_definitions(maque_builtin_shader_test PRIVATE
            MQ_SHADER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/builtin/shaders"
            MQ_SHADER_EXPECT_COUNT=$<TARGET_PROPERTY:maque_builtin_shaders,MAQUE_SHADER_COUNT>)
        maque_deploy_asan(maque_builtin_shader_test)
        add_test(NAME builtin_shaders COMMAND maque_builtin_shader_test)
    endif()
else()
    message(WARNING "maque: 未找到 wgpu-native；maque::gfx 关闭")
endif()
