// alpha.js —— 负责 Alpha / AlphaSensor 的脚本（独立 VM）。
//
// 注册：
//   Alpha 上的 ping         → on_alpha_ping（orchestrator 发来）
//   Alpha 上的 sensor_ping  → on_alpha_sensor（从子节点 AlphaSensor 冒泡上来）
// 发送：
//   on_alpha_ping 直接把消息转给 **Beta 脚本负责的节点**（beta_forward），
//   再给 Root 回一条 alpha_ping —— 这就是“不同节点的跨脚本消息”。
'use strict';

const S = { pings: 0, sensors: 0 };
let N = {};

function on_start() {
  N.alpha = entity_find('Alpha');
  N.beta = entity_find('Beta');
  N.root = entity_find('Root');
  node_on(N.alpha, 'ping', 'on_alpha_ping');
  node_on(N.alpha, 'sensor_ping', 'on_alpha_sensor');
  log('MSG_ALPHA_READY a=' + N.alpha + ' b=' + N.beta);
}

function on_alpha_ping(target, value) {
  S.pings++;
  log('MSG_ALPHA_PING target=' + target + ' value=' + value + ' n=' + S.pings);
  // 跨脚本：Alpha 的处理器把消息转给 Beta 的节点（处理器名 on_beta_forward 只在 beta.js 里）。
  node_emit_number(N.beta, 'beta_forward', value + 1);
  // 回到 Root 的汇总脚本。
  node_emit_number(N.root, 'alpha_ping', value * 10);
  // 视觉反馈：收到消息时 Alpha 变亮（节点状态写入同一棵世界树）。
  node_set_number(N.alpha, 'scn.Sprite', 'r', 0.9);
  node_set_number(N.alpha, 'scn.Sprite', 'g', 0.9);
}

function on_alpha_sensor(target, value) {
  S.sensors++;
  // target = 处理器注册所在节点（消息从 AlphaSensor 冒泡到 Alpha 后命中这里，
  // 所以 target 是 Alpha 而不是 AlphaSensor；value 仍是 emit 时的载荷）。
  log('MSG_ALPHA_SENSOR target=' + target + ' value=' + value + ' n=' + S.sensors);
  node_emit_number(N.root, 'sensor_reply', value + 1);
}

function update(dt) {}
