# maque::gfx —— WebGPU 渲染插件（.so，实现 DSL 生成的 mq_renderer_v1）
set(WGPU_NATIVE_ROOT "${CMAKE_SOURCE_DIR}/third_party/wgpu-native"
    CACHE PATH "wgpu-native SDK root (include/webgpu + lib)")
if(MAQUE_ANDROID AND MAQUE_ANDROID_JNI_LIBS_DIR)
    set(WGPU_NATIVE_LIB "${MAQUE_ANDROID_JNI_LIBS_DIR}/${ANDROID_ABI}/libwgpu_native.so"
        CACHE FILEPATH "Android ABI-specific wgpu-native library" FORCE)
endif()
find_library(WGPU_NATIVE_LIB NAMES wgpu_native
    PATHS "${WGPU_NATIVE_ROOT}/lib" NO_DEFAULT_PATH)

if(WGPU_NATIVE_LIB)
    # 内建 shader 打包流水线（assets/builtin/shaders/<name>/*.wgsl → 压缩源码，可按 shader 剔除）
    include("${CMAKE_SOURCE_DIR}/cmake/shader_pack.cmake")
    maque_builtin_shaders("${CMAKE_SOURCE_DIR}/assets/builtin/shaders"
                          "${CMAKE_BINARY_DIR}/generated/builtin_shaders"
                          BUILTIN_SHADER_SOURCES BUILTIN_SHADER_DIR)

    if(MAQUE_ANDROID)
        add_library(maque_gfx STATIC src/gfx.cpp)
        add_library(maque::gfx ALIAS maque_gfx)
    else()
        add_library(maque_gfx MODULE src/gfx.cpp)
    endif()
    target_include_directories(maque_gfx PRIVATE "${WGPU_NATIVE_ROOT}/include")
    if(MAQUE_ANDROID)
        target_link_libraries(maque_gfx PRIVATE ${WGPU_NATIVE_LIB} maque::module maque::schema maque::builtin_shaders log)
    else()
        target_link_libraries(maque_gfx PRIVATE ${WGPU_NATIVE_LIB} maque::module maque::schema maque::builtin_shaders)
    endif()
    set_target_properties(maque_gfx PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
    if(NOT MAQUE_ANDROID)
        set_target_properties(maque_gfx PROPERTIES
            PREFIX "" OUTPUT_NAME "maque_gfx"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            BUILD_RPATH "${WGPU_NATIVE_ROOT}/lib")
    endif()
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

        # 两色着色（Tint Black）像素级：mesh 顶点 48B（含 @location(3) dark）+ 官方 GLSL 公式。
        # 断言"dark 不给 / dark=黑"逐字节相同（不回归）、"非黑 dark"明显不同（有鉴别力）。
        # 注意：官方 raptor/batching 素材**没有** dark 通道（实测），故本测试用合成纹理 + alien 的 dark 值。
        add_executable(maque_gfx_twocolor_test tests/twocolor_test.cpp)
        target_link_libraries(maque_gfx_twocolor_test PRIVATE maque::module maque::schema)
        target_compile_definitions(maque_gfx_twocolor_test PRIVATE MQ_GFX_PATH="$<TARGET_FILE:maque_gfx>")
        add_dependencies(maque_gfx_twocolor_test maque_gfx)
        maque_deploy_plugin_deps(maque_gfx_twocolor_test)
        add_test(NAME gfx_twocolor COMMAND maque_gfx_twocolor_test)

        # 官方素材 → 像素：用官方 alien-pro.json（唯一 dark 非黑：eyeball light=dfa921ff/dark=c57c1e）
        # 走 spine2d 取绘制数据 + dark 通道，再进 gfx 渲染并断言字节级像素值。
        # 需要 spine2d 与 tests/data/spine 的素材；素材缺失时测试自行 SKIP（返回 0）。
        add_executable(maque_gfx_twocolor_spine_test tests/twocolor_spine_test.cpp)
        target_link_libraries(maque_gfx_twocolor_spine_test PRIVATE maque::module maque::schema maque::spine2d)
        target_compile_definitions(maque_gfx_twocolor_spine_test PRIVATE MQ_GFX_PATH="$<TARGET_FILE:maque_gfx>")
        add_dependencies(maque_gfx_twocolor_spine_test maque_gfx)
        maque_deploy_plugin_deps(maque_gfx_twocolor_spine_test)
        add_test(NAME gfx_twocolor_spine
                 COMMAND maque_gfx_twocolor_spine_test "${CMAKE_SOURCE_DIR}/tests/data/spine")

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
