# 005 · Windows 场景热重载路径分隔符不匹配

- **类型**：功能（热重载不可用；**不是内存/崩溃**，记录以免再次误导复现）
- **状态**：open
- **影响面**：Windows + `MAQUE_WATCH=1` 时**改 `.scn` 文件不触发重载**；脚本热重载不受影响
- **发现版本**：`d517096` 之后；调查 001 时尝试用 `MAQUE_WATCH` 复现，发现场景永远不会 reload

> 记录原因：它会让“编辑场景 → 热重载”这条最常用的调试路径静默失效，也会让 001 这类
> 场景生命周期 bug 无法通过 watch 复现（必须用 `Reload.scene` 控制面）。

## 症状

Windows 下：

```bash
MAQUE_WATCH=1 MAQUE_PACE=1 ./build/samples/player/maque_player <scene.scn>
# 运行中修改 .scn 文件
```

日志只有 `runtime: hot reload watching N file(s)`，**没有** `runtime: hot reload scene '...'`；
`Reload.files` 显示监视路径是 `...\scene.scn`（反斜杠），而脚本热重载可以正常触发
（日志 `runtime: hot reload instance #0 script 'x.js'`）。

## 最小复现

1. 启动：`MAQUE_WATCH=1 MAQUE_PACE=1 MAQUE_MAX_FRAMES=240 ./maque_player scenes/render_texture/render_texture_3d.scn`
2. 运行中给场景文件末尾追加一个换行（合法 JSON 不变）；
3. 期望出现 `hot reload scene`，实际没有；同样的操作改 `.js` 会触发脚本重载。

## 根因

`poll_watcher()` 的路径来源与比较目标用了不同的分隔符：

- watcher 路径 `p`：`mq_watcher_add(watcher, (fs::path(scn_dir) / scn_vfs_path.substr(1)).string())`
  → Windows 下是反斜杠（`D:\...\render_texture_3d.scn`）；
- 场景比较目标：`c->eng->scn_vfs_path` 是 VFS 路径（`/render_texture_3d.scn`，正斜杠）；
- `ends_with(p, scn_vfs_path)` 于是永远为 false。

脚本匹配没有这个问题：`local` 和 `p` 都由 `fs::path` 构造，两边都是反斜杠。
所以表现为“脚本热更正常、场景热更静默失效”。

代码：[runtime.cpp:6990](../../modules/runtime/src/runtime.cpp#L6990)（`ends_with(p, c->eng->scn_vfs_path)`）。

## 修复建议（未实现）

- 只对**场景文件**这一路比较做分隔符归一化：
  ```cpp
  const std::string norm = fs::path(p).lexically_normal().generic_string();
  const bool scene_hit = ends_with(norm, scn_vfs_path) || ends_with(p, scn_vfs_path);
  ```
  或比较 `fs::path(p).filename()` / 场景的相对路径；脚本与嵌套实例的匹配保持原逻辑，避免把
  `local` 的反斜杠也改掉；
- 回归：新增 Windows 可跑的 e2e（`MAQUE_WATCH=1` + 追加场景内容 + 断言出现 `hot reload scene`），
  可并入 `script_reload_e2e` 或单独 `scene_watch_e2e`；
- 验收：Windows 上改场景能触发热重载，且 Linux 行为不变。

## 回归与证据

- 当前 **open**：无自动化回归。
- 手工证据（Windows）：
  - `Reload.files` 返回 `"files":["...\\render_texture_3d.scn"]`；
  - 运行中追加场景换行，240 帧内无 `hot reload scene`；
  - 同一进程里追加 `.js` 注释可以触发 `hot reload instance #0 script`；
  - 控制面 `Reload.scene` 可以正常重载（001 的修复即用此路径验证）。
- 相关 001：这是为什么“用 `MAQUE_WATCH` 复现场景生命周期 bug”在 Windows 上不可行。

## 相关

- [001](001-scene-lifecycle-leak.md)
- [32 · 场景实例与事件作用域](../32-场景实例与事件作用域.md)（热重载章节）
