# WASM / 浏览器目标（P3，ADR-0016、docs/05）

> 状态：**已完成可移植化改造与构建工具链；本机未安装 Emscripten SDK，未产出 .wasm 二进制**。
> 本文记录"已经做好的部分 / 还差什么 / 怎么构建 / 已知缺口"，避免把它当成"已交付可运行网页"。

## 1. 已经就绪（与本机工具链无关）

| 事项 | 落点 | 为什么对 WASM 关键 |
|------|------|--------------------|
| **主循环归宿主** | `modules/runtime/include/runtime/mq_host.h` + `samples/host` | 浏览器不允许引擎霸占主循环；JS 用 `requestAnimationFrame` 调 `mq_host_tick`（docs/05 §3） |
| **纯 C 宿主 ABI** | `mq_host_scene_create/destroy/tick/size/push_pointer/push_key/frame/checksum/control` | 可被 `extern "C"` 导出、被 JS 直接调用（无需 C++ 名字修饰） |
| **输入注入** | `mq_host_push_pointer/key` | 浏览器 Pointer/Touch/Keyboard/IME 事件归一化后注入同一确定性事件流（docs/19 §9） |
| **多实例并存** | `host_multi_instance` 测试 | ADR-0014：浏览器里同时跑多个实例（预览/对比/多人分屏） |
| **确定性不依赖平台** | 固定步长、引擎 RNG、快照/校验和 | WASM 浮点语义确定 → 回放最稳（docs/10 §6 L2/L3） |
| **无窗口渲染路径** | `mq_host_tick` 输出 RGBA 到宿主缓冲 | 浏览器可直接 `putImageData`/上传纹理，无需 SDL 窗口 |
| **资源加载可替换** | VFS 挂载矩阵（file/mem/zip/builtin/res/URL） | 浏览器用 `mem://`（预加载包）+ fetch 替代本地文件 |
| **工具链文件** | `cmake/toolchain-emscripten.cmake` | 一条命令配置：SDL2 端口、导出符号、ES6 模块 |

## 2. 构建（需先安装 Emscripten）

```bash
git clone https://github.com/emscripten-core/emsdk && cd emsdk
./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh

cd /path/to/maque
emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-emscripten.cmake \
  -DMAQUE_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
```

产物：`build-wasm/samples/host/maque_host.js` + `.wasm`（ES6 模块）。

## 3. JS 侧接法（主循环归宿主）

```js
import Module from './maque_host.js';

const mod = await Module();
// 1) 场景资源打包进 MEMFS（emcc --preload-file），或用 mem:// 注入
mod.FS.writeFile('/scenes/ui.scn', scnText);
mod.FS.writeFile('/scenes/ui.js',  jsText);

// 2) 创建实例（渲染插件在 WASM 下由 JS 侧 WebGPU 承担）
const scn = mod.stringToUTF8 ? null : null;
const handle = mod._mq_host_scene_create(strPtr('/scenes/ui.scn'), 0);

// 3) 宿主自己的帧循环
function frame() {
  const ok = mod._mq_host_tick(handle, frameBufPtr, frameBytes);
  if (ok === 0) presentToCanvas(mod.HEAPU8, frameBufPtr);   // 或直接上传 WebGPU 纹理
  if (!mod._mq_host_should_quit(handle)) requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// 4) 输入：浏览器事件 → 引擎确定性事件流
canvas.addEventListener('pointerdown', (e) =>
  mod._mq_host_push_pointer(handle, 0 /*DOWN*/, 0, e.offsetX, e.offsetY));

// 5) AI/CI 面：控制面文本进出（可挂 WebSocket / PostMessage）
const resp = mod.UTF8ToString(mod._mq_host_control(handle, ctrlPtr));
```

## 4. 已知缺口（需要工具链或后续工作）

| 缺口 | 说明 | 处理方向 |
|------|------|----------|
| **渲染后端** | gfx 是 `wgpu-native` 的 `.so` 插件，浏览器里不可加载 | ① 用 JS WebGPU 在宿主侧渲染（`mq_host_tick` 已把绘制数据准备好）；② 或编一个软光栅插件（`-sUSE_SDL=2` 无 WebGPU 时可用） |
| **网络** | `net` 依赖 libwebsockets（原生 socket） | 浏览器走 `WebSocket`/`fetch`；VFS 的 URL 后端在 WASM 下应改为 fetch（当前 `MQ_HAVE_CURL` 分支在 WASM 构建里关闭） |
| **动态库** | 浏览器无 `dlopen` | 渲染器/后续模块在 WASM 下编为静态库，`mq_host_query` 直接链接 |
| **线程** | 无 `-pthread` 时 `wgpu` 的轮询循环不可用 | 引擎模拟层本就单线程（docs/10 D6），宿主渲染在 JS 侧 |
| **二进制体积** | quickjs + wuffs + 引擎 | 用 `-Os`、`--closure 1`、按需裁剪 schema 类型（codegen 已支持） |
| **实测** | 未安装 Emscripten，未跑通 | 安装后按 §2/§3 接线，并补一个浏览器端 smoke（加载场景 → tick 60 帧 → checksum 与原生一致） |

## 5. 验收建议（工具链到位后）

1. 同一场景、同一输入，WASM 与原生（同平台）**逐帧校验和一致**（docs/10 §6：WASM 偏 L2/L3，应最稳）；
2. 浏览器里 `mq_host_tick` 60fps 稳定、无 WASM 内存增长告警；
3. `_mq_host_control` 经 WebSocket 暴露控制面 → 浏览器里也能被 AI 驱动（docs/11 §2）。
