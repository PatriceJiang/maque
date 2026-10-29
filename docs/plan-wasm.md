# WASM / 浏览器目标（P3，ADR-0016、docs/05）

> 当前状态：**P0 可构建链路已落地，P1 WebGPU 直呈尚未完成**。
> P0 用静态 null renderer 验证“预载场景 → QuickJS/物理/动画 → 固定步推进 → checksum”；
> 它不是发布渲染后端。正式浏览器版本只支持 WebGPU，不提供 WebGL、SDL_Renderer 或 CPU 软件光栅回退。

## 1. 分阶段边界

| 阶段 | 目标 | 状态 |
|------|------|------|
| **P0 确定性 smoke** | 单一 WASM、MEMFS 预载场景、无动态库、无原生 socket/进程 API、`mq_host_step` 多实例 checksum 一致 | 已接线；GitHub Actions 真实 Emscripten 构建门 |
| **P1 可见 2D** | `emdawnwebgpu` 静态 renderer、canvas surface、solid/textured/mesh 直呈、DOM 输入与 DPR/resize | 待实现 |
| **P2 浏览器服务** | fetch/manifest、IndexedDB/OPFS、WebSocket/HMR、用户手势解锁音频 | 待实现 |
| **P3 能力对齐** | 3D/PBR、阴影/环境贴图、异步 capture、浏览器 ImGui 与性能优化 | 待实现 |

P0 的 null renderer 只分配稳定纹理/网格句柄并接受 renderer ABI；帧输出最多填背景色。
它让 runtime 的场景装配路径保持完整，但不会掩盖 P1 必须完成的 WebGPU 工作。

## 2. 已落地的 P0 结构

- **官方 Emscripten toolchain**：推荐 `emcmake cmake`；`cmake/toolchain-emscripten.cmake`
  仅作为兼容包装，内部链入 SDK 的正式 toolchain，不再伪造 `CMAKE_SYSTEM_NAME`。
- **浏览器裁剪 profile**：`-DMAQUE_WASM=ON` 关闭 native tests、Tracy、Lua、HarfBuzz、
  `dlopen` renderer、native player/inspector，并选择 platform/control/ImGui stub。
- **静态 renderer**：runtime 在 WASM 下直接绑定 `maque::renderer_null`，不经过 `.so`/`.dll`。
- **纯模拟宿主入口**：`mq_host_step(scene)` 推进一个固定 `1/60` 帧，不执行
  `mq_host_tick` 的额外离屏渲染和同步 RGBA 回读。
- **资源预载**：`samples/web` 把 `scenes/anim/` 打进 `.data`，挂到
  `/scenes/anim/` 的 MEMFS 路径。
- **墙钟驱动**：`samples/web/bootstrap.mjs` 使用 rAF timestamp accumulator，最多补 8 步；
  不是“一次 rAF 固定推进一帧”，因此高刷屏不会让游戏加速。
- **确定性门**：`tests/wasm_p0_smoke.mjs` 在同一 WASM 实例内创建两个场景，推进 60 帧并比较 checksum。

## 3. 构建与验证

先安装并激活 Emscripten SDK（CI 固定 4.0.10）：

```bash
git clone https://github.com/emscripten-core/emsdk
cd emsdk
./emsdk install 4.0.10
./emsdk activate 4.0.10
source ./emsdk_env.sh

cd /path/to/maque
cd modules/core/data && npm install --no-audit --no-fund && cd ../../..
emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMAQUE_WASM=ON
cmake --build build-wasm --target maque_web --parallel
node tests/wasm_p0_smoke.mjs \
  "$PWD/build-wasm/samples/web/maque_web.mjs" \
  /scenes/anim/anim.scn 60
```

产物位于 `build-wasm/samples/web/`：

- `maque_web.mjs`
- `maque_web.wasm`
- `maque_web.data`
- `index.html`
- `bootstrap.mjs`

浏览器必须经 HTTP(S) 提供文件，不能直接用 `file://`：

```bash
python3 -m http.server 8000 -d build-wasm/samples/web
# http://127.0.0.1:8000/
```

P0 页面只显示 frame/checksum；可见游戏画面属于 P1 WebGPU 交付。

## 4. 宿主时序

P0/P1 共用以下原则：

1. JS 完成模块与资源初始化；
2. `mq_host_scene_create` 创建实例；
3. rAF timestamp 进入 accumulator；
4. accumulator 每满 `1/60` 调一次 `mq_host_step`；
5. P1 再调用 WebGPU render/present；P0 无可见渲染；
6. 输入在固定步之前入队；控制面可通过 `mq_host_control` 由 `postMessage` 包装。

`mq_host_tick` 保留给需要 CPU RGBA 的原生嵌入宿主，浏览器正式路径不使用它。

## 5. P1 WebGPU 唯一路径

P1 不新增 WebGL fallback。实现方向固定为：

- 使用 Emscripten 4.0.10+ 的 `--use-port=emdawnwebgpu`；
- 把现有 `gfx.cpp` 从 native `MODULE + wgpu-native` 拆出静态 browser target；
- Web 构建使用 port 提供的 `<webgpu/webgpu.h>`，不链接仓库里的 native `wgpu_native` 二进制；
- surface 使用 `EmscriptenSurfaceSourceCanvasHTMLSelector` 绑定 `<canvas>`；
- adapter/device 初始化改为异步 ready 状态，不在浏览器主线程 `ProcessEvents + sleep` 忙等；
- 正常帧直接写 swapchain，不做 GPU→CPU→JS→GPU 往返；截图单独走异步 readback；
- 先交付 2D solid/textured/mesh，再启用 3D/PBR。

当前 PBR 单 fragment 阶段绑定的 sampled texture 数量高于 WebGPU 保证的最低 16，
P3 前必须做纹理槽压缩或能力分档，不能把桌面 `wgpu-native` 上限当作浏览器下限。

## 6. 当前明确关闭的能力

P0 中以下能力返回 unsupported 或不创建：native TCP/UDS inspector、libcurl URL VFS、
libwebsockets、文件 watcher、真实音频设备、Tracy、Lua、动态模块卸载、GPU capture。

可直接复用的部分包括反射、持久化 codec、QuickJS、固定步模拟、物理、导航、动画、
输入语义层、VFS 的 file/MEMFS/mem/builtin 路由、控制面的同步 JSON dispatch、音频 mixer/null backend。

## 7. 质量门

- 本机无 Emscripten 时：`MAQUE_HEADLESS_STUBS=ON` 编译相同 stub/null 后端，并跑
  `maque_host <scene> 60 --two-instances --step-only`；
- Emscripten CI：构建 `maque_web`，Node 加载 `.mjs/.wasm/.data`，两个实例 60 帧 checksum 必须相同且非零；
- schema：`cd modules/core/data && node parser.js --check`；
- P1 增加浏览器 WebGPU canvas 像素与 resize/DPR/input e2e 后，才可把平台状态标为“可玩”。
