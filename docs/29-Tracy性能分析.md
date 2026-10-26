# Tracy 性能分析集成（帧/模块/脚本入口）

> 目标：把 Tracy（https://github.com/wolfpld/tracy）接到引擎里，让**核心循环、事件派发、脚本入口**、
> 各子系统耗时与**基础运行数据**（frametime / logic / render / present / CPU / 内存 / FPS / draws）
> 都能在 Tracy 时间线里直接看到；按**模块上色**，F3 一键启动 profiler GUI。

## 0. 一句话上手

```bash
# ① 一次性：下载/构建 profiler GUI + capture CLI 到 .deps/tracy/（二进制不入库）
#   Windows：官方发行包（GUI + CLI）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\tracy\get-profiler.ps1
#   Linux/macOS：从同一份源码构建 GUI（F3 用）与 capture CLI
tools/tracy/build-profiler.sh
tools/tracy/build-capture.sh

# ② 直接玩：游戏里按 F3 → 启动客户端 + 拉起 profiler GUI（GUI 会自动连上 127.0.0.1:8086）
./build/samples/player/maque_player scenes/ball.scn
```

- 构建开关：`-DMAQUE_TRACY=ON`（默认）/ `OFF`（全部埋点宏展开为 no-op、不链接第三方库）。
- **默认零开销**：普通运行不启动客户端、不监听端口、不占线程（`TRACY_DELAYED_INIT` +
  `TRACY_MANUAL_LIFETIME` + `TRACY_ON_DEMAND`）；只有 F3 / `MAQUE_TRACY=1` 才启动。
- 想从启动就开（等外部 GUI 连）：`MAQUE_TRACY=1 ./build/samples/player/maque_player <scene>`。

## 1. 架构与文件

| 位置 | 作用 |
|------|------|
| `third_party/tracy/` | **vendored 客户端源码**（仅 `public/`，v0.11.1；含 LICENSE）。`CMakeLists.txt` 编成 `maque::tracy_client` |
| `modules/core/tracy/` | **统一封装**（`maque::tracy`，`core/tracy/mq_tracy.h`）：模块颜色、RAII zone、plot/message/frame、手动启停 |
| `tools/tracy/get-profiler.ps1` | Windows：下载官方 `tracy-profiler.exe` / `tracy-capture.exe` 到 `.deps/tracy/`（不入库） |
| `tools/tracy/build-capture.sh` | Linux/macOS：从同版本源码构建 `tracy-capture`（CLI，无 GUI 依赖）到 `.deps/tracy/` |
| `tools/tracy/build-profiler.sh` | Linux/macOS：从同版本源码构建 `tracy-profiler`（GUI，F3 拉起）到 `.deps/tracy/` |
| `tests/tracy_e2e.sh` | 端到端：默认不启动、capture 能收到帧/zone、F3 拉起 profiler 进程 |

**为什么先封装再埋点**：

1. 各模块不直接 include 第三方头 —— 命名/颜色/开关只有一处；
2. **按模块上色**：每个模块一个固定颜色（见 §3），时间线里一眼看出耗时归属；
3. `MAQUE_TRACY=OFF` 时宏全部 no-op，无第三方依赖、零链接；
4. 客户端**手动启动**：未启动时 zone 立即返回（不碰 Tracy 全局状态），普通运行/CI 不监听端口。

**不埋 `maque_gfx.dll`（渲染插件）**：它是独立 DLL，链接 Tracy 会产生"第二个客户端"（两个监听者抢 8086）。
渲染耗时在 runtime 侧用 `render` 颜色的 zone 包住插件调用（见 §4），已足够定位。

## 2. 埋点 API（模块里只用这套宏）

```cpp
#include "core/tracy/mq_tracy.h"

void foo(const char* name) {
    MQ_TRACY_ZONE(runtime);                          // 函数级 zone（颜色 = 模块色）
    MQ_TRACY_ZONE_N(script, "eval");                 // 命名 zone
    MQ_TRACY_ZONE_NAMED(z, script, "call");          // 需要动态文本时命名变量
    MQ_TRACY_ZONE_TEXT(z, name);                     // 动态 zone 名（事件名/脚本函数名）
}
```

| 宏 | 作用 |
|----|------|
| `MQ_TRACY_ZONE(<模块>)` | 当前作用域的 RAII zone，颜色 = 模块色 |
| `MQ_TRACY_ZONE_N(<模块>, "name")` | 命名 zone（静态字符串） |
| `MQ_TRACY_ZONE_NAMED(var, <模块>, "name")` | 命名 zone 变量（配合 `MQ_TRACY_ZONE_TEXT`） |
| `MQ_TRACY_ZONE_TEXT(var, text)` | 给 zone 设置动态名（事件名、函数名…） |
| `MQ_TRACY_FRAME()` | 标记一个显示帧结束（Tracy 的 FrameMark，FPS/帧统计用） |
| `MQ_TRACY_PLOT("name", v)` | 曲线点（数值） |
| `MQ_TRACY_PLOT_CONFIG("name", type, step, fill, color)` | 曲线样式（type: 0=number 1=memory 2=percentage） |
| `MQ_TRACY_MESSAGE("text")` / `MQ_TRACY_MESSAGE_COLOR(...)` | 时间线标记 |

模块 token 就是 `maque::tracy::Module` 的枚举名：`runtime / script / input / platform / physics / nav /
anim / ui / render / text / asset / vfs / imgui / audio`。新增模块：在 `mq_tracy.h` 的枚举和
`color()` 里各加一项（颜色 `0xRRGGBB`，保证互不相同）。

生命周期（`maque::tracy` 命名空间）：`started()` / `startup()` / `shutdown()`，都幂等；
`startup()` 后 `tracy::StartupProfiler()` 监听 127.0.0.1:8086（端口可用 `MAQUE_TRACY_PORT` 改）。

## 3. 模块颜色表

| 模块 | 颜色 | 模块 | 颜色 |
|------|------|------|------|
| runtime | `#4E79A7` 蓝 | text | `#BAB0AC` 灰 |
| script | `#59A14F` 绿 | asset | `#8CD17D` 浅绿 |
| input | `#F28E2B` 橙 | vfs | `#499894` 深青 |
| platform | `#B07AA1` 紫 | imgui | `#D372AD` 品红 |
| physics | `#E15759` 红 | audio | `#86BCB6` 蓝绿 |
| nav | `#76B7B2` 青 | anim | `#FF9DA7` 粉 |
| ui | `#EDC948` 黄 | render | `#9C755F` 棕 |

单测 `maque_tracy_test` 断言颜色唯一（时间线里能区分模块）。

## 4. 当前埋点覆盖

**核心循环**（runtime，蓝）：
- `loop`（一轮主循环，含多步补偿/墙钟 sleep）→ `engine.setup` / `engine.teardown` / `scene.load`
- 每步 `runtime` zone 下：`input.sample`（平台事件泵+原始事件）、`sim`（逻辑阶段总）、
  `script.update`（update 回调派发）、`script.event`（**事件派发**，动态名 = 事件名）、
  `anim.tween` / `anim.step` / `particles`、`nav.step`、`physics.step`、`ui.step`、`flatten`、
  `anim.spine.step`、`extract`、`anim.spine.extract`、`text.flush`、`render.frame`、`present`。

**脚本入口**（script，绿）：
- ABI 派发层：`vm.create` / `vm.destroy` / `eval`（文本=文件名）/ `has` / `call`（文本=函数名）/
  `last.error` / `gc` / `frame.boundary` / `register`
- 事件总线：`bus.call` / `bus.call3`（文本=处理器名）/ `hot.reload` / `bus.gc`
- 后端：`js.eval` / `js.call` / `js.gc`、`lua.eval` / `lua.call` / `lua.gc`（区分 JS/Lua 耗时）

**各子系统外层入口**（对应模块色）：input（`begin_frame`/`dispatch`/`inject`）、
physics（`step`）、nav（`find_path`/`step`）、anim spine（`step`/`draw`）、ui canvas（`resolve`）、
text（`measure`/`layout`）、imgui（`begin_frame`/`end_frame`/`backend_render`/`overlay`）、
vfs（`read`）、asset（`import`/`cook`/`pack`）、platform（`pump`/`input.poll`/`present.draw`/`present`）。

原则：**只加在外层入口，不进热内循环**（zone 本身有成本；细粒度靠采样调用栈视图，不靠堆 zone）。

## 5. 基础数据（Tracy 曲线）

启动客户端后自动配置并每步/每秒上报：

| 曲线 | 含义 | 颜色 |
|------|------|------|
| `frametime` | 本步总耗时（logic+render+present，ms） | 蓝 |
| `logic` / `render` / `present` | 三段耗时（ms） | 绿/棕/紫 |
| `fps` | 平滑 FPS | 黄 |
| `draws` | 提交项数（图元 + 网格批次） | 灰 |
| `entities` | 实体数 | 浅绿 |
| `cpu` | 进程 CPU%（自上次采样的差分，0..100×核数，1Hz） | 橙 |
| `memory` | 进程常驻内存（WorkingSet / RSS，1Hz） | 青 |

- 帧标记：每个模拟步一次 `FrameMark`（主循环可能一轮跑多步 → 与 `MAQUE_TRACE_STATS` 的 fps 口径一致）。
- CPU/内存来自平台层新 ABI：`mq_platform_process_stats`（Windows `GetProcessTimes` +
  `GetProcessMemoryInfo`；POSIX `getrusage` + `/proc/self/statm`）。
- 已有 `MAQUE_TRACE_STATS=1` 文本诊断保留（无 Tracy 时也能看帧预算）。

## 6. F3 启动 profiler

F3 处理在 `Engine::step_frame` 的键盘热键段（与 F2 相同的沿检测；`MAQUE_SIM_KEY_AT` 可注入测试）：

1. 若客户端未启动 → `maque::tracy::startup()`（监听 8086）；
2. 找 profiler 可执行文件：`MAQUE_TRACY_PROFILER` → 从 CWD 逐级向上找 `.deps/tracy/tracy-profiler.exe`
   （也从 `tools/tracy/` 找）→ 找到就用 `mq_platform_spawn_detached` 以 `-a 127.0.0.1 -p <port>` 拉起；
3. 找不到时打印明确提示（Windows 跑 `tools/tracy/get-profiler.ps1`、Linux/macOS 跑
   `tools/tracy/build-profiler.sh` 构建 GUI，或设 `MAQUE_TRACY_PROFILER`），不会崩。

已启动过则只补拉 GUI（`tracy_gui_launched` 防重复弹窗）。GUI 是**独立进程**：退出游戏后 trace 仍可看。

环境变量汇总：

| 变量 | 作用 |
|------|------|
| `MAQUE_TRACY=1` | 启动即启动客户端（等外部 GUI 连），不需要按 F3 |
| `MAQUE_TRACY_PROFILER=<exe>` | 指定 profiler 路径（跳过自动搜索） |
| `MAQUE_TRACY_PORT=<port>` | 客户端监听端口/GUI 连接端口（默认 8086） |

## 7. 无 GUI 抓 trace（CI 用）

```bash
MAQUE_TRACY=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=6000 ./build/samples/player/maque_player <scene> &
.deps/tracy/tracy-capture -o out.tracy -a 127.0.0.1 -p 8086 -s 2   # Windows 是 tracy-capture.exe
```

`tracy-capture` 输出 `Frames:` / `Zones:` / `Trace size`；`tests/tracy_e2e.sh` 用它断言
帧数 >10、zone 数 >100、trace 非空（这是"埋点真的进了 trace"的硬证据）。
需要离线查看时用 `tracy-profiler out.tracy`；CSV 导出用 `tracy-csvexport out.tracy`。

## 8. 构建/实现细节（改这块前先读）

- `third_party/tracy/CMakeLists.txt` 的客户端配置（PUBLIC，调用方 TU 必须看到同一套宏）：
  `TRACY_ENABLE`、`TRACY_ON_DEMAND`、`TRACY_DELAYED_INIT`、`TRACY_MANUAL_LIFETIME`、
  **`TRACY_NO_CRASH_HANDLER`**。
- ⚠️ **`TRACY_NO_CRASH_HANDLER` 是必须的**：Tracy 自带崩溃处理器会与 **ASan** 冲突。实测
  Debug+ASan 构建里不关它时 `tracy-capture` 只能收到 **3 帧、0 zone**（协议半途停摆，很像"埋点没生效"）；
  关掉后同一构建 129 帧 / 5418 zone 正常。崩溃诊断由仓库自己的 `MAQUE_CRASH_REPORT` / ASan 负责。
- ⚠️ 封装层在 `namespace maque::tracy` 内，引用 Tracy 全局名必须写 `::tracy::`（裸 `tracy::` 会解析成
  `maque::tracy` 自己）；`FrameMark` 宏也同理，直接调 `::tracy::Profiler::SendFrameMark(nullptr)`。
- 客户端**未启动时**任何 zone/plot/message 都必须 no-op：`ScopedZone` 先查 `started()` 再构造
  `std::optional<::tracy::ScopedZone>`（Tracy 在延迟初始化下全局未就绪，构造函数会解空指针）。
- 退出前 `shutdown()`（`mq_run_scene_ex` / `mq_host_scene_destroy` 末尾）：`TRACY_MANUAL_LIFETIME` 下不冲刷
  会漏数据，而且 Tracy 的堆分配会被 ASan 报泄漏。
- 嵌入式宿主多实例共用 8086：默认按单实例使用；多实例同时销毁时第一个 destroy 会 shutdown 客户端
  （已知限制，需要多实例同时 profile 时各自设 `MAQUE_TRACY_PORT`）。

## 9. 测试与回归

```bash
ctest --test-dir build-win -R 'tracy' --output-on-failure     # 单测 + e2e
```

- `tracy`：封装层单测（两种构建形态、幂等启停、颜色唯一、未启动安全性、`MAQUE_TRACY_SELFTEST` 钩子）。
- `tracy_e2e`：默认不启动 / capture 抓到帧与 zone / F3 启动客户端 + 拉起 profiler 进程
  （缺 `tracy-capture` 或构建为 `MAQUE_TRACY=OFF` 时 SKIP 77）。
- 性能回归：埋点默认零开销（未启动时每 zone 一次原子读 + 函数返回）；启动后开销属 Tracy 正常范围，
  不要在 Release 的 60fps 演示机上长期开着 GUI 抓帧做性能对比（抓 trace 本身有开销）。
