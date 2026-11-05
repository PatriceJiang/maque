# 35 · Linux 采样剖析（perf）：agent 的热点采集与自动优化闭环

> 上游：[`09 可观测性与诊断`](09-可观测性与诊断.md) §4、[`29 Tracy`](29-Tracy性能分析.md)（时间线/zone）、
> [`11 控制平面`](11-控制平面与AI接口.md)（A2/A4）。
> 本文回答三个问题：**为什么选 perf（而不是 oprofile）**、**引擎怎么启用/采集**、
> **agent 如何用报告自动定位→修改→验证**。

## 0. 一句话上手

```bash
# ① 构建（Linux 默认已开；-DMAQUE_PROFILE=ON 会补 -g + 帧指针，Release 也能解析源码行）
cmake -S . -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAQUE_PROFILE=ON
cmake --build build-perf --target maque_player

# ② 检查权限/符号（不采样也可以先跑）
python3 tools/perf/maque_perf.py check --binary build-perf/samples/player/maque_player

# ③ 录一段固定长度的 profile，生成 report.md / hotspots.json
python3 tools/perf/maque_perf.py record --out .maque/perf/base --frames 600 -- \
  ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
cat .maque/perf/base/report.md

# ④ 改代码、重编译、再录一次，做回归门
python3 tools/perf/maque_perf.py record --out .maque/perf/cand --frames 600 -- \
  ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
python3 tools/perf/maque_perf.py compare --baseline .maque/perf/base \
  --candidate .maque/perf/cand --fail-on-regression
ctest --test-dir build-perf --output-on-failure
```

也可以用**引擎内自附加**（不包一层 perf，适合已启动的游戏/控制面会话）：

```bash
MAQUE_PROFILE=1 MAQUE_PROFILE_FRAMES=600 MAQUE_PROFILE_DIR=.maque/perf/live \
  ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
```

## 1. 决策：perf，不接 oprofile

用户要求“Linux only，perf 或 oprofile”。本仓库**明确选择 perf**：

| 维度 | perf（采用） | oprofile（不采用） |
|------|--------------|---------------------|
| 维护状态 | 内核源码树内，随内核发布 | 长期维护模式；新内核/新发行版支持不稳定（Ubuntu 26.04 默认不装） |
| 依赖 | `perf(1)` 二进制；`perf_event_open` | 需要 `oprofile` 内核模块 + 常驻 `oprofiled` 守护进程 + 初始化计数器 |
| 权限 | `perf_event_paranoid<=1` 或 `CAP_PERFMON`；用户态可低权限 | 通常需要 root 启停守护进程 |
| 数据/工具链 | `perf.data`、`perf report`、`perf script`、`perf annotate`；支持 DWARF/帧指针、内联、调用栈、源码行 | `opreport`/`opannotate`，调用栈与源码行能力较弱 |
| CI/容器 | 一个二进制 + 内核能力；可用 `--cap-add PERFMON` | 需要模块/守护进程，容器内基本不可行 |
| 与 agent 的契合 | 子进程模式（`perf record -p` / `perf record -- cmd`）+ 文本/JSON 报告，直接可被 Python 解析 | 需要额外的会话/数据库管理 |

因此：**只支持 Linux + perf**；其它平台/未编入时 `maque::profile` 是 stub（控制面返回
`supported:false`），不需要 `#if`。oprofile 不提供后端，也不在文档里留半截兼容层。

## 2. 与 Tracy 的分工（两者都要）

| 问题 | 工具 | 数据 |
|------|------|------|
| “这一帧 render 用了 8ms，logic 2ms？” | **Tracy**（docs/29） | zone 时间线、frametime/logic/render/present、内存域 |
| “600 帧里 CPU 花在哪些函数/源码行？谁占 40%？” | **perf**（本文） | 统计采样、self time、源码行、模块聚合 |
| “这个函数被谁调用？” | perf（`perf report -g graph,0.5,caller`）/ Tracy 调用栈 | 调用图 |
| “内存涨了还是泄漏？” | Tracy 内存域 / `Perf.get` 指标 | 六域 + 峰值 |
| “改完到底快了没有？” | `tools/perf/maque_perf.py bench` + `compare` | 墙钟/帧内耗时中位数 + 热点差 |

**不要用 perf 取代 Tracy**：perf 是统计采样（看不到完整帧结构、没有 zone 名）；
Tracy 是显式埋点（能看到阶段划分，但看不到未埋点的函数内热点）。两者互补。

## 3. 架构与文件

| 位置 | 作用 |
|------|------|
| `modules/core/profile/` | `maque::profile`：perf 子进程生命周期 + `perf report` 解析 + 产物（`supported/start/stop/refresh/mark/status`） |
| `modules/core/profile/include/core/profile/mq_profile.h` | 公共 C++ 内部接口（非跨模块 C ABI；只有 runtime 用） |
| `modules/core/profile/tests/profile_test.cpp` | 报告文本解析单测（不需要真实 perf） |
| `modules/runtime/src/engine/profile.cpp` | 引擎策略：`MAQUE_PROFILE` 环境、F5、自动到帧停止、`meta.json` context、obs 指标 |
| `modules/runtime/src/control/system.cpp` | 控制面 `Perf.profile_*` 五个方法 |
| `tools/perf/maque_perf.py` | agent/CI CLI：check/record/report/parse/bench/compare |
| `tests/profile_e2e.sh` | 生命周期 e2e（用 fake perf，无需权限） |
| `tests/perf_report_e2e.sh` | 解析/对比离线 e2e（固定文本） |
| `tests/data/perf/fake_perf.py` | 假 perf：record 睡到 SIGINT、report/script 输出固定文本 |

**实现边界**：

- perf 是**外部子进程**（`perf record -p <本进程>` 或外部包一层）。引擎侧没有采样线程、
  没有信号处理器、没有 `perf_event_open` 调用。
- 启动时等一小段（默认 600ms）确认 perf 还活着：权限不足会立刻退出，于是能在同一帧给出
  可操作错误，而不是“录了 600 帧才发现 perf.data 是空的”。
- 停止用 **SIGINT**（perf 收到才会冲刷 `perf.data`；SIGTERM 可能丢尾部），5s 未退才 SIGKILL。
- 报告解析只读 `perf report --stdio` 文本；不链接 libperf、不依赖 `perf` 的 Python 绑定。

## 4. 构建开关与符号

| 开关 | 默认（Linux） | 作用 | 关闭时 |
|------|---------------|------|--------|
| `MAQUE_PROFILE` | **ON** | 编译 perf 后端；并给全项目加 `-g -fno-omit-frame-pointer` | stub：`supported()==false`、控制面返回 unsupported；`MAQUE_PROFILE` 环境变量被忽略并打印原因 |

- **优化级别不变**：只加调试信息与帧指针；Release 仍然是 `-O3 -DNDEBUG`，采样能解析到
  `file.cpp:行号`，`--call-graph fp` 成本低。
- 需要更准的调用栈：`MAQUE_PROFILE_CALLGRAPH=dwarf`（perf 的 DWARF 展开；更慢、需要更大栈）。
  默认 `fp`（帧指针）足够定位热点函数；DWARF 用于“必须拿到完整 caller 链”的场景。
- 其它平台（Windows/macOS/WASM/Android）配置期强制 `MAQUE_PROFILE=OFF`；代码仍可编译（stub）。
- `-DMAQUE_PROFILE=OFF` 用于最小发布二进制；strip 过的二进制 perf 只能报告到符号/地址，
  源码行会缺失（`check --binary` 会警告）。

## 5. 运行时开关（环境变量）

| 变量 | 默认 | 作用 |
|------|------|------|
| `MAQUE_PROFILE=1` | 关 | 启动期启用 perf 录制。`0`/`off`/空 = 关闭；非 Linux 或 OFF 构建打印一次原因后继续运行 |
| `MAQUE_PROFILE_FRAMES=N` | 0 | 从开始录制算起跑 N 个模拟帧后**自动停止并生成报告**；0 = 手动/F5/退出时停 |
| `MAQUE_PROFILE_START=N` | 0 | 第 N 个模拟帧才开始（跳过加载/首帧预热） |
| `MAQUE_PROFILE_DIR=<dir>` | `.maque/profile/run-<时间戳>` | 产物目录（`.maque/` 已在 `.gitignore`） |
| `MAQUE_PROFILE_PERF=<path>` | `perf` | 指定 perf 可执行文件（测试用 fake perf；也可指向自定义包装器） |
| `MAQUE_PROFILE_FREQ=N` | 999 | 采样频率 Hz（`perf -F`） |
| `MAQUE_PROFILE_CALLGRAPH=fp\|dwarf\|none\|lbr` | `fp` | `none` = 不采调用栈（最轻） |
| `MAQUE_PROFILE_KERNEL=0` | 1 | 不采内核侧样本（`--exclude-kernel`；内核符号需要额外 `kptr_restrict` 权限） |
| `MAQUE_PROFILE_REPORT_ON_EXIT=1` | **0** | 进程退出时同步生成报告。默认 **关闭**：长录制的 `perf report` 可能耗时几十秒，会让“关窗口”看起来卡死；`perf.data` 已落盘，事后用 `maque_perf.py report --data <dir>` 即可 |

语义：

- 自动录制在**第一个 `step_frame`**（frame 0）开始；`FRAMES=N` 时在 frame N 的**帧首**
  （渲染该帧之前）停止 → 正好覆盖 frame 0..N-1。**加载/装配期不在其中**（要覆盖启动期用外部
  `record`）；自附加启动会等 perf 存活（默认 600ms，失败则立刻报权限错误）。
- `FRAMES=0` 时一直录到 F5 停止、控制面停止或进程退出；退出时 `teardown()` 会兜底
  `SIGINT`（最多等 2s 后 SIGKILL）并**默认跳过 `perf report`**，所以关窗口不会被长报告阻塞。
  需要退出时自动出报告就用 `MAQUE_PROFILE_REPORT_ON_EXIT=1`；否则事后运行
  `python3 tools/perf/maque_perf.py report --data <dir>`。
- 录制期间 `F5` 是**引擎热键**（按下沿切换 start/stop），不会下发给游戏；
  未启用 profiling 时 F5 完全归游戏/脚本。
- `Perf.get` 额外提供指标：`profile.recording`（0/1）、`profile.samples`、
  `profile.hotspots`、`profile.elapsed_ms`（在 start/stop 与每次 `Perf.get` 查询时刷新；
  热路径不付出字符串拷贝/状态查询）。

## 6. 控制面（agent 运行时采集）

五个方法（`schema/control.yaml`，docs/11）。**返回值统一包一层**
`{ok, error?, ... , profile:{…}}`；`profile` 内是 `maque::profile::hotspots_json` 的结构。

| 方法 | 参数 | 说明 |
|------|------|------|
| `Perf.profile_status` | — | 状态（`state/pid/samples/report_ok/产物流`）；非 Linux/OFF 返回 `supported:false` |
| `Perf.profile_start` | `freq`, `callgraph`, `label`, `dir`, `frames` | 启动 perf record；`frames>0` 到帧自动停 |
| `Perf.profile_stop` | `report`（默认 true） | SIGINT → 等 → `perf report` → `hotspots.json`/`meta.json` |
| `Perf.profile_report` | `top`（默认 20）, `refresh` | 返回前 top 个热点；`refresh=true` 重跑报告（建议 stop 之后，录制中的 `perf.data` 可能未完成） |
| `Perf.profile_mark` | `label` | 追加 `{"frame","t_ms","label"}` 到 `marks.jsonl` |

**权限**：`profile_start/stop/mark` 是**变更类**（启动外部进程 / 写产物），
默认只读控制会话（`MAQUE_INSPECT` 不带 `_RW`）会得到 `-32001`。
两种做法：

```bash
# A) 启动就用环境自动录（不需要控制面写权限）
MAQUE_PROFILE=1 MAQUE_PROFILE_FRAMES=600 MAQUE_INSPECT=7777 ./maque_player <scene>
#    agent 只读查询 Perf.profile_status / Perf.get

# B) 需要按需 start/stop → 显式打开控制面写权限（只在你信任的 agent/本机使用）
MAQUE_INSPECT=7777 MAQUE_INSPECT_RW=1 ./maque_player <scene>
```

一次典型的 MCP/JSON-RPC 序列：

```json
{"jsonrpc":"2.0","id":1,"method":"Perf.profile_start",
 "params":{"freq":999,"callgraph":"fp","dir":".maque/perf/mcp","frames":0}}
{"jsonrpc":"2.0","id":2,"method":"Runtime.step","params":{"frames":600}}
{"jsonrpc":"2.0","id":3,"method":"Perf.profile_mark","params":{"label":"after-physics"}}
{"jsonrpc":"2.0","id":4,"method":"Perf.profile_stop","params":{"report":true}}
{"jsonrpc":"2.0","id":5,"method":"Perf.profile_report","params":{"top":10}}
```

> `Perf.profile_report` 的 C++ 返回是**符号级**（dso/symbol/percent/samples）；
> 需要源码行与模块聚合时，用 `tools/perf/maque_perf.py report --data <dir>` 对同一份
> `perf.data` 再生成一次（会覆盖 `hotspots.json`，并写 `report.md`）。

## 7. 产物与报告格式

一次录制在 `<dir>/` 下产生：

| 文件 | 内容 |
|------|------|
| `perf.data` | perf 原始采样（可用 `perf report -i`/`perf annotate` 继续挖） |
| `perf.log` | `perf record` 的 stdout+stderr（权限/警告在这里） |
| `perf-report.txt` | `perf report --stdio --no-children --sort dso,symbol` 的原始文本 |
| `hotspots.json` | 机器可读热点（agent 主入口） |
| `meta.json` | 后端/版本/pid/起止帧/elapsed/context（scene/build/window）/产物路径 |
| `marks.jsonl` | `Perf.profile_mark` 的帧号/相对时间/标签 |
| `report.md` | 人读报告（仅 Python `report` 生成） |
| `perf-script.txt` | `perf script -F ip,sym,dso,srcline`（仅 Python `report` 生成，用于源码行） |

`hotspots.json`（Python 版）关键字段：

```json
{
  "backend": "perf",
  "total_samples": 4821,
  "source_samples": 4302,
  "hotspots": [
    {"percent": 18.42, "samples": 888, "dso": "maque_player",
     "symbol": "maque::rt::Engine::step_frame()",
     "source": "modules/runtime/src/engine/frame.cpp:16",
     "module": "modules/runtime"}
  ],
  "modules": [{"module": "modules/runtime", "percent": 41.0, "samples": 1977}],
  "warnings": []
}
```

- 热点是 **self time**（`--no-children`），不含子调用；用来回答“这个函数自己烧了多少 CPU”。
- `module` 由源码路径推导（`modules/core/profile`、`modules/render/gfx`、`third_party/*`…）；
  没有源码行时用命名空间/DSO 启发式（`maque::rt::` → runtime 等），见
  `tools/perf/maque_perf.py:classify_module`。
- `compare` 以 **percent** 为主（而不是样本数），避免不同采样频率/时长下误判。

## 8. 权限、容器与常见错误

### 8.1 perf_event_paranoid

```bash
cat /proc/sys/kernel/perf_event_paranoid     # 2/3/4 = 无法采样；1 = 允许用户态；0/-1 = 连内核也允许
sudo sysctl -w kernel.perf_event_paranoid=1  # 临时（重启失效）
# 持久：/etc/sysctl.d/99-maque-perf.conf → kernel.perf_event_paranoid = 1
```

- **agent/CI 推荐 1**：只采用户态（引擎本身），不需要内核符号。
- 需要内核侧（锁/内存分配系统调用）才用 0，并注意 `kptr_restrict` →
  `sudo sysctl -w kernel.kptr_restrict=0`（只影响内核符号名）。
- 容器：`docker run --cap-add=PERFMON ...`（或 `--privileged`，不推荐）；
  `perf` 二进制与 `perf.data` 必须同一版本/可读；容器内 `perf_event_paranoid` 是宿主的。
- `CAP_PERFMON` 可只授予 perf 二进制：`sudo setcap cap_perfmon+ep /usr/bin/perf`。
- **自附加（`perf record -p`）** 在 Yama/ptrace 限制严格的容器里可能被拒；此时用
  `record` 的**外部包裹**模式（`perf record -- <command>`，本工具默认），它不依赖 attach。

### 8.2 错误对照

| 现象 | 原因 | 处理 |
|------|------|------|
| `perf: command not found` | 没装 perf | Ubuntu：`sudo apt install linux-tools-$(uname -r)`；或 `MAQUE_PROFILE_PERF=<path>` |
| `Access to performance monitoring ... is limited` / `paranoid` | 权限 | 8.1；或容器加 `PERFMON` |
| `perf.data` 为空 / `no samples parsed` | 没权限、目标立即退出、事件不可用 | 先 `maque_perf.py check`；确认 `--frames` > 0；看 `perf.log` |
| 大量 `0x…` / `[unknown]` | 无符号/被 strip / JIT 代码 / 内核符号不可读 | 用 `-DMAQUE_PROFILE=ON` 构建；不要 strip；JIT（QuickJS）无符号属已知限制 |
| 源码行覆盖率 < 50% | Release 无 `-g`、split DWARF、路径被重映射 | 确认 CMake 配置输出有 `Linux perf profiling ON`；`check --binary` |
| `perf report` 版本不认 `--show-nr-samples` | 老 perf | C++/Python 都会自动去掉该选项重试（Samples 变 0，percent 仍有效） |
| `perf script` 失败 | 老 perf 不支持 `srcline` 字段 | 自动退回 `ip,sym,dso`；热点仍可用，源码行缺失 |
| WSL / 虚拟机 | 无 PMU/被宿主禁用 | 用 `perf` 的软件事件仍可能失败；改用 Tracy/`bench`，或外部 Linux 主机 |

## 9. Agent 自动优化闭环（本文的重点）

> 引擎**不会**自动改 C++ 代码（自动改写源码超出引擎职责、风险不可控）。这里说的
> “自动优化”是**agent 驱动的闭环**：工具负责可重复采集 + 机器可读归因 + 回归门，
> agent 负责读代码、改代码、跑测试。所有中间产物都是文件/JSON，不靠人看日志。

推荐流程（每一步都可脚本化）：

1. **固定基准**：同一个场景、同一个构建类型（Release + `MAQUE_PROFILE=ON`）、同样的
   `--frames`、同样的 `MAQUE_WINDOW=0x0`/分辨率。`record` 默认会设 `MAQUE_PACE=off` 与
   `MAQUE_TRACE_STATS=1`，避免墙钟限速影响。
2. **先测墙钟基线**（不需要 perf 权限）：
   ```bash
   python3 tools/perf/maque_perf.py bench --out .maque/bench/base --frames 600 --reps 3 -- \
     ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
   ```
3. **采集热点**（同一命令加 perf；`record` 会跑 600 帧）：
   ```bash
   python3 tools/perf/maque_perf.py record --out .maque/perf/base --frames 600 \
     -- ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
   ```
4. **读归因**：`report.md` 的 Hotspots 表；优先看 `source` 有行号、`module` 是引擎模块、
   `percent` 稳定在前 5 的条目。需要调用者时：
   ```bash
   perf report -i .maque/perf/base/perf.data -g graph,0.5,caller --stdio | head -100
   perf annotate -i .maque/perf/base/perf.data --stdio -s '<hot symbol>' | head -80
   ```
5. **改代码**：一次只改一个热点/一类问题（避免归因混淆）；记录假设与预期（如“去掉每帧
   `unordered_map` 查找，预期 step_frame 自身占比 -5pp”）。
6. **重编译 + 重采**（输出到 `cand`）：
   ```bash
   cmake --build build-perf --target maque_player
   python3 tools/perf/maque_perf.py record --out .maque/perf/cand --frames 600 \
     -- ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
   python3 tools/perf/maque_perf.py bench --out .maque/bench/cand --frames 600 --reps 3 -- \
     ./build-perf/samples/player/maque_player scenes/pbr/cube.scn
   ```
7. **回归门**（任一失败就回退/继续分析）：
   ```bash
   python3 tools/perf/maque_perf.py compare --baseline .maque/perf/base \
     --candidate .maque/perf/cand --fail-on-regression
   python3 tools/perf/maque_perf.py compare --baseline .maque/bench/base \
     --candidate .maque/bench/cand --fail-on-regression
   ctest --test-dir build-perf --output-on-failure
   ```
8. **判定标准（避免“优化噪声”）**：
   - 每侧至少 **2000 samples**（600 帧 × ~1kHz 通常够；不够就加大 `--frames` 或 `--frequency`）；
   - 热点占比变化 > **3% 相对值**且 > **0.5pp** 才算变化（工具默认阈值）；
   - `bench` 用 3 次中位数，不要单次；对比同一构建类型；
   - 逻辑改动要另跑 `ctest`（尤其 `determinism` / `runtime_oracle`）确认不改变模拟结果；
   - 回退时保留 `base/cand` 两个目录作为证据（`meta.json` 里有场景/帧数/构建上下文）。
9. **记录结论**：把 `report.md` 的 before/after 表、改动点、回归门结果写进提交说明/`docs`；
   性能类 bug 按 `AGENTS.md` 要求建档（`docs/bugs/`）。

**不要做的事**：

- 不要在 Debug/ASan 构建上追求绝对毫秒（慢 ~2×）；用 Release 做优化，用 Debug 做正确性。
- 不要用 perf 记录期间的墙钟作为性能结论（采样有开销）；用 `bench` 单独测。
- 不要只看 `perf report` 的总占比就改代码——先确认源码行属于热路径、再看调用者/分支。
- 不要同时改多处再比较（无法归因）；不要跨场景/跨帧数比较。

## 10. 工具命令参考

```bash
python3 tools/perf/maque_perf.py check [--perf P] [--binary BIN] [--json]
python3 tools/perf/maque_perf.py record --out DIR [--frames N] [--frequency HZ]
        [--callgraph fp|dwarf|none|lbr] [--top N] [--headless|--window]
        [--env K=V] [--] <command...>
python3 tools/perf/maque_perf.py report --data DIR|perf.data [--out DIR] [--top N]
python3 tools/perf/maque_perf.py parse --report FILE [--script FILE] --out DIR [--top N]
python3 tools/perf/maque_perf.py bench --out DIR [--frames N] [--reps R]
        [--headless|--window] [--env K=V] [--] <command...>
python3 tools/perf/maque_perf.py compare --baseline DIR --candidate DIR
        [--threshold PCT] [--min-delta PP] [--fail-on-regression] [--json]
```

- `record` 默认 `--headless`（`MAQUE_WINDOW=0x0`）并设 `MAQUE_PACE=off`；需要真实窗口时加
  `--window`。
- `record` 会**移除继承的 `MAQUE_PROFILE`**，避免外部 perf 与引擎内自附加双开。
- `parse` 完全离线：输入已保存的 `perf report`/`perf script` 文本，CI/无 perf 环境可测。
- `compare` 同时比较 `bench.json`（墙钟/帧内耗时）与 `hotspots.json`（百分比）；没有的侧跳过。

## 11. 测试与回归

```bash
ctest --test-dir build -R '^(profile|profile_e2e|perf_report_e2e)$' --output-on-failure
```

- `profile`：解析器单测（标准/旧版无 Samples/C++ 模板/垃圾行/不支持构建的 API 形状）。
- `profile_e2e`（Linux + TARGET maque_gfx）：用 `tests/data/perf/fake_perf.py` 覆盖
  ① 环境自动录制 + 第 N 帧停止 ② 控制面 start→step→mark→stop→report
  ③ perf 缺失时继续运行并打印可操作错误 ④ `MAQUE_PROFILE=0` 零日志；
  ③b `bench` + `compare` 自比较（无 perf 权限也能跑的回归门）；⑥ 若本机真实 `perf` 可用（`perf_event_paranoid<=1`/`CAP_PERFMON`）再走一次
  **外部 record → 工具 report**，断言 `total_samples>0`、热点非空；无权限时打印跳过、不失败。
  构建 `MAQUE_PROFILE=OFF` 时整体 SKIP 77。
- `perf_report_e2e`：固定文本的百分比/样本/源码行/模块归类 + 自比较 0 + 人为回退必须被
  `--fail-on-regression` 捕获（不需要真实 perf）。
- **真实 perf 门**：`profile_e2e` 的 ⑥ 会自动探测真实 perf（可为 CI 设
  `MAQUE_PROFILE_REAL_PERF=<perf>` 覆盖路径），有权限就走真实 `perf record -- <player>`；
  无权限环境保持 fake，不阻塞 CI。

## 12. 设计约束与非目标

- **Linux only**：perf 是内核接口；Windows/macOS/WASM/Android 一律 stub。WASM 的
  profiling 走浏览器 DevTools/Perfetto（docs/05），不在本文范围。
- **不改变模拟行为**：perf 只观察进程；不写场景、不推进帧、不碰 RNG。采样开销会改变
  墙钟速度，但固定步长模拟的 `world_trail` 不变（可回归验证）。
- **进程级单例**：`perf record` 附加整个进程；多 Engine/多实例宿主同一时刻只能一个录制
  （与 Tracy 相同限制）。需要并行剖析时各自独立进程。
- **不做自动改代码**：工具提供 `hotspots.json`/`compare` 回归门；优化决策由 agent/人做。
- **不做 JIT 符号**：QuickJS/Lua 生成的代码在 perf 里是匿名地址；需要时先关脚本或用
  Tracy zone 归因（未来可接 `perf inject --jit`/`/tmp/perf-<pid>.map`）。
- **不做 GPU 采样**：WebGPU 时间戳/GPU 计数器不在 perf 的 CPU 采样范围；用渲染器的
  Tracy zone/后续 GPU timestamp。
- **退出时报告默认不做**：`teardown()` 只 `SIGINT` + 最多 2s 等待；`MAQUE_PROFILE_REPORT_ON_EXIT=1`
  才同步跑 `perf report`（长录制会慢）。`MAQUE_PROFILE_FRAMES=N` 的自动停止仍会生成报告
  （发生在游戏运行中，不在关窗路径）。

## 13. 已知坑（改实现前读）

- **不要用 SIGTERM 停 perf**：可能丢 `perf.data` 尾部；必须 SIGINT + 超时兜底。
- **`perf record -p` 的启动窗口**：attach 是异步的，前几帧可能采不到；要完整覆盖启动期
  就用外部 `record` 包裹（本工具默认）；自附加模式则把加载/装配重活放在录制前（不采样它们）。
- **`--show-nr-samples` 不是所有 perf 版本都有**：解析器必须能退回只有 Overhead 的格式
  （C++/Python 都实现了）。
- **`[.]` 标记是符号列的开始**：C++ 模板/匿名命名空间里有括号，不能按“最后一个括号”切
  DSO；按 `[.]` 标记切（C++ 解析器）或按 ` (dso)` 的最后一个括号切（Python script 解析）。
- **`perf script` 的行数才是真实样本数**：`perf report` 的 Samples 列可能被压缩/格式化；
  Python 优先用 script 计数，report 只作 fallback。
- **`perf.data` 版本绑定**：换 perf 版本后旧 `perf.data` 可能读不了；重新录。
- **控制面写权限**：`profile_start/stop/mark` 是变更类；只读会话请用启动期
  `MAQUE_PROFILE=1`，或显式 `MAQUE_INSPECT_RW=1`。
- **产物目录**：默认 `.maque/profile/run-<时间戳>`（不入库）；**不要**把 `perf.data` 提交
  （大且不可复现）；只提交 `report.md`/`hotspots.json` 摘要。
