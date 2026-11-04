// docs/34 P0: factory form — 每个挂载点一个实例；顶层 var 是实例闭包状态。
// 模块级计数器（测试用）：同一 VM 内 child.js 只应求值一次。
globalThis.__child_module_loads = (globalThis.__child_module_loads || 0) + 1;
export default function () {
'use strict';
// 子场景脚本（docs/32）：每个 SceneInstance 一份独立 VM/状态。
var self_id = -1, root = -1, body = -1, ticks = 0, pings = 0;

function on_start() {
  self_id = scene_self();
  root = scene_self_root();
  body = node_find_path(root, 'Body');
  log('CHILD_START inst=' + self_id + ' root=' + root + ' body=' + body +
      ' module=' + globalThis.__child_module_loads);
  // 事件注册带实例作用域：两个实例的同名 handler 互不串扰（docs/32 §4.2）。
  node_on(body, 'selfping', 'child_selfping');
}

function update(dt) {
  ticks++;
  if (ticks == 3) node_emit_number(body, 'from_child', 42);        // 冒泡到父实例的壳节点
  if (ticks == 5) node_emit_number(body, 'selfping', ticks);       // 只在**本实例**内派发
}

// 命名事件处理器（父实例 scene_emit(instance, 'ping'/'report', v) 直接按名字路由）。
function ping(id, v) { pings++; log('CHILD_PING inst=' + self_id + ' v=' + v + ' pings=' + pings); }
function report(id, v) { log('CHILD_REPORT inst=' + self_id + ' pings=' + pings); }
function child_selfping(id, v) { log('CHILD_SELF inst=' + self_id + ' v=' + v); }

function on_destroy() { log('CHILD_DESTROY inst=' + self_id + ' ticks=' + ticks + ' pings=' + pings); }

return { on_start, update, ping, report, child_selfping, on_destroy };
}
