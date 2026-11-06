// 3D 拾取（透视相机）：scenes/pick/perspective.scn
//   相机 (0,0,5)、fov_y=60°、320x240 → z=0 处可见半宽 = 5·tan(30°)·(4/3) ≈ 3.849。
//   cube 缩放 0.5 → 世界半宽 0.3 → 屏幕半宽 ≈12.5px；
//   · (160,120) 中心命中；
//   · (150,120) 世界 x≈-0.24（仍在 cube 内）命中；
//   · (140,120) 世界 x≈-0.48（cube 外）未命中；
//   · (160,60)  世界 y≈1.44（cube 上方）未命中。
// 这条用例专门覆盖透视投影的逆变换（ortho 用例覆盖不到 NDC w 的齐次除法）。
function update() {
  var f = director_frame();
  if (f < 5) return;
  if (f > 5) return;
  var cube = entity_find('cube');
  var center = scene_pick(160, 120);
  var near_edge = scene_pick(150, 120);
  var outside = scene_pick(140, 120);
  var above = scene_pick(160, 60);
  var ok = (center === cube) && (near_edge === cube) && (outside === -1) && (above === -1);
  log('[pick_persp] ok=' + (ok ? 1 : 0) + ' center=' + center + ' nearEdge=' + near_edge +
      ' outside=' + outside + ' above=' + above + ' expect=' + cube);
  quit();
}
