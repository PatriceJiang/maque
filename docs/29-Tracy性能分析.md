# Tracy 性能分析集成（帧/模块/内存域）

> 目标：把 Tracy（https://github.com/wolfpld/tracy）接到引擎里，让**核心循环、事件派发、脚本入口**、
> 各子系统耗时与**基础运行数据**（frametime / logic / render / present / CPU / 内存 / FPS / draws）
> 都能在 Tracy 时间线里直接看到；按**分区（冷暖）+ 模块**上色，F3 一键启动 profiler GUI。
>
> 第二轮扩展（§10 起）：**内存域**（脚本 / 节点 / 纹理 / GPU / 字体图集 / 全局堆）与
> `MQ_TRACY_MEM_*` / `MQ_TRACY_ALLOC_*` / 锁 等**不同类型的宏**；编译开关 + 运行时开关。

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

# ③ 无 GUI 看内存域（不需要 profiler / capture）：
MAQUE_TRACY_MEM_REPORT=1 MAQUE_WINDOW=0x0 ./build/samples/player/maque_player scenes/text/text.scn
# [mem] tick: script 255.6 KiB | node 3.8 KiB | texture 498.0 KiB | gpu 0 B | font 706.7 KiB | heap 0 B
# [mem] tick: area ui 706.7 KiB | 3d 498.0 KiB | system 3.8 KiB | script 255.6 KiB | tracked 1.43 MiB
```

- 构建开关：`-DMAQUE_TRACY=ON`（默认）/ `OFF`（全部埋点宏展开为 no-op、不链接第三方库）。
- 内存域开关：`-DMAQUE_TRACY_MEMORY=ON`（默认）/ `OFF`（记账宏全部 no-op）；
  `-DMAQUE_TRACY_ALLOC=ON`（全局 new/delete 拦截 → `heap` 域，默认 **OFF**）。
- **默认零开销**：普通运行不启动客户端、不监听端口、不占线程（`TRACY_DELAYED_INIT` +
  `TRACY_MANUAL_LIFETIME` + `TRACY_ON_DEMAND`）；只有 F3 / `MAQUE_TRACY=1` 才启动。
- 想从启动就开（等外部 GUI 连）：`MAQUE_TRACY=1 ./build/samples/player/maque_player <scene>`。

## 1. 架构与文件

| 位置 | 作用 |
|------|------|
| `third_party/tracy/` | **vendored 客户端源码**（仅 `public/`，v0.11.1；含 LICENSE）。`CMakeLists.txt` 编成 `maque::tracy_client` |
| `modules/core/tracy/` | **统一封装**（`maque::tracy`，`core/tracy/mq_tracy.h`）：分区/模块/内存域颜色、RAII zone、plot/message/frame、内存记账、手动启停 |
| `modules/core/tracy/src/tracy.cpp` | 生命周期、曲线/消息、**内存域计数器**（原子）+ 采样节流 + 报告 |
| `modules/core/tracy/src/mem_alloc.cpp` | **可选**全局 `operator new/delete` 拦截（`-DMAQUE_TRACY_ALLOC=ON` 才编译/链接） |
| `tools/tracy/get-profiler.ps1` | Windows：下载官方 `tracy-profiler.exe` / `tracy-capture.exe` 到 `.deps/tracy/`（不入库） |
| `tools/tracy/build-capture.sh` | Linux/macOS：从同版本源码构建 `tracy-capture`（CLI，无 GUI 依赖）到 `.deps/tracy/` |
| `tools/tracy/build-profiler.sh` | Linux/macOS：从同版本源码构建 `tracy-profiler`（GUI，F3 拉起） |
| `tests/tracy_e2e.sh` | 端到端：默认不启动、capture 能收到帧/zone、F3 拉起 profiler 进程 |
| `tests/tracy_mem_e2e.sh` | 内存域端到端：五个域非零、分区/冷暖、运行时开关、退出快照（不需要 GUI） |

**为什么先封装再埋点**：

1. 各模块不直接 include 第三方头 —— 命名/颜色/开关只有一处；
2. **按分区 + 模块上色**：UI / 3D 暖色、System / Script 冷色（§3），时间线里一眼看出归属；
3. `MAQUE_TRACY=OFF` 时宏全部 no-op，无第三方依赖、零链接；
4. 客户端**手动启动**：未启动时 zone 立即返回（不碰 Tracy 全局状态），普通运行/CI 不监听端口。

**不埋 `maque_gfx.dll`（渲染插件）**：它是独立 DLL，链接 Tracy 会产生"第二个客户端"（两个监听者抢 8086）。
渲染耗时在 runtime 侧用 `render` 颜色的 zone 包住插件调用（见 §4），已足够定位。
GPU 资源字节通过**渲染器 ABI 查询**（`resource_bytes2`）回到 runtime 再上报（§10.2），不需要插件链接 Tracy。

## 2. 埋点 API（模块里只用这套宏）

```cpp
#include "core/tracy/mq_tracy.h"

void foo(const char* name) {
    MQ_TRACY_ZONE(runtime);                          // 函数级 zone（颜色 = 模块色）
    MQ_TRACY_ZONE_N(script, "eval");                 // 命名 zone
    MQ_TRACY_ZONE_NAMED(z, script, "call");          // 需要动态文本时命名变量
    MQ_TRACY_ZONE_TEXT(z, name);                     // 动态 zone 名（事件名/脚本函数名）
    MQ_TRACY_ZONE_VALUE(z, bytes);                   // zone 上显示数值
    MQ_TRACY_ZONE_AREA_N(world3d, "shadow");         // 按**分区**上色（暖/冷）
    MQ_TRACY_MEM_ZONE_N(texture, "upload");          // 按**内存域**上色
}
```

| 宏 | 作用 |
|----|------|
| `MQ_TRACY_ZONE(<模块>)` | 当前作用域的 RAII zone，颜色 = 模块色 |
| `MQ_TRACY_ZONE_N(<模块>, "name")` | 命名 zone（静态字符串） |
| `MQ_TRACY_ZONE_NAMED(var, <模块>, "name")` | 命名 zone 变量（配合 `MQ_TRACY_ZONE_TEXT`） |
| `MQ_TRACY_ZONE_TEXT(var, text)` | 给 zone 设置动态名（事件名、函数名…） |
| `MQ_TRACY_ZONE_VALUE(var, v)` | 给 zone 附一个数值（Tracy zone 详情里可见） |
| `MQ_TRACY_ZONE_AREA(<分区>)` / `_N(分区, "name")` | 按分区（`ui`/`world3d`/`system`/`script`）上色的 zone |
| `MQ_TRACY_ZONE_AREA_NAMED(var, <分区>, "name")` | 分区色 + 动态名 |
| `MQ_TRACY_FRAME()` | 标记一个显示帧结束（Tracy 的 FrameMark，FPS/帧统计用） |
| `MQ_TRACY_PLOT("name", v)` | 曲线点（数值） |
| `MQ_TRACY_PLOT_CONFIG("name", type, step, fill, color)` | 曲线样式（type: 0=number 1=memory 2=percentage） |
| `MQ_TRACY_PLOT_MEM("name", bytes)` / `_C(..., color)` | 字节曲线（自动按 memory 格式配置一次） |
| `MQ_TRACY_MESSAGE("text")` / `MQ_TRACY_MESSAGE_COLOR(...)` | 时间线标记 |
| `MQ_TRACY_MEM_ADD/SUB/SET(<域>, bytes)` | 内存记账（delta 增/减、采样型绝对值覆盖） |
| `MQ_TRACY_MEM_ZONE(<域>)` / `_N(<域>, "name")` | 按内存域上色的 zone |
| `MQ_TRACY_MEM_DELTA(<域>)` / `_N(<域>, "name")` | **内存增量 zone**：构造/析构之间该域的净分配字节自动进 `zone value` |
| `MQ_TRACY_MEM_REPORT()` | 立即打印一次内存表（同 `MAQUE_TRACY_MEM_REPORT`） |
| `MQ_TRACY_ALLOC_EVENT(ptr, size, <域>)` / `MQ_TRACY_FREE_EVENT(ptr, <域>)` | Tracy **Memory 视图**分配事件（按指针配对） |
| `MQ_TRACY_ALLOC_CALLSTACK(ptr, size, <域>, depth)` | 同上，但带调用栈（开销更大） |
| `MQ_TRACY_LOCKABLE(type, var)` / `MQ_TRACY_LOCKABLE_N(type, var, desc)` | 可追踪的锁（默认 `= type var`，开启锁追踪时是懒创建的 Tracy Lockable） |
| `MQ_TRACY_LOCK_MARK(var)` | 标记"当前持有该锁"（Tracy 的 lock mark） |

模块 token 就是 `maque::tracy::Module` 的枚举名：`runtime / script / input / platform / physics / nav /
anim / ui / render / text / asset / vfs / imgui / audio`。新增模块：在 `mq_tracy.h` 的枚举、`area()`、
`color()`、`name()` 里各加一项（颜色 `0xRRGGBB`，保证互不相同；单元测试会断言唯一性）。
分区 token 是 `maque::tracy::Area`：`ui / world3d / system / script`。
内存域 token 是 `maque::tracy::Mem`：`script / node / texture / gpu / font / heap`。

生命周期（`maque::tracy` 命名空间）：`started()` / `startup()` / `shutdown()`，都幂等；
`startup()` 后 `tracy::StartupProfiler()` 监听 127.0.0.1:8086（端口可用 `MAQUE_TRACY_PORT` 改）。
`shutdown()` 在 `MAQUE_TRACY_MEM_REPORT=1` 时会先打印一次最终内存快照（只打一次）。

## 3. 分区与颜色（冷暖）

时间线最粗的一层分类是**分区**：**暖色 = 玩家能看到的东西（UI / 3D），冷色 = 基础设施
（System / Script）**。每个模块在所属分区色系里取一个色阶，所以不用看名字、看色温就能判断归属。

| 分区 | 色温 | 基色 | 模块（色阶示例） |
|------|------|------|------------------|
| `ui` | **暖** | `#E8A33D` 琥珀 | ui `#E8B84B`、text `#D9A441`、imgui `#C98A3C`、input `#F0C96B` |
| `world3d` | **暖** | `#E15759` 珊瑚红 | render `#E15759`、anim `#FF9DA7`、physics `#C4484A`、nav `#EE8A72` |
| `system` | **冷** | `#4E79A7` 蓝 | runtime `#4E79A7`、platform `#6C8FC4`、asset `#3F6E9B`、vfs `#5BA3B5`、audio `#7FA8D0` |
| `script` | **冷** | `#59A14F` 绿 | script `#59A14F` |

内存域颜色（同样按分区色系）：

| 内存域 | 分区 | 颜色 | 含义 |
|--------|------|------|------|
| script | script（冷） | `#6BC97A` | 脚本 VM 堆（QuickJS/Lua 自己的分配器） |
| node | system（冷） | `#8FA8C8` | 场景节点树（实体 + 组件 blob + 名字/实例元数据） |
| texture | world3d（暖） | `#E08A6B` | GPU 纹理（含字形/ImGui 图集、RenderTexture、env cube） |
| gpu | world3d（暖） | `#F0625D` | GPU 缓冲（网格顶点/索引） |
| font | ui（暖） | `#F5C542` | CPU 字体/字形图集（位图 + 字形表） |
| heap | system（冷） | `#9AA5B1` | 全局 C++ 堆（仅 `-DMAQUE_TRACY_ALLOC=ON`） |

单测 `maque_tracy_test` 断言：模块色唯一、内存域色唯一、四个分区的冷暖分类、模块/内存域 → 分区映射正确。

## 4. 当前埋点覆盖

**核心循环**（runtime，蓝）：
- `loop`（一轮主循环，含多步补偿/墙钟 sleep）→ `engine.setup` / `engine.teardown` / `scene.load`
- 每步 `runtime` zone 下：`input.sample`（平台事件泵+原始事件）、`sim`（逻辑阶段总）、
  `script.update`（update 回调派发）、`script.event`（**事件派发**，动态名 = 事件名）、
  `anim.tween` / `anim.step` / `particles`、`nav.step`、`physics.step`、`ui.step`、`flatten`、
  `anim.spine.step`、`extract`、`anim.spine.extract`、`text.flush`、`render.frame`、`present`。
- 每步末尾：内存域采样节流（`memory_due` → 节点/GPU/字体 + `memory_publish`，默认 1s 一次）。

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
内存记账的 delta 型调用（`memory_add/sub`）可以进分配器（每块一次原子操作，见 §10）。

## 5. 基础数据（Tracy 曲线）

启动客户端后自动配置并每步/每秒上报：

| 曲线 | 含义 | 颜色 |
|------|------|------|
| `frametime` | 本步总耗时（logic+render+present，ms） | System 蓝 |
| `logic` / `render` / `present` | 三段耗时（ms） | Script 绿 / 3D 红 / System 蓝 |
| `fps` | 平滑 FPS | UI 金 |
| `draws` | 提交项数（图元 + 网格批次） | 灰 |
| `entities` | 实体数 | node 蓝 |
| `cpu` | 进程 CPU%（自上次采样的差分，0..100×核数，1Hz） | input 橙 |
| `memory` | 进程常驻内存（WorkingSet / RSS，1Hz） | System 青 |
| `mem/script` … `mem/heap` | 各内存域当前字节（memory 格式） | 域色（§3） |
| `mem/area/ui` … `mem/area/script` | 分区总量（`ui`/`3d`/`system`/`script`） | 分区色 |
| `mem/tracked` | 已分类总量（script+node+texture+gpu+font，不含 heap） | 品红 |

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

环境变量汇总（启动参数）：

| 变量 | 作用 |
|------|------|
| `MAQUE_TRACY=1` | 启动即启动客户端（等外部 GUI 连），不需要按 F3 |
| `MAQUE_TRACY_PROFILER=<exe>` | 指定 profiler 路径（跳过自动搜索） |
| `MAQUE_TRACY_PORT=<port>` | 客户端监听端口/GUI 连接端口（默认 8086） |
| `MAQUE_TRACY_MEM=0` | **运行时关闭内存记账**（默认开；关闭后不采样、不打印、不推曲线） |
| `MAQUE_TRACY_MEM_REPORT=1` | 每个采样周期打印 `[mem]` 文本报告，退出时打印快照（无需 GUI/客户端） |
| `MAQUE_TRACY_MEM_INTERVAL=<秒>` | 采样/报告周期（默认 1.0；最小 0.05） |

## 7. 无 GUI 抓 trace（CI 用）

```bash
MAQUE_TRACY=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=6000 ./build/samples/player/maque_player <scene> &
.deps/tracy/tracy-capture -o out.tracy -a 127.0.0.1 -p 8086 -s 2   # Windows 是 tracy-capture.exe
```

`tracy-capture` 输出 `Frames:` / `Zones:` / `Trace size`；`tests/tracy_e2e.sh` 用它断言
帧数 >10、zone 数 >100、trace 非空（这是"埋点真的进了 trace"的硬证据）。
需要离线查看时用 `tracy-profiler out.tracy`；CSV 导出用 `tracy-csvexport`。

内存域有独立的文本门（不需要 GUI）：`tests/tracy_mem_e2e.sh` 断言 script/node/texture/font 非零、
分区分量正确、`MAQUE_TRACY_MEM=0` 零输出、退出快照存在。

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
- **锁对象不能在 client 启动前构造**（Tracy 的 `Lockable` 构造即发 LockAnnounce → 断言
  `s_profilerData`）。封装的 `MQ_TRACY_LOCKABLE` 用 `maque::tracy::Lockable` 懒创建跟踪对象：
  第一次加锁时若 `started()` 才建 Tracy 锁，否则永远走普通锁 —— 全局/成员锁对象在任何时候都安全。

## 9. 测试与回归

```bash
ctest --test-dir build-win -R 'tracy' --output-on-failure     # 单测 + 两个 e2e
```

- `tracy`：封装层单测（两种构建形态、幂等启停、颜色/分区唯一性、未启动安全性、内存记账语义、
  全局 new/delete 拦截（仅 ALLOC 构建）、`MAQUE_TRACY_SELFTEST` 钩子）。
- `tracy_e2e`：默认不启动 / capture 抓到帧与 zone / F3 启动客户端 + 拉起 profiler 进程
  （缺 `tracy-capture` 或构建为 `MAQUE_TRACY=OFF` 时 SKIP 77）。
- `tracy_mem_e2e`：内存域文本门（五个域非零、冷暖分区、`MAQUE_TRACY_MEM=0` 零输出、退出快照）。
- 性能回归：埋点默认零开销（未启动时每 zone 一次原子读 + 函数返回）；启动后开销属 Tracy 正常范围。
  内存记账（`MAQUE_TRACY_MEMORY`）在 delta 型分配器里每块一次原子加；**全局 new/delete 拦截**
  （`MAQUE_TRACY_ALLOC`）每块多 16B 头 + 两次原子操作，只用于专门的剖析构建，不要进 Release 基准。

## 10. 内存域（谁写哪个数）

### 10.1 采样与上报

- **delta 型**（`script` / `heap`）：分配器在 alloc/free 处调 `memory_add/sub`，实时累计。
- **采样型**（`node` / `texture` / `gpu` / `font`）：每 `MAQUE_TRACY_MEM_INTERVAL`（默认 1s）由 runtime
  `Engine::memory_sample()` 读一次"当前真相"，用 `memory_set` 覆盖。
- 采样节流：`memory_due(dt)` 累计模拟 dt，到期返回 true → 采样 + `memory_publish()`。
- **进程级**：内存域计数器与 Tracy client 一样是整个进程唯一的一份（profiler 是进程级设施；
  多实例宿主共用一个 client/端口，限制同 §8）。模拟逻辑不读这些计数器，因此不进校验和/不影响确定性。
- **关闭时（`MAQUE_TRACY_MEM=0`）**：所有记账函数立即返回，不采样、不推曲线、不打印；
  已记录值保留（需要干净基线用 `memory_reset()`）。
- 未编入（`MQ_TRACY_MEMORY_ENABLED=0`）：宏是 inline no-op，调用点不用改，零状态零开销。

### 10.2 每个域的单一写入者

| 域 | 写入者 | 口径 |
|----|--------|------|
| `script` | `modules/script/src/vm_mem.h` + `js_backend.cpp` / `lua_backend.cpp` | QuickJS `JS_NewRuntime2` 自定义 5 回调；Lua `lua_newstate` 自定义 alloc。账本 = `malloc_usable_size`/`_msize`（与 QuickJS 自己的 malloc_size 同口径），**不额外加分配头**；`MAQUE_TRACY_MEM=0` 时只多一个 bool 检查 |
| `node` | `runtime.cpp` `Engine::memory_sample()` | 场景实体表 + 组件 blob + 名字/树/实例元数据（估算：`unordered_map` 桶按 3 指针/节点计）。**派生量，不参与校验和** |
| `texture` | gfx `resource_bytes2` → runtime | 所有存活 `Texture`（含字形/ImGui 图集、RenderTexture、env cube）；`mip_texture_bytes` 按 mip/面/字节宽度累加 |
| `gpu` | gfx `resource_bytes2` → runtime | 所有 `MeshGpu`（顶点 + 索引 + wire 索引）；旧插件无 `resource_bytes2` 时回退 `resource_bytes` 总量记到 gpu |
| `font` | `mq_font_memory_bytes()`（text 模块）+ `mq_imgui_font_atlas_bytes()`（imgui 模块） | 字体对象 + 路径 + 内置位图 + 每字号图集 RGBA 容量 + 字形表容量 + hb 字体表；加上调试面板静态图集（2048² RGBA = 16 MiB，只在面板首次 Build 后计入；官方 SDL_Renderer 后端自管图集 → 0）。不含 FreeType 内部缓存 |
| `heap` | `mem_alloc.cpp`（仅 `-DMAQUE_TRACY_ALLOC=ON`） | 全局 `operator new/delete`：每块 16B 头（对齐分配 24B）记录 `accounted + size`，free 回冲；Tracy 内部递归分配不计账 |

前 5 个域**互不重叠**（GPU 纹理与缓冲分开、字体只算 CPU 侧），可以相加；
`heap` 是"其余全部 C++ 堆"，与明细**有交叠**，不要相加。
`memory_total()` / `mem/tracked` 只汇总前 5 个域；`memory_area_total()` 同样不含 `heap`。

### 10.3 全局 new/delete 拦截（`-DMAQUE_TRACY_ALLOC=ON`）

- 目标是"所有 C++ 堆分配"都有归属：`Mem::heap` 计数 + Tracy Memory 视图事件（`MemAllocNamed`/
  `MemFreeNamed`，按指针配对）。
- 静态库里的 `operator new` 只有被链接器拉进来才生效 → `tracy.cpp` 在 `startup()` 里引用
  `mem_alloc_link_anchor()` 强制拉入 `mem_alloc.cpp`。
- 覆盖全部标准变体：`new`/`new[]`/nothrow/aligned（>16B 对齐用 malloc + 手工对齐 + 原始指针头）/
  sized delete / aligned sized delete。
- 代价：每块 +16B（对齐 +24B）+ 每分配两次原子操作。**默认 OFF**，只用于专门的性能/泄漏剖析构建。
- ⚠️ **Windows + ASan 不兼容**：clang-cl 的 ASan 运行时自己定义 `operator new`，会与全局替换冲突
  （lld-link: `operator new ... was replaced`）。CMake 检测到 `MAQUE_SANITIZE` 含 address 时会警告并
  关闭 `MAQUE_TRACY_ALLOC`；要验证它请用不带 ASan 的构建（单测里有条件断言）。
- 递归护栏：`thread_local` 标记 + `g_in_mem_event`；Tracy 内部用 new 分配的队列块不会被记账，
  free 时也不会回冲（`accounted` 标记）。

### 10.4 GPU 资源按类拆分（渲染器 ABI，append-only）

```c
typedef struct mq_renderer_resource_usage {
    uint32_t struct_size;
    uint64_t texture_bytes;   /* 存活纹理（含 RenderTexture/cube/env） */
    uint64_t buffer_bytes;    /* 网格顶点/索引 */
    uint64_t other_bytes;     /* 预留（当前 0） */
} mq_renderer_resource_usage;

int32_t (*resource_bytes2)(void* self, mq_renderer_resource_usage* out);  /* 0=成功 */
```

- 追加在 `mq_renderer_v1` **表尾**（`read_texture` 之后），旧插件按 `header.struct_size` 判定不存在；
  `resource_bytes` 语义不变（= texture + buffer）。
- gfx：`Device::texture_bytes` / `buffer_bytes` 在创建/销毁/重建处同步；
  null renderer：`texture_bytes` = 存活像素，`buffer_bytes` = 0。
- runtime 用 `MQ_RENDERER_HAS(rapi, resource_bytes2)` 守卫；旧插件回退到 `stat_vram_bytes` 记 gpu。

### 10.5 文本/字体

`mq_font_memory_bytes(mq_font*)`（text 模块 ABI 追加）**只读**，不触发图集创建：
`sizeof(mq_font) + path.capacity() + builtin_bits.capacity() + Σ(rgba.capacity() + glyphs.capacity()*sizeof(Glyph))`
（+ HarfBuzz 句柄表容量）。`mq_imgui_font_atlas_bytes()`（imgui 模块公开面追加）返回调试面板静态图集的
像素字节（`w*h*4`；未 Build / 官方后端自管 → 0）。runtime 对 `Engine::fonts` 求和并加上 ImGui 图集后
`memory_set(Mem::font, …)`。

### 10.6 报告格式

```
[mem] tick: script 255.6 KiB | node 3.8 KiB | texture 498.0 KiB | gpu 0 B | font 706.7 KiB | heap 0 B
[mem] tick: area ui 706.7 KiB | 3d 498.0 KiB | system 3.8 KiB | script 255.6 KiB | tracked 1.43 MiB
[mem] shutdown: peak script 274.3 KiB | node 3.8 KiB | texture 498.0 KiB | gpu 0 B | font 706.7 KiB | heap 0 B
```

- tag：`tick`（周期）/ `shutdown`（退出）/ `manual`（`MQ_TRACY_MEM_REPORT()`）。
- 退出时 current 可能已被 teardown 清零（VM/字体/纹理都释放了）→ 额外打一行 **peak**（高水位线）；
  e2e 用峰值断言"这次运行真的分配过"。
- ImGui stats 面板（F2）也显示明细/分区两行（`MEM …`）；关闭时显示 `MEM off`。
- 每次 `memory_publish` 在有 profiler 连接时还会推 `mem/domain/*`、`mem/area/*`、`mem/tracked` 曲线。

## 11. 编译开关汇总

| 开关 | 默认 | 作用 | 关闭时 |
|------|------|------|--------|
| `MAQUE_TRACY` | ON | 编入 Tracy 客户端 + 全部埋点宏 | 全部 no-op、不链接第三方 |
| `MAQUE_TRACY_MEMORY` | ON | 内存域记账/曲线/报告（`MQ_TRACY_MEM_*`） | 宏 no-op；script/renderer/text 的记账钩子不编入 |
| `MAQUE_TRACY_ALLOC` | **OFF** | 全局 new/delete 拦截 → `heap` 域 + Memory 视图 | 无分配头开销；`heap` 恒 0 |
| `MAQUE_TRACY_LOCKS` | ON | `MQ_TRACY_LOCKABLE`/`LOCK_MARK`（懒创建，无锁对象则零成本） | 退化为普通成员/空操作 |

组合约束：`ALLOC` 需要 `MEMORY`（否则 CMake 警告并关闭）；Windows + ASan 与 `ALLOC` 互斥（见 §10.3）。
