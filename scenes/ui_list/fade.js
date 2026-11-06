'use strict';
// 演示一次淡入：content_alpha 从 0 → 1，完成后保持 1（不做持续正弦，避免整块内容闪烁）。
// 过渡区间由 ListView 的 fade_top/fade_bottom 决定；fade_unit 可选 Pixels / Percent。
let list = -1;
let t = 0;
function on_start() { list = node_find_path(-1, 'list'); }
function update(dt) {
  if (list < 0) return;
  t += dt;
  node_set_number(list, 'ui.ListView', 'content_alpha', Math.min(1, t / 1.0));
}
