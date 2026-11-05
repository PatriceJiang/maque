'use strict';
// 世界坐标 y 向上；每个 probe 同时检查“该命中/不该命中”。
function probe(name, x, y, want) {
  const v = ui_hit_at(x, y);
  const ok = want ? (v >= 0) : (v < 0);
  log('hit_' + name + '=' + v + (ok ? ' OK' : ' FAIL'));
}
function on_start() {
  // 外层 normal 120×120，内层 reverse 40×40：环带命中、洞内不命中、外层外不命中。
  probe('nested_ring', 30.0, 210.0, true);
  probe('nested_hole', 80.0, 260.0, false);
  probe('nested_out', 150.0, 250.0, false);
  // clip_children=false：自身按钮被裁，子按钮不受影响。
  probe('self_in', 320.0, 220.0, true);
  probe('self_out', 290.0, 220.0, false);
  probe('child_hit', 400.0, 220.0, true);
}
