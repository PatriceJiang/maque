#!/usr/bin/env bash
# scn.Label **呈现网格** 端到端（docs/17/18）：非整数倍渲染缩放下的字距 / 基线均匀性。
#
# 背景（用户反馈「文字歪歪扭扭，是不是布局精度不够？」）：
#   设计 advance 在 1.25× / 0.833× 等非整数倍缩放下不是整数设备像素。若把"设计位置×倍率"
#   逐字取整，会得到 14/14/13/14 这样的**字距抖动**（以及逐字基线抖动）。正确做法：
#   pixel_snap 下横向用**设备整数 advance 累计**、行基线只吸附一次；设计度量仍与倍率无关。
#
# 场景 scenes/label/glyph_grid.scn（480x240，无脚本，静态三行）：
#   caps     "HHHHHHHH"       font 16 白  → 同字形：设备字距必须**完全相等**、基线行完全一致
#   xheight  "acemnorsuvwxz"  font 16 绿  → 全部 x-height 字形：底部基线行完全一致
#   mixed    "the quick brown fox" font 14 品红 → 单行、有墨迹
#
# 每个倍率都跑一遍 `MAQUE_RENDER_SCALE=<scale> --shot`（headless；显式倍率会生效），
# 从 PPM 逐像素分析。倍率集合含整数(1)、非整数(1.25/1.5)、缩小(0.833)、超采样(2.5)。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "label_glyph_grid_e2e: FAIL $*" >&2; exit 1; }

SCALES="1 1.25 1.5 0.833 1.1 2.5"
for S in $SCALES; do
    MAQUE_RENDER_SCALE="$S" "$PLAYER" --shot "$WORK/g_$S.ppm" --shot-frame 1 "$SCENE" \
        > "$WORK/run_$S.log" 2>&1 || fail "scale $S 运行失败（$WORK/run_$S.log）"
    [ -s "$WORK/g_$S.ppm" ] || fail "scale $S 未产出 PPM"
done

"$PY" - "$WORK" <<'PY' || exit 1
import glob, os, sys

work = sys.argv[1]
scales = [('1', 1.0), ('1.25', 1.25), ('1.5', 1.5), ('0.833', 0.833), ('1.1', 1.1), ('2.5', 2.5)]
bad = []
def ck(cond, msg):
    if not cond: bad.append(msg)

def load_ppm(path):
    raw = open(path, 'rb').read()
    parts = raw.split(b'\n', 3)
    if len(parts) < 4 or parts[0].strip() != b'P6':
        bad.append(f'不是合法 PPM: {path}')
        return None
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def color_groups(w, h, px, pred, x0, x1, y0, y1, gap):
    """在区域内按列分组（列间隔 > gap 视为不同字形）；返回 [(x0,x1,top,bot)]"""
    x0 = max(0, int(x0)); y0 = max(0, int(y0)); x1 = min(w, int(x1)); y1 = min(h, int(y1))
    pts = []
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            if pred(px[i], px[i + 1], px[i + 2]): pts.append((x, y))
    if not pts: return []
    cols = sorted({p[0] for p in pts})
    runs, cur = [], [cols[0]]
    for x in cols[1:]:
        if x - cur[-1] <= gap: cur.append(x)
        else: runs.append(cur); cur = [x]
    runs.append(cur)
    out = []
    for run in runs:
        s = set(run)
        ys = [p[1] for p in pts if p[0] in s]
        out.append((run[0], run[-1], min(ys), max(ys)))
    return out

def region_bytes(w, px, box):
    """字形的原始 RGB 字节（逐像素元组）——同一个码点必须复用同一张图集位图。"""
    x0, x1, top, bot = (int(v) for v in box)
    return tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]
                 for y in range(top, bot + 1) for x in range(x0, x1 + 1))

white = lambda r, g, b: r > 150 and g > 150 and b > 150
green = lambda r, g, b: g > 150 and r < 150 and b < 150
pink  = lambda r, g, b: r > 150 and b > 120 and g < 130
blue  = lambda r, g, b: b > 150 and r < 0.8 * b and g < 0.9 * b
yellow = lambda r, g, b: r > 150 and g > 120 and b < 120 and g < 0.95 * r

def row_bands(w, h, px, pred, x0, x1, y0, y1):
    """区域内按行分带（连续墨迹行），返回 [(top,bot)]"""
    x0 = max(0, int(x0)); x1 = min(w, int(x1))
    y0 = max(0, int(y0)); y1 = min(h, int(y1))
    rows = []
    for y in range(y0, y1):
        row = y * w * 3
        if any(pred(px[row + x * 3], px[row + x * 3 + 1], px[row + x * 3 + 2]) for x in range(x0, x1)):
            rows.append(y)
    if not rows: return []
    bands, cur = [], [rows[0]]
    for y in rows[1:]:
        if y - cur[-1] <= 1: cur.append(y)
        else: bands.append((cur[0], cur[-1])); cur = [y]
    bands.append((cur[0], cur[-1]))
    return bands

for tag, sc in scales:
    path = os.path.join(work, f'g_{tag}.ppm')
    ppm = load_ppm(path)
    if not ppm: continue
    w, h, px = ppm
    gap = max(1, int(0.8 * sc + 0.5))        # 列间隔阈值随设备倍率
    caps_msg = 'caps fail'

    # ---- caps: 同一字形 "HHHHHHHH" ----
    # caps 行顶（设计 px）= H - 200 = 40；取 20 设计像素余量
    caps = color_groups(w, h, px, white, 0, w, 20 * sc, (40 + 24) * sc, gap)
    caps = [g for g in caps if g[3] - g[2] >= 2 * sc]     # 过滤噪点/边缘
    caps_ref = None
    ck(len(caps) == 8, f'[{tag}] caps 字形分组应 8（实际 {len(caps)}）')
    if len(caps) == 8:
        starts = [g[0] for g in caps]
        gaps = [starts[i + 1] - starts[i] for i in range(7)]
        bottoms = [g[3] for g in caps]
        ck(max(gaps) == min(gaps),
           f'[{tag}] caps 设备字距应完全相等（实际 {gaps}）')
        ck(max(bottoms) == min(bottoms),
           f'[{tag}] caps 基线行应完全一致（实际 {bottoms}）')
        # 设备位图在整数网格上：起点与 bitmap 宽共同构成等间距
        ck(all(g > 0 for g in gaps), f'[{tag}] caps 字距必须为正（实际 {gaps}）')
        # 同一个码点（H）必须复用**同一张图集位图**：逐字节完全相同。
        # 这条直接守住"同一个 o 在不同位置看起来不同"类问题 —— 布局/取整绝不允许改变字形像素。
        ref = region_bytes(w, px, caps[0])
        ck(all(region_bytes(w, px, g) == ref for g in caps[1:]),
           f'[{tag}] caps 同字形位图应逐字节一致（同码点必须复用同一图集位图）')
        caps_ref = ref
        caps_msg = f'caps gaps={gaps[0]} bottoms={bottoms[0]}'

    # ---- xheight: 所有 x-height 字形底部 = 基线 ----
    # xheight 行顶（设计 px）= H - 130 = 110
    xhs = color_groups(w, h, px, green, 0, w, (110 - 4) * sc, (110 + 24) * sc, gap)
    xhs = [g for g in xhs if g[3] - g[2] >= 2 * sc]
    ck(len(xhs) >= 11, f'[{tag}] xheight 字形分组应 >=11（实际 {len(xhs)}）')
    if xhs:
        bottoms = [g[3] for g in xhs]
        ck(max(bottoms) - min(bottoms) <= 1,
           f'[{tag}] xheight 基线行不一致（实际 {bottoms}）')

    # ---- mixed: 单行文本有墨迹 ----
    mx = color_groups(w, h, px, pink, 0, w, (180 - 4) * sc, (180 + 26) * sc, gap)
    ck(len(mx) >= 2, f'[{tag}] mixed 行应有多段墨迹（实际 {len(mx)}）')

    # ---- mixbase: 同 line 不同高度字形（K/h 升部 + o x-height）基线行必须一致 ----
    # 行顶（设计 px）= H - 40 = 320；font 24，区域取到 line 底
    mb = color_groups(w, h, px, yellow, 0, w, (320 - 6) * sc, (320 + 38) * sc, gap)
    mb = [g for g in mb if g[3] - g[2] >= 2 * sc]
    ck(len(mb) >= 3, f'[{tag}] mixbase 应有 >=3 个字形（实际 {len(mb)}）')
    if mb:
        bottoms = [g[3] for g in mb]
        ck(max(bottoms) == min(bottoms),
           f'[{tag}] mixbase 不同字高基线应一致（实际 {bottoms}）')

    # ---- lines: 4 行同字号文本 → 行基线间距必须恒定（行高按设备整数累计）----
    # lines 行顶（设计 px）= H - 100 = 260；4 行 * ~22px 行高 = 88
    lb = row_bands(w, h, px, blue, 0, w, (260 - 6) * sc, (260 + 96) * sc)
    ck(len(lb) == 4, f'[{tag}] lines 应有 4 行墨迹（实际 {len(lb)}）')
    line_bottoms = []
    lines_ref = None
    if len(lb) == 4:
        for (a, b) in lb:
            gs = color_groups(w, h, px, blue, 0, w, a, b + 1, gap)
            gs = [g for g in gs if g[3] - g[2] >= 2 * sc]
            ck(len(gs) == 4, f'[{tag}] lines 每行应有 4 个 H 字形（实际 {len(gs)}，行 {a}-{b}）')
            if gs:
                bots = [g[3] for g in gs]
                ck(max(bots) == min(bots), f'[{tag}] lines 行内基线不一致（实际 {bots}，行 {a}-{b}）')
                # 同一个 H 在本行/跨行必须复用图集里的**同一张位图**（逐字节一致）
                if lines_ref is None:
                    lines_ref = region_bytes(w, px, gs[0])
                ck(all(region_bytes(w, px, g) == lines_ref for g in gs),
                   f'[{tag}] lines 本行 H 位图应逐字节一致（图集复用）')
                line_bottoms.append(max(bots))
    if len(line_bottoms) == 4:
        line_gaps = [line_bottoms[i + 1] - line_bottoms[i] for i in range(3)]
        ck(max(line_gaps) == min(line_gaps),
           f'[{tag}] lines 行基线间距应恒定（实际 {line_gaps}）')

    print(f'label_glyph_grid_e2e: scale={tag:>5} {caps_msg}; xheight bottoms '
          f'{sorted({g[3] for g in xhs})[:3]}')

if bad:
    print('label_glyph_grid_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
print('label_glyph_grid_e2e: PASS（1/1.25/1.5/0.833/1.1/2.5 倍率下字距与基线均均匀）')
PY

echo "label_glyph_grid_e2e: ok"
exit 0
