# maque

| Platform | CI status |
|---|---|
| Linux x86_64 | [![Linux CI](https://img.shields.io/github/actions/workflow/status/PatriceJiang/maque/build.yml?branch=main&label=Linux)](https://github.com/PatriceJiang/maque/actions/workflows/build.yml) |
| Windows x64 | [![Windows CI](https://img.shields.io/github/actions/workflow/status/PatriceJiang/maque/build.yml?branch=main&label=Windows)](https://github.com/PatriceJiang/maque/actions/workflows/build.yml) |
| WASM / Emscripten | [![WASM CI](https://img.shields.io/github/actions/workflow/status/PatriceJiang/maque/build.yml?branch=main&label=WASM)](https://github.com/PatriceJiang/maque/actions/workflows/build.yml) |
| Android arm64-v8a | [![Android CI](https://img.shields.io/github/actions/workflow/status/PatriceJiang/maque/android.yml?branch=main&label=Android)](https://github.com/PatriceJiang/maque/actions/workflows/android.yml) |

麻雀虽小，五脏俱全。**maque 是一个"可被 AI 自主驱动与观测"的轻量游戏引擎**，
同时服务**嵌入式**与**独立游戏**，并以 **WASM/浏览器**为一等目标平台。

> 核心理念：**引擎就是一个"浏览器"，游戏就是一堆资源**。
> 宿主零游戏逻辑，入口就一句 `mq_run_scene("ball.scn")`；玩法全在 `.scn` + `.js` 资源里。

## 特性

- **多实例 / 零全局可变状态 / 插件化**（模块即 `.so`，跨边界 C ABI）。
- **反射中枢**：`schema/` 一处声明 → 生成 C++ 类型 / C ABI 描述符 / JSON / **脚本绑定**。
- **schema = 目录**：一个文件一个模块（目录镜像 `modules/`），入口 `schema.yaml` 登记全部文件，
  文件间 `imports` 声明依赖（合并顺序 = 依赖优先的后序遍历；结构引用必须落在 import 闭包内）。
- **声明式接口绑定**：`api_list` 一次声明，生成 C++ 头 + JS 绑定（支持 MANUAL 头内联实现）。
- **脚本优先**：quickjs(JS) 作为游戏逻辑层；帧边界 GC；**帧边界热重载**（按文件增量 + 原子替换 + 槽位不变；状态边界见 docs/14 §7）。
- **真实 WebGPU 渲染**（wgpu-native）：离屏渲染 + readback；相机（中心/缩放/旋转）、
  分层与 z 稳定排序、按"管线 × 贴图"分批、直通 alpha 合成。
- **输入即数据**（docs/19）：SDL2 归一化指针/键盘/文本 → 确定性事件流 → 捕获/目标/冒泡分发 →
  **可录制、可回放、可注入**；UI 控件按 role 命中（引擎不认具体组件）。
- **确定性物理**：自研 2D 求解器（固定步长/固定迭代/按 body.index 稳定排序；C ABI vtable，可换后端）。
- **快照 / 回滚 / 失步定位**：反射驱动二进制 codec + 快照池；逐帧世界校验和轨迹。
- **VFS**：`file/mem/zip/builtin/res` 挂载 + Android APK `asset://` + **http(s) 目录挂载**（libcurl：整局游戏可经 HTTP 加载）
  + 直接 URL + 内容哈希校验；**图像解码** Wuffs。
- **键值存储 / 存档**：`storage.*` 脚本 API，模式可配（`file://`（默认 `<场景目录>/.maque`）/`mem://`/`http(s)://`/`ws(s)://`）
  + `(uid,session)` 命名空间；控制面可读写（AI/CI 可核对存档）。见 docs/25。
- **场景 → 图片**：`maque_player --shot out.png [--shot-frame N] scene.scn`（离屏渲染一张图；VSCode 预览插件在 `editors/vscode/`）。
- **资产管线**：导入 / 依赖图 / 拓扑序 / 校验 / 烘焙 / 打包 / dump JSON（内容寻址 ID）。
- **控制平面**：JSON-RPC domain.method + **MCP 包装**（tools/list、tools/call）→ AI/CI 可加载、
  逐帧驱动、注入输入、截帧、取校验和、热重载。
- **嵌入宿主接口**（`runtime/mq_host.h`）：主循环归宿主（浏览器 rAF / RTOS 任务 / 自研循环），
  多实例并存且逐帧确定；WASM 目标见 `docs/plan-wasm.md`。
- **编辑器/AI 编辑面**：`Scene.get/set(dry_run)/spawn/destroy/save`（反射驱动，落盘可再次加载）。

## 快速开始

```bash
# 1) codegen 依赖（首次）
cd modules/core/data && npm install && cd -

# 2) 构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 3) 测试
ctest --test-dir build --output-on-failure

# 4) 运行示例（加载 .scn + .js/.lua，经 WebGPU 渲染）
./build/samples/player/maque_player scenes/ball.scn        # 弹跳球（点击生成球）
./build/samples/player/maque_player scenes/sokoban/sokoban.scn   # 推箱子（WASD/箭头 · R 重开 · Z 撤销 · N 下一关）
./build/samples/player/maque_player scenes/td/menu.scn     # 小小塔防（场景组合：3 塔 / 4 敌人 / 3 关卡 + 全局脚本）
./build/samples/player/maque_player scenes/lua/lua.scn     # 同一份逻辑的 Lua 版弹跳球
```

> **示例一览**（`scenes/`）：
> | 场景 | 演示内容 |
> |------|----------|
> | `ball.scn` | 下落弹跳 + 点击生成（物理直觉、输入、确定性） |
> | `sokoban/sokoban.scn` | **推箱子**：网格关卡、键盘驱动、撤销、关卡推进（JS） |
> | `td/menu.scn` | **小小塔防**：菜单 + 3 关卡 + 3 塔/4 敌人独立 `.scn` + 全局脚本/全局状态（scn 组合示例） |
> | `ui.scn` | UI 控件（按钮/进度条/滑杆/复选）与命中测试 |
> | `text/text.scn` | 文本：Label / TextLabel / RichText（富文本标记、换行、像素级精确） |
> | `nodes/nodes.scn` | 节点/组件/事件/补间/定时器 API |
> | `canvas/canvas.scn` | 画布与分辨率策略（同一 UI 适配不同窗口/显示器） |
> | `lua/lua.scn` | Lua 脚本后端（与 JS 版**同一份语义**，世界演化逐帧一致） |
> | `multilang/multilang.scn` | **多语言协作**：JS 玩法 + Lua AI，只通过事件总线互相调用 |
> | `anim/` · `nav/` | 动画采样/物理粒子 · A* 导航与追踪 |
>
> **Inspector（独立进程检查器，plans/17）**：
> ```bash
> MAQUE_INSPECT=7777 ./build/samples/player/maque_player scenes/sokoban/sokoban.scn   # 游戏开端点（默认只读）
> ./build/samples/inspector/maque_inspector --connect 7777                          # attach（场景树/属性/控制台）
> ```

> **依赖**：
> - 构建期 codegen 需 `node` + `yaml`（`npm install`）。
> - WebGPU 需 `third_party/wgpu-native/lib/libwgpu_native.so`（**二进制不入库**，见该目录说明）。
> - 系统依赖：SDL2、libcurl、libwebsockets（`apt-get install libsdl2-dev libcurl4-openssl-dev libwebsockets-dev`）。
> - 已 vendored：`quickjs-ng`、`wuffs`。

## 目录结构

```
modules/
  core/data       schema/ + codegen(parser.js) → maque::schema
  core/reflect    反射运行时 (mq::reflect)
  core/handle     世代句柄槽表
  core/module     模块宿主 (.so / registry)
  math            数学库
  observability   可观测性（结构化事件 + 指标, per-instance）
  control         控制平面（JSON-RPC，方法注册；AI/CI 可驱动）
  io/vfs          虚拟文件系统（file/mem/zip/http(s) 挂载 + 内容哈希）
  net             网络 (libwebsockets)
  platform        平台层 (SDL2 窗口/输入)
  data/persist    持久化/反序列化 + JSON DOM
  image           图像解码 (Wuffs)
  render/gfx      WebGPU 渲染插件（.so，实现 DSL 生成的 mq_renderer_v1）
  api             引擎 API 实现 + 生成绑定
  runtime         引擎运行时 ("浏览器")：Engine + Scene + main_loop
  script          脚本 (quickjs)
samples/player    宿主（仅一行 mq_run_scene；--shot 渲染一张图）
android/          Android Studio 模板（SDLActivity + scenes 全量 APK assets）
editors/vscode    .scn 预览插件（本地安装，见该目录 README）
scenes/           游戏资源 (.scn + .js)
assets/builtin/   内建资源（shaders/<name>/：一个 shader 一个目录 → 配置期打包为压缩源码）
third_party/      wgpu-native / quickjs-ng / wuffs
docs/             设计文档 (00–21 + components/ + assets/ + milestones/)
plans/            各模块开发计划
```

## 文档

- 设计文档系列：`docs/README.md`（根为 `docs/00-设计哲学与约束.md`）。
- 里程碑与路线图：`docs/milestones/00-路线图.md`。
- 模块开发计划：`plans/README.md`。
- 面向 AI 协作者：`AGENTS.md`。

## 状态

**平台**：Linux（GCC/Clang）、**Windows x64**（clang-cl + Ninja）与 **Android arm64**
均为支持目标；Android Studio 模板、APK 场景打包和场景浏览器见 [`android/README.md`](android/README.md)。
WASM/浏览器见 P3。

M0–M5、M7、M8 与 P1 已完成；M4/M6/M9 见下（细节 `docs/milestones/README.md`）：

### Windows 构建（clang-cl + ASan）

```powershell
# 一次性：取/构建第三方依赖（SDL2 · zlib-ng · zstd · freetype · libcurl · libwebsockets）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1

# 配置 + 构建 + 测试（默认 Debug + AddressSanitizer）
tools\win\build.cmd -RunTests
tools\win\build.cmd -NoSanitize -BuildType Release -RunTests   # 不带 ASan 的 Release
```

- 依赖装到 `.deps/dist`（**不入库**），并按同一 clang-cl + `/MD` 构建，避免运行时不匹配；
  `.deps/deps.cmake` 由脚本生成，根 CMake 自动 include。
- wgpu-native 用官方 Windows x64 MSVC 包（`wgpu_native.dll` + `.lib` 放入
  `third_party/wgpu-native/lib/`，**不入库**）。
- 细节与已知平台差异（UDS 仅 Linux、`wss://` 需要 TLS 版 lws、Windows 上模块不 `FreeLibrary` 等）
  见 `AGENTS.md` 的「Windows」与「ASan / UBSan」两节。

- **M4** ✅ VFS 多后端（file/mem/zip/builtin/URL）+ 内容哈希 + Wuffs 解码 + 材质。
- **M6** 🟡 确定性 2D 物理求解器（`modules/world/physics`，C ABI vtable + 世代句柄）；
  本机无 Rust 工具链，Rapier 后端按 ADR-0028 走"退化自研求解器"路径，接口不变。
- **M9** 🟡 帧边界热重载（脚本/场景，`Reload.*` + `dry_run`）；编辑器壳待做。
- **P3** 🟡 WASM P0 已接线：官方 Emscripten toolchain + 浏览器裁剪 profile + 静态 null renderer +
  `mq_host_step` + MEMFS 预载 + 多实例 checksum CI；正式浏览器渲染明确只走 WebGPU，
  `emdawnwebgpu` canvas 直呈属于下一阶段 P1。见 `docs/plan-wasm.md`。

回归：`ctest --test-dir build`（27 项，含端到端：`player_run`、`determinism`、`control_plane`、
`ui_e2e`、`hot_reload_e2e`、`anim_e2e`、`editor_e2e`、`host_tick`、`host_multi_instance`）。
