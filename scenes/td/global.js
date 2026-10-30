'use strict';
// 全局脚本（scn.Scene.global_script）——小小塔防的游戏级服务层。
//
// 与「场景实例」的分工（docs/32 §16）：
//   · 每关/菜单是根场景，各自带自己的行为脚本（关卡逻辑/菜单交互）；
//   · 本脚本**跨 director_load_scene 常驻**（VM 状态不丢），负责：
//       1) 全局状态默认值与进度（解锁关数/最高分/提示文本）—— 所有 VM 用 global_* 共享；
//       2) HUD（顶部条 + 金币/生命/波次/分数），每次换场景后重建；
//       3) 结算流程：关卡通过 global_emit('level_finished', 1|2) 通知这里，
//          这里解锁下一关、写提示、显示横幅，2.4s 后切回菜单；
//       4) 全局热键：Esc 返回选关，R 重开当前关。
//
// 全局状态键（全部以 td. 开头）：gold/lives/score/wave/wave_total/kills/level/unlocked/best/result/notice。
var S = {
  base: '', hud: [], title: -1, status: -1, banner: -1,
  finished: false, timer: 0, returned: false, lastStatus: ''
};

// ---- 默认值（只在首次加载时写；切换场景不会覆盖玩家进度）-------------------
if (!global_has('td.unlocked')) {
  global_set('td.unlocked', 1);
  global_set('td.best', 0);
  global_set('td.gold', 0);
  global_set('td.lives', 0);
  global_set('td.level', 1);
  global_set('td.wave', 0);
  global_set('td.wave_total', 0);
  global_set('td.score', 0);
  global_set('td.kills', 0);
  global_set('td.result', 0);
  global_set('td.active_level', 0);
  global_set_text('td.notice', '');
}

function normPath(p) {
  var s = String(p ? p : '').replace(/\\/g, '/');
  while (s.length > 1 && s.charAt(s.length - 1) === '/') s = s.substring(0, s.length - 1);
  return s;
}
function pathJoin(a, b) {
  var x = normPath(a), y = normPath(b);
  if (!x || x === '.') return y;
  return x + '/' + y;
}
function isMenu() {
  return String(scene_title()).indexOf('选关') >= 0;
}

function makeLabel(name, x, y, text, size, color) {
  var id = entity_spawn();
  if (id < 0) return -1;
  node_add_component(id, 'scn.Label');
  node_set_name(id, name);
  node_set_number(id, 'scn.Label', 'x', x);
  node_set_number(id, 'scn.Label', 'y', y);
  node_set_number(id, 'scn.Label', 'font_size', size);
  node_set_string(id, 'scn.Label', 'text', text);
  node_set_number(id, 'scn.Label', 'r', color[0]);
  node_set_number(id, 'scn.Label', 'g', color[1]);
  node_set_number(id, 'scn.Label', 'b', color[2]);
  node_set_number(id, 'scn.Label', 'a', 1.0);
  S.hud.push(id);
  return id;
}

function makeBar(w, h) {
  var id = entity_spawn();
  if (id < 0) return -1;
  node_add_component(id, 'scn.Transform2D');
  node_add_component(id, 'scn.Sprite');
  node_set_name(id, 'HUD_Bar');
  node_set_position(id, w * 0.5, h - 20);
  node_set_number(id, 'scn.Sprite', 'width', w);
  node_set_number(id, 'scn.Sprite', 'height', 40);
  node_set_number(id, 'scn.Sprite', 'r', 0.04);
  node_set_number(id, 'scn.Sprite', 'g', 0.06);
  node_set_number(id, 'scn.Sprite', 'b', 0.10);
  node_set_opacity(id, 0.72);
  S.hud.push(id);
  return id;
}

function destroyHud() {
  for (var i = 0; i < S.hud.length; i++) {
    if (S.hud[i] >= 0 && node_valid(S.hud[i]) > 0) node_destroy(S.hud[i]);
  }
  S.hud = [];
  S.title = -1;
  S.status = -1;
  S.banner = -1;
  S.lastStatus = '';
}

function buildHud() {
  destroyHud();
  var w = screen_w(), h = screen_h();
  if (w <= 0 || h <= 0) return;
  makeBar(w, h);
  S.title = makeLabel('HUD_Title', 10, h - 17, '', 14, [0.95, 0.88, 0.62]);
  S.status = makeLabel('HUD_Status', 10, h - 31, '', 11, [0.72, 0.82, 0.95]);
  updateHud(true);
  log('td_global hud w=' + w + ' h=' + h + ' entities=' + S.hud.length);
}

function updateHud(force) {
  if (S.title < 0 || S.status < 0) return;
  var title, status;
  if (isMenu()) {
    title = '小小塔防 · 选关';
    status = '已解锁 ' + global_get('td.unlocked') + '/3   最高分 ' + global_get('td.best')
           + '   按 1/2/3 选关 · Enter 继续 · R 重置';
  } else {
    var lv = global_get('td.level');
    title = '小小塔防 · 第 ' + lv + ' 关';
    status = '金币 ' + global_get('td.gold') + '   生命 ' + global_get('td.lives')
           + '   波次 ' + global_get('td.wave') + '/' + global_get('td.wave_total')
           + '   分数 ' + global_get('td.score') + '   击杀 ' + global_get('td.kills');
  }
  if (force || status !== S.lastStatus) {
    node_set_string(S.title, 'scn.Label', 'text', title);
    node_set_string(S.status, 'scn.Label', 'text', status);
    S.lastStatus = status;
  }
}

function makeBanner(text, color) {
  var w = screen_w(), h = screen_h();
  var size = 30;
  var approx = 0;
  for (var i = 0; i < text.length; i++) approx += (text.charCodeAt(i) >= 0x2e80 ? 1.0 : 0.58) * size;
  var x = Math.max(8, (w - approx) * 0.5);
  S.banner = makeLabel('HUD_Banner', x, h * 0.5 - 14, text, size, color);
}

// ---- 生命周期 -------------------------------------------------------------
function on_start() {
  S.base = normPath(scene_dir());
  if (!S.base) S.base = '.';
  S.finished = false;
  S.timer = 0;
  S.returned = false;
  // 回到菜单 = 上一局已结束：清掉 result，避免全局 update 再次结算。
  if (isMenu()) global_set('td.result', 0);
  buildHud();
  log('td_global ready scene=' + JSON.stringify(scene_title()) + ' menu=' + (isMenu() ? 1 : 0)
      + ' level=' + global_get('td.level') + ' unlocked=' + global_get('td.unlocked')
      + ' best=' + global_get('td.best') + ' base=' + JSON.stringify(S.base));
}

function on_key_down(target, key) {
  var k = (typeof key === 'number') ? key : -1;
  if (k === 1) {                                   // Esc：回选关
    if (!isMenu()) {
      log('td_global hotkey=menu');
      director_load_scene(pathJoin(S.base, 'menu.scn'));
    }
    return;
  }
  if (k === 82) {                                  // R：重开当前关
    if (!isMenu() && S.timer <= 0) {
      var lv = global_get('td.level');
      var path = pathJoin(S.base, 'level' + lv + '.scn');
      log('td_global hotkey=restart path=' + path);
      director_load_scene(path);
    }
  }
}

// 关卡脚本通过 global_emit('level_finished', 1=胜 / 2=负) 调用这里。
function level_finished(id, result) {
  if (S.finished) return;
  S.finished = true;
  var lv = global_get('td.level');
  if (lv < 1) lv = 1;
  var score = global_get('td.score');
  global_set('td.result', result);
  var notice = '';
  if (result === 1) {
    var unlocked = global_get('td.unlocked');
    var nu = lv + 1;
    if (nu > 3) nu = 3;
    if (nu > unlocked) unlocked = nu;
    global_set('td.unlocked', unlocked);
    if (score > global_get('td.best')) global_set('td.best', score);
    notice = (lv >= 3) ? ('全部通关！总分 ' + score)
                       : ('第 ' + lv + ' 关完成：得分 ' + score + '，已解锁第 ' + unlocked + ' 关');
  } else {
    notice = '第 ' + lv + ' 关失败：生命归零。再试一次？';
  }
  global_set_text('td.notice', notice);
  S.timer = 2.4;
  makeBanner(result === 1 ? '关卡完成' : '防守失败', result === 1 ? [0.55, 1.0, 0.62] : [1.0, 0.48, 0.48]);
  log('td_global result=' + result + ' level=' + lv + ' score=' + score
      + ' unlocked=' + global_get('td.unlocked') + ' timer=' + S.timer
      + ' frame=' + director_frame());
}

function update(dt) {
  updateHud(false);
  if (S.timer > 0) {
    S.timer -= dt;
    if (S.timer <= 0 && !S.returned) {
      S.returned = true;
      var path = pathJoin(S.base, 'menu.scn');
      log('td_global return path=' + path);
      director_load_scene(path);
    }
  }
}
