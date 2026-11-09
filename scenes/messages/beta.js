// beta.js —— 负责 Beta 的脚本（独立 VM）。
//
// 注册：
//   Beta 上的 ping          → on_beta_ping（orchestrator 直接发来）
//   Beta 上的 beta_forward  → on_beta_forward（Alpha 脚本转发过来）
// 两种消息都回到 Root 的 beta_pong，由 orchestrator.js 计数。
'use strict';

const S = { pings: 0, forwards: 0 };
let N = {};

function on_start() {
  N.beta = entity_find('Beta');
  N.root = entity_find('Root');
  node_on(N.beta, 'ping', 'on_beta_ping');
  node_on(N.beta, 'beta_forward', 'on_beta_forward');
  // 哨兵：alpha_ping 只在 Root 上被汇总脚本处理；Beta 节点上的同名事件注册不应收到它。
  node_on(N.beta, 'alpha_ping', 'on_beta_alpha_snoop');
  log('MSG_BETA_READY b=' + N.beta);
}

function on_beta_alpha_snoop(target, value) {
  log('MSG_BETA_SNOOP target=' + target + ' value=' + value);
}

function on_beta_ping(target, value) {
  S.pings++;
  log('MSG_BETA_PING target=' + target + ' value=' + value + ' n=' + S.pings);
  node_emit_number(N.root, 'beta_pong', value + 100);
  node_set_number(N.beta, 'scn.Sprite', 'r', 1.0);
  node_set_number(N.beta, 'scn.Sprite', 'g', 0.35);
}

function on_beta_forward(target, value) {
  S.forwards++;
  log('MSG_BETA_FORWARD target=' + target + ' value=' + value + ' n=' + S.forwards);
  node_emit_number(N.root, 'beta_pong', value + 200);
  node_set_number(N.beta, 'scn.Sprite', 'r', 0.95);
  node_set_number(N.beta, 'scn.Sprite', 'g', 0.55);
}

function update(dt) {}
