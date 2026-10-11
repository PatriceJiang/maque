# maque WASM/浏览器 工具链（P3，docs/05 §2、ADR-0016）
#
# 用法（需安装 Emscripten SDK，见 docs/plan-wasm.md）：
#   emcmake cmake -S . -B build-wasm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-emscripten.cmake \
#                 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm
#
# 该工具链做三件事：
#   ① 关掉 WASM 上不可用的后端（HTTP/WebSocket 走浏览器 API；原生 WebGPU 插件不参与）；
#   ② 把主循环交给宿主（JS）——引擎导出 runtime/mq_host.h 的 C ABI，
#      JS 侧用 emscripten_set_main_loop 调 mq_host_tick（见 docs/plan-wasm.md 示例）；
#   ③ 生成可被浏览器直接加载的模块（ES6 + 导出宿主符号）。
set(CMAKE_SYSTEM_NAME Emscripten)
set(CMAKE_SYSTEM_VERSION 1)

# 平台适配：SDL2 由 Emscripten 端口提供（-sUSE_SDL=2）
set(MQ_WASM ON CACHE BOOL "maque WASM/浏览器目标" FORCE)
set(MAQUE_BUILD_TESTS OFF CACHE BOOL "WASM 构建不跑本机测试" FORCE)

# 引擎侧开关（CMakeLists 依据它们裁剪平台相关依赖）
add_compile_definitions(MQ_PLATFORM_WASM=1)

# 浏览器里没有原生进程/动态库/文件系统：
#   · 渲染插件（.so + wgpu-native）不参与 → 由 JS 侧 WebGPU 或内置软光栅承担；
#   · VFS 的 http/https 走浏览器 fetch（见 docs/plan-wasm.md 的挂载策略）；
#   · 文件系统经 Emscripten 的 MEMFS/预加载包提供资源。
set(CMAKE_EXECUTABLE_SUFFIX ".js")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

# 宿主导出：JS 通过 Module._mq_host_* 驱动引擎（主循环归宿主，docs/05 §3）
set(MQ_WASM_EXPORTS
    "_mq_host_scene_create,_mq_host_scene_destroy,_mq_host_tick,_mq_host_size,\
_mq_host_push_pointer,_mq_host_push_key,_mq_host_frame,_mq_host_checksum,\
_mq_host_should_quit,_mq_host_control,_malloc,_free")

set(MQ_WASM_LINK_FLAGS
    "-sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS=${MQ_WASM_EXPORTS} \
-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,HEAPU8 \
-sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web -sSTACK_SIZE=1048576")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MQ_WASM_LINK_FLAGS}")
