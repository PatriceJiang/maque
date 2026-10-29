#!/usr/bin/env bash
# scn.Label 专用**样式/装饰**端到端回归（docs/17/18；与 text_e2e.sh 相互独立）。
# 场景 scenes/label/style.scn：每个用例独占一个横向 band + 唯一颜色/布局，
# 脚本在固定帧 log `label_summary {json}`（排版度量），本脚本按 PPM 像素断言：
#   f) color       ：纯色 Label 的 ink 颜色 ≈ 组件 r/g/b（容差）
#   g) transparent ：a=1 / a=0.5 / a=0 三档 → 半透明 ink 最大通道 ≈ 一半；a=0 零 ink
#   h) bold        ：同文本同字号 bold 的 ink 像素数比普通多 >15%
#   i) italic      ：同文本同字号 "IIIII" → 上半 ink 平均 x − 下半 > 1.5px；普通 ≈ 0
#   j) shadow      ：灰底 Sprite + 白字黑影（shadow_x/y=3）→ 暗像素质心相对白字右移/下移 >1.5px
#   k) outline     ：outline_size=2 + 红色 outline → 红像素存在且红/白 bbox 中心距 <4px
# 最后：同命令两跑 world_trail 一致（确定性，docs/10）。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK/frames"
export MAQUE_HEADLESS=1

fail() { echo "label_style_e2e: FAIL $*" >&2; exit 1; }

MAQUE_MAX_FRAMES=12 "$PLAYER" "$SCENE" "$WORK/frames" > "$WORK/out.txt" 2>&1 || true

SUMMARY=$(grep -o 'label_summary .*' "$WORK/out.txt" | head -1)
[ -n "$SUMMARY" ] || fail "script summary not found (style.js 应在第 6 帧 log label_summary)"
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
    if name not in d:
        bad.append(f"[{name}] 用例缺失（label_summary 里没有该实体）")
        return None
    return d[name]

def diff(p):
    return max(abs(p[0] - BG[0]), abs(p[1] - BG[1]), abs(p[2] - BG[2]))

def region(x0, y0, x1, y1):
    x0 = max(0, int(x0)); y0 = max(0, int(y0))
    x1 = min(w, int(x1)); y1 = min(h, int(y1))
    return [(x, y, at(x, y)) for y in range(y0, y1) for x in range(x0, x1)]

def ink_of(m, pad_x=8, pad_y=10):
    """用例 label 的像素区域（按度量定位）：行 = 文本顶..文本高，x = label x..text_w"""
    top = h - m['y']
    return region(m['x'] - pad_x, top - pad_y,
                  m['x'] + max(m['text_w'], 24) + pad_x, top + m['text_h'] + pad_y)

def centroid(pts):
    return sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)

# ---------------- f) color ----------------
c = case('color')
if c:
    ck('f) color: 排版度量 > 0', c['text_w'] > 0, c['text_w'])
    ink = [p for p in ink_of(c) if diff(p[2]) > 25]
    ck('f) color: 有 ink 像素', len(ink) > 100, len(ink))
    bright = [p for p in ink if p[2][0] >= 235]
    ck('f) color: 存在接近组件色（r=1.0）的核心像素', len(bright) > 20, len(bright))
    if bright:
        avg_g = sum(p[2][1] for p in bright) / len(bright)
        avg_b = sum(p[2][2] for p in bright) / len(bright)
        max_r = max(p[2][0] for p in ink)
        ck('f) color: r 通道拉满', max_r >= 240, max_r)
        ck('f) color: g 通道 ≈ 0.45*255', 90 <= avg_g <= 140, round(avg_g, 1))
        ck('f) color: b 通道 ≈ 0.5*255', 100 <= avg_b <= 155, round(avg_b, 1))

# ---------------- g) transparent：a=1 / a=0.5 / a=0 ----------------
af = case('alpha_full'); ah = case('alpha_half'); az = case('alpha_zero')
if af and ah and az:
    for nm, m in (('alpha_full', af), ('alpha_half', ah), ('alpha_zero', az)):
        ck(f'g) {nm}: 排版度量 > 0（透明度不影响排版）', m['text_w'] > 0, m['text_w'])
    ck('g) 三档 text_w 一致（透明度不改变排版）',
       abs(af['text_w'] - ah['text_w']) <= 0.5 and abs(af['text_w'] - az['text_w']) <= 0.5,
       (af['text_w'], ah['text_w'], az['text_w']))
    reg_full = ink_of(af, pad_x=4, pad_y=6)
    reg_half = ink_of(ah, pad_x=4, pad_y=6)
    reg_zero = ink_of(az, pad_x=4, pad_y=6)
    full_max = max(max(p[2]) for p in reg_full)
    half_max = max(max(p[2]) for p in reg_half)
    zero_diff = max(diff(p[2]) for p in reg_zero)
    ck('g) alpha=1.0: 不透明白字（最大通道 ≥230）', full_max >= 230, full_max)
    ck('g) alpha=0.5: 最大通道约为一半（0.30~0.70）',
       0.30 * full_max <= half_max <= 0.70 * full_max, (half_max, full_max))
    ck('g) alpha=0.0: 零 ink（区域 = 背景）', zero_diff <= 12, zero_diff)

# ---------------- h) bold ----------------
bn = case('bold_normal'); bo = case('bold_on')
if bn and bo:
    ck('h) bold: 文本不再相同（bold 推进加宽）', bo['text_w'] > bn['text_w'], (bo['text_w'], bn['text_w']))
    ink_normal = [p for p in ink_of(bn) if diff(p[2]) > 35]
    ink_bold = [p for p in ink_of(bo) if diff(p[2]) > 35]
    ck('h) bold: 普通版有 ink', len(ink_normal) > 100, len(ink_normal))
    ck('h) bold: 粗体版有 ink', len(ink_bold) > 100, len(ink_bold))
    if ink_normal and ink_bold:
        ratio = len(ink_bold) / len(ink_normal)
        ck('h) bold: ink 像素数比普通多 >15%', ratio > 1.15,
           (len(ink_bold), len(ink_normal), round(ratio, 3)))

# ---------------- i) italic（"IIIII"：竖条对称，普通 lean≈0） ----------------
inm = case('italic_normal'); iom = case('italic_on')
def lean(m):
    """斜体倾斜量：ink 上半平均 x − 下半平均 x。区域严格限制在字形行（不含其它 band）。"""
    top = h - m['y']
    reg = region(m['x'] - 4, top - 4, m['x'] + max(m['text_w'], 24) + 4, top + m['text_h'] - 1)
    ink = [p for p in reg if diff(p[2]) > 35]
    if len(ink) < 20:
        return None, 0
    ys = [p[1] for p in ink]
    mid = (min(ys) + max(ys)) / 2.0
    top = [p[0] for p in ink if p[1] < mid]
    bot = [p[0] for p in ink if p[1] >= mid]
    if not top or not bot:
        return None, len(ink)
    return sum(top) / len(top) - sum(bot) / len(bot), len(ink)
if inm and iom:
    l_normal, n_normal = lean(inm)
    l_italic, n_italic = lean(iom)
    ck('i) italic: 普通版有 ink', n_normal > 100 or l_normal is not None, (n_normal, l_normal))
    ck('i) italic: 斜体版有 ink', n_italic > 100, n_italic)
    if l_normal is not None:
        ck('i) italic: 普通版无倾斜（|lean| ≤ 1.0）', abs(l_normal) <= 1.0, round(l_normal, 2))
    if l_italic is not None:
        ck('i) italic: 斜体上半右移 > 1.5px', l_italic > 1.5, round(l_italic, 2))
        if l_normal is not None:
            ck('i) italic: 斜体 lean 显著大于普通', l_italic > l_normal + 1.0,
               (round(l_italic, 2), round(l_normal, 2)))

# ---------------- j) shadow（灰底 Sprite band：屏幕 y ∈ [H-160, H-60]） ----------------
SH_DX = SH_DY = None
sh = case('shadow')
if sh:
    ck('j) shadow: 排版度量 > 0', sh['text_w'] > 0, sh['text_w'])
    y0, y1 = h - 160, h - 60            # style.scn 的灰底 band（世界 y 60..160）
    reg = region(40, y0, 600, y1)
    gray = [p for p in reg if 100 <= p[2][0] <= 160 and abs(p[2][0] - p[2][1]) <= 8 and abs(p[2][0] - p[2][2]) <= 8]
    ck('j) shadow: 灰色背景 Sprite 覆盖 band（>50% 像素）', len(gray) > 0.5 * len(reg),
       (len(gray), len(reg)))
    white = [p for p in reg if p[2][0] > 190 and p[2][1] > 190 and p[2][2] > 190]
    dark = [p for p in reg if p[2][0] < 60 and p[2][1] < 60 and p[2][2] < 60]
    ck('j) shadow: 白字 ink 存在', len(white) > 100, len(white))
    ck('j) shadow: 黑色阴影像素存在', len(dark) > 100, len(dark))
    if white and dark:
        wc = centroid(white); dc = centroid(dark)
        SH_DX = dc[0] - wc[0]; SH_DY = dc[1] - wc[1]
        ck('j) shadow: 暗像素质心相对白字右移 > 1.5px', SH_DX > 1.5, round(SH_DX, 2))
        ck('j) shadow: 暗像素质心相对白字下移 > 1.5px（shadow_y 正 = 屏幕向下）', SH_DY > 1.5, round(SH_DY, 2))

# ---------------- k) outline ----------------
ol = case('outline')
if ol:
    ck('k) outline: 排版度量 > 0', ol['text_w'] > 0, ol['text_w'])
    reg = ink_of(ol, pad_x=14, pad_y=12)
    red = [p for p in reg if p[2][0] > 120 and p[2][1] < 70 and p[2][2] < 70]
    white = [p for p in reg if p[2][0] > 200 and p[2][1] > 200 and p[2][2] > 200]
    ck('k) outline: 红色描边像素存在', len(red) > 50, len(red))
    ck('k) outline: 白色字形像素存在', len(white) > 50, len(white))
    if red and white:
        rc = centroid(red); wc = centroid(white)
        dist = ((rc[0] - wc[0]) ** 2 + (rc[1] - wc[1]) ** 2) ** 0.5
        ck('k) outline: 红/白墨迹中心基本重合（<4px）', dist < 4.0, round(dist, 2))

if bad:
    print('label_style_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
print(f"label_style_e2e: 样式断言全部通过（bold ink {len(ink_bold)} vs {len(ink_normal)}；"
      f"italic lean {0 if l_italic is None else round(l_italic, 2)}px；"
      f"shadow 质心偏移 {0 if SH_DX is None else round(SH_DX, 2)}px/"
      f"{0 if SH_DY is None else round(SH_DY, 2)}px）")
PY

# ---- 确定性：同输入两跑 world_trail 一致（docs/10）----
MAQUE_MAX_FRAMES=12 "$PLAYER" "$SCENE" "$WORK/frames2" > "$WORK/out2.txt" 2>&1 || true
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/out.txt" | head -1 | cut -d= -f2)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/out2.txt" | head -1 | cut -d= -f2)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "determinism broken: $T1 != $T2"

echo "label_style_e2e: ok (trail=$T1)"
exit 0
