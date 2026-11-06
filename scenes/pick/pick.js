// 3D 拾取回归（scenes/pick/pick.scn）：正交相机 320x240 / ortho_height=4 →
// 世界 x 轴在屏幕上 ±2.6667，cube_a 中心 x=-1 → 设计坐标 100，cube_b 中心 x=+1 → 220。
function update() {
  var f = director_frame();
  if (f < 5) return;
  if (f > 5) return;
  var a = scene_pick(100, 120);          // cube_a 中心
  var b = scene_pick(220, 120);          // cube_b 中心
  var gap = scene_pick(160, 120);        // 两 cube 之间（背景）
  var above = scene_pick(100, 220);      // cube_a 正上方（背景）
  var edge = scene_pick(60, 120);        // cube_a 左侧（背景）
  var ray_a = scene_raycast(0, 0, 5, -1, 0, -5, 100);   // 世界射线 → cube_a
  var ray_b = scene_raycast(0, 0, 5, 1, 0, -5, 100);    // world ray → cube_b
  var ray_gap = scene_raycast(0, 0, 5, 0, 0, -5, 100);  // 中间空隙
  var ray_short = scene_raycast(0, 0, 5, -1, 0, -5, 4); // 距离限制 → 未到 cube
  var ea = entity_find('cube_a'), eb = entity_find('cube_b');
  var ok = (a === ea) && (b === eb) && (gap === -1) && (above === -1) && (edge === -1) &&
           (ray_a === ea) && (ray_b === eb) && (ray_gap === -1) && (ray_short === -1);
  log('[pick] ok=' + (ok ? 1 : 0) + ' a=' + a + ' b=' + b + ' gap=' + gap + ' above=' + above +
      ' edge=' + edge + ' rayA=' + ray_a + ' rayB=' + ray_b + ' rayGap=' + ray_gap +
      ' rayShort=' + ray_short + ' expect=' + ea + ',' + eb);
  quit();
}
