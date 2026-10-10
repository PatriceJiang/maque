# maque

麻雀虽小，五脏俱全。**maque 是一个"可被 AI 自主驱动与观测"的轻量游戏引擎**，
同时服务**嵌入式**与**独立游戏**，并以 **WASM/浏览器**为一等目标平台。

> 核心理念：**引擎就是一个"浏览器"，游戏就是一堆资源**。
> 宿主零游戏逻辑，入口就一句 `mq_run_scene("ball.scn")`；玩法全在 `.scn` + `.js` 资源里。

## 特性

- **多实例 / 零全局可变状态 / 插件化**（模块即 `.so`，跨边界 C ABI）。
- **反射中枢**：`schema.yaml` 一处声明 → 生成 C++ 类型 / C ABI 描述符 / JSON / **脚本绑定**。
- **声明式接口绑定**：`api_list` 一次声明，生成 C++ 头 + JS 绑定（支持 MANUAL 头内联实现）。
- **脚本优先**：quickjs(JS) 作为游戏逻辑层；帧边界 GC，确定性友好。
- **真实 WebGPU 渲染**（wgpu-native）：离屏渲染 + readback。
- **VFS**：`file://` / 直接 URL（libcurl http/https/file）挂载式资源加载。
- **图像解码**：Wuffs（PNG/JPEG/GIF/BMP）。
- **平台层**：SDL2 窗口/输入；网络底座 libwebsockets。
- 确定性：固定步长、引擎 RNG、输入即数据、可回放（设计目标）。

## 快速开始

```bash
# 1) codegen 依赖（首次）
cd modules/core/data && npm install && cd -

# 2) 构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 3) 测试
ctest --test-dir build --output-on-failure

# 4) 运行示例（加载 .scn + .js，经 WebGPU 渲染）
./build/samples/player/maque_player scenes/ball.scn
```

> **依赖**：
> - 构建期 codegen 需 `node` + `yaml`（`npm install`）。
> - WebGPU 需 `third_party/wgpu-native/lib/libwgpu_native.so`（**二进制不入库**，见该目录说明）。
> - 系统依赖：SDL2、libcurl、libwebsockets（`apt-get install libsdl2-dev libcurl4-openssl-dev libwebsockets-dev`）。
> - 已 vendored：`quickjs-ng`、`wuffs`。

## 目录结构

```
modules/
  core/data       schema.yaml + codegen(parser.js) → maque::schema
  core/reflect    反射运行时 (mq::reflect)
  core/handle     世代句柄槽表
  core/module     模块宿主 (.so / registry)
  math            数学库
  io/vfs          虚拟文件系统 (file/URL)
  net             网络 (libwebsockets)
  platform        平台层 (SDL2 窗口/输入)
  data/persist    持久化/反序列化 + JSON DOM
  image           图像解码 (Wuffs)
  render/gfx      WebGPU 渲染插件（.so，实现 DSL 生成的 mq_renderer_v1）
  api             引擎 API 实现 + 生成绑定
  runtime         引擎运行时 ("浏览器")：Engine + Scene + main_loop
  script          脚本 (quickjs)
samples/player    宿主（仅一行 mq_run_scene）
scenes/           游戏资源 (.scn + .js)
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

里程碑 M0–M4a 基本完成（地基 → 首切片 → 脚本 → 实体/组件 → 场景资源 → VFS → 接口绑定机制）；
SD2/libcurl/libwebsockets/Wuffs 已集成。详见 `docs/milestones/`。
