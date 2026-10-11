# 17 · Inspector（游戏检查器）开发计划

> 上游设计：`docs/09`（可观测性与诊断）、`docs/11`（控制平面与 AI 接口）、`docs/21`（接口声明与绑定生成）、
> `docs/04`（VFS）、`docs/10`（确定性与回放）。
> 一句话：**一个独立进程的 ImGui 工具，通过 TCP 复用控制面协议，实时查看/操作运行中的游戏。**

## 1. 目标与非目标

**目标**

- 独立进程（attach/detach 运行中的游戏，不重启、不链接游戏代码）；
- 面板：场景树、节点/组件属性（**schema 驱动**，无需为每个组件写 UI）、文件系统树（VFS 视图）、
  网络状态、日志、截图/实时预览、性能指标、回放时间线、输入模拟器、MCP 控制台；
- **复用数据协议**：JSON-RPC 2.0 控制面 + 反射/模式（`Types.describe` / `maque_schema.json`）+
  MCP 工具面（`Control.mcp`）—— 不发明第二套协议；
- 与 AI 工具链同源：Inspector 能做的，MCP 客户端也能做（同一批方法）。

**非目标（本期）**

- 不做游戏内嵌 UI（ImGui 不进入游戏进程；游戏侧只增加一个 TCP 控制端点）；
- 不做远程公网场景（默认仅 `127.0.0.1`；跨机需显式绑定 + token）；
- 不做资源**编辑后热保存**的完整资产管线（`Scene.save`/`Reload.*` 已有，编辑体验后续迭代）。

## 2. 硬约束（决定架构）

| 约束 | 影响 |
|------|------|
| **不改游戏确定性**（docs/10） | 控制端点**单线程、帧边界轮询**，非阻塞 IO，不用线程；变更类方法（set/spawn/destroy/inject/reload）只影响"当前会话"，且**可记录进输入日志**以便精确回放（M5） |
| **协议单一真相**（docs/21） | 新方法一律进 `schema.yaml` 的 `control_list`；新 ABI 一律进 `module_abi`；生成物入库 |
| **不引入重依赖** | 游戏侧新增依赖仅：POSIX socket（零依赖）；工具侧新增 `third_party/imgui`（MIT，仅 inspector 链接） |
| **窗口/呈现已踩过的坑**（docs/05） | Inspector 用 **SDL2 + SDL_Renderer2（软件）** 后端渲染 ImGui，**不用 OpenGL/EGL**（避免与游戏内 wgpu 的 EGL 争用；两者本就不同进程，但同机 GL 上下文仍会互相干扰） |
| **无显示环境也要能测** | Inspector 提供 `--headless`（无窗口、只跑脚本化请求），CI 可跑；GUI 模式在无 DISPLAY 时跳过 |

## 3. 架构

```
┌──────────────────────── 游戏进程（已有 + 少量新增）────────────────────────┐
│  runtime ── frame loop（单线程，固定步长）                                  │
│     │                                                                      │
│     ├── mq_control_dispatch(请求行) ──► 31 个已有方法（反射/快照/回放/输入模拟）│
│     │        ▲                                                             │
│     └── mq_control_server_poll(帧边界)  ← 新增：TCP 端点（非阻塞，多客户端）  │
│              │  行分隔 JSON-RPC 2.0（与 stdin 路径**同一** dispatch）        │
└──────────────┼─────────────────────────────────────────────────────────────┘
               │ TCP 127.0.0.1:7777（可选 token）
┌──────────────▼──────────── inspector（独立进程，新增 samples/inspector）────┐
│  client（JSON-RPC 客户端：请求 id / 待答表 / 轮询 / 重连）                    │
│  ui（ImGui + SDL2/SDLRenderer2，dockable 布局）                              │
│  panels：SceneTree · Properties(schema) · FileSystem(VFS) · Network · Logs   │
│          · Capture/Preview · Metrics · Replay · InputSim · MCP Console       │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 4. 协议（复用 + 最小扩展）

**复用**（零改动）：JSON-RPC 2.0 行分隔帧、`mq_control_dispatch`、`Types.describe`、
`Scene.entities/get/set/spawn/destroy`、`World.*`、`Replay.*`、`Input.sim_*`、`Perf.get`、
`Reload.*`、`Control.mcp`。

**需要新增的方法**（全部进 `control_list`，实现落在 runtime）：

| 方法 | 用途 | 数据源（现状 → 需要补什么） |
|------|------|------------------------------|
| `Control.capabilities` | 握手：协议版本、已编译方法集、只读/可写、schema 哈希 | 生成表（新增：由 `maque_control_gen.cpp` 输出方法清单） |
| `Control.ping` | 保活/延迟测量 | 帧号 + 时间 |
| `Log.tail {since, level, domain, limit}` | 日志面板（增量拉取） | **缺日志缓冲** → 新增 `mq_obs_log_*`（环形缓冲：域/级别/帧号/文本） |
| `VFS.tree {path, depth}` / `VFS.stat` / `VFS.read {path, max}` | 文件系统树（含 file:// zip/ mem/ builtin 挂载点） | **缺列目录** → `module_abi.vfs` 增 `mq_vfs_list(path, index, out_name, out_kind, out_size)` |
| `Net.status` | 网络面板（连接数、rx/tx、WS 消息、重连、HTTP 队列） | net 模块当前只有 lws 上下文 → 增计数器 + `mq_net_status()` |
| `Asset.list {offset, limit, kind}` | 资产浏览器（复用已有 asset DB 枚举） | 已有 `mq_asset_count/info_at` ✓ 仅需包一层控制方法 |
| `Renderer.capture {view, scale, format}` | 截图 / 实时预览 | 已有 CPU 帧缓冲（`pixels`）✓；编码见下 |
| `Renderer.stats` | 绘制统计（批次数/图元数/纹理数） | gfx 插件增 `stats`（顶点/批/纹理/提交耗时） |
| `Scene.tree` | 场景树（层级 + 组件清单，一次拉取） | 可在客户端用 `Scene.entities` 组装；提供 `Scene.tree` 减少往返（可选） |
| `Types.schema` | 属性编辑器直接用模式（类型/字段/默认值/attrs/枚举值） | 已有 `Types.describe` ✓；补充返回 `attrs`（range/unit/enum values/transient/readonly） |

**截图的载荷策略**（避免大行阻塞）：

1. `Renderer.capture {format:"rgba", scale:0.5}` → `{w,h,b64:"…"}`（base64 内联，行上限 8MB 保护）；
   - 预览默认 `scale=0.5, 10Hz`（只读、不落盘）；
   - 截图默认 `scale=1.0` 单次；
2. 需要更大/更频繁时再引入二进制旁路（`Blob.open/read/close`），**本期不做**；
3. 落盘 PNG：Inspector 侧用 `stb_image_write.h`（public domain，单头）编码并保存；
   游戏侧 `Renderer.capture {path:"…"}` 也可直接用 PPM（已有 `write_ppm` 复用）。

> 注：`third_party/wuffs` 只含 PNG **解码器**（已核实无 encoder），故编码走 stb（仅 inspector 用）。

## 5. 需要新增/改动的落点（接口先行）

| 落点 | 改动 | 依据 |
|------|------|------|
| `schema.yaml` → `module_abi: control` | 新增 `mq_control_server_*`（create/destroy/poll/client_count/stats/close_client） | 生成 `abi/control.h` |
| `schema.yaml` → `module_abi: obs` | 新增 `mq_obs_log_push/at/count/clear`（环形缓冲） | 生成 `abi/obs.h` |
| `schema.yaml` → `module_abi: vfs` | 新增 `mq_vfs_list`（列目录，含 kind/size） | 生成 `abi/vfs.h` |
| `schema.yaml` → `module_abi: net` | 新增 `mq_net_status`（计数器结构） | 生成 `abi/net.h` |
| `schema.yaml` → `interface: renderer` | `record: stats` + `capture` 能力位 | 生成 `maque_ifaces.h` |
| `schema.yaml` → `control_list` | §4 表中 11 个方法 | 生成 `maque_control.h` + 类型化 MCP schema |
| 新模块 `modules/control/transport/` | TCP 端点（POSIX socket，非阻塞，行分隔帧，多客户端配额） | 独立单测 |
| 新 host `samples/inspector/` | ImGui 工具（client + 面板） | e2e |
| `third_party/imgui/`（pin commit） | 仅 tools 链接；`imgui_impl_sdl2` + `imgui_impl_sdlrenderer2` | MIT |
| 文档 | `docs/25-游戏检查器（Inspector）.md`（协议/面板/操作手册） | — |

## 6. 面板与数据源对照

| 面板 | 数据源（协议方法） | 备注 |
|------|-------------------|------|
| **Scene Tree** | `Scene.entities` / `Scene.tree` | 层级、active、组件徽标；选中 → 属性面板 |
| **Properties** | `Types.describe` / `Types.schema` + `Scene.get/set` | **schema 驱动**：数值滑条（range/unit）、枚举下拉（values）、颜色、字符串、只读/transient 灰显；`dry_run` 预览；撤销=写回旧值 |
| **File System** | `VFS.tree` / `VFS.stat` / `VFS.read` | 显示挂载点（file://、zip、mem、builtin）+ 哈希（`mq_vfs_hash_path`）→ 资源变更定位 |
| **Network** | `Net.status` | 连接/字节/消息/重连；后续可加 WS 报文采样 |
| **Logs** | `Log.tail` | 域/级别过滤、帧号跳转、暂停跟随；点击错误行 → 崩溃上下文/检查点 |
| **Capture / Preview** | `Renderer.capture` | 实时预览（缩略）+ 一键截图落盘 PNG + 与 `MAQUE_CAPTURE` 对齐 |
| **Metrics** | `Perf.get` + `obs` gauges | 帧耗时/实体数/批次数曲线（客户端滑动窗口） |
| **Replay** | `World.trail/checksum/snapshot/restore`、`Replay.checkpoints/seek/log` | 时间线、检查点跳转、失步二分（docs/10 §5） |
| **Input Sim** | `Input.sim_load/sim_status/sim_events/inject` | 直接在 UI 里录/放点击序列（与 `tests/data/sim/*.sim.json` 同一格式） |
| **MCP Console** | `Control.mcp` | 任意方法调用 + 结果 JSON 查看（AI 与人类同一入口） |

## 7. 里程碑（每个里程碑都可验收）

**M0 · 传输层（游戏侧可被连接）**
- 交付：`modules/control/transport`（TCP 非阻塞端点）+ `MAQUE_CONTROL=tcp:[host:]port`、
  `MAQUE_CONTROL_TOKEN`（可选）、`Control.ping/capabilities`。
- 验收：`printf '{"jsonrpc":"2.0","id":1,"method":"Control.ping"}\n' | nc 127.0.0.1 7777` 有响应；
  同一请求经 stdin 与 TCP 得到**逐字节相同**的响应；断线/半包/超长行不崩。
- 测试：`modules/control/tests/transport_test.cpp`（自连接、半包、多客户端、行上限）、
  `tests/control_tcp_e2e.sh`（起游戏 → 连 TCP → 跑 5 个只读方法 → 断线重连）。

**M1 · 检查器所需的方法与 ABI**
- 交付：§4 表中方法 + `obs` 日志环形缓冲 + `vfs` 列目录 + `net` 状态 + `Renderer.capture/stats` + `Asset.list`。
- 验收：`--headless` 客户端脚本能拉到：场景实体树、单节点属性、VFS 树、日志尾部 100 行、
  一帧截图（非空像素）、一次 perf 采样；**只读方法不改变 `World.checksum`**（测试断言）。
- 测试：`tests/inspector_headless_e2e.sh`（断言 JSON 字段与像素）、各模块单测（log ring/vfs list/net status/renderer stats）。

**M2 · Inspector 外壳（能连、能看）**
- 交付：`samples/inspector`（SDL2 窗口 + ImGui dockable + 连接对话框 + 请求轮询/重连 + 状态栏显示
  帧号/checksum/后端/延迟）；`third_party/imgui` vendored。
- 验收：attach 运行中的游戏，状态栏帧号持续增长；`Ctrl+C` 关掉游戏 → 工具显示"已断开"并可重连。
- 测试：`--headless` 冒烟（无 DISPLAY 时 GUI 用例 SKIP，沿用现有 `SKIP_RETURN_CODE` 约定）。

**M3 · 面板全量**
- 交付：§6 十个面板（先 SceneTree/Properties/Logs/Capture 四个高价值，再 FileSystem/Network/Metrics/Replay/InputSim/MCP Console）。
- 验收：改一个属性（如 `scn.Sprite.radius`）→ 画面立即变化且 `Scene.get` 回读一致；
  编辑器对**每个** `class_list` 类型都能生成可编辑控件（遍历 schema 的自动化用例：随机 50 个类型不得崩溃）。
- 测试：schema-驱动 UI 的**生成式**用例（headless：对每类型做 get→set→get 等价）。

**M4 · 与 AI 同源**
- 交付：Inspector 每一项操作都能由 MCP 调用等价完成；`docs/25` 记录方法↔面板映射；
  `--headless --script ops.json` 支持脚本化会话（AI/CI 用）。
- 验收：同一 `ops.json` 分别经 Inspector 与 MCP 客户端执行 → 游戏侧 `World.checksum` 相同。

**M5 · 加固与确定性守卫**
- 交付：多客户端配额与背压（每帧字节上限/慢客户端丢弃策略）、只读模式（`--read-only` + 游戏侧
  `MAQUE_CONTROL_ALLOW_MUTATE`，绑定非本地时默认拒绝变更）、**控制操作记账**（变更类方法写入
  输入日志的可选段 → attach 会话可精确回放）、UI 上的"偏离基线"提示（`World.trail` 与记录对比）。
- 验收：只读客户端无法变更世界（`Scene.set` 返回结构化拒绝）；记录→回放 `world_trail` 一致。
- 测试：`tests/control_record_replay_e2e.sh`。

## 8. 风险与对策

| 风险 | 对策 |
|------|------|
| 变更类操作破坏回放确定性（docs/10） | 帧边界应用 + 控制操作记账（M5）+ UI 明示偏离；只读模式默认在非本地绑定 |
| 截图/预览造成卡顿 | 仅按需捕获、支持降采样与限频；抓帧复用已有 CPU 帧缓冲（零额外渲染） |
| ImGui 引入图形依赖，与环境 GL/EGL 冲突 | 用 `imgui_impl_sdlrenderer2`（软件），与游戏 wgpu 完全隔离；工具崩溃不影响游戏 |
| 协议漂移（新方法与 MCP/schema 不一致） | 单一真相 `schema.yaml`；`codegen_drift` 测试；MCP `inputSchema` 与 `control_list` 同源生成 |
| 大载荷（截图）阻塞控制面 | 行上限保护 + 降采样 + 后续二进制旁路；服务端每帧字节预算 |
| 多客户端竞态 | 单线程顺序处理 + 每客户端独立缓冲与配额；不做跨客户端事务 |

## 9. 工作量估算（相对粒度）

| 里程碑 | 规模 | 依赖 |
|--------|------|------|
| M0 传输 | 小（~400 行 + 测试） | control 模块 |
| M1 方法/ABI | 中（~900 行：obs log / vfs list / net status / capture / 11 方法） | obs、vfs、net、gfx 插件 |
| M2 外壳 | 中（~600 行 + vendored imgui） | SDL2、imgui |
| M3 面板 | 大（~1500 行，10 面板） | M1/M2 |
| M4 AI 同源 | 小（脚本化 + 文档） | M1/M3 |
| M5 加固 | 中（~500 行） | M0/M3 |

## 10. 待确认（影响接口冻结）

1. **传输**：只做 TCP，还是同时提供 Unix domain socket（本机更低延迟、无需端口管理）？
2. **默认权限**：默认**只读**（需显式开启编辑）还是默认可编辑（仅本地绑定）？
3. **预览频率**：默认 10Hz/0.5 缩放是否够用（决定是否需要二进制旁路）？
4. **ImGui 分支**：用 `docking` 分支（可停靠布局）还是 `master`（固定布局，依赖更稳定）？
5. **是否进 CI**：GUI 冒烟用例是否需要（无 DISPLAY 环境自动 SKIP）？

---

**建议的启动顺序**：M0 → M1（只做 `Control.ping/capabilities` + `Log.tail` + `VFS.tree` + `Renderer.capture` 四个方法先跑通闭环）
→ M2 外壳看到真实数据 → 再横向铺 M3 面板。
