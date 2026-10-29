# maque 里程碑（纵向切片）

> 总览与执行顺序见 **[00-路线图](00-路线图.md)**。每个里程碑都是一条"数据→运行→退出"的真实链路。

| 里程碑 | 主题 | 状态 | 文档 |
|--------|------|------|------|
| M0 | 地基（反射/句柄/模块/数学） | ✅ | — |
| M1 | 首个纵向切片：`.scn`→反序列化→WebGPU→退出 | ✅ | [01-下落弹跳球](01-下落弹跳球.md) |
| M2 | 脚本（quickjs/JS 驱动） | ✅ | [02-脚本驱动](02-脚本驱动.md) |
| M3 | 实体与组件（世界模型，渲染从组件提取） | ✅ | [03-实体与组件](03-实体与组件.md) |
| M4 | VFS 与资源加载 | ✅ file/mem/zip/builtin/URL + 内容哈希 + 图像解码 | [04-VFS与资源](04-VFS与资源.md) |
| M5 | 输入/UI（SDL2 窗口/事件 + 基础 UI） | ✅ 归一化指针/键盘/文本 → 确定性事件流 → 捕获/目标/冒泡 → Button/Label | — |
| M6 | 物理（确定性刚体/碰撞） | 🟡 自研确定性 2D 求解器（ADR-0028 降级路径；Rapier 需 Rust 工具链） | — |
| M7 | 渲染进阶（相机/分层/2D 批处理） | ✅ 相机中心/缩放/旋转、图元自转、layer/z 稳定排序、直通 alpha 合成、按管线×贴图分批 | — |
| M8 | 可观测/控制面（AI 加载·调试·预览·采集） | ✅ 结构化事件/指标 + JSON-RPC + MCP 包装 + 快照/回放/校验和 | — |
| M9 | 热重载/编辑器最小化 | 🟡 帧边界热重载（脚本/场景）+ dry_run + 控制面编辑面；编辑器壳待做 | — |
| P1 | AI 五指标收尾（输入即数据/快照/回滚/自省） | ✅ 录制回放逐帧一致、世界快照往返、Types.describe/Scene.entities | — |
| P3 | WASM/浏览器目标 | 🟡 P0 已接线（Emscripten 构建 + MEMFS + 静态 null renderer + checksum smoke）；P1 WebGPU-only 直呈待实现 | [plan-wasm](../../docs/plan-wasm.md) |

**原则**：脚本（quickjs）是一等公民（M2 起）；确定性贯穿；每个切片都可由脚本驱动。

## 验证入口（均可脚本化 / AI 驱动）

| 能力 | 入口 |
|------|------|
| 结构化指标 | 控制面 `Perf.get`（frames / entities / draws） |
| 世界校验和与逐帧轨迹 | `World.checksum` / `World.trail`（失步定位） |
| 快照 / 回滚 | `World.snapshot` / `World.restore`（`MAQUE_SNAPSHOT` 落盘） |
| 输入注入 | `Input.inject`（AI/CI 无需真机设备） |
| 录制 / 回放 | `MAQUE_RECORD` / `MAQUE_REPLAY` + `Replay.log`（含 frames/trail 元数据） |
| 逐帧驱动 | `Runtime.step`（确定性推进） |
| 自省 | `Types.describe` / `Scene.entities` |
| 热重载 | `MAQUE_WATCH=1` + `Reload.script` / `Reload.scene`（`dry_run` 预览） |
| AI 工具面 | `Control.mcp`（initialize / tools/list / tools/call） |
| 宿主主循环 | `runtime/mq_host.h`：`mq_host_tick` 由宿主驱动（`samples/host`） |
| 编辑面 | `Scene.get/set/spawn/destroy/save`（`tests/editor.sh`、`editor_e2e`） |
| 动画/粒子 | `scn.AnimationPlayer` / `scn.ParticleEmitter`（`anim_e2e`） |
| 导航 | `nav_setup/nav_block/nav_goto/nav_tick` + `scn.NavAgent`（`nav_e2e`） |
| 崩溃复现 | `MAQUE_CRASH_REPORT`（报告含 seed/帧号/检查点/重放指引；`crash_context_e2e`） |

### 质量门

| 门 | 命令 | 当前 |
|----|------|------|
| 全量回归 | `ctest --test-dir build` | **32/32 通过**（10 个端到端 + 22 个模块单测） |
| 内存/UB | `cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"` → `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan` | **32/32 通过**（`detect_leaks=0` 仅用于排除 wgpu/SDL/Mesa 的全局单例噪音；自研模块单独跑 LeakSanitizer 无泄漏） |
| codegen 校验 | `cd modules/core/data && node parser.js --check` | 240 类型 / 13 命名空间 |
| 确定性 | `determinism`、`ui_e2e`（录制回放逐帧一致）、`anim_e2e`（回滚保真）、`nav_e2e` | 全部通过 |
