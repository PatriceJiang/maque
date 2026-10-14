// nav.js —— 建导航网格 → 指定目标 → 引擎按固定步长寻路/追踪（docs/15 导航）
'use strict';

function on_start() {
  nav_setup(20, 15, 16);                  // 20×15 格，每格 16 像素（覆盖 320×240）
  nav_block(6, 0, 1, 5);                  // 一堵墙（x=6，y 0..4）
  const walker = entity_find('Walker');
  nav_goto(walker, 280.0, 200.0);         // 目标在墙的右下
  log('nav.js ready: walker=' + walker);
}
function update() {
  nav_tick();                             // 固定步长推进（内部 A* + 路径平滑 + 追踪）
  if (input_key(1) > 0) quit();
}
