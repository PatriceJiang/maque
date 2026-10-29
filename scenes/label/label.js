// label.js —— scn.Label 专用**排版**回归（与 scenes/text/text.js 无关，独立用例）。
// 在固定帧把每个用例的实测排版度量 log 成一行 `label_summary {json}`，
// 由 tests/label_layout_e2e.sh 结合 PPM 像素断言（换行 / 对齐 / 宽度限制 / 高度限制）。
'use strict';

const CASES = [
  'wrap_word',      // 换行（Word）：max_width=120 长文本
  'wrap_none',      // 对照：同文本 wrap=None，单行 + text_w 夹到 max_width
  'align_begin',    // 对齐 Begin
  'align_center',   // 对齐 Center
  'align_end',      // 对齐 End
  'wrap_limit',     // 宽度限制：max_width=100 + wrap Word
  'shrink',         // 高度限制 + Shrink：font_size=24 / max_width=140 / max_height=60
  'shrink_control', // 对照：同文本同宽，**无**高度限制
  'clip'            // 高度限制 + Clip：同上但 overflow=Clip
];

const FIELDS = ['text_w', 'text_h', 'line_count', 'fit_font_size',
                'x', 'y', 'max_width', 'max_height', 'font_size', 'align', 'wrap'];

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
  log('label.js ready');
}

function update() {
  S.frames += 1;
  if (S.frames === 6) log('label_summary ' + JSON.stringify(dump()));
}
