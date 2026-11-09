// orchestrator.js —— 消息测试的驱动方（独立 VM）。
//
// 场景里有 3 个根场景脚本 + 1 个嵌套子场景脚本，各自“负责”不同节点：
//   orchestrator.js（本文件）→ Root（消息汇总）
//   alpha.js                → Alpha / AlphaSensor
//   beta.js                 → Beta
//   responder.js            → Responder 节点上的子场景（scn.SceneInstance）
//
// 消息只走节点事件 / scene_emit：node_on(id, event, handler) 注册在**目标节点**上，
// node_emit_number(id, event, value) 从任意脚本触发；事件沿父链冒泡。
// 子场景是独立实例/VM：父 → 子用 scene_emit(instance, event, value)，
// 子 → 父用 scene_emit(0, …) 或从挂载壳 node_emit_number 冒泡（见 responder.js）。
'use strict';

const S = { frame: 0, pongs: 0, sensor_replies: 0, alpha_pings: 0, sub_pongs: 0, sub_nodes: 0,
            bus_pings: 0, bus_children: 0, bus_listener: -1 };
let N = {};

function on_start() {
  N.root = entity_find('Root');
  N.alpha = entity_find('Alpha');
  N.beta = entity_find('Beta');
  N.sensor = entity_find('AlphaSensor');
  N.status = entity_find('Status');
  N.shell = entity_find('Responder');

  // 找到静态子场景实例（根实例 0 之外、source 指向 responder.scn 的那个）。
  N.sub = -1;
  for (let i = 0; i < scene_instance_count(); i++) {
    const id = scene_instance_at(i);
    if (id !== scene_self() && String(scene_instance_source(id)).indexOf('responder.scn') >= 0) N.sub = id;
  }

  // 回复处理器注册在 Root 上：Alpha/Beta 的脚本 node_emit_number(Root, …) 时命中这里。
  // 处理器名在每个 VM 里独立存在；这里用带 hub 前缀的名字，避免与 Alpha/Beta 的处理器重名
  //（事件总线按处理器名在实例内路由，同名会被多个 VM 同时命中）。
  node_on(N.root, 'alpha_ping', 'on_hub_alpha');
  node_on(N.root, 'beta_pong', 'on_hub_beta_pong');
  node_on(N.root, 'sensor_reply', 'on_sensor_reply');
  // 子场景从挂载壳 node_emit_number(…, 'sub_node', …) 冒泡到这里。
  node_on(N.shell, 'sub_node', 'on_sub_node');

  // 全局消息总线（不绑节点）：函数回调 + 字符串 handler 两种形式各一个。
  S.bus_listener = msg_subscribe('game', 'demo', 'ping', function (a, b, c) {
    S.bus_pings++;
    log('MSG_BUS_ROOT_PING a=' + a + ' b=' + b + ' c=' + c + ' n=' + S.bus_pings);
  });
  const l2 = msg_subscribe('game', 'demo', 'from_child', 'on_bus_from_child');
  log('MSG_ORCH_READY a=' + N.alpha + ' b=' + N.beta + ' s=' + N.sensor +
      ' sub=' + N.sub + ' shell=' + N.shell + ' instances=' + scene_instance_count() +
      ' bus_listener=' + S.bus_listener + ' bus_listener2=' + l2);
  ui_set_text(N.status, 'hub: ready');
}

function on_bus_from_child(a, b, c) {
  S.bus_children++;
  log('MSG_BUS_ROOT_FROM_CHILD a=' + a + ' b=' + b + ' c=' + c + ' n=' + S.bus_children);
}

// Alpha → 汇总方（跨脚本回复）
function on_hub_alpha(target, value) {
  S.alpha_pings++;
  log('MSG_ORCH_ALPHA target=' + target + ' value=' + value + ' n=' + S.alpha_pings);
}

// Beta → 汇总方（跨脚本回复）
function on_hub_beta_pong(target, value) {
  S.pongs++;
  log('MSG_ORCH_PONG target=' + target + ' value=' + value + ' n=' + S.pongs);
}

// Alpha 处理完冒泡到 Alpha 的传感器消息后 → 汇总方
function on_sensor_reply(target, value) {
  S.sensor_replies++;
  log('MSG_ORCH_SENSOR_REPLY target=' + target + ' value=' + value + ' n=' + S.sensor_replies);
}

// 子场景 → 根实例：scene_emit(0, 'sub_pong', v) 直接按名字命中本函数。
function sub_pong(target, value) {
  S.sub_pongs++;
  log('MSG_ORCH_SUB_PONG target=' + target + ' value=' + value + ' n=' + S.sub_pongs);
}

// 子场景 → 父场景：从挂载壳冒泡上来的节点事件。
function on_sub_node(target, value) {
  S.sub_nodes++;
  log('MSG_ORCH_SUB_NODE target=' + target + ' value=' + value + ' n=' + S.sub_nodes);
}

function update(dt) {
  S.frame++;

  if (S.frame === 2) {
    // 同一个事件名 ping 发给两个不同节点：由两个脚本各自的处理器处理。
    const ha = node_emit_number(N.alpha, 'ping', 10);
    const hb = node_emit_number(N.beta, 'ping', 20);
    // 把 Beta 的 ping 发给 Alpha：Alpha 没有注册该事件（Beta 的处理器在 Beta 上），必须 0 命中。
    const hw = node_emit_number(N.alpha, 'beta_ping', 99);
    log('MSG_ORCH_EMIT phase=direct alpha=' + ha + ' beta=' + hb + ' wrong=' + hw);
    ui_set_text(N.status, 'direct: alpha=' + ha + ' beta=' + hb + ' wrong=' + hw);
  }

  if (S.frame === 3) {
    // 发给子节点 AlphaSensor：Alpha 脚本注册在父节点 Alpha 上 → 冒泡命中。
    const hs = node_emit_number(N.sensor, 'sensor_ping', 30);
    log('MSG_ORCH_EMIT phase=sensor hits=' + hs);

    // 父 → 子场景：直接调用子实例 VM 里的 sub_ping 处理器。
    const hsub = scene_emit(N.sub, 'sub_ping', 40);
    log('MSG_ORCH_SUB_EMIT hits=' + hsub);

    // 全局消息：同一 (bus, subject, event) 会同时命中根脚本和子场景脚本的订阅。
    const hp = msg_publish('game', 'demo', 'ping', 7, 8, 0);
    log('MSG_ORCH_BUS_PUB phase=first hits=' + hp);
  }

  if (S.frame === 4) {
    // 只摘掉 Alpha 节点上的 ping；Beta 上的同名事件注册不受影响 → 0 / 1。
    const removed = node_off(N.alpha, 'ping');
    const ha = node_emit_number(N.alpha, 'ping', 11);
    const hb = node_emit_number(N.beta, 'ping', 21);
    log('MSG_ORCH_OFF removed=' + removed + ' alpha_after=' + ha + ' beta_after=' + hb);

    // 取消全局订阅：再发 ping 只应命中子场景的 listener（hits=1），根回调不再触发。
    log('MSG_ORCH_BUS_UNSUB=' + msg_unsubscribe(S.bus_listener));
    const hp2 = msg_publish('game', 'demo', 'ping', 9, 10, 0);
    log('MSG_ORCH_BUS_PUB phase=after_unsub hits=' + hp2);
    ui_set_text(N.status, 'off: alpha=' + ha + ' beta=' + hb);
  }

  if (S.frame === 5) {
    log('MSG_ORCH_SUMMARY alpha=' + S.alpha_pings + ' pongs=' + S.pongs +
        ' sensor=' + S.sensor_replies + ' sub_pong=' + S.sub_pongs + ' sub_node=' + S.sub_nodes +
        ' bus_ping=' + S.bus_pings + ' bus_child=' + S.bus_children);
    ui_set_text(N.status, 'pongs=' + S.pongs + ' sub=' + S.sub_pongs + ' bus=' + S.bus_pings);
  }
}
