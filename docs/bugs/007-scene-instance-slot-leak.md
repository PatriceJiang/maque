# 007 · 场景实例槽位 / 动态壳映射泄漏（卸载不回收）

- **类型**：内存（泄漏）+ 正确性（实例预算被历史实例耗尽）
- **状态**：fixed
- **影响面**：所有 `scene_instantiate` + `scene_unload` 循环（TD 每波敌人、弹幕、可反复开关的弹窗），
  以及根场景热重载（`Reload.scene`）重建嵌套实例的路径
- **发现版本**：`43f2700`；`MAQUE_TRACY_MEM_REPORT=1` + 动态实例 churn 场景复现

## 症状

`scene_instantiate`/`scene_unload` 反复循环时：

- `[mem] node` **线性增长**（与循环次数成正比），而 `[mem] script` 完全平台期 ——
  VM 本身随总线销毁正常释放，泄漏在 Engine 侧的实例记录；
- 第 255 次动态实例化后永久失败：`runtime: scene instance limit reached (256)`，
  `scene_instantiate` 返回 -1，之后所有波次都刷不出来。

实测（400 帧、每帧 1 次 spawn + 1 次 unload，`MAQUE_SCENE_INSTANCE_LIMIT=100000`）：

```text
frame=100  spawned=98   node 45.0 KiB
frame=200  spawned=198  node 67.0 KiB
frame=300  spawned=298  node 88.9 KiB
frame=400  spawned=398  node 110.9 KiB   # script 全程 511.4 KiB 不变
```

默认预算下同一夹具在第 256 次后 `failed=443`（`--frames 700`）。

## 最小复现

一个空场景 + 每帧「先卸载上一帧动态实例、再实例化同源子场景」的脚本
（`tests/scene_instance_e2e.sh` 的 churn 段即此夹具）：

```js
var prev = -1;
function update(dt) {
  if (prev >= 0) { scene_unload(prev); prev = -1; }
  prev = scene_instantiate(-1, 'child.scn');
}
```

运行：

```bash
MAQUE_WINDOW=0x0 MAQUE_PRESENT=off MAQUE_TRACY_MEM_REPORT=1 \
  ./build/samples/player/maque_player --frames 400 churn.scn
```

修复前：`node` 随帧数线性增长，>255 次后报实例上限；修复后：`node` 回到平台期，无上限错误。

修复后同一夹具 700 帧（698 次 spawn/unload）：

```text
frame=100..700  spawned=698  failed=0  count=1
[mem] tick/node  2.2 KiB（全程平台期；script 511.4 KiB）
```

## 根因

1. `Engine::instances` 是**以实例 id 为下标的 vector**；`destroy_instance_content` 只把
   `state` 置 2（destroyed），记录本身永不从表里移除。每个卸载的实例都留下一个墓碑
   （`SceneInstanceRuntime` 里的 source/base/params/scripts/roots/children 等堆缓冲），
   并继续占用 `instance_limit` 预算。
2. 动态实例自动创建的挂载壳在卸载时 `scene->remove()` 了实体，但没有删
   `entity_instance[shell]` / `node_events[shell]`，这两个表随循环次数无界增长。
3. 根场景热重载（`reload_scene`）销毁子实例内容后同样把旧记录留在表里；
   每次 `Reload.scene` 再叠一层。

> 脚本 VM 没有泄漏：`destroy_instance_content` 会 `mq_script_bus_destroy`，
> `[mem] script` 在 churn 中恒定。问题只在 Engine 的实例记录/映射。

## 修复

- `instances` 改为 `std::map<int, std::unique_ptr<SceneInstanceRuntime>>`（有序、确定性遍历，
  不再要求 id == 下标）。
- 实例 id 仍然**单调、永不复用**：`next_instance_id` 跨场景重建保留水位，
  陈旧句柄 `instance_at(id)` 查不到 → `scene_instance_valid` 为假，不会别名到新实例。
- 卸载（`apply_instance_ops` 的 unload）在内容销毁后立即 `instances.erase(id)`；
  根场景重载用 `prune_destroyed_instances()` 清掉全部 state==2 的子实例记录。
- 删除动态挂载壳时同步 `entity_instance.erase(shell)` / `node_events.erase(shell)`。
- 预算改为按**存活实例数**（map 大小）判定，不再被历史实例耗尽。

## 回归与证据

- `tests/scene_instance_e2e.sh` 新增 churn 段：连续 400 次 spawn/unload 后
  `failed=0`、无 `scene instance limit`、`Scene.instances` 只剩根实例（`ctest -R scene_instance_e2e` 通过）。
- 修复前后 `[mem] node` 曲线对比（见「症状」）。
- 关联回归（全部通过）：`scene_instance_e2e`、`scene_serialization_e2e`、`td_e2e`、
  `multilang_e2e`、`prefab_e2e`/`nested_prefab_e2e`/`inline_prefab_e2e`、`render_texture_e2e`、
  `script_reload_e2e`。
- `Reload.scene`（含 2 个嵌套实例）单次实测：旧记录 #1/#2 释放、新记录 #3/#4 创建，
  返回 `reloaded=true` 且退出码 0。

## 相关

- [32 · 场景实例与事件作用域](../32-场景实例与事件作用域.md) §5.2/§5.3
