// 4x4 拾取 demo（scenes/pick_demo/pick_demo.scn）
//   · 16 个独立对象（8 cube + 8 sphere），每个自带不同 asset.PbrMaterial；
//   · 点击 → scene_pick 命中最近对象 → 该对象换色（base_color + emissive）+ 日志 + HUD；
//   · 同一对象连续点击会按调色板轮换，HUD 显示点击次数。
//
// 坐标：scene_pick 的 x/y 与 input_x/input_y 同一套（设计/世界坐标，y 向上，原点左下）。
// 点击用 on_pointer_down 回调（引擎把指针事件发给根脚本；target=-1 表示没有命中 2D UI）。

var HUD = -1;
var HITS = {};

// 线性 RGB（asset.PbrMaterial 是 linear 语义；渲染器再做 ACES + sRGB 输出）
var PALETTE = [
  [1.00, 0.10, 0.06],   // 0 red
  [0.08, 0.75, 0.22],   // 1 green
  [0.06, 0.35, 1.00],   // 2 blue
  [1.00, 0.72, 0.04],   // 3 yellow
  [0.52, 0.16, 0.95],   // 4 violet
  [0.03, 0.70, 0.85],   // 5 cyan
];

function on_start() {
  HUD = entity_find('hud_status');
  log('[pick_demo] ready shapes=16 palette=' + PALETTE.length);
}

function on_pointer_down(target, x, y) {
  var id = scene_pick(x, y);
  if (id < 0) {
    log('[pick_demo] miss x=' + x.toFixed(2) + ' y=' + y.toFixed(2));
    set_label(HUD, 'last: miss at (' + x.toFixed(1) + ', ' + y.toFixed(1) + ')');
    return;
  }
  var n = HITS[id] || 0;
  HITS[id] = n + 1;
  var c = PALETTE[n % PALETTE.length];
  node_set_number(id, 'asset.PbrMaterial', 'base_color.r', c[0]);
  node_set_number(id, 'asset.PbrMaterial', 'base_color.g', c[1]);
  node_set_number(id, 'asset.PbrMaterial', 'base_color.b', c[2]);
  node_set_number(id, 'asset.PbrMaterial', 'emissive.x', c[0] * 0.22);
  node_set_number(id, 'asset.PbrMaterial', 'emissive.y', c[1] * 0.22);
  node_set_number(id, 'asset.PbrMaterial', 'emissive.z', c[2] * 0.22);
  node_set_number(id, 'asset.PbrMaterial', 'metallic', 0.15);
  node_set_number(id, 'asset.PbrMaterial', 'roughness', 0.32);

  var name = entity_name(id);
  var r = node_get_number(id, 'asset.PbrMaterial', 'base_color.r');
  log('[pick_demo] hit id=' + id + ' name=' + name + ' palette=' + (n % PALETTE.length) +
      ' clicks=' + HITS[id] + ' r=' + r.toFixed(2));
  set_label(HUD, 'hit ' + name + '  ·  palette ' + (n % PALETTE.length) + '  ·  clicks ' + HITS[id]);
}
