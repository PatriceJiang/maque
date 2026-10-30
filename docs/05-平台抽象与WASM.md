# 05 · 平台抽象与 WASM

> 上游：`00`（ADR-0016）、`02`（模块 ABI）、`04`（VFS）。
> **WASM/浏览器是一等目标平台**（OD-8 已确认），与原生、嵌入式并列。
> 本文列出它回写其它文档的所有约束，并定义**薄平台抽象层**。

## 1. 平台矩阵

| 平台 | 模块加载 | 线程 | IO | GPU |
|------|----------|------|----|----|
| 原生 (Linux/macOS/Win) | `.so` + `dlopen` | 真线程 | VFS / OS | WebGPU |
| 嵌入式 | 静态链接 + 编译期模块集 | 单线程或多核（视芯片） | VFS / `res://` | WebGPU（**须有 Vulkan/Metal/D3D12**，ADR-0029） |
| **Web (WASM)** | 见 §3 | 默认单线程协作式 | **仅 VFS**（fetch/OPFS/内存） | **WebGPU（浏览器原生）** |

**公共下限**：WebGPU 渲染 · quickjs 脚本 · VFS IO · coroutine 异步。

## 2. WASM 的硬约束（逐条）

| 约束 | 影响 | 对策 |
|------|------|------|
| **不能任意 `dlopen` `.so`** | 模块加载模型改变 | §3 双模加载 |
| **无裸文件系统** | 一切 IO 走 VFS | 文档 04 已适配（http/zip/mem/res） |
| **默认无线程**（除非 SharedArrayBuffer） | executor 无法用 OS 线程 | §4 单线程协作式 |
| **SAB 需 COOP/COEP 头** | 影响远程游戏托管（微游戏场景） | 无 SAB 时降级单线程 |
| **沙箱、无原生 exec** | 远程内容天然受限 | 免费提供一层隔离（§7） |
| **异常/栈回溯受限** | A2 崩溃采集变难 | 保留 DWARF + JS 栈，见 §8 |
| **浮点确定性强**（WASM 规范） | 实际利好 | 支撑 A3 回放（文档 10） |

## 3. 模块加载：逻辑统一，物理分平台（改写 ADR-0007 的平台部分）

**逻辑模块边界不变**：仍是 C ABI + 版本化 vtable + registry（文档 02）。
**物理加载按平台**：

- **(a) 原生**：`.so` + `dlopen` + `mq_module_query`（文档 02）。
- **(b) 嵌入式**：静态链接 + 编译期确定的模块集。
- **(c) WASM**，三选一：
  1. **静态链接进单一 wasm**（最简单，失去运行时插件）
  2. **Emscripten `SIDE_MODULE`** 动态链接（能力受限，需评估）
  3. **运行时实例化独立 `.wasm` 模块** + 共享内存 + 显式 imports（最接近原生插件）

宿主提供 **`mq_module_host`** 抽象，屏蔽上述差异。结论：
> **模块边界是逻辑的，加载是平台相关的。**

## 4. 线程模型：默认单线程，并行显式 opt-in

**默认单线程**。引擎一切（模拟 + 协作式异步）跑在单线程上；确定性因此是**默认属性**，
而非靠纪律维持。**并行必须显式申请**：

```c
mq_worker* w = mq_worker_create(host, /* config */);   // 显式建 worker（线程/线程池）
mq_job*    j = mq_job_submit(w, job_desc, args);       // 显式提交 job
```

约定：

- **worker** = 持有线程/线程池与任务队列的对象（instance 所有，非全局）。
- **job** = 粗粒度任务；worker 之间**不共享可变内存**，通过消息 / 句柄 / 序列化通信。
- **模拟线程永不阻塞**：禁止在模拟线程上等待 job 或做阻塞 IO。
- **确定性边界**：默认单线程 ⇒ 确定。一旦使用 worker，**确定性责任转移给调用方**；
  需要可复现时，模拟必须单线程（文档 10）。
- **WASM 降级**：无线程时 `mq_worker_create` 返回**同步/延迟执行 worker**（job 在主线程排队执行），
  或直接不支持；因 API 是显式的，上层无需条件编译。

### 平台矩阵

| 平台 | worker 实现 | 默认 |
|------|-------------|------|
| 原生 | 真线程 / 线程池 | 单线程，worker 可选 |
| 嵌入式 | 单线程或有限多核 worker | 单线程 |
| WASM(no-SAB) | 同步降级或不可用 | 单线程 |
| WASM(SAB) | 真线程（需 COOP/COEP） | 单线程，worker 可选 |

## 5. 薄平台抽象层

**只抽象真正有平台差异的东西**，且**不抽象热路径**：

- VFS（文档 04）· 线程/executor · 模块加载 · 单调时钟 · 日志 sink · GPU（WebGPU 已统一）· 栈回溯。

## 6. 应用分层：引擎 wasm + 游戏内容

浏览器里的加载是**两级**：

```
engine.wasm（引擎本体）──→ 启动 VFS ──→ 拉取 game 包 (https/zip)
                                    └─→ 游戏逻辑跑在 quickjs
```

这正好复用文档 04 的 `fs.mount("assets/", "https://...")` 与 `ai` 打包产物。

## 7. 安全（更新 OD-9）

- **浏览器**：WASM 沙箱 + 同源策略**免费**提供远程内容隔离。
- **原生/嵌入式**：无 WASM 时，远程加载仍需**自建沙箱**（进程隔离 / 能力限制）。

## 8. A2 崩溃采集在 WASM

- `std::stacktrace` 依赖 DWARF/sourcemap → **保留 line tables**，勿 strip。
- 结合 JS 侧 `window.onerror` + wasm 栈，拼接完整调用链。
- 记录 RNG seed / 帧号 / 输入日志（文档 10）以支持回放。

## 9. 对其它文档的影响清单

| 文档 | 影响 |
|------|------|
| 02 模块 ABI | 增加 WASM 加载分支（§3） |
| 04 VFS | WASM 下**唯一** IO 路径；http 走 `fetch` |
| 08 可观测 / 09 确定性 | WASM 差异与"确定性强"的利用 |
| 10 控制平面 | 浏览器内经 WebSocket / 直接 JS 桥接 |
| 12 渲染 | WebGPU 在浏览器原生统一 |

## 10. 遗留风险

| 风险 | 处理 |
|------|------|
| 三平台行为分叉 | 公共下限 + 平台矩阵测试 + CI 多目标构建 |
| WASM 无线程拖慢 executor | 单线程协作式，热路径避免阻塞 |
| SIDE_MODULE 成熟度 | 优先"静态链接"或"独立 .wasm 实例化"，SIDE_MODULE 作为可选 |
| 崩溃栈信息缺失 | 保留 DWARF + JS 栈拼接 |

## 呈现（窗口输出）

- 平台层的**唯一呈现通道 = `SDL_Renderer`**：`mq_platform_draw_rect` 把宿主渲染出的 RGBA8 帧
  上传为流式纹理并画进 renderer 后台缓冲；`mq_platform_present_ui`（+ 可选抓帧）只
  `SDL_RenderPresent` 一次。`mq_platform_present_rect` / `mq_platform_present` 是"画 + 立即 present"
  的单阶段便捷包装。给 ImGui 面板留的扩展点是：**画完游戏帧、present 之前**在同一后台缓冲上再画一趟
  （ImGui 官方 SDL_Renderer 后端），这样游戏画面与调试面板天然同屏、同步、只交换一次。
  ⚠️ **不能再用 `SDL_GetWindowSurface`**：SDL2 不允许同一窗口混用 window surface 与 renderer。
  历史上混用导致"ImGui 一开、游戏画面就不上屏"（游戏帧写窗口表面后被 renderer present 覆盖）。
- **后端选择**：默认 Windows = `accelerated`（SDL_Renderer = Direct3D，不建 GL/EGL上下文，
  不与渲染插件的 wgpu 争用）；其它平台默认 `software`（`SDL_RENDERER_SOFTWARE`，同样不碰 GL/EGL）。
  `MAQUE_PRESENT=off|software|accelerated` 可覆盖；加速后端创建失败自动 software 回退，再失败则关闭呈现。
- **设计分辨率 ≠ 渲染分辨率**：`MAQUE_WINDOW` 只改窗口；场景 `width/height` 是世界/UI 的
  **参考坐标系**，不用于分配主颜色附件。`ui.Canvas` 把设计坐标映射到 drawable 中的 viewport，输入使用
  同一映射返回世界坐标。默认 `scn.Scene.render_scale=1`，主渲染目标等于窗口 drawable 的设备像素尺寸；
  `MAQUE_RENDER_SCALE=N` 可覆盖为 `round(drawable × N)`。`ui.Canvas.render_scale>0` 仅作为旧场景兼容入口。
  无窗口 / headless / 截图以 `设计宽×views × 设计高` 作为虚拟屏幕，所以默认截图尺寸仍稳定为设计尺寸。
- **WGPU 条件直呈**：当主目标尺寸等于 surface 且不要求 CPU 回读时，3D/2D 直接以 acquired swapchain
  texture 为颜色附件，随后 `wgpuSurfacePresent`，不创建中间颜色纹理、不执行最终 blit。只有
  `render_scale != 1`（或后续真正需要采样整帧的后效）使目标与 surface 不同时，才分配中间目标并用
  blit shader 缩放到 surface。MSAA 可直接 resolve 到最终目标，不要求额外 blit；transmission 自己的
  scene-color copy 是材质 pass 内部依赖，不等同于最终呈现 blit。显式 SDL `software|accelerated` 模式仍走
  RGBA8 回读、`SDL_UpdateTexture`、`SDL_RenderCopy` 的兼容呈现链。
- **窗口可 resize / 最大化，尺寸变化时自动重建渲染目标**：创建 game 窗口即带
  `SDL_WINDOW_RESIZABLE`，`MAQUE_WINDOW_MAXIMIZED=1` 可启动即最大化。帧首收到窗口事件后重新读取
  drawable；目标尺寸更新为 `drawable × render_scale`，gfx 的深度/MSAA缓存按尺寸失效，WGPU surface
  通过 `resize_surface` 重配，并打印：
  `runtime: window resized <旧窗口> → <新窗口>: render target <旧目标> → <新目标> (render_scale <N>, views=<n>)`。
  因为倍率相对屏幕，显式 `render_scale=0.5` 在 resize 后仍保持“屏幕一半”，而不是固定为某个设计尺寸。
  CI 用 `MAQUE_WINDOW_EVENT_AT="<frame>:<op>[;...]"` 下发真实 resize/maximize/restore 事件，
  `tests/window_resize_e2e.sh` 同时验证原生目标与 0.5 倍目标。
- **无窗口/dummy 驱动**（`MAQUE_HEADLESS=1`）时呈现是安全空操作，headless/CI 不受影响。
- 抓帧：`MAQUE_CAPTURE=<前缀>` 从 **renderer 后台缓冲**（present 之前 = 用户即将看到的整帧，
  含游戏画面与 ImGui 面板）回读 PPM（默认前 3 帧；`MAQUE_CAPTURE_FRAMES` / `MAQUE_CAPTURE_EVERY`
  可调）；尺寸 = 窗口绘制区**设备像素**，故窗口 resize / 最大化后新抓帧的尺寸随之变化。
  `tests/window_present_e2e.sh` / `tests/imgui_present_e2e.sh` / `tests/window_resize_e2e.sh`
  用它做真实窗口路径的像素级断言。
