# maque 模块开发计划

> 上游：`../docs/`（设计文档系列，根为 `docs/00-设计哲学与约束.md`）。
> 本目录把设计稿转成**可执行、可验收**的模块开发计划：**每个模块一个独立 owner agent，独立单测**。

## 1. 开发范式（强制）

**接口 + 测试先行，后开发**（TDD）：

```
① 接口层（完备）  → ② 测试例（红）  → ③ 实现（绿）  → ④ 重构  → ⑤ 验收
```

铁律：

1. **接口层必须完备**：公共头（C ABI）+ 类型 + 错误模型 + 版本化，**先冻结**再写实现。
   接口不完备不得进入实现阶段。
2. **测试先行**：测试**从接口**推导，先写（应失败），再实现到通过。
3. **独立单测**：每个模块的单测**只依赖本模块 + 其公共接口**；
   对上游依赖使用 **fake / stub / mock**，**不得**依赖其它模块的完整实现。
4. **游戏场景驱动**：测试用例以**真实游戏场景**为来源（如"玩家移动 / 射击 / 关卡加载"），
   既有细粒度单测，也有面向场景的集成用例。
5. **确定性可测**：凡涉及模拟的模块，测试须覆盖**确定性/回放一致性**（docs/10）。

## 2. 每个模块计划的必备章节

见 [`_template.md`](_template.md)：

1. 模块概述（职责 / 所在层 / 上游文档）
2. **接口层设计（完备）**：C ABI 头、类型、函数、错误模型、版本化
3. 依赖与边界（允许依赖谁 / 禁止做什么）
4. **测试计划**：从接口推导的用例清单（含边界 / 错误 / 确定性）
5. 开发计划：分阶段的 TDD 步骤（红→绿→重构）
6. 验收标准（DoD）：覆盖率、确定性、性能/内存预算
7. 风险
8. Owner agent（角色标识）

## 3. 模块与 Owner Agent 分派

| # | 模块 | 计划文件 | Owner agent | 主参考文档 |
|---|------|----------|-------------|-----------|
| 01 | math | [01-math.md](01-math.md) | `agent-math` | docs/06, 07 |
| 02 | core（data / 句柄 / 反射） | [02-core-reflection.md](02-core-reflection.md) | `agent-core` | docs/06, 07 |
| 03 | module host（加载 / ABI / registry） | [03-module-host.md](03-module-host.md) | `agent-host` | docs/02, 03 |
| 04 | platform（抽象 / WASM） | [04-platform.md](04-platform.md) | `agent-platform` | docs/05 |
| 05 | executor（任务 / reactor） | [05-executor.md](05-executor.md) | `agent-executor` | docs/05, 10 |
| 06 | VFS（多协议 / 挂载） | [06-vfs.md](06-vfs.md) | `agent-vfs` | docs/04 |
| 07 | persistence（codec × backend） | [07-persistence.md](07-persistence.md) | `agent-persistence` | docs/08, 06 |
| 08 | asset pipeline（ai：导入/烘焙/打包/dump） | [08-asset-ai.md](08-asset-ai.md) | `agent-asset` | docs/08 |
| 09 | observability（日志 / 指标 / 崩溃） | [09-observability.md](09-observability.md) | `agent-obs` | docs/09 |
| 10 | control plane（CDP 式 / MCP） | [10-control-plane.md](10-control-plane.md) | `agent-control` | docs/11, 16 |
| 11 | scene（节点树 / ECS 双视图） | [11-scene.md](11-scene.md) | `agent-scene` | docs/12, 06 |
| 12 | animation | [12-animation.md](12-animation.md) | `agent-animation` | docs/15 |
| 13 | physics（Rapier FFI） | [13-physics.md](13-physics.md) | `agent-physics` | docs/15 |
| 14 | particle | [14-particle.md](14-particle.md) | `agent-particle` | docs/15 |
| 15 | renderer（WebGPU / pass 层） | [15-renderer.md](15-renderer.md) | `agent-renderer` | docs/13 |
| 16 | script（quickjs 为主） | [16-script.md](16-script.md) | `agent-script` | docs/14 |

## 4. 交付顺序（依赖驱动）

```
阶段 A（地基）：math → core → module host → platform → executor
阶段 B（IO）：  VFS → persistence → asset pipeline
阶段 C（AI 力）：observability → control plane
阶段 D（世界）：scene → animation → physics → particle
阶段 E（呈现）：renderer → script
```

## 5. 状态

> 计划文档 16 篇已全部生成。状态按**当前代码与 CTest 实际覆盖**更新（`ctest --test-dir build`）。

| # | 模块 | 计划 | 接口冻结 | 单测通过 | 实现完成 | 对应目标/测试 |
|---|------|------|----------|----------|----------|----------------|
| 01 | math | ✅ | ✅ | ✅ | ✅ | `math`（Vec/Quat/Mat4/Transform + 纯函数） |
| 02 | core（data/句柄/反射） | ✅ | ✅ | ✅ | ✅ | `reflect` `handle` `snapshot` + codegen 235 类型 |
| 03 | module host | ✅ | ✅ | ✅ | ✅ | `module`（dlopen/registry/版本校验）+ gfx 为真 `.so` |
| 04 | platform | ✅ | ✅ | ✅ | ✅ | `platform`（SDL2 归一化指针/键盘/文本 + 注入） |
| 05 | executor（任务/reactor） | ✅ | ⬜ | ⬜ | ⬜ | 未开始（帧内无异步约束下优先级低） |
| 06 | VFS | ✅ | ✅ | ✅ | ✅ | `vfs`（file/mem/zip/builtin/URL + 内容哈希） |
| 07 | persistence | ✅ | ✅ | ✅ | ✅ | `persist`（JSON + 二进制 codec）+ 快照池 |
| 08 | asset pipeline（ai） | ✅ | ✅ | ✅ | ✅ | `asset`（导入/依赖/拓扑/烘焙/打包/dump） |
| 09 | observability | ✅ | ✅ | ✅ | ✅ | `obs`（结构化事件 + 指标 + dump_json） |
| 10 | control plane | ✅ | ✅ | ✅ | ✅ | `control`（JSON-RPC + MCP 包装 + 方法枚举） |
| 11 | scene | ✅ | ✅ | ✅ | ✅ | runtime 实体/组件 + UI 命中 + 世界快照 |
| 12 | animation | ✅ | ✅ | ⬜ | ⬜ | 类型已入 schema；求值器未实现 |
| 13 | physics | ✅ | ✅ | 🟡 | 🟡 | 自研确定性 2D 求解器（Rapier 需 Rust 工具链，见 ADR-0028 降级） |
| 14 | particle | ✅ | ✅ | ⬜ | ⬜ | 类型已入 schema；发射器未实现 |
| 15 | renderer（WebGPU/pass 层） | ✅ | ✅ | ✅ | 🟡 | 相机/分层/批处理/alpha 合成已落地；pass 图待做 |
| 16 | script（quickjs） | ✅ | ✅ | ✅ | 🟡 | 绑定生成 + 事件回调 + 热重载；capability 沙箱待做 |

**声明即真相（docs/21）**：公开接口全部在 `modules/core/data/schema/` 声明一次 → 生成
类型头/描述符/JSON（`class_list`）、引擎 API 声明 + JS 绑定（`api_list`）、插件 C vtable（`interface`）、
控制面方法 + MCP schema（`control_list`）、脚本事件回调路由（`callbacks`）、模块 C ABI 头（`module_abi`）。
**禁止手写公开接口**；生成的 ABI 由静态断言守卫，漂移即编译失败。

**附加模块（超出 16 篇计划，随里程碑新增）**：`input`（确定性输入流，docs/19）、
`watcher`（帧边界重载，docs/08/14）、`world/physics`（确定性 2D 物理）、
`world/nav`（确定性 2D 网格 A* + 平滑 + 追踪）、`asset`（资产管线）、
`image`（Wuffs 解码）、`net`（libwebsockets）、`api`（DSL 生成的宿主 API/绑定）、
`runtime`（引擎"浏览器" + 嵌入宿主 `mq_host_*`）。

**e2e 回归**（`samples/player`）：`player_run`、`determinism`、`control_plane`、`ui_e2e`、
`hot_reload_e2e`、`anim_e2e`、`editor_e2e`、`prefab_e2e`、`crash_context_e2e`、`nav_e2e`、
`host_tick`、`host_multi_instance`。
| [17](17-inspector.md) | 游戏检查器（ImGui + TCP 控制面，独立进程） |
