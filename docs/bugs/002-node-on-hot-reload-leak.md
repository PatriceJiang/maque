# 002 · 脚本热重载 `node_on` 注册泄漏 / 重复派发

- **类型**：内存 + 正确性
- **状态**：fixed（`f529b8f`）
- **影响面**：所有使用 `node_on` 的脚本与热重载路径（根脚本 / 子实例 / 全局 / 覆盖层）；JS 与 Lua 都受影响
- **发现版本**：`d517096` 之后；连续 `MAQUE_WATCH` / `Reload.script` 复现

## 症状

每次脚本热重载后，同一事件会多触发一次：

```text
初始：      ping          （1 次）
reload #1： ping ping     （2 次）
reload #2： ping ping ping（3 次）
...
```

两个后果：

- **功能错误**：`on_ping` 被调用 N 次（重复扣血、重复生成、重复日志等）；
- **内存增长**：`node_events` 中每个 `NodeHandler` 含两个 `std::string`，旧注册永不释放，
  随热重载次数无界增长。

> 脚本 VM 本身没有泄漏：`mq_script_bus_swap` 会销毁旧 VM，`reset_instances` 会销毁总线。
> 泄漏在 Engine 侧的**处理器注册表**。

## 最小复现

`tests/script_reload_e2e.sh` 就是最小夹具：脚本在 `on_start` 里 `node_on(0,"ping","on_ping")`，
`on_ping` 打印 `director_frame()`，`update` 每 10 帧 emit 一次；`MAQUE_WATCH=1` 运行中追加注释触发两次重载。

修复前：同一帧出现多条 `ping f=<相同帧号>`。
修复后：每个帧号恰好一条。

也可以手工用控制面：`Runtime.step 20` → 追加脚本内容 → `Reload.script` → `Runtime.step 20`，重复 5 次，
观察一次 emit 的 `ping` 条数从 2 线性涨到 6。

## 根因

`node_on(...)` 只做 `node_events[entity].push_back(...)`，没有替换语义。
热重载的流程是“销毁旧 VM → 换入新 VM → 对新 VM 派发 `on_start` 让脚本重新注册”，
但旧 VM 留在 `node_events` 里的条目没有摘除 → 每轮重载追加一份。

另外，覆盖层热重载的 `on_start` 没有设置 `in_overlay` 上下文，`node_on` 会按根实例
（`owner=0`）登记，归属和总线都是错的。

## 修复

- `NodeHandler` 增加 `slot`：注册时通过“哪个 VM 定义了该 handler 函数”判定脚本槽位
  （单脚本总线判不出也无妨，会整体清理）。
- 四类重载在 `mq_script_bus_swap` 之后、`on_start` 之前调用 `purge_node_handlers`：
  - 根脚本 `reload_script`（`owner=0`）；
  - 子实例 `reload_instance_script`（`owner=instance id`）；
  - 全局脚本 `reload_global_script`（`owner=-3`）；
  - 覆盖层 `reload_overlay_script`（`owner=-2`），并补上 `in_overlay` 上下文。
- 清理策略：
  - 单脚本总线：清空该实例的全部注册（覆盖 handler 被删除/改名的情况）；
  - 多脚本总线：只删该槽位的注册；槽位未知时只删“新 VM 不再定义、且整条总线也没有任何 VM 定义”的陈旧项，
    **不误删其它脚本的注册**。
- `unload_global` / `unload_overlay` 也清理对应 owner，避免换脚本后残留。

代码入口：[runtime.cpp:394](../../modules/runtime/src/runtime.cpp#L394)（`NodeHandler`）、
[11309](../../modules/runtime/src/runtime.cpp#L11309)（判定槽位）、
[11324](../../modules/runtime/src/runtime.cpp#L11324)（清理）、
[11926](../../modules/runtime/src/runtime.cpp#L11926)（注册时写 slot）。

## 回归与证据

- 新增 `ctest -R script_reload_e2e`：watcher 触发两次重载，断言“同一帧号只出现一次 ping”，
  Windows 也可跑（不依赖 FIFO）。ASan / 非 ASan 均通过。
- 修复前实测同一 emit 的 `ping` 次数：**2 → 3 → 4 → 5 → 6**；修复后恒为 **1**。
- 多脚本保留：只重载 `m.js` 时，`n.js` 的 handler 仍恰好触发一次，`m.js` 的新注册也恰好一次。
- 脚本 VM 无泄漏：JS 场景 ↔ Lua 场景互切 250 次（2000 帧），`[mem] script` 在
  257.8 KiB（JS）/ 64.5 KiB（Lua）之间循环并精确回到原值，无累积趋势。
- 关联回归：`bindings_e2e`、`multilang_e2e`、`render_texture_e2e`、`td_e2e` 通过。

## 相关

- [14 · 脚本 §热重载](../14-脚本.md)
- [003](003-lua-mixed-binding.md)：同一次调查中发现的 Lua 绑定参数错位（不是本泄漏的成因，但会掩盖事件验证）。
