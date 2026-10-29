// style.js —— scn.Label 专用**样式**回归（与 scenes/text/text.js 无关，独立用例）。
// 像素断言在 tests/label_style_e2e.sh（颜色/透明/bold/italic/shadow/outline 各用唯一 band 隔离）；
// 脚本在固定帧 log 一行 `label_summary {json}` 提供排版度量（证明样式不影响排版确定性）。
'use strict';

const CASES = ['color', 'alpha_full', 'alpha_half', 'alpha_zero',
               'bold_normal', 'bold_on', 'italic_normal', 'italic_on',
               'shadow', 'outline'];

const FIELDS = ['text_w', 'text_h', 'line_count', 'font_size', 'x', 'y'];

const S = { frames: 0 };

function dump() {
  const out = {};
  for (const name of CASES) {
    const id = entity_find(name);
    const rec = { id: id };
    for (const f of FIELDS) rec[f] = node_get_number(id, 'scn.Label', f);
    out[name] = rec;
  }
  return out;
}

function on_start() {
  log('style.js ready');
}

function update() {
  S.frames += 1;
  if (S.frames === 6) log('label_summary ' + JSON.stringify(dump()));
}
