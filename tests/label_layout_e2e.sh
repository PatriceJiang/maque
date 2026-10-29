#!/usr/bin/env bash
# scn.Label 专用**排版**端到端回归（docs/17/18；与 text_e2e.sh 相互独立）。
# 场景 scenes/label/label.scn：9 个 Label，各用唯一颜色 + 唯一 band 隔离，脚本在固定帧
# log 一行 `label_summary {json}` 提供实测排版度量（text_w/text_h/line_count/fit_font_size…），
# 本脚本同时断言度量与 PPM 像素（字形真的按期望落位/被裁剪）。
# 覆盖：
#   a) 换行（Word）：max_width=120 长文本 → 多行、text_h 明显高于单行；wrap=None 对照单行
#   b) 对齐 Begin/Center/End：同文本/max_width=200 同 x → 像素 bbox 左边界单调右移且差 >20px
#   c) 宽度限制：max_width=100 + wrap Word → text_w<=100、行数>=3、像素不越过 max_width
#   d) 高度限制 Shrink：font_size=24/max_width=140/max_height=60 → fit_font_size<24、text_h<=60；
#      无高度限制对照 text_h>60
#   e) 高度限制 Clip：同上 overflow=Clip → fit_font_size=0、text_h<=60、ink 不越过 band 下界，
#      且墨迹像素数显著少于未裁剪对照
# 最后：同命令两跑 world_trail 一致（确定性，docs/10）。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK/frames"
export MAQUE_HEADLESS=1

fail() { echo "label_layout_e2e: FAIL $*" >&2; exit 1; }

MAQUE_MAX_FRAMES=12 "$PLAYER" "$SCENE" "$WORK/frames" > "$WORK/out.txt" 2>&1 || true

SUMMARY=$(grep -o 'label_summary .*' "$WORK/out.txt" | head -1)
[ -n "$SUMMARY" ] || fail "script summary not found (label.js 应在第 6 帧 log label_summary)"
FRAME=$(ls "$WORK/frames"/frame_*.ppm 2>/dev/null | sort | tail -1)
[ -n "$FRAME" ] || fail "no PPM frame produced (need renderer)"

"$PY" - "$SUMMARY" "$FRAME" <<'PY' || exit 1
import json, sys

d = json.loads(sys.argv[1][len('label_summary '):])
raw = open(sys.argv[2], 'rb').read()
parts = raw.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]

def at(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]

BG = at(1, 1)          # 场景背景（默认深色；不硬编码 RGB，按帧取）

bad = []
def ck(name, cond, got=None):
    if not cond:
        bad.append(f"{name} (got {got})")

def case(name):
    """取用例度量；缺失直接算失败（避免 KeyError 掩盖问题）"""
    if name not in d:
        bad.append(f"[{name}] 用例缺失（label_summary 里没有该实体）")
        return None
    return d[name]

def region_ink(x0, y0, x1, y1, thresh=30):
    """区域内相对背景的墨迹像素（任一通道与背景差 > thresh）"""
    x0 = max(0, int(x0)); y0 = max(0, int(y0))
    x1 = min(w, int(x1)); y1 = min(h, int(y1))
    out = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = at(x, y)
            if max(abs(p[0] - BG[0]), abs(p[1] - BG[1]), abs(p[2] - BG[2])) > thresh:
                out.append((x, y, p))
    return out

def bbox(ink):
    xs = [p[0] for p in ink]; ys = [p[1] for p in ink]
    return min(xs), min(ys), max(xs), max(ys)

def band_rows(m, pad_bottom=8):
    top = h - m['y']
    return top, top + m['text_h'] + pad_bottom

# ---------------- a) 换行：Word vs None ----------------
ww = case('wrap_word'); wn = case('wrap_none')
if ww and wn:
    ck('a) wrap_word: 折行成多行', ww['line_count'] >= 2, ww['line_count'])
    ck('a) wrap_word: text_h 明显高于单行', ww['text_h'] >= 1.8 * wn['text_h'],
       (ww['text_h'], wn['text_h']))
    ck('a) wrap_word: text_w <= max_width+1', ww['text_w'] <= ww['max_width'] + 1, ww['text_w'])
    ck('a) wrap_none: 单行', wn['line_count'] == 1, wn['line_count'])
    ck('a) wrap_none: text_w 夹到 max_width', wn['text_w'] <= wn['max_width'] + 1, wn['text_w'])
    ck('a) wrap_none: text_h 小于折行版', wn['text_h'] < ww['text_h'], (wn['text_h'], ww['text_h']))
    # 像素：wrap_word 的 ink 不越过 max_width（band x 只取到 wrap_none 起点之前，颜色隔离）
    top, bot = band_rows(ww)
    ink = region_ink(0, top - 2, ww['x'] + ww['max_width'] + 40, bot)
    ck('a) wrap_word: 有字形像素', len(ink) > 100, len(ink))
    if ink:
        _, _, r0, _ = bbox(ink)
        ck('a) wrap_word: 像素不越过 max_width', r0 <= ww['x'] + ww['max_width'] + 3, r0)
    # 像素：wrap_none（单行）不换行 → ink 高度只有一行
    topn, botn = band_rows(wn)
    inkn = region_ink(wn['x'] - 4, topn - 2, w + 1, botn)
    ck('a) wrap_none: 有字形像素（被 max_width 裁剪）', len(inkn) > 50, len(inkn))
    if inkn:
        _, t0, r1, b1 = bbox(inkn)
        ck('a) wrap_none: 单行高度', (b1 - t0) <= wn['text_h'] + 4, (t0, b1, wn['text_h']))
        ck('a) wrap_none: 像素不越过 max_width（wrap=None 裁剪）',
           r1 <= wn['x'] + wn['max_width'] + 3, r1)

# ---------------- b) 对齐 Begin / Center / End ----------------
def color_ink(x0, y0, x1, y1, pred, thresh=25):
    x0 = max(0, int(x0)); y0 = max(0, int(y0))
    x1 = min(w, int(x1)); y1 = min(h, int(y1))
    out = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = at(x, y)
            if max(abs(r - BG[0]), abs(g - BG[1]), abs(b - BG[2])) <= thresh:
                continue
            if pred(r, g, b):
                out.append((x, y))
    return out

ab = case('align_begin'); ac = case('align_center'); ae = case('align_end')
if ab and ac and ae:
    ck('b) 三者 align 值', (ab.get('align'), ac.get('align'), ae.get('align')) == (0, 1, 2),
       (ab.get('align'), ac.get('align'), ae.get('align')))
    top = h - ab['y']
    rows = (top - 4, top + ab['text_h'] + 10)
    xs = (0, 420)
    begin = color_ink(xs[0], rows[0], xs[1], rows[1],
                      lambda r, g, b: r > 100 and g < 0.6 * r and b < 0.6 * r and abs(g - b) < 30)
    center = color_ink(xs[0], rows[0], xs[1], rows[1],
                       lambda r, g, b: g > 100 and r < 0.6 * g and b < 0.6 * g)
    end = color_ink(xs[0], rows[0], xs[1], rows[1],
                    lambda r, g, b: b > 100 and 0.45 * b < r < 0.8 * b and g < 0.55 * b)
    ck('b) align_begin 有像素', len(begin) > 20, len(begin))
    ck('b) align_center 有像素', len(center) > 20, len(center))
    ck('b) align_end 有像素', len(end) > 20, len(end))
    if begin and center and end:
        bl = min(p[0] for p in begin); cl = min(p[0] for p in center); el = min(p[0] for p in end)
        ck('b) 对齐：begin 左边界 < center 左边界 - 20', bl < cl - 20, (bl, cl))
        ck('b) 对齐：center 左边界 < end 左边界 - 20', cl < el - 20, (cl, el))
        ck('b) 对齐：Begin 从文本框左边开始', abs(bl - ab['x']) <= 4, (bl, ab['x']))

# ---------------- c) 宽度限制（wrap Word + max_width=100） ----------------
wl = case('wrap_limit')
if wl:
    line_h = wn['text_h'] if wn else 17.0
    ck('c) 宽度限制：text_w <= 100+1', wl['text_w'] <= wl['max_width'] + 1, wl['text_w'])
    ck('c) 宽度限制：行数 >= 3', wl['line_count'] >= 3, wl['line_count'])
    ck('c) 宽度限制：text_h >= 2.5*单行高', wl['text_h'] >= 2.5 * line_h, (wl['text_h'], line_h))
    top, bot = band_rows(wl)
    ink = region_ink(0, top - 2, 180, bot)
    ck('c) 宽度限制：有字形像素', len(ink) > 100, len(ink))
    if ink:
        _, _, r, b = bbox(ink)
        ck('c) 宽度限制：像素右边界不越过 max_width',
           r <= wl['x'] + wl['max_width'] + 3, (r, wl['x'] + wl['max_width']))
        ck('c) 宽度限制：像素高度多行', (b - (h - wl['y'])) >= 2 * line_h, (b, h - wl['y'], line_h))

# ---------------- d) 高度限制 Shrink ----------------
sh = case('shrink'); sc = case('shrink_control')
if sh and sc:
    ck('d) Shrink：fit_font_size 生效（>0）', sh['fit_font_size'] > 0.5, sh['fit_font_size'])
    ck('d) Shrink：fit_font_size < font_size', sh['fit_font_size'] < sh['font_size'] - 0.5,
       (sh['fit_font_size'], sh['font_size']))
    ck('d) Shrink：text_h <= max_height+0.5', sh['text_h'] <= sh['max_height'] + 0.5,
       (sh['text_h'], sh['max_height']))
    ck('d) Shrink：对照（无高度限制）text_h > 60', sc['text_h'] > 60.0, sc['text_h'])
    ck('d) Shrink：缩字号后确实比对照矮', sh['text_h'] < sc['text_h'], (sh['text_h'], sc['text_h']))
    top, _ = band_rows(sh, pad_bottom=2)
    ink = region_ink(0, top - 2, 180, top + sh['max_height'] + 2)
    ck('d) Shrink：有字形像素', len(ink) > 50, len(ink))
    if ink:
        _, _, _, b = bbox(ink)
        ck('d) Shrink：像素不越过 band 下界', b <= top + sh['max_height'] + 2, (b, top + sh['max_height']))

# ---------------- e) 高度限制 Clip ----------------
cl = case('clip')
if cl and sc:
    ck('e) Clip：fit_font_size = 0（未缩字号）', cl['fit_font_size'] <= 0.5, cl['fit_font_size'])
    ck('e) Clip：text_h 夹到 max_height', cl['text_h'] <= cl['max_height'] + 0.5,
       (cl['text_h'], cl['max_height']))
    ck('e) Clip：排版行数仍按完整文本（未改变排版）', cl['line_count'] >= 2, cl['line_count'])
    top, _ = band_rows(cl, pad_bottom=2)
    band_bottom = top + cl['max_height']
    cyan = color_ink(0, top - 2, 180, h + 1,
                     lambda r, g, b: b > 100 and g > 0.7 * b and r < 0.6 * b)
    ck('e) Clip：有字形像素（前若干行仍绘制）', len(cyan) > 50, len(cyan))
    if cyan:
        _, _, _, b = bbox(cyan)
        ck('e) Clip：ink 不越过 band 下界', b <= band_bottom + 3, (b, band_bottom))
    # 对照（无高度限制、同文本同宽）的墨迹应明显更多 → 证明高度限制真的裁掉了内容
    topc = h - sc['y']
    orange = color_ink(180, topc - 4, 420, min(h, topc + sc['text_h'] + 10),
                       lambda r, g, b: r > 100 and 0.3 * r < g < 0.65 * r and b < 0.4 * r)
    ck('e) 对照（无高度限制）有字形像素', len(orange) > 200, len(orange))
    if cyan and orange:
        ck('e) Clip 墨迹像素数显著少于未裁剪对照', len(cyan) < 0.75 * len(orange),
           (len(cyan), len(orange)))

if bad:
    print('label_layout_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
print(f"label_layout_e2e: 排版断言全部通过（wrap_lines={ww and ww['line_count']} "
      f"wrap_h={ww and ww['text_h']} width_h={wl and wl['text_h']} "
      f"shrink_fit={sh and round(sh['fit_font_size'], 2)} shrink_h={sh and sh['text_h']} "
      f"clip_h={cl and cl['text_h']}）")
PY

# ================= stress 子例（多行 / 宽度扫描 / 高度扫描 / 1000 字中英混排） =================
# 场景由 $(dirname "$SCENE")/stress*.scn 自动发现（按文件名排序），逐个跑并断言；
# 仍然只有 label_layout_e2e 这一条 ctest（CMake 无需为新场景改注册）。
DIR="$(dirname "$SCENE")"
STRESS_COUNT=0
for S in "$DIR"/stress*.scn; do
    [ -f "$S" ] || continue
    name=$(basename "$S" .scn)
    mkdir -p "$WORK/$name/frames"
    MAQUE_MAX_FRAMES=12 "$PLAYER" "$S" "$WORK/$name/frames" > "$WORK/$name.log" 2>&1 || true
    SUMMARY=$(grep -o 'label_stress_summary .*' "$WORK/$name.log" | head -1)
    [ -n "$SUMMARY" ] || fail "[$name] label_stress_summary not found (stress.js 应在第 6 帧 log；场景/脚本未加载?)"
    FRAME=$(ls "$WORK/$name/frames"/frame_*.ppm 2>/dev/null | sort | tail -1)
    [ -n "$FRAME" ] || fail "[$name] no PPM frame produced (need renderer)"
    "$PY" - "$name" "$SUMMARY" "$FRAME" <<'PY' || exit 1
import json, sys

name = sys.argv[1]
d = json.loads(sys.argv[2][len('label_stress_summary '):])
raw = open(sys.argv[3], 'rb').read()
parts = raw.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]

def at(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]

BG = at(1, 1)
bad = []
def ck(n, cond, got=None):
    if not cond:
        bad.append(f"{n} (got {got})")

cases = d.get('cases', {})
def C(n):
    if n not in cases:
        bad.append(f"用例缺失: {n}（summary.cases 里没有该实体）")
        return None
    return cases[n]

def diff(p):
    return max(abs(p[0] - BG[0]), abs(p[1] - BG[1]), abs(p[2] - BG[2]))

def region(x0, y0, x1, y1):
    x0 = max(0, int(x0)); y0 = max(0, int(y0))
    x1 = min(w, int(x1)); y1 = min(h, int(y1))
    out = []
    for yy in range(y0, y1):
        for xx in range(x0, x1):
            p = at(xx, yy)
            if diff(p) > 30:
                out.append((xx, yy, p))
    return out

# 各用例颜色（与场景里的 r/g/b 对应；按颜色分类，避免相邻 band 互相干扰）
def p_red(r, g, b):    return r > 90 and g < 0.62 * r and b < 0.62 * r and abs(g - b) < 40
def p_blue(r, g, b):   return b > 90 and 0.55 * b < g < 0.95 * b and r < 0.62 * b
def p_yellow(r, g, b): return r > 120 and g > 0.5 * r and b < 0.6 * r
def p_green(r, g, b):  return g > 90 and r < 0.78 * g and b < 0.7 * g
def p_pink(r, g, b):   return r > 90 and b > 0.62 * r and g < 0.62 * r

def color_ink(m, pred, y0=None, y1=None, x1=None):
    """用例区域内的该颜色 ink 像素。默认行 = 文本顶 .. 文本底；可显式覆盖（查裁剪下界）。"""
    top = h - m['y']
    width = max(m['max_width'], m['text_w'], 24)
    rx1 = x1 if x1 is not None else m['x'] + width + 10
    ry0 = y0 if y0 is not None else top - 6
    ry1 = y1 if y1 is not None else top + m['text_h'] + 10
    return [p for p in region(m['x'] - 4, ry0, rx1, ry1) if pred(*p[2])]

def bb(pts):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)

summary = {}

if name == 'stress_lines':
    sl, ml = C('single_line'), C('multiline')
    wn, wm, ww = C('width_narrow'), C('width_mid'), C('width_wide')
    if sl and ml:
        ck('multiline: 5 行显式换行 → line_count==5', ml['line_count'] == 5, ml['line_count'])
        ck('multiline: text_h >= 4.5*单行', ml['text_h'] >= 4.5 * sl['text_h'],
           (ml['text_h'], sl['text_h']))
        ink_ml = color_ink(ml, p_blue)
        ck('multiline: 有字形像素', len(ink_ml) > 200, len(ink_ml))
        if ink_ml:
            _, t, _, b = bb(ink_ml)
            ck('multiline: ink 跨 ≥4 个行带', (b - t) > 4 * sl['text_h'], (b - t, 4 * sl['text_h']))
        ink_sl = color_ink(sl, p_red)
        ck('single_line: 单行对照有 ink', len(ink_sl) > 50, len(ink_sl))
        if ink_sl:
            _, t, _, b = bb(ink_sl)
            ck('single_line: 单行高度 ≈ text_h', (b - t) <= sl['text_h'] + 2, (b - t, sl['text_h']))
    if wn and wm and ww:
        for nm, m in (('narrow', wn), ('mid', wm), ('wide', ww)):
            ck(f'width_{nm}: text_w <= max_width+1', m['text_w'] <= m['max_width'] + 1, m['text_w'])
        ck('width: line_count 严格递减 (narrow>mid>wide)',
           wn['line_count'] > wm['line_count'] > ww['line_count'],
           (wn['line_count'], wm['line_count'], ww['line_count']))
        ck('width: text_h 同步递减 (narrow>mid>wide)',
           wn['text_h'] > wm['text_h'] > ww['text_h'],
           (wn['text_h'], wm['text_h'], ww['text_h']))
        for nm, m, pred in (('narrow', wn, p_yellow), ('mid', wm, p_green), ('wide', ww, p_pink)):
            pix = color_ink(m, pred)
            ck(f'width_{nm}: 有字形像素', len(pix) > 100, len(pix))
            if pix:
                _, t, r, b = bb(pix)
                ck(f'width_{nm}: ink 右边界 <= x+max_width+3', r <= m['x'] + m['max_width'] + 3,
                   (r, m['x'] + m['max_width']))
                ck(f'width_{nm}: ink 行跨度 > 1.5*单行高', (b - t) > 1.5 * sl['text_h'],
                   (b - t, 1.5 * sl['text_h']))
        summary = (f"multiline lines={ml and ml['line_count']} h={ml and ml['text_h']}; "
                   f"width lines={wn['line_count']}/{wm['line_count']}/{ww['line_count']} "
                   f"h={wn['text_h']:.0f}/{wm['text_h']:.0f}/{ww['text_h']:.0f}")

elif name == 'stress_heights':
    restricted = [('height_40', C('height_40'), p_red),
                  ('height_80', C('height_80'), p_blue),
                  ('height_160', C('height_160'), p_green)]
    control = C('height_control')
    present = [(nm, m, pred) for nm, m, pred in restricted if m]
    for nm, m, pred in present:
        ck(f'{nm}: text_h <= max_height+0.5', m['text_h'] <= m['max_height'] + 0.5,
           (m['text_h'], m['max_height']))
        ck(f'{nm}: line_count >= 2', m['line_count'] >= 2, m['line_count'])
        ck(f'{nm}: fit_font_size > 0 且 <= font_size', 0 < m['fit_font_size'] <= m['font_size'],
           (m['fit_font_size'], m['font_size']))
        top = h - m['y']
        pix = color_ink(m, pred, y0=top - 6, y1=top + m['max_height'] + 24)
        ck(f'{nm}: 有字形像素', len(pix) > 50, len(pix))
        if pix:
            _, _, _, b = bb(pix)
            ck(f'{nm}: ink 底部 <= band_top+max_height+3', b <= top + m['max_height'] + 3,
               (b, top + m['max_height']))
    fits = [m['fit_font_size'] for nm, m, _ in present if nm in ('height_40', 'height_80', 'height_160')]
    if len(fits) == 3:
        ck('height: fit_font_size 严格递增 (40<80<160)', fits[0] < fits[1] < fits[2],
           [round(v, 2) for v in fits])
        ck('height: 三个受限用例 fit 都 < 20', all(v < 20.0 for v in fits), [round(v, 2) for v in fits])
    if control:
        ck('height_control: 无高度限制 text_h > 160', control['text_h'] > 160.0, control['text_h'])
        for nm, m, _ in present:
            ck(f'height_control: text_h 大于 {nm}', control['text_h'] > m['text_h'],
               (control['text_h'], m['text_h']))
        pix_c = color_ink(control, p_yellow)
        ck('height_control: 有字形像素', len(pix_c) > 200, len(pix_c))
    hs = {nm: m for nm, m, _ in present}
    summary = ("fits=" + "/".join(f"{hs[k]['fit_font_size']:.2f}" for k in ('height_40', 'height_80', 'height_160')
                                  if k in hs)
               + " h=" + "/".join(f"{hs[k]['text_h']:.0f}" for k in ('height_40', 'height_80', 'height_160')
                                 if k in hs)
               + f" control_h={control and control['text_h']}")

elif name == 'stress_mixed':
    mx = d.get('mixed')
    if mx:
        ck('mixed: chars == 1000', mx.get('chars') == 1000, mx.get('chars'))
        ck('mixed: cjk >= 300', mx.get('cjk', 0) >= 300, mx.get('cjk'))
        ck('mixed: latin >= 300', mx.get('latin', 0) >= 300, mx.get('latin'))
    else:
        bad.append('mixed: summary 缺 mixed 统计（stress.js 应写 chars/cjk/latin）')
    full, shr, clp, cjk = C('mixed_full'), C('mixed_shrink'), C('mixed_clip'), C('cjk_control')
    if full:
        ck('mixed_full: text_w <= 281', full['text_w'] <= 281.0, full['text_w'])
        ck('mixed_full: line_count >= 25', full['line_count'] >= 25, full['line_count'])
        ck('mixed_full: text_h >= 300', full['text_h'] >= 300.0, full['text_h'])
        pix = color_ink(full, p_red)
        ck('mixed_full: ink 像素数 > 3000', len(pix) > 3000, len(pix))
        if pix:
            _, t, r, b = bb(pix)
            ck('mixed_full: ink 右边界 <= x+280+3', r <= full['x'] + 280 + 3, (r, full['x'] + 280))
            ck('mixed_full: ink 行跨度 >= 300', (b - t) >= 300, (b - t))
    if shr:
        ck('mixed_shrink: fit_font_size < 16', shr['fit_font_size'] < 16.0, shr['fit_font_size'])
        ck('mixed_shrink: text_h <= 300.5', shr['text_h'] <= 300.5, shr['text_h'])
        ck('mixed_shrink: line_count >= 5', shr['line_count'] >= 5, shr['line_count'])
        top = h - shr['y']
        pix = color_ink(shr, p_blue, y0=top - 6, y1=top + 300 + 24)
        ck('mixed_shrink: 有字形像素', len(pix) > 200, len(pix))
        if pix:
            _, _, _, b = bb(pix)
            ck('mixed_shrink: ink 底部 <= band_top+300+3', b <= top + 300 + 3, (b, top + 300))
    if clp:
        ck('mixed_clip: fit_font_size <= 0.5', clp['fit_font_size'] <= 0.5, clp['fit_font_size'])
        ck('mixed_clip: text_h <= 200.5', clp['text_h'] <= 200.5, clp['text_h'])
        top = h - clp['y']
        pix = color_ink(clp, p_green, y0=top - 6, y1=top + 200 + 24)
        ck('mixed_clip: 有字形像素', len(pix) > 100, len(pix))
        if pix:
            _, _, _, b = bb(pix)
            ck('mixed_clip: ink 底部 <= band_top+200+3', b <= top + 200 + 3, (b, top + 200))
    if cjk:
        ck('cjk_control: 纯中文有 ink', True)
        ck('cjk_control: text_w > 60（CJK 字体真的加载）', cjk['text_w'] > 60.0, cjk['text_w'])
        pix = color_ink(cjk, p_yellow)
        ck('cjk_control: 有字形像素', len(pix) > 50, len(pix))
    summary = (f"chars={mx and mx.get('chars')} cjk={mx and mx.get('cjk')} latin={mx and mx.get('latin')} "
               f"full lines={full and full['line_count']} h={full and full['text_h']} "
               f"shrink fit={shr and round(shr['fit_font_size'], 2)} h={shr and shr['text_h']} "
               f"clip h={clp and clp['text_h']} cjk_w={cjk and cjk['text_w']}")

elif name == 'stress_baseline':
    # 行内混排字号（<size=32>）共享基线：大字形向上伸出、底部行与小字形一致（±1px）。
    mx, pl = C('baseline_mixed'), C('baseline_plain')

    def col_groups(pts):
        """按列分组：列间隔 >1px 视为不同字形。返回 [{x0,x1,top,bot,h}]。"""
        cols = sorted({p[0] for p in pts})
        if not cols:
            return []
        runs, cur = [], [cols[0]]
        for x in cols[1:]:
            if x - cur[-1] > 1:
                runs.append(cur); cur = [x]
            else:
                cur.append(x)
        runs.append(cur)
        out = []
        for run in runs:
            xs = set(run)
            ys = [p[1] for p in pts if p[0] in xs]
            out.append({'x0': run[0], 'x1': run[-1], 'top': min(ys), 'bot': max(ys),
                        'h': max(ys) - min(ys) + 1})
        return out

    if mx and pl:
        ck('baseline: 混排 text_h 取行内最大 ascent（> plain+10）',
           mx['text_h'] > pl['text_h'] + 10, (mx['text_h'], pl['text_h']))
        top_m = h - mx['y']
        top_p = h - pl['y']
        g_m = col_groups(region(mx['x'] - 4, top_m - 6, mx['x'] + mx['text_w'] + 10,
                                top_m + mx['text_h'] + 10))
        g_p = col_groups(region(pl['x'] - 4, top_p - 6, pl['x'] + pl['text_w'] + 10,
                                top_p + pl['text_h'] + 10))
        ck('baseline_mixed: 字形按列分组 >=2', len(g_m) >= 2, len(g_m))
        ck('baseline_plain: 字形按列分组 >=2', len(g_p) >= 2, len(g_p))
        if len(g_m) >= 2:
            small_h = min(g['h'] for g in g_m)
            bigs = [g for g in g_m if g['h'] > 1.6 * small_h]
            smalls = [g for g in g_m if g['h'] <= 1.3 * small_h]
            ck('baseline_mixed: 存在高字形（h > 1.6×小字形）', len(bigs) >= 1,
               ([g['h'] for g in g_m], small_h))
            ck('baseline_mixed: 存在小字形', len(smalls) >= 1, len(smalls))
            if bigs and smalls:
                small_bot = max(g['bot'] for g in smalls)
                small_top = min(g['top'] for g in smalls)
                ck('baseline_mixed: 小字形底部行一致（±1px）',
                   max(abs(g['bot'] - small_bot) for g in smalls) <= 1,
                   [g['bot'] for g in smalls])
                ck('baseline_mixed: 高字形底部行 == 小字形底部行（±1px，共享基线）',
                   all(abs(g['bot'] - small_bot) <= 1 for g in bigs),
                   ([g['bot'] for g in bigs], small_bot))
                ck('baseline_mixed: 高字形顶部高于小字形顶部（大字向上伸出）',
                   all(g['top'] < small_top - 1 for g in bigs),
                   ([g['top'] for g in bigs], small_top))
                mixed_baseline = max(g['bot'] for g in g_m) - top_m
                ck('baseline_mixed: baseline 偏移 = 大字形 ascent 量级（> 小字形高度）',
                   mixed_baseline > small_h + 5, (mixed_baseline, small_h))
        ck('baseline_plain: 字形高度一致（±1px）',
           max(g['h'] for g in g_p) - min(g['h'] for g in g_p) <= 1,
           [g['h'] for g in g_p])
        ck('baseline_plain: 字形底部行一致（±1px）',
           max(g['bot'] for g in g_p) - min(g['bot'] for g in g_p) <= 1,
           [g['bot'] for g in g_p])
        plain_baseline = max(g['bot'] for g in g_p) - top_p
        if len(g_m) >= 2:
            mixed_baseline = max(g['bot'] for g in g_m) - top_m
            ck('baseline: 混排 baseline 偏移 > plain baseline 偏移 +5px（行 ascent=max）',
               mixed_baseline > plain_baseline + 5, (mixed_baseline, plain_baseline))
        summary = (f"mixed h={mx['text_h']} groups={[(g['h'], g['bot']) for g in g_m]} "
                   f"plain h={pl['text_h']} groups={[(g['h'], g['bot']) for g in g_p]}")

else:
    bad.append(f'未知 stress 场景名: {name}（测试脚本未分派断言）')

if bad:
    print(f'[{name}] FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
print(f"label_layout_e2e: subcase {name} ok ({summary})")
PY
    STRESS_COUNT=$((STRESS_COUNT + 1))
done
[ "$STRESS_COUNT" -ge 3 ] || fail "stress 子例场景数 < 3（实际 $STRESS_COUNT；期望 scenes/label/stress*.scn）"

# ---- 确定性：同输入两跑 world_trail 一致（docs/10）----
MAQUE_MAX_FRAMES=12 "$PLAYER" "$SCENE" "$WORK/frames2" > "$WORK/out2.txt" 2>&1 || true
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/out.txt" | head -1 | cut -d= -f2)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/out2.txt" | head -1 | cut -d= -f2)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "determinism broken: $T1 != $T2"

echo "label_layout_e2e: ok (trail=$T1)"
exit 0
