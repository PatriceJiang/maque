'use strict';
// 证明 ListView 内的 scn.Button 可被命中：按下变色，页脚显示 target。
let down = -1;
let foot = -1;
let orig_r = 0.21, orig_g = 0.28, orig_b = 0.44;
function on_start() { foot = node_find_path(-1, 'foot'); }
function on_pointer_down(target, x, y) {
  if (down >= 0 || target < 0) return;
  if (node_get_number(target, 'scn.Button', 'width') <= 0.0) return;
  down = target;
  orig_r = node_get_number(target, 'scn.Button', 'r');
  orig_g = node_get_number(target, 'scn.Button', 'g');
  orig_b = node_get_number(target, 'scn.Button', 'b');
  node_set_number(target, 'scn.Button', 'r', 1.0);
  node_set_number(target, 'scn.Button', 'g', 0.62);
  node_set_number(target, 'scn.Button', 'b', 0.16);
  if (foot >= 0) node_set_string(foot, 'scn.Label', 'text', 'button DOWN · target=' + target);
}
function on_pointer_up(target, x, y) {
  if (down < 0) return;
  node_set_number(down, 'scn.Button', 'r', orig_r);
  node_set_number(down, 'scn.Button', 'g', orig_g);
  node_set_number(down, 'scn.Button', 'b', orig_b);
  if (foot >= 0) node_set_string(foot, 'scn.Label', 'text', 'button UP · target=' + down);
  down = -1;
}
