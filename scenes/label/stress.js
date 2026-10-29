// stress.js —— label 套件 **stress 子例**共享脚本（stress_lines / stress_heights / stress_mixed）。
// on_start 用 node_set_string 写入各用例文本；第 6 帧 log 一行 `label_stress_summary {json}`：
//   { "scene": "...", "cases": { name: {text_w,text_h,line_count,fit_font_size,font_size,x,y,
//     max_width,max_height} }, "mixed": {chars,cjk,latin} }
// 只对本场景实际存在的用例名生效（entity_find < 0 跳过）→ 同一份脚本服务 3 个场景。
// 所有文本内容与生成长度都是**确定性**的（固定 unit 重复 + 截断到恰好 1000 个 code point）。
'use strict';

// 宽度/高度扫描共用的长英文句（5×the quick brown fox + 收尾；约 234 字符）
const LONG = ('the quick brown fox jumps over the lazy dog ').repeat(5) + 'and then stops.';
// 显式换行：5 行
const MULTILINE = 'line one\nline two\nline three\nline four\nline five';
// 单行对照（同字体同字号）
const SINGLE = 'single line reference';
// 纯中文对照（验证 CJK 字体真的加载；系统字体无 CJK 时会退化为方块/缺失）
const CJK = '中文排版测试';

const MIXED_UNIT = '中文排版测试 mixed English words 换行对齐宽度 height 限制 ';

// 确定性生成恰好 1000 个 code point 的中英混排文本，并统计 CJK / 拉丁字符数
function genMixed() {
  let s = '';
  while (Array.from(s).length < 1000) s += MIXED_UNIT;
  const chars = Array.from(s).slice(0, 1000);
  let cjk = 0, latin = 0;
  for (const ch of chars) {
    const cp = ch.codePointAt(0);
    if (cp >= 0x4E00 && cp <= 0x9FFF) cjk++;
    else if (cp >= 0x20 && cp <= 0x7E) latin++;
  }
  return { text: chars.join(''), chars: chars.length, cjk: cjk, latin: latin };
}

const MIXED = genMixed();

// 用例名 → 文本（未列出的名字不设置，保持场景里写的默认文本）
const CASE_TEXT = {
  single_line: SINGLE,
  multiline: MULTILINE,
  width_narrow: LONG,
  width_mid: LONG,
  width_wide: LONG,
  height_40: LONG,
  height_80: LONG,
  height_160: LONG,
  height_control: LONG,
  mixed_full: MIXED.text,
  mixed_shrink: MIXED.text,
  mixed_clip: MIXED.text,
  cjk_control: CJK,
  // 混排基线：行内不同字号共享同一基线（大字号向上伸出，底部行一致）
  baseline_mixed: 'HH<size=32>H</size>HH',
  baseline_plain: 'HHHHH'
};

const CASE_ORDER = ['single_line', 'multiline', 'width_narrow', 'width_mid', 'width_wide',
                    'height_40', 'height_80', 'height_160', 'height_control',
                    'mixed_full', 'mixed_shrink', 'mixed_clip', 'cjk_control',
                    'baseline_mixed', 'baseline_plain'];

const METRIC_FIELDS = ['text_w', 'text_h', 'line_count', 'fit_font_size',
                       'font_size', 'x', 'y', 'max_width', 'max_height'];

const S = { frames: 0 };

function on_start() {
  S.ids = {};
  for (const name of CASE_ORDER) {
    const id = entity_find(name);
    if (id < 0) continue;
    S.ids[name] = id;
    if (name in CASE_TEXT) node_set_string(id, 'scn.Label', 'text', CASE_TEXT[name]);
  }
  log('stress.js ready (' + Object.keys(S.ids).length + ' cases)');
}

function summary() {
  const cases = {};
  for (const name of CASE_ORDER) {
    if (!(name in S.ids)) continue;
    const id = S.ids[name];
    const rec = {};
    for (const f of METRIC_FIELDS) rec[f] = node_get_number(id, 'scn.Label', f);
    cases[name] = rec;
  }
  let scene = 'stress_lines';
  if ('mixed_full' in S.ids) scene = 'stress_mixed';
  else if ('baseline_mixed' in S.ids) scene = 'stress_baseline';
  else if ('height_40' in S.ids) scene = 'stress_heights';
  const out = { scene: scene, cases: cases };
  if ('mixed_full' in S.ids) out.mixed = { chars: MIXED.chars, cjk: MIXED.cjk, latin: MIXED.latin };
  return out;
}

function update() {
  S.frames += 1;
  if (S.frames === 6) log('label_stress_summary ' + JSON.stringify(summary()));
}
