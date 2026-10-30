'use strict';
// 菜单场景脚本：选关 / 继续 / 重置进度。
//
// 组合要点：
//   · 本场景的按钮与文字都在 menu.scn 里（独立场景资产）；
//   · 全局状态 td.unlocked / td.best / td.notice 由全局脚本（global.js）维护，这里只读写；
//   · director_load_scene 切到独立关卡 .scn（关卡再组合塔与敌人场景）。
var S = { base: '', ids: {}, started: false };

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

function clampLevel(n) {
  n = Math.floor(n);
  if (n < 1) n = 1;
  if (n > 3) n = 3;
  return n;
}

function cacheIds() {
  S.ids = {
    progress: entity_find('Progress'),
    notice: entity_find('Notice'),
    btn1: entity_find('Btn1'), btn2: entity_find('Btn2'), btn3: entity_find('Btn3'),
    lab1: entity_find('L1Label'), lab2: entity_find('L2Label'), lab3: entity_find('L3Label'),
    play: entity_find('BtnPlay'), reset: entity_find('BtnReset')
  };
}

function setButton(btn, r, g, b) {
  if (btn < 0) return;
  node_set_number(btn, 'scn.Button', 'r', r);
  node_set_number(btn, 'scn.Button', 'g', g);
  node_set_number(btn, 'scn.Button', 'b', b);
}

function setLabelColor(id, r, g, b) {
  if (id < 0) return;
  node_set_number(id, 'scn.Label', 'r', r);
  node_set_number(id, 'scn.Label', 'g', g);
  node_set_number(id, 'scn.Label', 'b', b);
}

function refresh() {
  var unlocked = clampLevel(global_get('td.unlocked'));
  var best = global_get('td.best');
  if (S.ids.progress >= 0) {
    node_set_string(S.ids.progress, 'scn.Label', 'text',
      '已解锁 ' + unlocked + '/3    最高分 ' + best);
  }
  var btns = [S.ids.btn1, S.ids.btn2, S.ids.btn3];
  var labs = [S.ids.lab1, S.ids.lab2, S.ids.lab3];
  for (var i = 0; i < 3; i++) {
    var locked = (i + 1) > unlocked;
    if (locked) {
      setButton(btns[i], 0.18, 0.18, 0.20);
      setLabelColor(labs[i], 0.72, 0.74, 0.78);
      node_set_string(labs[i], 'scn.Label', 'text', '第 ' + (i + 1) + ' 关 (锁)');
    } else {
      setButton(btns[i], 0.14, 0.34, 0.26);
      setLabelColor(labs[i], 0.88, 1.0, 0.90);
      node_set_string(labs[i], 'scn.Label', 'text', '第 ' + (i + 1) + ' 关');
    }
  }
  if (S.ids.notice >= 0) {
    var notice = global_get_text('td.notice');
    if (!notice) notice = '选择关卡，守住终点。';
    node_set_string(S.ids.notice, 'scn.Label', 'text', notice);
  }
}

function startLevel(n) {
  if (S.started) return;
  n = clampLevel(n);
  var unlocked = clampLevel(global_get('td.unlocked'));
  if (n > unlocked) {
    global_set_text('td.notice', '第 ' + n + ' 关还没解锁：先通过第 ' + unlocked + ' 关。');
    refresh();
    log('td_menu locked level=' + n + ' unlocked=' + unlocked);
    return;
  }
  S.started = true;
  global_set('td.level', n);
  global_set('td.score', 0);
  global_set('td.kills', 0);
  global_set('td.wave', 0);
  global_set('td.wave_total', 0);
  global_set('td.result', 0);
  global_set_text('td.notice', '');
  var path = pathJoin(S.base, 'level' + n + '.scn');
  log('td_menu start level=' + n + ' path=' + path);
  director_load_scene(path);
}

function resetProgress() {
  global_set('td.unlocked', 1);
  global_set('td.best', 0);
  global_set('td.score', 0);
  global_set('td.kills', 0);
  global_set('td.wave', 0);
  global_set('td.wave_total', 0);
  global_set('td.result', 0);
  global_set_text('td.notice', '进度已重置：只解锁第 1 关。');
  refresh();
  log('td_menu reset unlocked=1 best=0');
}

// ---- 生命周期 -------------------------------------------------------------
function on_start() {
  S.base = normPath(scene_dir());
  if (!S.base) S.base = '.';
  S.started = false;
  cacheIds();
  refresh();
  log('td_menu ready unlocked=' + clampLevel(global_get('td.unlocked'))
      + ' best=' + global_get('td.best') + ' base=' + JSON.stringify(S.base));
}

function on_pointer_down(target, x, y) {
  if (target === S.ids.btn1) startLevel(1);
  else if (target === S.ids.btn2) startLevel(2);
  else if (target === S.ids.btn3) startLevel(3);
  else if (target === S.ids.play) startLevel(global_get('td.unlocked'));
  else if (target === S.ids.reset) resetProgress();
}

function on_key_down(target, key) {
  var k = (typeof key === 'number') ? key : -1;
  if (k === 49) startLevel(1);
  else if (k === 50) startLevel(2);
  else if (k === 51) startLevel(3);
  else if (k === 3) startLevel(global_get('td.unlocked'));   // Enter
  else if (k === 82) resetProgress();                         // R
}

function on_destroy() {
  log('td_menu destroy');
}
