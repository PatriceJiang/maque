# Bug 档案（docs/bugs/）

> 目的：把**内存 / 崩溃 / 性能**类问题变成可复现、可回归的资产，而不是一次性的口头结论。
> 这里只记录**已经发生**的问题；机制与约定仍写在对应的设计文档里。

## 何时必须记录

- **必录**：内存泄漏 / 越界 / 重复释放 / 堆损坏、崩溃 / 断言 / 未定义行为、性能回退或明显抖动。
- **建议录**：功能 / 正确性 bug，尤其是跨语言、跨平台、热重载、序列化这类难复现的问题。
- 原则：**先建档、再修复**；修复提交引用 bug 编号；未修也要记（状态 `open`），写清最小复现与下一步。

## 文件规范

- 命名：`NNN-短名.md`，三位递增、**不复用编号**；短名用 kebab-case。
- 每个文件至少包含：症状 / 类型 / 状态 / 影响面 / 最小复现 / 根因 / 修复或规避 / 回归与证据。
- 状态只用 `open` / `fixed` / `wontfix`；提交前更新本页索引。
- 只写可验证的事实；未量化、未复现的地方要明确标注，不要用猜测填空。
- 路径用仓库相对路径；不要写本机绝对路径、用户名等环境信息。

## 索引

| 编号 | 标题 | 类型 | 状态 | 回归 |
|------|------|------|------|------|
| [001](001-scene-lifecycle-leak.md) | 场景生命周期泄漏 / 热重载 bad-free | 内存 + 崩溃 | fixed | `render_texture_e2e` |
| [002](002-node-on-hot-reload-leak.md) | 脚本热重载 `node_on` 注册泄漏 / 重复派发 | 内存 + 正确性 | fixed | `script_reload_e2e` |
| [003](003-lua-mixed-binding.md) | Lua mixed 绑定参数错位（`node_emit` 等） | 正确性 | fixed | `bindings_e2e` |
| [004](004-render-target-attachment-churn.md) | RenderTexture 多尺寸导致 depth/MSAA 每帧重建 | 性能 + 内存 | fixed | `render_texture_e2e` |
| [005](005-windows-scene-watch-path.md) | Windows 场景热重载路径分隔符不匹配 | 功能 | open | 待补 |
| [006](006-imgui-present-clipping-font-glyph.md) | WGPU 直呈 ImGui 裁剪错误与静态图集缺字 | 正确性 | fixed | `imgui`、`imgui_present_e2e` |
| [007](007-scene-instance-slot-leak.md) | 场景实例槽位 / 动态壳映射泄漏（卸载不回收） | 内存 + 正确性 | fixed | `scene_instance_e2e` |
| [008](008-wgpu-direct-present-no-poll.md) | wgpu 直呈路径不排空事件队列 → 延迟销毁资源累积 | 内存 | fixed | 手动 RSS（直呈 1280×720） |
| [009](009-static-scene-redraw-cpu.md) | 静态场景每帧重绘导致 CPU 占用偏高 | 性能 | fixed | 手动 CPU / `MAQUE_TRACE_STATS` |
| [010](010-imgui-direct-pointer-mapping.md) | WGPU 直呈下 ImGui 鼠标位置映射错误（点击/拖动偏移） | 正确性 | fixed | `imgui_present_e2e`（第 ⑦ 段） |
| [011](011-pipeline-format-thrash.md) | RenderTexture/主目标格式交替导致 3D 管线每 pass 重建 | 性能 | fixed | `render_texture_e2e` |
| [012](012-windows-checkout-e2e-false-fail.md) | Windows 检出（autocrlf）/窗口模式/反向探针让 e2e 门假失败与假跳过 | 测试基础设施 | fixed | `env_cubemap_e2e`、`gltf_import_e2e`、`render3d_scene_e2e`、`render3d_present_e2e`、`imgui_engine_options_e2e`、`spine_*` |
| [013](013-sprite-z-index-ignored.md) | `scn.Sprite` 的 `node_set_z_index` 被静默忽略（按创建序绘制） | 正确性 | fixed | `tank_zorder`、`tank_smoke`/`tank_rules` 截图 |
| [014](014-reflection-component-hotpath.md) | 反射/组件访问热路径：每帧遍历的查表与分配开销（checksum/视觉哈希/逐帧系统） | 性能 | fixed | 19–20 场景回归 + `world_trail` 确定性；端到端 profile 待复测 |

## 模板

```markdown
# NNN · 标题

- **类型**：内存 / 崩溃 / 性能 / 正确性
- **状态**：open | fixed | wontfix
- **影响面**：哪些场景 / 平台 / 模块
- **发现版本**：commit + 触发命令

## 症状

## 最小复现

## 根因

## 修复

## 回归与证据

## 相关
```
