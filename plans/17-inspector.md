# 17 · Inspector（游戏检查器）开发计划

> 上游设计：`docs/09`（可观测性与诊断）、`docs/11`（控制平面与 AI 接口）、`docs/21`（接口声明与绑定生成）、
> `docs/04`（VFS）、`docs/10`（确定性与回放）。
> 一句话：**一个独立进程的 ImGui 工具，通过 TCP/UDS 复用控制面协议，实时查看/操作运行中的游戏。**

**已确认决策**（2026-10-11）：

| # | 决策 | 影响 |
|---|------|------|
| D1 | 传输 = **TCP（本机）+ Unix domain socket** | 同一 socket 抽象两分支；UDS 默认路径 `/tmp/maque-<pid>.sock` |
| D2 | **默认只读**，显式开启编辑 | 权限成为**协议与 DSL 的一等公民**（§5），非本地绑定强制只读 |
| D3 | ImGui 用 **master 分支**（无 docking） | 布局自绘（分栏 + 分隔条 + 标签页），见 §6.1；依赖更稳 |
| D4 | 先调整计划、评审后再动手 | 本文即评审稿；接口冻结前不进入实现 |

---

## 0. 进度

| 里程碑 | 状态 | 说明 |
|--------|------|------|
| M0 端点 + 最小面板 | ✅ 已完成（2026-10-11） | 游戏侧 TCP/UDS 端点（`MAQUE_INSPECT`，帧边界轮询、单线程、只读默认 + `-32001` 拒绝与计数）；`Control.info`；inspector 进程（ImGui master + SDL2/SDLRenderer2，中文界面）：场景树 / 属性（`Scene.get(id,"")` 反射 dump）/ 控制台 / 模式；`--headless` 供 CI；`tests/inspector_e2e.sh`（TCP+UDS、只读拒绝、反射查询、attach/detach 无侵入） |
| M1 属性编辑器 | ⏳ | 需要 `Scene.set` 的权限开关与 UI 控件（schema 驱动：Types.describe → 输入控件） |
| M2 预览/截图/日志 | ⏳ | 截图经 `Capture.*`（待加方法）；日志流式订阅 |
| M3 回放时间线 / 输入模拟器面板 | ⏳ | 复用 Replay.* / Input.sim_* |

---

## 1. 目标与非目标

**目标**

- 独立进程（attach/detach 运行中的游戏，不重启、不链接游戏代码）；
- 面板：场景树、节点/组件属性（**schema 驱动**，无需为每个组件写 UI）、文件系统树（VFS 视图）、
  网络状态、日志、截图/实时预览、性能指标、回放时间线、输入模拟器、MCP 控制台；
- **复用数据协议**：JSON-RPC 2.0 控制面 + 反射/模式（`Types.describe` / `maque_schema.json`）+
  MCP 工具面（`Control.mcp`）—— 不发明第二套协议；
- 与 AI 工具链同源：Inspector 能做的，MCP 客户端也能做（同一批方法、同一权限模型）。

**非目标（本期）**

- 不做游戏内嵌 UI（ImGui 不进游戏进程；游戏侧只增加一个控制端点）；
- 不做公网远程（默认 loopback / UDS；非本地绑定强制只读且需显式开启）；
- 不做完整资产编辑管线（`Scene.save` / `Reload.*` 已有，编辑体验后续迭代）。

## 2. 硬约束（决定架构）

| 约束 | 影响 |
|------|------|
| **不改游戏确定性**（docs/10） | 端点**单线程、帧边界轮询**，非阻塞 IO，无线程；变更类操作在 M5 记账进输入日志 |
| **协议单一真相**（docs/21） | 新方法一律进 `schema/` 的 `control_list`；新 ABI 一律进 `module_abi`；生成物入库 |
| **权限必须可声明**（D2） | `control_list` 项新增 `mutating: true` 标记 → 生成 `mq_control_is_mutating()` 与 MCP 元数据；服务端按会话权限拒绝 |
| **不引入重依赖** | 游戏侧：POSIX socket（零依赖）；工具侧：`third_party/imgui`（MIT，仅 inspector 链接） |
| **不碰 GL/EGL**（docs/05 教训） | Inspector 用 **SDL2 + SDL_Renderer2（软件）** 渲染 ImGui，不用 OpenGL/EGL |
| **无显示环境可测** | Inspector 提供 `--headless`（无窗口，跑脚本化请求）；GUI 用例无 DISPLAY 时 SKIP |

## 3. 架构

```
┌──────────────────────── 游戏进程（已有 + 少量新增）────────────────────────┐
│  runtime ── frame loop（单线程，固定步长）                                  │
│     ├── mq_control_dispatch(请求行)  ← 31 个已有方法（反射/快照/回放/输入模拟）│
│     ├── mq_control_is_mutating(方法) ← 新增：由 DSL 生成（权限判定单一真相）  │
│     └── mq_control_server_poll(帧边界) ← 新增：TCP/UDS 端点（非阻塞、多客户端）│
│              │  行分隔 JSON-RPC 2.0（与 stdin 路径**同一个** dispatch）      │
└──────────────┼──────────────────────────────────────────────────────────────┘
      TCP 127.0.0.1:7777  或  UDS /tmp/maque-<pid>.sock（可选 token）
┌──────────────▼──────────── inspector（独立进程，samples/inspector）────────┐
│  client（JSON-RPC：请求 id / 待答表 / 轮询 / 重连 / 权限状态）                │
│  ui（ImGui master + SDL2/SDLRenderer2；自绘分栏与分隔条，见 §6.1）            │
│  panels：SceneTree · Properties · FileSystem · Network · Logs · Capture      │
│          · Metrics · Replay · InputSim · MCP Console                        │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 4. 协议（复用 + 最小扩展）

**复用**（零改动）：JSON-RPC 2.0 行分隔帧、`mq_control_dispatch`、`Types.describe`、
`Scene.entities/get/set/spawn/destroy`、`World.*`、`Replay.*`、`Input.sim_*`、`Perf.get`、
`Reload.*`、`Control.mcp`。

**新增方法**（全部进 `control_list`；`†` = 变更类，需编辑权限）：

| 方法 | 用途 | 数据源现状 → 需补什么 |
|------|------|----------------------|
| `Control.ping` | 保活 / 延迟测量 | 帧号 + 时间 |
| `Control.capabilities` | 握手：协议版本、ABI 版本、**方法清单 + 是否变更类**、当前会话权限、schema 哈希、传输类型 | 新增（由 codegen 的方法表 + `mq_control_is_mutating` 生成） |
| `Log.tail {since, level, domain, limit}` | 日志面板（增量拉取） | **缺日志缓冲** → `mq_obs_log_*` 环形缓冲（域/级别/帧号/文本） |
| `VFS.tree {path, depth}` / `VFS.stat` / `VFS.read {path, max}` | 文件系统树（file:// / zip / mem / builtin 挂载点） | **缺列目录** → `mq_vfs_list(path, index, name, kind, size)` |
| `Net.status` | 网络面板 | net 模块只有 lws 上下文 → 计数器 + `mq_net_status()` |
| `Asset.list {offset, limit, kind}` | 资产浏览器 | 已有 `mq_asset_count/info_at` ✓ 仅需包一层 |
| `Renderer.capture {view, scale, format}` | 截图 / 实时预览 | 已有 CPU 帧缓冲 ✓（编码见下） |
| `Renderer.stats` | 批次数/图元数/纹理数/提交耗时 | gfx 插件增 `stats` |
| `Types.schema` | 属性编辑器直接用模式（含 `attrs`：range/unit/enum values/readonly/transient） | 扩 `Types.describe` 的返回 |
| `Scene.tree` | 一次拉取层级 + 组件清单（减少往返） | 由实体表组装 |
| `† Scene.set` / `† Scene.spawn` / `† Scene.destroy` / `† Input.inject` / `† Input.sim_*` / `† Reload.*` / `† Runtime.step` / `† World.restore` / `† Replay.seek` | 已有方法，**补 `mutating` 标记** | DSL 标记 + 服务端权限校验 |

**截图载荷策略**（避免大行阻塞控制面）：

1. `Renderer.capture {format:"rgba", scale:0.5}` → `{w,h,b64}`；预览默认 `scale=0.5 @10Hz`（可调），
   截图默认 `scale=1.0` 单次；行上限 8MB 保护（超限返回结构化错误）；
2. 更大/更频繁时再引入二进制旁路 `Blob.open/read/close`（**本期不做**）；
3. 落盘 PNG：Inspector 侧用 `stb_image_write.h`（public domain 单头）；游戏侧保留 PPM（已有 `write_ppm`）。
   依据：`third_party/wuffs` 仅含 PNG **解码器**（已核实无 encoder）。

## 5. 权限模型（D2 落地）

**单一真相**：`control_list` 每项声明 `mutating: true/false`（缺省 false）→ codegen 产出
`mq_control_is_mutating(const char* method)`、MCP 工具元数据、`Control.capabilities` 的方法表。

**启动参数**（游戏侧）：

| 环境变量 | 语义 | 默认 |
|----------|------|------|
| `MAQUE_CONTROL` | `-`（stdin）· `tcp:[host:]port` · `unix:/path.sock` | 不开启 |
| `MAQUE_CONTROL_ALLOW_MUTATE` | `1` 允许变更类方法 | **0（只读）** |
| `MAQUE_CONTROL_TOKEN` | 可选：连接首帧需携带的 token（`Control.hello {token}`） | 空 = 不校验（仅 UDS / loopback 下允许） |

**强制规则**（不依赖客户端自觉）：

1. 绑定地址非 loopback（TCP）→ **强制只读**，即使 `ALLOW_MUTATE=1`（并在启动日志明确提示）；
2. 只读会话下调用变更类方法 → 结构化错误
   `{"jsonrpc":"2.0","id":N,"error":{"code":-32001,"message":"read-only session","data":{"method":"Scene.set","hint":"MAQUE_CONTROL_ALLOW_MUTATE=1"}}}`；
3. `Control.capabilities` 明确回报当前会话权限 → Inspector 在只读时**灰显**编辑控件并显示锁标识；
4. 变更类方法在 M5 记账进输入日志（attach 会话仍可精确回放，docs/10 D3）。

**测试**（M0/M1 就要有）：只读会话逐方法断言"变更类被拒且 **`World.checksum` 不变**"；
可写会话断言变更生效；非 loopback 绑定的强制只读（用 `127.0.0.2` 绑定模拟）。

## 6. Inspector（工具侧）

### 6.1 布局（ImGui **master**，无 docking → 自绘）

```
┌ 菜单/工具条：连接 · 权限锁 · 只读/编辑切换 · 帧号 · checksum · 延迟 · 暂停跟随 ┐
├───────────────┬───────────────────────────────────────────────────────────────┤
│ 左栏(可折叠)  │  中栏：主视图（标签页）                                        │
│  · Scene Tree │   Properties │ Logs │ FileSystem │ Network │ Metrics │ Replay   │
│  · Assets     │                                                               │
│               ├───────────────────────────────────────────────────────────────┤
│               │  下栏：Capture 预览 / MCP Console / Input Sim（标签页）        │
└───────────────┴───────────────────────────────────────────────────────────────┘
```
- 自绘分栏：一个约 50 行的 splitter 辅助（拖动改变比例、比例持久化到 `imgui.ini` 同目录的配置文件）；
- 面板以标签页组织，避免多窗口管理复杂度（master 无 docking）；
- 所有面板共用同一个请求泵（每帧至多 N 个在途请求；按面板可见性决定轮询频率）。

### 6.2 面板 ↔ 数据源

| 面板 | 数据源 | 关键交互 |
|------|--------|----------|
| Scene Tree | `Scene.entities` / `Scene.tree` | 层级、active、组件徽标；选中 → 属性面板；右键：销毁（需权限） |
| Properties | `Types.schema` + `Scene.get/set` | **schema 驱动**：数值滑条（range/unit）、枚举下拉（values）、颜色、字符串、只读/transient 灰显；`dry_run` 预览；撤销 = 写回旧值；只读时灰显 |
| File System | `VFS.tree/stat/read` | 挂载点视图 + 路径哈希（`mq_vfs_hash_path`）→ 资源变更定位 |
| Network | `Net.status` | 连接数 / rx / tx / WS 消息 / 重连次数 |
| Logs | `Log.tail` | 域/级别过滤、帧号跳转、暂停跟随；错误行 → 崩溃上下文/检查点 |
| Capture | `Renderer.capture` | 预览（缩略、可调频率）+ 一键 PNG 落盘 |
| Metrics | `Perf.get` + obs gauges | 帧耗时/实体/批次曲线（客户端滑动窗口） |
| Replay | `World.*` / `Replay.*` | 时间线、检查点跳转、失步二分（docs/10 §5） |
| Input Sim | `Input.sim_*` / `Input.inject` | UI 内录制/回放点击序列（与 `tests/data/sim/*.sim.json` 同格式） |
| MCP Console | `Control.mcp` | 任意方法调用 + 结果 JSON（AI 与人类同一入口） |

## 7. 需要新增/改动的落点（接口先行）

| 落点 | 改动 |
|------|------|
| `schema/` → `control_list` | §4 表 11 个方法 + 变更类 `mutating` 标记（含已有变更方法） |
| `schema/` → `module_abi: control` | `mq_control_server_create/destroy/poll/client_count/stats/close_client`、`mq_control_is_mutating` |
| `schema/` → `module_abi: obs` | `mq_obs_log_push/at/count/clear`（环形缓冲） |
| `schema/` → `module_abi: vfs` | `mq_vfs_list`（列目录：name/kind/size） |
| `schema/` → `module_abi: net` | `mq_net_status`（计数器结构体） |
| `schema/` → `interface: renderer` | `stats` 记录 + `capture` 能力位 |
| 新模块 `modules/control/transport/` | TCP + UDS 端点（非阻塞、行分隔帧、多客户端配额） |
| 新 host `samples/inspector/` | ImGui 工具（client + 面板 + `--headless`） |
| `third_party/imgui`（pin commit，**master**） | `imgui_impl_sdl2` + `imgui_impl_sdlrenderer2` |
| `third_party/stb/stb_image_write.h`（pin，public domain） | 仅工具侧 PNG 编码 |
| 文档 | `docs/25-游戏检查器（Inspector）.md`（协议/权限/面板/操作手册） |

## 8. 里程碑（每项含验收与测试）

**M0 · 传输与权限骨架**
- 交付：`modules/control/transport`（TCP + UDS）、`Control.ping/capabilities`、
  `control_list` 的 `mutating` 标记 + `mq_control_is_mutating`、只读/可写会话与强制只读规则（§5）。
- 验收：同一请求经 **stdin / TCP / UDS 三路响应逐字节相同**；只读会话变更类方法被拒且
  `World.checksum` 不变；半包/超长行/客户端骤断不崩；多客户端并发只读查询互不影响。
- 测试：`modules/control/tests/transport_test.cpp`（自连接/半包/配额/两种传输）、
  `tests/control_tcp_e2e.sh`（三路一致性 + 权限矩阵 + 重连）。

**M1 · 检查器所需方法与 ABI**
- 交付：§4 方法与 ABI 扩展（obs log / vfs list / net status / renderer capture+stats / asset list / types schema）。
- 验收：`--headless` 脚本能拉到：实体树、单节点属性、VFS 树、日志尾部、一帧截图（非空像素）、perf 采样；
  **以上全部只读方法执行后 `World.checksum` 不变**。
- 测试：`tests/inspector_headless_e2e.sh` + 各模块单测（log ring / vfs list / net status / renderer stats）。

**M2 · Inspector 外壳**
- 交付：`samples/inspector`（SDL2 + SDLRenderer2 + ImGui master + 自绘分栏 + 连接对话框 + 重连 +
  状态栏：帧号/checksum/权限/延迟）；vendored imgui。
- 验收：attach 运行中的游戏，状态栏帧号持续增长；关掉游戏 → 显示"已断开"并可重连；
  只读会话下所有编辑控件灰显。
- 测试：`--headless` 冒烟 + GUI 用例（无 DISPLAY → SKIP，沿用 `SKIP_RETURN_CODE` 约定）。

**M3 · 面板全量**
- 交付：§6.2 十个面板（顺序：SceneTree → Properties → Logs → Capture → FileSystem → Network →
  Metrics → Replay → InputSim → MCP Console）。
- 验收：改一个属性（如 `scn.Sprite.radius`）→ 画面立即变化且 `Scene.get` 回读一致；
  **生成式用例**：遍历 `class_list` 全部类型做 get→set→get 等价，不得崩溃。
- 测试：schema 驱动 UI 的 headless 生成式用例。

**M4 · 与 AI 同源**
- 交付：`--headless --script ops.json`；`docs/25` 的方法↔面板映射表。
- 验收：同一 `ops.json` 经 Inspector 与 MCP 客户端执行 → `World.checksum` 相同。

**M5 · 加固与确定性守卫**
- 交付：多客户端配额与背压（每帧字节预算、慢客户端丢弃策略）、**控制操作记账**（变更类方法写入
  输入日志的可选段）、UI"偏离基线"提示（当前 `World.trail` vs 记录基线）、UDS 权限（socket 文件 0600）。
- 验收：记录→回放 `world_trail` 一致（含控制操作）；慢客户端不拖慢游戏帧（帧耗时统计对比）。
- 测试：`tests/control_record_replay_e2e.sh`。

## 9. 风险与对策

| 风险 | 对策 |
|------|------|
| 变更操作破坏回放确定性 | 帧边界应用 + 默认只读 + M5 记账 + UI 偏离提示 |
| 权限绕过 | 服务端**逐方法**判定（`mq_control_is_mutating` 生成表），非 loopback 强制只读，不信任客户端 |
| 截图/预览卡顿 | 按需捕获 + 降采样 + 限频 + 每帧字节预算；抓帧复用已有 CPU 缓冲 |
| ImGui（master，无 docking）布局受限 | 自绘 splitter + 标签页（§6.1）；不引入多窗口管理 |
| 工具与游戏的图形栈冲突 | 工具用 SDL_Renderer2（软件）；与游戏 wgpu 完全隔离，工具崩溃不影响游戏 |
| 协议漂移 | 单一真相 `schema/` + `codegen_drift` 测试 + MCP schema 同源生成 |
| 大载荷阻塞 | 行上限 + 结构化错误 + （后续）二进制旁路 |

## 10. 工作量估算

| 里程碑 | 规模 | 依赖 |
|--------|------|------|
| M0 传输与权限 | 中（~600 行 + 测试） | control 模块、codegen（mutating 标记） |
| M1 方法与 ABI | 中（~900 行） | obs、vfs、net、gfx 插件 |
| M2 外壳 | 中（~700 行 + vendored imgui/stb） | SDL2、imgui master |
| M3 面板 | 大（~1500 行） | M1/M2 |
| M4 AI 同源 | 小（~200 行 + 文档） | M1/M3 |
| M5 加固 | 中（~500 行） | M0/M3 |

## 11. 仍待确认（小项，不阻塞接口冻结）

1. **预览默认参数**：`scale=0.5 @10Hz` 是否合适（决定后续要不要二进制旁路）？
2. **GUI 冒烟**：是否需要在有 DISPLAY 的机器上把 ImGui 用例也纳入 CI（无 DISPLAY 自动 SKIP）？
3. **UDS 路径**：`/tmp/maque-<pid>.sock` 是否可接受（还是放 `$XDG_RUNTIME_DIR`）？

---

**建议启动顺序**：M0（传输 + 权限）→ M1 先做 `Log.tail` / `VFS.tree` / `Renderer.capture` / `Control.capabilities`
四个方法跑通闭环 → M2 外壳看到真实数据 → 再横向铺 M3 面板。
