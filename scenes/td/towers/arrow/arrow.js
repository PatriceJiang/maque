'use strict';
// 箭塔（Arrow）——独立场景 + 独立行为脚本。
//
// 组合契约（docs/32 §16）：
//   · 被关卡以 scn.SceneInstance 实例化时，父场景通过 `params` 传入 {"tier":N}；
//     本脚本用 scene_self_params() 读取（无参数 = 独立运行，进入 demo 模式）。
//   · 敌人也是独立场景实例；箭塔用 scene_instance_source() 识别 /enemies/ 实例，
//     用 scene_instance_root() 读挂载壳位置，用 scene_emit(inst, "on_damage", v) 造成伤害。
//   · 弹丸是普通精灵节点（不是场景），由塔自己创建/销毁。
var TYPE = 'arrow';
var BASE = { range: 100.0, damage: 7.0, rate: 0.55, bulletSpeed: 560.0, headLen: 12.0 };

var S = {
  inst: -1, root: -1, head: -1, x: 0, y: 0,
  tier: 1, range: 100, damage: 7, rate: 0.55,
  cool: 0.3, demo: false, demoAngle: 0, logAcc: 0,
  bullets: [], shots: 0
};

function parseParams() {
  var raw = scene_self_params();
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (e) { log('td_tower_bad_params type=' + TYPE + ' raw=' + raw); return null; }
}

function towerRoot() {
  var r = scene_self_root();
  if (r < 0) return -1;
  if (entity_name(r) === 'Tower') return r;
  return node_find_path(r, 'Tower');
}

function on_start() {
  S.inst = scene_self();
  var p = parseParams();
  S.root = towerRoot();
  S.head = node_find_path(S.root, 'Head');
  if (!p) {
    // 独立运行：居中显示并缓慢旋转炮塔，证明该 .scn 可单独渲染。
    S.demo = true;
    if (S.root >= 0) node_set_position(S.root, screen_w() * 0.5, screen_h() * 0.5);
    S.x = S.root >= 0 ? node_position_x(S.root) : screen_w() * 0.5;
    S.y = S.root >= 0 ? node_position_y(S.root) : screen_h() * 0.5;
    log('td_tower_demo type=' + TYPE + ' inst=' + S.inst + ' root=' + S.root + ' mode=demo');
    return;
  }
  S.tier = p.tier ? p.tier : 1;
  if (S.tier < 1) S.tier = 1;
  var k = 1.0 + (S.tier - 1) * 0.55;
  S.damage = BASE.damage * k;
  S.range = BASE.range + (S.tier - 1) * 10.0;
  S.rate = BASE.rate * (1.0 - (S.tier - 1) * 0.10);
  S.x = node_position_x(scene_self_root());
  S.y = node_position_y(scene_self_root());
  S.cool = 0.15 + (S.inst % 5) * 0.06;
  log('td_tower ready type=' + TYPE + ' inst=' + S.inst + ' pos=' + Math.round(S.x) + ',' + Math.round(S.y)
      + ' tier=' + S.tier + ' dmg=' + S.damage.toFixed(1) + ' range=' + S.range.toFixed(1) + ' mode=game');
}

function pickTarget() {
  var best = -1, bd = S.range * S.range;
  var n = scene_instance_count();
  for (var i = 0; i < n; i++) {
    var inst = scene_instance_at(i);
    if (inst === S.inst || inst === 0) continue;
    if (scene_instance_valid(inst) <= 0) continue;
    var src = scene_instance_source(inst);
    if (!src || src.indexOf('/enemies/') < 0) continue;
    var root = scene_instance_root(inst);
    if (root < 0) continue;
    var dx = node_position_x(root) - S.x;
    var dy = node_position_y(root) - S.y;
    var d2 = dx * dx + dy * dy;
    if (d2 <= bd) { bd = d2; best = inst; }
  }
  return best;
}

function aimAt(px, py) {
  if (S.head < 0) return;
  var a = Math.atan2(py - S.y, px - S.x);
  node_set_angle(S.head, a * 180.0 / Math.PI);
  node_set_position(S.head, Math.cos(a) * BASE.headLen, Math.sin(a) * BASE.headLen);
}

function spawnBullet(color) {
  var b = entity_spawn();
  if (b < 0) return -1;
  node_add_component(b, 'scn.Transform2D');
  node_add_component(b, 'scn.Sprite');
  node_set_position(b, S.x, S.y);
  node_set_number(b, 'scn.Sprite', 'radius', 3.2);
  node_set_number(b, 'scn.Sprite', 'r', color[0]);
  node_set_number(b, 'scn.Sprite', 'g', color[1]);
  node_set_number(b, 'scn.Sprite', 'b', color[2]);
  return b;
}

function hit(target, tx, ty, dmg) {
  // 箭塔：单体伤害。目标可能在同一帧已死亡 → scene_emit 返回 0，忽略即可。
  scene_emit(target, 'on_damage', dmg);
}

function fire(target) {
  var root = scene_instance_root(target);
  if (root < 0) return;
  var tx = node_position_x(root), ty = node_position_y(root);
  aimAt(tx, ty);
  var bid = spawnBullet([0.55, 1.0, 0.55]);
  if (bid < 0) return;
  S.bullets.push({ id: bid, target: target, x: S.x, y: S.y, speed: BASE.bulletSpeed,
                   dmg: S.damage, t: 0 });
  S.shots++;
  log('td_tower fire type=' + TYPE + ' inst=' + S.inst + ' target=' + target + ' shots=' + S.shots);
}

function stepBullets(dt) {
  for (var i = S.bullets.length - 1; i >= 0; i--) {
    var b = S.bullets[i];
    b.t += dt;
    if (b.t > 3.0 || scene_instance_valid(b.target) <= 0) {
      node_destroy(b.id);
      S.bullets.splice(i, 1);
      continue;
    }
    var root = scene_instance_root(b.target);
    if (root < 0) { node_destroy(b.id); S.bullets.splice(i, 1); continue; }
    var tx = node_position_x(root), ty = node_position_y(root);
    var dx = tx - b.x, dy = ty - b.y;
    var dist = Math.sqrt(dx * dx + dy * dy);
    var step = b.speed * dt;
    if (dist <= step + 6.0) {
      hit(b.target, tx, ty, b.dmg);
      node_destroy(b.id);
      S.bullets.splice(i, 1);
      continue;
    }
    b.x += dx / dist * step;
    b.y += dy / dist * step;
    node_set_position(b.id, b.x, b.y);
  }
}

function update(dt) {
  if (S.demo) {
    S.demoAngle += dt * 55.0;
    if (S.head >= 0) {
      node_set_angle(S.head, S.demoAngle);
      node_set_position(S.head, Math.cos(S.demoAngle * Math.PI / 180.0) * BASE.headLen,
                        Math.sin(S.demoAngle * Math.PI / 180.0) * BASE.headLen);
    }
    S.logAcc += dt;
    if (S.logAcc >= 1.0) {
      S.logAcc = 0;
      log('td_tower_demo type=' + TYPE + ' inst=' + S.inst + ' angle=' + Math.round(S.demoAngle % 360));
    }
    return;
  }
  stepBullets(dt);
  S.cool -= dt;
  if (S.cool > 0) return;
  var target = pickTarget();
  if (target < 0) return;
  var root = scene_instance_root(target);
  if (root < 0) return;
  aimAt(node_position_x(root), node_position_y(root));
  fire(target);
  S.cool = S.rate;
}

function on_destroy() {
  log('td_tower_destroy type=' + TYPE + ' inst=' + S.inst + ' shots=' + S.shots);
}
