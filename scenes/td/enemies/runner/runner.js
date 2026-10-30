'use strict';
// 疾跑者（Runner）——独立场景 + 独立行为脚本：速度快、血量低。
//
// 组合契约（docs/32 §16）：
//   · 关卡用 scene_instantiate(-1, "enemies/runner/runner.scn", params) 动态生成；
//     params = {uid, kind, hp, speed, reward, path:[[x,y],...], flying?}。
//   · 本脚本只移动自己的**挂载壳**（scene_self_root），并向父实例发命名事件：
//     on_enemy_dead / on_enemy_leak（父场景 VM 里同名函数）。
//   · 无 params = 独立运行：在屏幕中央绕方形路径巡逻，证明该 .scn 可单独渲染。
var TYPE = 'runner';
var BASE_HP = 16.0;
var BASE_SPEED = 58.0;
var BASE_COLOR = [0.95, 0.82, 0.25];
var BAR_HALF = 10.0;
var BAR_Y = 14.0;

var S = {
  inst: -1, root: -1, mover: -1, body: -1, hpBg: -1, hpFill: -1,
  uid: 0, hp: 24, maxhp: 24, speed: 30,
  x: 0, y: 0, seg: 1, path: [],
  slowT: 0, slowFactor: 0.5, flash: 0,
  dead: false, demo: false, demoLog: 0
};

function parseParams() {
  var raw = scene_self_params();
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (e) { log('td_enemy_bad_params type=' + TYPE + ' raw=' + raw); return null; }
}

function enemyRoot() {
  var r = scene_self_root();
  if (r < 0) return -1;
  if (entity_name(r) === 'Enemy') return r;
  return node_find_path(r, 'Enemy');
}

function findParts() {
  S.body = node_find_path(S.root, 'Body');
  S.hpBg = node_find_path(S.root, 'HpBg');
  S.hpFill = node_find_path(S.root, 'HpFill');
}

function setHpBar() {
  if (S.hpFill < 0) return;
  var frac = S.maxhp > 0 ? S.hp / S.maxhp : 0;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  var w = BAR_HALF * 2.0 * frac;
  node_set_number(S.hpFill, 'scn.Sprite', 'width', w);
  node_set_position(S.hpFill, -BAR_HALF + w * 0.5, BAR_Y);
  var r = 0.35, g = 0.90, b = 0.45;
  if (frac <= 0.25) { r = 1.00; g = 0.35; b = 0.30; }
  else if (frac <= 0.55) { r = 1.00; g = 0.80; b = 0.25; }
  node_set_number(S.hpFill, 'scn.Sprite', 'r', r);
  node_set_number(S.hpFill, 'scn.Sprite', 'g', g);
  node_set_number(S.hpFill, 'scn.Sprite', 'b', b);
}

function applyTint() {
  if (S.body < 0) return;
  var r = BASE_COLOR[0], g = BASE_COLOR[1], b = BASE_COLOR[2];
  if (S.slowT > 0) { r = r * 0.55 + 0.20; g = g * 0.75 + 0.10; b = 1.0; }
  if (S.flash > 0) { r = 1.0; g = 0.9; b = 0.9; }
  node_set_number(S.body, 'scn.Sprite', 'r', r);
  node_set_number(S.body, 'scn.Sprite', 'g', g);
  node_set_number(S.body, 'scn.Sprite', 'b', b);
}

function setPos(x, y) {
  S.x = x; S.y = y;
  if (S.mover >= 0) node_set_position(S.mover, x, y);
}

function on_start() {
  S.inst = scene_self();
  S.root = enemyRoot();
  findParts();
  var p = parseParams();
  if (!p) {
    S.demo = true;
    var cx = screen_w() * 0.5, cy = screen_h() * 0.5;
    S.path = [[cx - 60, cy - 40], [cx + 60, cy - 40], [cx + 60, cy + 40], [cx - 60, cy + 40], [cx - 60, cy - 40]];
    S.mover = S.root;
    S.hp = S.maxhp = BASE_HP;
    setHpBar();
    setPos(S.path[0][0], S.path[0][1]);
    log('td_enemy_demo type=' + TYPE + ' inst=' + S.inst + ' root=' + S.root + ' mode=demo');
    return;
  }
  S.uid = p.uid ? p.uid : 0;
  S.hp = p.hp ? p.hp : BASE_HP;
  S.maxhp = S.hp;
  S.speed = p.speed ? p.speed : BASE_SPEED;
  S.path = (p.path && p.path.length >= 2) ? p.path : [[20, 20], [460, 300]];
  S.mover = S.inst > 0 ? scene_self_root() : S.root;
  S.seg = 1;
  setPos(S.path[0][0], S.path[0][1]);
  setHpBar();
  log('td_enemy ready type=' + TYPE + ' inst=' + S.inst + ' uid=' + S.uid
      + ' hp=' + S.hp + ' speed=' + S.speed + ' path=' + S.path.length + ' mode=game');
}

function on_damage(id, dmg) {
  if (S.dead) return;
  S.hp -= dmg;
  S.flash = 0.08;
  applyTint();
  if (S.hp <= 0) die();
  else setHpBar();
}

function on_slow(id, factor) {
  if (S.dead) return;
  S.slowT = 2.0;
  S.slowFactor = factor > 0 ? factor : 0.5;
  applyTint();
}

function emitParent(event) {
  if (S.inst <= 0) return;
  var parent = scene_instance_parent(S.inst);
  if (parent >= 0) scene_emit(parent, event, S.uid);
}

function die() {
  if (S.dead) return;
  S.dead = true;
  if (S.demo) { S.dead = false; S.hp = S.maxhp; setHpBar(); applyTint(); return; }
  log('td_enemy_die type=' + TYPE + ' inst=' + S.inst + ' uid=' + S.uid);
  emitParent('on_enemy_dead');
  scene_unload(S.inst);
}

function leak() {
  if (S.dead) return;
  S.dead = true;
  if (S.demo) { S.dead = false; S.seg = 1; setPos(S.path[0][0], S.path[0][1]); return; }
  log('td_enemy_leak type=' + TYPE + ' inst=' + S.inst + ' uid=' + S.uid);
  emitParent('on_enemy_leak');
  scene_unload(S.inst);
}

function update(dt) {
  if (S.dead) return;
  if (S.flash > 0) { S.flash -= dt; if (S.flash <= 0) { S.flash = 0; applyTint(); } }
  if (S.slowT > 0) { S.slowT -= dt; if (S.slowT <= 0) { S.slowT = 0; applyTint(); } }
  if (S.demo) {
    S.demoLog += dt;
    if (S.demoLog >= 1.0) {
      S.demoLog = 0;
      log('td_enemy_demo type=' + TYPE + ' inst=' + S.inst + ' pos=' + Math.round(S.x) + ',' + Math.round(S.y));
    }
  }
  var speed = S.speed * (S.slowT > 0 ? S.slowFactor : 1.0);
  var remain = speed * dt;
  while (remain > 0 && S.seg < S.path.length) {
    var tx = S.path[S.seg][0], ty = S.path[S.seg][1];
    var dx = tx - S.x, dy = ty - S.y;
    var dist = Math.sqrt(dx * dx + dy * dy);
    if (dist <= 0.0001) { S.seg++; continue; }
    if (dist <= remain) {
      S.x = tx; S.y = ty; S.seg++; remain -= dist;
    } else {
      S.x += dx / dist * remain;
      S.y += dy / dist * remain;
      remain = 0;
    }
  }
  if (S.mover >= 0) node_set_position(S.mover, S.x, S.y);
  if (S.seg >= S.path.length) {
    if (S.demo) { S.seg = 1; setPos(S.path[0][0], S.path[0][1]); }
    else leak();
  }
}

function on_destroy() {
  if (!S.demo) log('td_enemy_destroy type=' + TYPE + ' inst=' + S.inst + ' uid=' + S.uid);
}
