#!/usr/bin/env bash
# 材质阴影端到端（plans/19-threejs-materials.md / M5c）：
#   scenes/pbr/material_shadow.scn = 大地面（shading_model=9 Shadow，base_color 黑，
#   alpha_mode=2 Blend）+ 上方 cube（Standard）+ 方向光。
#   同一场景拍两张：MAQUE_SHADOW=1（开，默认）与 MAQUE_SHADOW=0（关，对照组），
#   断言：
#     ① 两张都成功渲染且尺寸一致；
#     ② 开阴影时“比关阴影更暗”的像素数显著 > 阈值（投影区变暗）；
#     ③ 反向（开比关更亮）像素数很少（防止把整体亮度变化误判成阴影）；
#     ④ 开/关画面确有差异（不是同一张图）；
#   场景文件由并行的 scenes 任务产出：不存在 → SKIP 77。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "material_shadow_e2e: skip（无显示环境）"
    exit 77
fi
[ -f "$SCN" ] || { echo "material_shadow_e2e: skip（缺 $SCN —— 材质阴影场景尚未产出）"; exit 77; }
[ "${HAVE_PY:-0}" = 1 ] || { echo "material_shadow_e2e: skip（无 python3，无法做像素断言）"; exit 77; }

fail() { echo "material_shadow_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_WINDOW MAQUE_CAPTURE MAQUE_CAPTURE_WGPU \
      MAQUE_CAPTURE_FRAMES MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ \
      MAQUE_ORBIT MAQUE_SS MAQUE_VIEWS 2>/dev/null || true
export MAQUE_PRESENT=off MAQUE_WINDOW=0x0

# 开（=1）与关（=0）各一张，其它条件完全一致
timeout 120 env MAQUE_SHADOW=1 "$PLAYER" --shot "$WORK/shadow_on.ppm" --shot-frame 1 "$SCN" \
    >"$WORK/shadow_on.txt" 2>&1 \
    || { cat "$WORK/shadow_on.txt"; fail "MAQUE_SHADOW=1 渲染失败"; }
timeout 120 env MAQUE_SHADOW=0 "$PLAYER" --shot "$WORK/shadow_off.ppm" --shot-frame 1 "$SCN" \
    >"$WORK/shadow_off.txt" 2>&1 \
    || { cat "$WORK/shadow_off.txt"; fail "MAQUE_SHADOW=0 渲染失败"; }
grep -q "3d scene cams=" "$WORK/shadow_on.txt" \
    || { cat "$WORK/shadow_on.txt"; fail "阴影开启时未走 3D 渲染路径"; }
grep -q "3d scene cams=" "$WORK/shadow_off.txt" \
    || { cat "$WORK/shadow_off.txt"; fail "阴影关闭时未走 3D 渲染路径"; }
grep -q "runtime: shot" "$WORK/shadow_on.txt" || fail "阴影开启时截图未写入"
grep -q "runtime: shot" "$WORK/shadow_off.txt" || fail "阴影关闭时截图未写入"

"$PY" - "$WORK/shadow_on.ppm" "$WORK/shadow_off.ppm" <<'PY' || exit 1
# 阴影开/关像素差断言：
#   方向光阴影（1024² shadow map + 3x3 PCF）→ 开时投影区明显变暗；
#   关时没有投影 → 反向计数应≈0（若反向也很多说明两张图不是“仅阴影开关”的差异）。
#   另加**空间一致性**约束（防 gfx 阴影数学 bug 复现）：变暗像素应连成一块、
#   集中在可见物体附近，而不是铺满整个 shadow 视锥/整屏。
import math
import sys
from collections import Counter

# ---- 阈值：结构性 + 对照组，宽松容差（跨 GPU 的 PCF/采样权重差异）----
DARK_DELTA = 12          # 单像素“显著变暗”的亮度阈值（开 < 关 - delta）
LIGHT_DELTA = 12         # 单像素“显著变亮”的阈值
MIN_DARK_PIXELS = 1200   # 投影区最少像素数（实测地面投影 2850；旧 shadows.scn 用例 ≥2000）
MAX_REVERSE_RATIO = 0.25  # 反向像素数 / 变暗像素数 上限
MIN_LARGEST_COMP_FRAC = 0.75  # 最大连通域占变暗像素比例下限（投影应是一块，实测 1.00）
MAX_CONTENT_DIST_FRAC = 0.40  # 变暗质心到可见物体质心的距离 / 图像对角线 上限（实测 0.11）
MAX_SPREAD_P90_FRAC = 0.25    # 变暗像素到质心的 p90 距离 / 对角线 上限（实测 0.06）


def die(msg):
    print('material_shadow_e2e: FAIL ' + msg, file=sys.stderr)
    sys.exit(1)


def load_ppm(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    if len(parts) != 4 or parts[0] != b'P6':
        die('PPM 头错误: %r' % (d[:16],))
    w, h = map(int, parts[1].split())
    data = parts[3]
    if len(data) != w * h * 3:
        die('PPM 数据长度不符: %d != %d' % (len(data), w * h * 3))
    return w, h, data


w, h, on = load_ppm(sys.argv[1])
w2, h2, off = load_ppm(sys.argv[2])
if (w, h) != (w2, h2):
    die('两张截图尺寸不一致: %dx%d vs %dx%d' % (w, h, w2, h2))


def lum_at(px, i):
    j = i * 3
    return 0.299 * px[j] + 0.587 * px[j + 1] + 0.114 * px[j + 2]


dark = []          # 开阴影更暗（= 投影区）的像素索引
light = 0          # 开阴影更亮（不应很多）
changed = 0        # 任意显著差异
diff_sum = 0
for i in range(w * h):
    d = lum_at(on, i) - lum_at(off, i)
    if d < -DARK_DELTA:
        dark.append(i)
    elif d > LIGHT_DELTA:
        light += 1
    if abs(d) > 3:
        changed += 1
    diff_sum += abs(d)

total = w * h

# 空间一致性：变暗像素的连通域（8 邻接）应主要是一块（地面投影）
darkset = set(dark)
seen = set()
comps = []
for start in dark:
    if start in seen:
        continue
    stack = [start]
    seen.add(start)
    pix = []
    while stack:
        i = stack.pop()
        pix.append(i)
        y, x = divmod(i, w)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                ny, nx = y + dy, x + dx
                if 0 <= nx < w and 0 <= ny < h:
                    j = ny * w + nx
                    if j in darkset and j not in seen:
                        seen.add(j)
                        stack.append(j)
    comps.append(pix)
comps.sort(key=len, reverse=True)
largest_frac = (len(comps[0]) / len(dark)) if dark else 0.0

# 可见物体（off 图里的非背景内容，主要是 cube）的质心与对角线尺度
bpx = []
for x in range(w):
    for y in (0, h - 1):
        j = (y * w + x) * 3
        bpx.append((off[j], off[j + 1], off[j + 2]))
for y in range(h):
    for x in (0, w - 1):
        j = (y * w + x) * 3
        bpx.append((off[j], off[j + 1], off[j + 2]))
bg = Counter(bpx).most_common(1)[0][0]
content = []
for i in range(w * h):
    j = i * 3
    if (abs(off[j] - bg[0]) + abs(off[j + 1] - bg[1]) + abs(off[j + 2] - bg[2])) > 25:
        content.append(i)
diag = math.hypot(w, h)
content_dist = -1.0
spread_p90 = -1.0
if dark and content:
    dcx = sum(i % w for i in dark) / len(dark)
    dcy = sum(i // w for i in dark) / len(dark)
    ccx = sum(i % w for i in content) / len(content)
    ccy = sum(i // w for i in content) / len(content)
    content_dist = math.hypot(dcx - ccx, dcy - ccy) / diag
    ds = sorted(math.hypot(i % w - dcx, i // w - dcy) for i in dark)
    spread_p90 = ds[int(len(ds) * 0.9)] / diag

print('material_shadow_e2e: %dx%d 投影变暗=%d 反向变亮=%d 显著差异=%d '
      '(dark/总像素=%.3f, 平均|Δ|=%.2f, 最大连通域占比=%.2f, '
      '质心距/对角线=%.2f, p90展布/对角线=%.2f)'
      % (w, h, len(dark), light, changed, len(dark) / total, diff_sum / total,
         largest_frac, content_dist, spread_p90))

if len(dark) < MIN_DARK_PIXELS:
    die('开阴影后显著变暗像素只有 %d（需 ≥ %d）——阴影没生效或投影不在画面内'
        % (len(dark), MIN_DARK_PIXELS))
if light > len(dark) * MAX_REVERSE_RATIO:
    die('反向（开阴影更亮）像素 %d 过多（> 变暗像素的 %.0f%%）——不是单纯的阴影开关差异'
        % (light, MAX_REVERSE_RATIO * 100))
if changed < MIN_DARK_PIXELS:
    die('开关阴影后画面差异像素只有 %d（需 ≥ %d）' % (changed, MIN_DARK_PIXELS))
if largest_frac < MIN_LARGEST_COMP_FRAC:
    die('变暗像素最大连通域只占 %.2f（需 ≥ %.2f）——阴影不连续，疑似采样/矩阵错误'
        % (largest_frac, MIN_LARGEST_COMP_FRAC))
if content_dist < 0 or content_dist > MAX_CONTENT_DIST_FRAC:
    die('变暗质心离可见物体质心过远（距/对角线=%.2f，需 ≤ %.2f）——'
        '疑似整屏/整个 shadow 视锥被误判为遮挡' % (content_dist, MAX_CONTENT_DIST_FRAC))
if spread_p90 < 0 or spread_p90 > MAX_SPREAD_P90_FRAC:
    die('变暗像素展布过大（p90/对角线=%.2f，需 ≤ %.2f）——阴影应集中在物体附近'
        % (spread_p90, MAX_SPREAD_P90_FRAC))
sys.exit(0)
PY

echo "material_shadow_e2e: PASS（MAQUE_SHADOW=1/0 像素差：投影区变暗）"
