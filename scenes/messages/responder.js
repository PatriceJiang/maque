// responder.js —— 子场景（responder.scn）的脚本，独立 VM / 独立生命周期。
//
// 父场景通过 scn.SceneInstance 在 Responder 节点加载本场景；它有自己的实体（ResponderBody）
// 与独立 VM。消息有两条返程：
//   ① 显式跨实例：scene_emit(0, 'sub_pong', v) → 根实例的汇总脚本 sub_pong()；
//   ② 节点事件冒泡：node_emit_number(scene_self_root(), 'sub_node', v)
//      → 从挂载壳节点冒泡，命中父场景注册在壳上的 on_sub_node()。
// 父 → 子入口是子 VM 里的 sub_ping(id, value)（scene_emit 按处理器名直接调用）。
'use strict';

var S = { root: -1, body: -1, pings: 0, bus_pings: 0, bus_listener: -1, frames: 0 };

function on_start() {
  S.root = scene_self_root();
  S.body = node_find_path(S.root, 'ResponderBody');
  // 全局消息总线：不绑节点，父/子场景的任意脚本都能发布；回调直接传函数。
  S.bus_listener = msg_subscribe('game', 'demo', 'ping', function (a, b, c) {
    S.bus_pings++;
    log('MSG_BUS_SUB_PING a=' + a + ' b=' + b + ' c=' + c + ' n=' + S.bus_pings);
  });
  log('MSG_SUB_READY inst=' + scene_self() + ' root=' + S.root + ' body=' + S.body +
      ' bus_listener=' + S.bus_listener);
}

function sub_ping(id, value) {
  S.pings++;
  log('MSG_SUB_PING target=' + id + ' value=' + value + ' n=' + S.pings);
  // ① 显式回到根实例（scene_emit 的 event = 目标 VM 里的处理器名）。
  log('MSG_SUB_PONG_SEND=' + scene_emit(0, 'sub_pong', value + 1));
  // ② 节点事件：从子场景挂载壳向上冒泡到父实例注册在壳上的处理器。
  log('MSG_SUB_NODE_SEND=' + node_emit_number(S.root, 'sub_node', value + 2));
  node_set_number(S.body, 'scn.Sprite', 'r', 0.95);
  node_set_number(S.body, 'scn.Sprite', 'g', 0.55);
}

function update(dt) {
  S.frames++;
  if (S.frames === 3) {
    // 子场景 → 全局总线：父场景的字符串 handler（on_bus_from_child）应收到。
    log('MSG_SUB_BUS_PUB hits=' + msg_publish('game', 'demo', 'from_child', 5, 6, 0));
  }
}

function on_destroy() { log('MSG_SUB_DESTROY inst=' + scene_self() + ' pings=' + S.pings); }
