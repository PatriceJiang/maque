
cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Maque CXX C ASM)

option(MAQUE_BUILD_TESTS "Build maque unit tests" ON)
option(MAQUE_WINDOWS_DEPS "Windows: 自动 include .deps/deps.cmake（第三方依赖预置变量）" ON)

# 确定性 D8（docs/10 §2）：关闭 FMA 收缩 —— 浮点结果不随编译器/优化级别变化。
# 符号可见性 R8（docs/02）：默认 hidden，跨模块导出只经 MQ_MODULE_EXPORT / extern "C"。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-ffp-contract=off)
endif()
# MSVC 风格命令行（cl.exe / clang-cl）：-fvisibility / -ffp-contract 是 GCC 风格 flag，不适用。
#   注意 clang-cl **同时**定义 __clang__ 与 _MSC_VER，故用 MSVC 判别而非 CMAKE_CXX_COMPILER_ID。
if(NOT MSVC)
    add_compile_options(-fvisibility=hidden)
    add_link_options(-fvisibility=hidden)
endif()

# C++23（约定 1）：MSVC 系需要显式 /std:c++latest —— 少了它连 std::print 都不存在
#（实测：默认模式下 `no member named 'print' in namespace 'std'`）。
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>)
    add_compile_options(/permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W3)
    add_compile_options(/bigobj)     # 模块多、模板多，默认 obj 段数不够
    add_compile_options(/fp:precise) # 确定性 D8：不做 FMA 收缩
endif()

# ---- ASan / UBSan（可选）------------------------------------------------------
# 用法：-DMAQUE_SANITIZE=address[,undefined]
#   Linux (GCC/Clang): -fsanitize=address / -fsanitize=undefined
#   Windows (clang-cl): 仅 address；与 /MD 配合走 clang 自带的**动态** ASan 运行时。
# 鉴别力自检（不是"能构建就算过"）：
#   clang-cl /std:c++latest -fsanitize=address t.cpp && ./t.exe   # 期望 heap-buffer-overflow
set(MAQUE_SANITIZE "" CACHE STRING "启用 sanitizer（address / undefined / address,undefined；空=关闭）")
set(MAQUE_ASAN_RUNTIME_DIR "" CACHE PATH "ASan 运行时 DLL 所在目录（留空=自动探测）")
if(MAQUE_SANITIZE)
    if(MSVC)
        if(NOT MAQUE_SANITIZE MATCHES "address")
            message(WARNING "maque: Windows 上 -fsanitize 仅支持 address；已忽略 '${MAQUE_SANITIZE}' 中的其它项")
        endif()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
        # ASan 与**调试版 CRT** 不兼容：clang 直接拒绝
        #   "invalid argument '/MDd' not allowed with '-fsanitize=address'"
        #   note: AddressSanitizer doesn't support linking with debug runtime libraries yet
        # 故强制 /MD。Debug 构建类型的其它语义（无优化、带调试信息）保留。
        if(CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDebugDLL" OR NOT CMAKE_MSVC_RUNTIME_LIBRARY)
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
            message(STATUS "maque: ASan 需要 /MD —— 已把 MSVC 运行时从 /MDd 改为 /MD")
        endif()

        # 动态 ASan 运行时（clang_rt.asan_dynamic-x86_64.dll）不在 PATH 上：
        # 不处理的话可执行文件启动即 0xC0000135（DLL not found）。
        # 用编译器的 resource dir 反推（clang-cl -print-resource-dir → <.../lib/clang/20>）。
        if(NOT MAQUE_ASAN_RUNTIME_DIR)
            execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
                            OUTPUT_VARIABLE _maque_rt OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET RESULT_VARIABLE _maque_rt_rc)
            if(_maque_rt_rc EQUAL 0 AND _maque_rt)
                set(MAQUE_ASAN_RUNTIME_DIR "${_maque_rt}/lib/windows" CACHE PATH "" FORCE)
            endif()
        endif()
        set(MAQUE_ASAN_RUNTIME_DLL "${MAQUE_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        if(EXISTS "${MAQUE_ASAN_RUNTIME_DLL}")
            message(STATUS "maque: ASan 运行时 DLL = ${MAQUE_ASAN_RUNTIME_DLL}")
        else()
            message(WARNING "maque: 未找到 ASan 运行时 DLL（${MAQUE_ASAN_RUNTIME_DLL}）")
        endif()

        # ---- 关键：CMake 用 lld-link 直接驱动链接，`-fsanitize=address` 不会被翻译成运行时库 ----
        #（现象：undefined symbol: __asan_shadow_memory_dynamic_address / __asan_init 等）
        # clang-cl 自己链接时会加下面两项，这里显式补上（按 `clang-cl -### ... -fsanitize=address`
        # 的实际命令行核对）：
        #   1) clang_rt.asan_dynamic-x86_64.lib            （ASan 动态运行时导入库）
        #   2) /wholearchive:...dynamic_runtime_thunk...lib（thunk；再 /include:__asan_seh_interceptor
        #      强制拉入，保证 SEH 拦截器不被 --gc-sections 丢掉）
        if(EXISTS "${MAQUE_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.lib"
           AND EXISTS "${MAQUE_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
            add_link_options("${MAQUE_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.lib"
                             "/wholearchive:${MAQUE_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
                             "/include:__asan_seh_interceptor")
            message(STATUS "maque: ASan 运行时库已显式接入链接行（lld-link 直连模式下必需）")
        else()
            message(WARNING "maque: 未找到 clang_rt.asan_dynamic*.lib（${MAQUE_ASAN_RUNTIME_DIR}）；"
                            "链接会报 __asan_* 未定义符号")
        endif()
    else()
        add_compile_options(-fsanitize=${MAQUE_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${MAQUE_SANITIZE})
    endif()
    message(STATUS "maque: sanitizer = ${MAQUE_SANITIZE}")
endif()

# Windows：把 .deps/deps.cmake 里的预置变量喂给后续 find_package/find_library。
# 该文件由 tools/win/fetch-deps.ps1 生成（依赖不入库）；缺失时按普通路径查找并走各模块降级。
if(WIN32 AND MAQUE_WINDOWS_DEPS)
    set(MAQUE_DEPS_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/.deps/deps.cmake"
        CACHE FILEPATH "Windows 第三方依赖预置变量文件")
    if(EXISTS "${MAQUE_DEPS_CMAKE}")
        include("${MAQUE_DEPS_CMAKE}")
        if(COMMAND maque_find_windows_deps)
            maque_find_windows_deps()
            message(STATUS "maque: 已载入 Windows 依赖预置（${MAQUE_DEPS_CMAKE}）")
        endif()
    else()
        message(STATUS "maque: 未找到 ${MAQUE_DEPS_CMAKE}（先跑 tools/win/fetch-deps.ps1；"
                       "缺失的库会按各模块的降级路径关闭）")
    endif()
endif()

# WASM/浏览器目标（P3，docs/plan-wasm.md）：由 cmake/toolchain-emscripten.cmake 置位。
# WASM 下渲染插件（wgpu-native .so）与原生网络后端不可用，由宿主 JS 侧承担（docs/05 §3）。
if(EMSCRIPTEN)
    message(STATUS "maque: WASM/浏览器目标（主循环归宿主；见 docs/plan-wasm.md）")
    set(MAQUE_WASM ON)
endif()

enable_testing()

# ---- 测试用 bash 解释器 --------------------------------------------------------
# e2e 回归全是 bash 脚本（tests/*.sh）。Windows 上 `bash` 会解析到
# C:\WINDOWS\system32\bash.exe —— 那是 **WSL 启动器**，与仓库脚本语义不一致（且无 WSL 发行版
# 时直接失败）。Git Bash 才是能跑这些脚本的解释器。显式探测并把绝对路径传给 add_test。
if(WIN32)
    # 注意 $ENV{ProgramFiles(x86)} 的括号必须转义，否则 CMake 报 "Syntax error ... when parsing string"。
    find_program(MAQUE_BASH NAMES bash.exe
        HINTS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramFiles}/Git/usr/bin"
              "$ENV{LOCALAPPDATA}/Programs/Git/bin"
              "$ENV{ProgramFiles\(x86\)}/Git/bin")
    if(MAQUE_BASH AND MAQUE_BASH MATCHES "System32|WindowsApps")
        message(WARNING "maque: 探测到的 bash 是 Windows 自带启动器（${MAQUE_BASH}）；"
                        "e2e 测试需要 Git Bash，请装 Git for Windows 或 -DMAQUE_BASH=<路径>")
        set(MAQUE_BASH "MAQUE_BASH-NOTFOUND")
    endif()
    if(NOT MAQUE_BASH)
        message(WARNING "maque: 未找到 Git Bash；tests/*.sh 的 e2e 用例在 Windows 上无法运行"
                        "（单元测试不受影响）。装 Git for Windows，或 -DMAQUE_BASH=<bash.exe 路径>")
    else()
        message(STATUS "maque: e2e 使用 bash = ${MAQUE_BASH}")
    endif()
else()
    find_program(MAQUE_BASH NAMES bash)
endif()

# ---- Windows 运行时 DLL 分发 --------------------------------------------------
# 背景：Windows 加载器**不会**用"被加载 DLL 自己所在目录"去搜索它的依赖，只按
#   ① exe 所在目录 ② PATH（③ 系统目录）…… 所以把 maque_gfx.dll 与它的依赖
#   （wgpu_native.dll / ASan 运行时）都放在构建根时，samples/player 下的宿主
#   dlopen 插件仍会失败（实测：cannot load renderer plugin）。
# 解决：凡是**会加载插件**的可执行文件，都在其 exe 旁放一份依赖 DLL。
#   maque_deploy_plugin_deps(<target>)：wgpu_native.dll；开了 ASan 再带运行时。
#   maque_deploy_asan()：只放 ASan 运行时（供不碰插件的测试用）。
#
# ⚠ 别再回去用 CTest 的 ENVIRONMENT 注入 PATH（试过并已放弃）：
#   ① `TEST_INCLUDE_FILES` 只落进**顶层** CTestTestfile，而顶层
#      `get_property(DIRECTORY PROPERTY TESTS)` 为空 → 对子目录测试是 no-op；
#   ② 即便在子目录生效，`$ENV{PATH}` 里的 `;` 会被 CMake 当**列表分隔符**，
#      注入后 PATH 变畸形（实测 "PATH=C:/…/windows;\C:\VulkanSDK…"），
#      测试进程启动即挂死（lua_test 卡 10 分钟不返回）。
# 「拷 DLL 到 exe 旁」没有这两个问题，且手工运行与 ctest 行为一致。
function(maque_deploy_asan target)
    if(NOT WIN32 OR NOT MAQUE_SANITIZE)
        return()
    endif()
    if(NOT TARGET "${target}" OR NOT EXISTS "${MAQUE_ASAN_RUNTIME_DLL}")
        return()
    endif()
    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MAQUE_ASAN_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM
        COMMENT "maque: 部署 ASan 运行时 DLL → ${target}")
endfunction()

function(maque_deploy_plugin_deps target)
    if(NOT WIN32 OR NOT TARGET "${target}")
        return()
    endif()
    set(_wgpu "${CMAKE_SOURCE_DIR}/third_party/wgpu-native/lib/wgpu_native.dll")
    if(EXISTS "${_wgpu}")
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_wgpu}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
            COMMENT "maque: 部署 wgpu_native.dll → ${target}")
    endif()
    maque_deploy_asan("${target}")
endfunction()

# 说明：ASan 运行时 DLL 的分发完全靠上面的 maque_deploy_asan()/maque_deploy_plugin_deps()，
# 由各模块在自己的 CMakeLists 里调用（跨目录调用 add_custom_command(TARGET) 会报
# "TARGET ... which is not built by this project"）。

# DSL 生成物目录（**入库**：生成的公开接口是仓库产物，docs/21）
#   - 单一真相 = modules/core/data/schema/（目录，入口 schema.yaml；一文件一模块 + import 依赖）
#   - 生成：`cmake --build build --target maque_codegen`（构建时自动重生成）
#   - 漂移校验：ctest -R codegen_drift（重新生成到临时目录并逐字节比对）
set(MAQUE_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modules/core/data/generated"
    CACHE PATH "Directory holding codegen outputs (tracked)")

# 脚本绑定探针目录（modules/core/data 在配置期生成 JS/Lua 探针；bindings_e2e 用）
set(MAQUE_BINDING_PROBE_DIR "${CMAKE_BINARY_DIR}/generated/binding_probe")

# 生成头（含各模块 abi/<mod>.h 的 shim 目标）对全项目可见：配置期已生成，无需逐目标接线。
include_directories("${MAQUE_GEN_DIR}")

# 极小跨平台兼容头（setenv / 低层 fd I/O 等 POSIX 缺口）：全项目可见，
# 避免每处各写一遍 _putenv_s / _open / _write。
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/modules/core/compat/include")

# 单测共享助手（tests/maque_test_fs.h：临时目录 + 不抛异常的文件读写）。
# 全项目可见：各模块 tests/*.cpp 直接 #include "maque_test_fs.h" 即可。
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/tests")

# script 需先于 data（data 生成绑定 TU 依赖 script）
add_subdirectory(modules/script)
add_subdirectory(modules/core/data)
add_subdirectory(modules/core/reflect)
add_subdirectory(modules/core/handle)
add_subdirectory(modules/core/module)
add_subdirectory(modules/math)
add_subdirectory(modules/observability)
add_subdirectory(modules/control)
add_subdirectory(modules/io/vfs)
add_subdirectory(modules/io/storage)
add_subdirectory(modules/io/watcher)
add_subdirectory(modules/asset)
add_subdirectory(modules/net)
add_subdirectory(modules/platform)
add_subdirectory(modules/input)
add_subdirectory(modules/input/simulator)
add_subdirectory(modules/data/persist)
add_subdirectory(modules/image)
add_subdirectory(modules/world/physics)
add_subdirectory(modules/world/nav)
add_subdirectory(modules/ui/canvas)
add_subdirectory(modules/render/text)
add_subdirectory(modules/render/gfx)
add_subdirectory(modules/api)
add_subdirectory(modules/runtime)
add_subdirectory(samples/player)
add_subdirectory(samples/inspector)
add_subdirectory(samples/host)
