// canvas.js —— 画布/分辨率策略演示（docs/17）：UI 按**设计分辨率**排版，运行时适配任意窗口/显示器。
// 场景设计分辨率 480x270；四个角与中心元素都按设计坐标放置，引擎负责映射到设备像素。
// 脚本可读 canvas_scale()/window_w()/canvas_offset_x() 等度量，据此做自适应排版（示例：信息行）。
'use strict';

const S = { log: [] };

function on_start() {
  S.canvas = entity_find('Canvas');
  S.info = entity_find('Info');
  S.probe = entity_find('MetricsProbe');
  S.btn = entity_find('HitButton');
  S.hits = 0;
  log('canvas.js ready design=' + screen_w() + 'x' + screen_h());
}

// 按钮命中（引擎按 role 做命中测试；这里演示"设计坐标"下的事件）
function on_click(target, x, y) {
  S.last_target = target;
  S.last_x = x; S.last_y = y;
  S.hits = (S.hits || 0) + 1;
  log('canvas click target=' + target + ' design=(' + Math.round(x) + ',' + Math.round(y) + ')');
}

function update() {
  S.frames = (S.frames || 0) + 1;
  if (S.frames % 30 === 0) {
    // 自适应排版示例：把"设计坐标"的度量写进信息行（窗口越大 scale 越大）
    const txt = 'scale=' + canvas_scale().toFixed(3)
              + ' win=' + Math.round(window_w()) + 'x' + Math.round(window_h())
              + ' off=' + Math.round(canvas_offset_x()) + ',' + Math.round(canvas_offset_y())
              + ' hits=' + (S.hits || 0);
    node_set_string(S.info, 'scn.Label', 'text', txt);
    S.summary = txt;
  }
  if (S.frames === 90) log('canvas_summary ' + JSON.stringify({
    scale: canvas_scale(), scale_x: canvas_scale_x(), scale_y: canvas_scale_y(),
    off_x: canvas_offset_x(), off_y: canvas_offset_y(),
    win_w: window_w(), win_h: window_h(), design_w: screen_w(), design_h: screen_h(),
    device_scale: canvas_device_scale(), hits: S.hits || 0,
    // 文本度量必须与窗口缩放无关（设备像素只用于栅格化，度量换算回设计像素）
    probe_w: ui_text_width(S.probe), probe_h: ui_text_height(S.probe),
    last_target: S.last_target === undefined ? -1 : S.last_target,
    last_x: S.last_x === undefined ? -1 : S.last_x, last_y: S.last_y === undefined ? -1 : S.last_y
  }));
}
