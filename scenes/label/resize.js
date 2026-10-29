// resize.js —— scn.Label 窗口 resize 布局不变性回归（tests/label_resize_e2e.sh）。
// 布局全部在**设计像素**空间：窗口 resize（ss 变化）只改字形栅格化倍率，**不应**改变
// text_w/text_h/line_count/fit_font_size。第 3 帧（resize 前）与第 9 帧（resize 后）各 log 一行
// `label_resize_summary {"stage":3|9,"design":[w,h],"cases":{...}}` 供脚本逐字段比对。
'use strict';

const LONG = 'the quick brown fox jumps over the lazy dog and then stops to look around';
const MIXED_UNIT = '中文混排 mixed English 换行 wrap 宽度 160 ';

function genMixed(n) {
  let s = '';
  while (Array.from(s).length < n) s += MIXED_UNIT;
  return Array.from(s).slice(0, n).join('');
}

const TEXTS = {
  wrap: LONG,
  shrink: LONG,
  mixed: genMixed(200),
  single: 'single line'
};

const CASES = ['wrap', 'shrink', 'mixed', 'single'];
const METRIC_FIELDS = ['text_w', 'text_h', 'line_count', 'fit_font_size',
                       'font_size', 'x', 'y', 'max_width', 'max_height'];

const S = { frames: 0, ids: {} };

function on_start() {
  for (const name of CASES) {
    const id = entity_find(name);
    if (id < 0) continue;
    S.ids[name] = id;
    if (name in TEXTS) node_set_string(id, 'scn.Label', 'text', TEXTS[name]);
  }
  log('resize.js ready (' + Object.keys(S.ids).length + ' cases)');
}

function summary(stage) {
  const cases = {};
  for (const name of CASES) {
    if (!(name in S.ids)) continue;
    const id = S.ids[name];
    const rec = {};
    for (const f of METRIC_FIELDS) rec[f] = node_get_number(id, 'scn.Label', f);
    cases[name] = rec;
  }
  return { stage: stage, design: [screen_w(), screen_h()], cases: cases };
}

function update() {
  S.frames += 1;
  if (S.frames === 3 || S.frames === 9) {
    const stage = S.frames === 3 ? 3 : 9;
    log('label_resize_summary ' + JSON.stringify(summary(stage)));
  }
}
