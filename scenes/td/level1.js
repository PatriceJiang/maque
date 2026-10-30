'use strict';
// 第 1 关 · 松林小径 —— 独立关卡场景 + 独立行为脚本。
//
// 组合关系（docs/32 §16）：
//   本场景 .scn：背景/路径/出生点/终点/WP* 路径点 + 3 个预置塔（scn.SceneInstance）；
//   本脚本：波次调度、经济与胜负；动态 scene_instantiate(-1, "enemies/...", params) 生成敌人；
//   塔/敌人：各自独立 .scn + 独立 VM，只通过 params（父→子）与 scene_emit（子→父/塔→敌）通信；
//   global.js：跨场景常驻的全局脚本，持有金币/生命/进度并把 HUD 画在顶层。
var LEVEL = 1;
var NAME = '松林小径';
var START_GOLD = 85;
var START_LIVES = 10;
var FALLBACK = [[16, 72], [140, 72], [140, 218], [298, 218], [298, 112], [464, 112]];

var WAVES = [
  { units: [ { kind: 'slime', count: 3, gap: 1.00, hp: 26, speed: 30, reward: 6 } ] },
  { units: [ { kind: 'slime', count: 2, gap: 0.90, hp: 28, speed: 30, reward: 6 },
             { kind: 'runner', count: 2, gap: 0.80, hp: 18, speed: 58, reward: 8 } ] },
  { units: [ { kind: 'slime', count: 3, gap: 0.80, hp: 30, speed: 32, reward: 6 },
             { kind: 'tank', count: 1, gap: 1.20, hp: 70, speed: 20, reward: 20 } ] }
];

var ENEMY_SCENES = {
  slime: 'enemies/slime/slime.scn',
  runner: 'enemies/runner/runner.scn',
  tank: 'enemies/tank/tank.scn',
  flyer: 'enemies/flyer/flyer.scn'
};

var S = {
  t: 0, logAcc: 0, wave: -1, phase: 0, wait: 1.5,
  pending: [], spawnT: 0, alive: 0, uid: 0, kills: 0, leaked: 0, over: false,
  ents: {}, path: [], label: -1, hint: -1
};

function readPath(fallback) {
  var pts = [];
  for (var i = 0; i < 32; i++) {
    var id = node_find_path(-1, 'WP' + i);
    if (id < 0) break;
    pts.push([node_position_x(id), node_position_y(id)]);
  }
  if (pts.length < 2) return fallback;
  return pts;
}

function on_start() {
  S.path = readPath(FALLBACK);
  S.label = entity_find('LevelName');
  S.hint = entity_find('Hint');
  // 每次（重新）装载本关都是新一局：金币/生命/分数归零重开。
  global_set('td.level', LEVEL);
  global_set('td.active_level', LEVEL);
  global_set('td.gold', START_GOLD);
  global_set('td.lives', START_LIVES);
  global_set('td.score', 0);
  global_set('td.kills', 0);
  global_set('td.wave', 0);
  global_set('td.wave_total', WAVES.length);
  global_set_text('td.notice', '');
  if (S.label >= 0) node_set_string(S.label, 'scn.Label', 'text', '第 ' + LEVEL + ' 关 · ' + NAME);
  if (S.hint >= 0) node_set_string(S.hint, 'scn.Label', 'text',
    '塔自动攻击 · 敌人到终点扣生命 · R 重开 · Esc 返回选关');
  S.phase = 0;
  S.wait = 1.5;
  log('td_level start level=' + LEVEL + ' name=' + JSON.stringify(NAME)
      + ' waves=' + WAVES.length + ' waypoints=' + S.path.length
      + ' gold=' + START_GOLD + ' lives=' + START_LIVES);
}

function startWave(i) {
  if (i >= WAVES.length) { S.phase = 3; return; }
  S.wave = i;
  S.phase = 1;
  S.pending = [];
  S.spawnT = 0;
  var w = WAVES[i];
  for (var u = 0; u < w.units.length; u++) {
    var unit = w.units[u];
    for (var k = 0; k < unit.count; k++) {
      S.pending.push({ kind: unit.kind, hp: unit.hp, speed: unit.speed,
                       reward: unit.reward, gap: unit.gap });
    }
  }
  global_set('td.wave', i + 1);
  log('td_wave wave=' + (i + 1) + ' total=' + WAVES.length + ' units=' + S.pending.length);
}

function spawnUnit(u) {
  var uid = S.uid++;
  var path = S.path;
  if (u.kind === 'flyer') path = [S.path[0], S.path[S.path.length - 1]];
  var params = { uid: uid, kind: u.kind, hp: u.hp, speed: u.speed, reward: u.reward, path: path };
  if (u.kind === 'flyer') params.flying = true;
  var inst = scene_instantiate(-1, ENEMY_SCENES[u.kind], JSON.stringify(params));
  if (inst < 0) {
    log('td_spawn_failed kind=' + u.kind + ' uid=' + uid + ' err=' + JSON.stringify(scene_last_error()));
    return;
  }
  S.ents[uid] = { kind: u.kind, reward: u.reward, inst: inst };
  S.alive++;
  log('td_spawn uid=' + uid + ' kind=' + u.kind + ' inst=' + inst + ' hp=' + u.hp
      + ' speed=' + u.speed + ' alive=' + S.alive);
}

function on_enemy_dead(id, uid) {
  var e = S.ents[uid];
  if (!e) return;
  delete S.ents[uid];
  S.alive--;
  S.kills++;
  var gold = global_get('td.gold') + e.reward;
  global_set('td.gold', gold);
  global_set('td.kills', global_get('td.kills') + 1);
  global_set('td.score', global_get('td.score') + e.reward * 10);
  log('td_enemy_dead uid=' + uid + ' kind=' + e.kind + ' reward=' + e.reward
      + ' gold=' + gold + ' alive=' + S.alive);
}

function on_enemy_leak(id, uid) {
  var e = S.ents[uid];
  if (!e) return;
  delete S.ents[uid];
  S.alive--;
  S.leaked++;
  var lives = global_get('td.lives') - 1;
  if (lives < 0) lives = 0;
  global_set('td.lives', lives);
  log('td_enemy_leak uid=' + uid + ' kind=' + e.kind + ' lives=' + lives + ' alive=' + S.alive);
  if (lives <= 0) finish(2);
}

function finish(result) {
  if (S.over) return;
  S.over = true;
  global_set('td.result', result);
  log('td_level finish level=' + LEVEL + ' result=' + result + ' kills=' + S.kills
      + ' leaks=' + S.leaked + ' score=' + global_get('td.score')
      + ' frame=' + director_frame());
  global_emit('level_finished', result);
}

function update(dt) {
  if (S.over) return;
  S.t += dt;
  S.logAcc += dt;
  if (S.logAcc >= 1.0) {
    S.logAcc -= 1.0;
    log('td_state ' + JSON.stringify({
      level: LEVEL, wave: global_get('td.wave'), phase: S.phase, alive: S.alive,
      kills: S.kills, leaks: S.leaked, gold: global_get('td.gold'),
      lives: global_get('td.lives'), score: global_get('td.score')
    }));
  }
  if (S.phase === 0) {
    S.wait -= dt;
    if (S.wait <= 0) startWave(S.wave + 1);
    return;
  }
  if (S.phase === 1) {
    S.spawnT += dt;
    while (S.pending.length > 0 && S.spawnT >= S.pending[0].gap) {
      S.spawnT -= S.pending[0].gap;
      spawnUnit(S.pending.shift());
    }
    if (S.pending.length === 0) {
      S.phase = 2;
      log('td_wave_spawned wave=' + (S.wave + 1) + ' alive=' + S.alive);
    }
    return;
  }
  if (S.phase === 2) {
    if (S.alive > 0) return;
    if (S.wave + 1 < WAVES.length) {
      S.phase = 0;
      S.wait = 2.0;
      log('td_wave_clear wave=' + (S.wave + 1) + ' next_wait=2.0');
    } else {
      finish(1);
    }
  }
}

function on_destroy() {
  log('td_level destroy level=' + LEVEL + ' kills=' + S.kills + ' leaks=' + S.leaked);
}
