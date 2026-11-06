// 2D 物理射线回归（scenes/pick/ray2d.scn）：静态盒 wall 中心 (160,120)、半宽/半高 20。
// 左面 x=140 → 从 (0,120) 向 +x 射 500 → t=140 命中；max_distance=100 → 未命中；
// y=200 的平行射线 → 未命中；从盒中心出发 → t=0 命中。
function update() {
  var f = director_frame();
  if (f < 4) return;
  if (f > 4) return;
  var wall = entity_find('wall');
  var hit = physics_raycast(0, 120, 1, 0, 500);
  var miss = physics_raycast(0, 200, 1, 0, 500);
  var short = physics_raycast(0, 120, 1, 0, 100);
  var inside = physics_raycast(160, 120, 1, 0, 500);
  var ok = (hit === wall) && (miss === -1) && (short === -1) && (inside === wall);
  log('[ray2d] ok=' + (ok ? 1 : 0) + ' wall=' + wall + ' hit=' + hit + ' miss=' + miss +
      ' short=' + short + ' inside=' + inside);
  quit();
}
