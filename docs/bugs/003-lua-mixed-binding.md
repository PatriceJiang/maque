# 003 · Lua mixed 绑定参数错位（`node_emit` 等）

- **类型**：正确性（脚本绑定）
- **状态**：fixed（`f529b8f`）
- **影响面**：Lua 后端 + 所有 `register_mixed` 绑定（24 个）：`node_emit`、`node_off`、`set_label`、
  `ui_set_text`、`camera_capture`、`node_set_name`、`node_has_component`、spine 系列等
- **发现版本**：`d517096` 之后；调查 002 的 Lua 侧事件时发现

> 这不是内存/崩溃/性能 bug，按 `docs/bugs/README.md` 的“建议录”范围记录：它解释了为什么
> Lua 侧的事件验证看起来“注册成功但永远 0 命中”，避免下一个人重走弯路。

## 症状

Lua 脚本里：

```lua
function on_start()
  local r = node_on(0, "ping", "on_ping")   -- r = 1，注册成功
  local h = node_emit(0, "ping")            -- h = 0，永远没有 handler 被调用
  log("reg=" .. r .. " emit=" .. h)
end
function on_ping() log("ping") end
```

输出 `reg=1.0 emit=0.0`；同样的 `node_emit` 在 JS 里正常。换成 `node_emit_number(0,"ping",1)` 又会正常命中，
所以不是 `node_on` 注册或总线派发的问题，而是 `node_emit` 的参数在 Lua 侧被错误编组。

## 最小复现

```lua
-- s.lua（场景 script=s.lua，含一个 id=0 的节点）
function on_start()
  local r = node_on(0, "ping", "on_ping")
  local h = node_emit(0, "ping")
  log("reg=" .. r .. " emit=" .. h)
end
function on_ping() log("ping") end
```

修复前：`reg=1.0 emit=0.0`，且没有 `ping`；修复后：`reg=1.0 emit=1.0`，并打印 `ping`。
可用 `ctest -R bindings_e2e` 覆盖绑定可调用性，但**参数语义**需要这个最小例。

## 根因

Lua 的 `register_mixed` 把参数布局写死成 `kinds = "s"`：

```cpp
HostFn h; h.kind = HostFn::Mixed; h.mixed = fn; h.user = user; h.kinds = "s";
```

而 mixed 的真实签名是“任意个数值 + 一个字符串”（通常字符串在末尾）。`trampoline` 按 `kinds` 逐位取参：

- 第 1 个实参 `0`（数字）被当成 `'s'` → `lua_tostring(L, 1)` 把栈上的 `0` **改写**为字符串 `"0"`；
- 第 2 个实参 `"ping"` 被当成 `'n'`（kinds 耗尽后默认）→ `lua_tonumber` 得到 `0`。

最终调用 `node_emit(id=0, event="0")`，事件名变成 `"0"`，自然 0 命中。
同样的错位影响所有 mixed 绑定（例如 `set_label(0, "Score")` 变成 `set_label(0, "0")`）。

## 修复

`register_mixed` 不再预设 `kinds`（留空），让 `trampoline` 按 Lua 实参实际类型拆分：
number → `nums`，string → `strs`。`n...`、`n...s` 均正确，且不会再用 `lua_tostring` 改写数值实参。

代码：[lua_backend.cpp:378](../../modules/script/src/lua_backend.cpp#L378)。

## 回归与证据

- 手工最小例：修复前 `sync=0.0`；修复后 `sync=1.0`，且 `on_ping` 日志出现。
- `ctest -R bindings_e2e`：ASan / 非 ASan 均通过（逐条检查 schema 绑定可调用性）。
- `ctest -R multilang_e2e`：JS ↔ Lua 双向事件通过。
- Lua 热重载：3 次 `Reload.script` 后每次 emit 恰好一次 `ping`（与 002 的清理策略一起验证）。

## 相关

- [14 · 脚本](../14-脚本.md)
- [002](002-node-on-hot-reload-leak.md)
