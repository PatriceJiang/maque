// 3D 拾取（导入资产）：scenes/pbr/pick_imported.scn
//   mesh_cube = assets/cube.mesh.json（bounds ±0.6），gltf_box = assets/sphere.gltf（bounds ±0.5）
//   两者都绕 Z 旋转 45°、缩放 0.5 → 世界半宽 0.3/0.25，旋转后 x 向投影 ≈0.424/0.354。
//   · 中心点击（100 / 220）必须命中；
//   · 旋转后的边缘点击（122 / 239）也必须命中 —— 不旋转时 0.3*60=18px / 0.25*60=15px，
//     这些点落在本地 AABB 之外 → 这条断言能抓住"忽略节点旋转/世界矩阵"的回归；
//   · 两物之间（160）与正上方（100,220）必须未命中。
function update() {
  var f = director_frame();
  if (f < 5) return;
  if (f > 5) return;
  var a = scene_pick(100, 120);
  var b = scene_pick(220, 120);
  var a_edge = scene_pick(122, 120);
  var b_edge = scene_pick(239, 120);
  var gap = scene_pick(160, 120);
  var above = scene_pick(100, 220);
  var ea = entity_find('mesh_cube'), eb = entity_find('gltf_box');
  var ok = (a === ea) && (b === eb) && (a_edge === ea) && (b_edge === eb) &&
           (gap === -1) && (above === -1);
  log('[pick_imported] ok=' + (ok ? 1 : 0) + ' a=' + a + ' b=' + b + ' aEdge=' + a_edge +
      ' bEdge=' + b_edge + ' gap=' + gap + ' above=' + above + ' expect=' + ea + ',' + eb);
  quit();
}
