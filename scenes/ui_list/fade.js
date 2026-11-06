'use strict';
// 演示整体淡入/淡出：content_alpha 只影响 ListView 内容，不影响滚动条/百分比。
let list = -1;
let t = 0;
function on_start() { list = node_find_path(-1, 'list'); }
function update(dt) {
  t += dt;
  if (list >= 0) {
    const a = 0.35 + 0.65 * (0.5 + 0.5 * Math.sin(t * 1.6));
    node_set_number(list, 'ui.ListView', 'content_alpha', a);
  }
}
