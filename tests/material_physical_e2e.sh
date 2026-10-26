#!/usr/bin/env bash
# 材质物理/虹彩端到端（plans/19-threejs-materials.md，Physical 模型）：
#   scenes/pbr/material_physical.scn = 并排两个球：sphere_standard(shading_model=0) 与
#   sphere_physical(shading_model=1，clearcoat/sheen/iridescence)，同 base_color、同灯光。
#   用 MAQUE_ORBIT 两个角度各拍一张（yaw 0.3 与 1.2，绕世界原点），断言：
#     ① 两角度都定位到 2 个对象区域（画面非空）；
#     ② 每个角度下 Physical 与 Standard 的亮度/色相/RGB 差异超过阈值（物理材质生效）；
#     ③ Physical 区域内部有明显**色相多样性**（iridescence 的视角偏色在球面上展开），
#        而 Standard 区域色相基本单一 —— 这是“物理材质没生效”的正向鉴别点；
#     ④ Physical 区域在**两个角度之间**的平均色相变化 > Standard 的对应变化（视角偏色）。
#   不依赖相机左右朝向假设：每个角度用“色相多样性/彩度”独立识别 Physical 侧。
#   场景文件由并行的 scenes 任务产出：不存在 → SKIP 77。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "material_physical_e2e: skip（无显示环境）"
    exit 77
fi
[ -f "$SCN" ] || { echo "material_physical_e2e: skip（缺 $SCN —— 材质物理场景尚未产出）"; exit 77; }
[ "${HAVE_PY:-0}" = 1 ] || { echo "material_physical_e2e: skip（无 python3，无法做像素断言）"; exit 77; }

fail() { echo "material_physical_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_WINDOW MAQUE_CAPTURE MAQUE_CAPTURE_WGPU \
      MAQUE_CAPTURE_FRAMES MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ \
      MAQUE_ORBIT MAQUE_SS MAQUE_VIEWS MAQUE_SHADOW 2>/dev/null || true
export MAQUE_PRESENT=off MAQUE_WINDOW=0x0 MAQUE_RENDER3D_INFO=1

# 两个环绕角度（MAQUE_ORBIT=yaw,pitch,dist；绕世界原点）
timeout 120 env MAQUE_ORBIT="0.3,0.2,5.5" "$PLAYER" --shot "$WORK/phys_a.ppm" --shot-frame 1 "$SCN" \
    >"$WORK/phys_a.txt" 2>&1 \
    || { cat "$WORK/phys_a.txt"; fail "角度 A（MAQUE_ORBIT=0.3,0.2,5.5）渲染失败"; }
timeout 120 env MAQUE_ORBIT="1.2,0.2,5.5" "$PLAYER" --shot "$WORK/phys_b.ppm" --shot-frame 1 "$SCN" \
    >"$WORK/phys_b.txt" 2>&1 \
    || { cat "$WORK/phys_b.txt"; fail "角度 B（MAQUE_ORBIT=1.2,0.2,5.5）渲染失败"; }
grep -q "3d scene cams=" "$WORK/phys_a.txt" || { cat "$WORK/phys_a.txt"; fail "角度 A 未走 3D 渲染路径"; }
grep -q "3d scene cams=" "$WORK/phys_b.txt" || { cat "$WORK/phys_b.txt"; fail "角度 B 未走 3D 渲染路径"; }
grep -q "runtime: shot" "$WORK/phys_a.txt" || fail "角度 A 截图未写入"
grep -q "runtime: shot" "$WORK/phys_b.txt" || fail "角度 B 截图未写入"

"$PY" - "$WORK/phys_a.ppm" "$WORK/phys_b.ppm" "$SCN" <<'PY' || exit 1
# 两角度 × 两对象的像素断言（每个角度独立识别 Physical 侧，不假设相机左右朝向）。
import json
import math
import sys
from collections import Counter

PPM_A, PPM_B, SCN = sys.argv[1], sys.argv[2], sys.argv[3]

# ---- 阈值（结构性 + 对照；括号内为 2026-10-26 M1 实测值）----
TH_BG_CANDIDATES = (25, 18, 12, 8, 6)
MIN_AREA = 300                    # 单对象像素数下限（实测 4658–8200）
MIN_OBJECTS = 2                   # 场景必须有两个可见对象
# 同一角度下 Physical vs Standard 的差异（任一维度显著即可；实测 rgb 距离 11.4 / 17.5）
MIN_LUM_DIFF = 6.0
MIN_CHROMA_DIFF = 2.0             # 归一化色度向量差 × 255
MIN_COLOR_DIFF = 8.0              # 平均 RGB 欧氏距离
MIN_HUE_DIFF = 8.0                # 加权色相角差（度）
# Physical 区域内部色相多样性（iridescence 展开；实测 Standard hue_bins=1，Physical 3–4）
MIN_PHYS_HUE_BINS = 2
# 两角度之间（iridescence 视角依赖；实测 Physical hueΔ=32°，Standard hueΔ=0°）
MIN_PHYS_HUE_SHIFT = 12.0
MAX_STD_HUE_SHIFT = 10.0
MIN_HUE_DIFF_DELTA = 10.0         # Physical hueΔ - Standard hueΔ 的下限


def die(msg):
    print('material_physical_e2e: FAIL ' + msg, file=sys.stderr)
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
    px = [None] * (w * h)
    for i in range(w * h):
        j = i * 3
        px[i] = (data[j], data[j + 1], data[j + 2])
    return w, h, px


def cdiff(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


def runs(flags, min_len, merge_gap):
    raw, s = [], None
    for i, f in enumerate(list(flags) + [False]):
        if f and s is None:
            s = i
        elif not f and s is not None:
            raw.append((s, i - 1))
            s = None
    merged = []
    for r in raw:
        if merged and r[0] - merged[-1][1] - 1 <= merge_gap:
            merged[-1] = (merged[-1][0], r[1])
        else:
            merged.append(r)
    return [r for r in merged if r[1] - r[0] + 1 >= min_len]


def locate(px, w, h, label):
    """定位 2 个对象区域（列带 × 行带，按 x 排序为 左/右）；只保留面积最大的两个。"""
    border = ([px[x] for x in range(w)] + [px[(h - 1) * w + x] for x in range(w)]
              + [px[y * w] for y in range(h)] + [px[y * w + w - 1] for y in range(h)])
    bg = Counter(border).most_common(1)[0][0]
    for th in TH_BG_CANDIDATES:
        mask = [cdiff(p, bg) > th for p in px]
        cols = [any(mask[y * w + x] for y in range(h)) for x in range(w)]
        regions = []
        for (x0, x1) in runs(cols, 6, 2):
            rows = [any(mask[y * w + x] for x in range(x0, x1 + 1)) for y in range(h)]
            for (y0, y1) in runs(rows, 6, 2):
                pix = [px[y * w + x] for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)
                       if mask[y * w + x]]
                if len(pix) >= 120:
                    regions.append({'x0': x0, 'x1': x1, 'y0': y0, 'y1': y1, 'pix': pix})
        if len(regions) >= MIN_OBJECTS:
            regions = sorted(regions, key=lambda r: -len(r['pix']))[:MIN_OBJECTS]
            regions = sorted(regions, key=lambda r: r['x0'])
            print('material_physical_e2e: %s 阈值=%d 区域=%d 背景=%s'
                  % (label, th, len(regions), tuple(bg)))
            return [region_metrics(r) for r in regions]
    die('%s 未定位到 ≥%d 个对象区域（可能对象与背景同色/粘连）' % (label, MIN_OBJECTS))


def region_metrics(reg):
    pix = reg['pix']
    n = len(pix)
    mr = sum(p[0] for p in pix) / n
    mg = sum(p[1] for p in pix) / n
    mb = sum(p[2] for p in pix) / n
    mean_l = 0.299 * mr + 0.587 * mg + 0.114 * mb
    chromas = [max(p) - min(p) for p in pix]
    mean_chroma = sum(chromas) / n
    # 归一化色度（去亮度）：用于比较色相/饱和度
    s = sum(p[0] + p[1] + p[2] for p in pix)
    cr = sum(p[0] for p in pix) / s if s else 0.0
    cg = sum(p[1] for p in pix) / s if s else 0.0
    # 加权色相 + hue 分布（占比 ≥5% 的 30° 档数 = 色相多样性）
    sx = sy = sw = 0.0
    hue_hist = Counter()
    for p in pix:
        mx, mn = max(p), min(p)
        c = mx - mn
        if c < 10:
            continue
        r_, g_, b_ = p[0] / 255.0, p[1] / 255.0, p[2] / 255.0
        dd = mx / 255.0 - mn / 255.0
        if dd <= 0:
            continue
        if mx == p[0]:
            hh = ((g_ - b_) / dd) % 6.0
        elif mx == p[1]:
            hh = (b_ - r_) / dd + 2.0
        else:
            hh = (r_ - g_) / dd + 4.0
        hdeg = (hh * 60.0) % 360.0
        rad = math.radians(hdeg)
        sx += c * math.cos(rad)
        sy += c * math.sin(rad)
        sw += c
        hue_hist[int(hdeg // 30)] += 1
    hue = math.degrees(math.atan2(sy, sx)) % 360.0 if (abs(sx) + abs(sy)) > 0 else None
    hue_bins = sum(1 for v in hue_hist.values() if v / n >= 0.05)
    return {'n': n, 'mr': mr, 'mg': mg, 'mb': mb, 'mean_l': mean_l,
            'mean_chroma': mean_chroma, 'cr': cr, 'cg': cg, 'hue': hue,
            'hue_bins': hue_bins, 'bbox': (reg['x0'], reg['y0'], reg['x1'], reg['y1'])}


def rgb_dist(m1, m2):
    return math.sqrt((m1['mr'] - m2['mr']) ** 2 + (m1['mg'] - m2['mg']) ** 2
                     + (m1['mb'] - m2['mb']) ** 2)


def chroma_dist(m1, m2):
    return 255.0 * math.sqrt((m1['cr'] - m2['cr']) ** 2 + (m1['cg'] - m2['cg']) ** 2)


def hue_delta(m1, m2):
    if m1['hue'] is None or m2['hue'] is None:
        return None
    d = abs(m1['hue'] - m2['hue']) % 360.0
    return min(d, 360.0 - d)


w, h, px_a = load_ppm(PPM_A)
w2, h2, px_b = load_ppm(PPM_B)
if (w, h) != (w2, h2):
    die('两张角度截图尺寸不一致: %dx%d vs %dx%d' % (w, h, w2, h2))

A = locate(px_a, w, h, '角度A')
B = locate(px_b, w, h, '角度B')
for side, ms in (('A', A), ('B', B)):
    for i, m in enumerate(ms):
        print('material_physical_e2e: %s区域%d bbox=%s area=%d rgb=(%.1f,%.1f,%.1f) '
              'mean_l=%.1f chroma=%.1f hue=%s hue_bins=%d'
              % (side, i, m['bbox'], m['n'], m['mr'], m['mg'], m['mb'], m['mean_l'],
                 m['mean_chroma'],
                 ('%.0f' % m['hue']) if m['hue'] is not None else 'n/a', m['hue_bins']))

# 场景 JSON：确认两个材质对象分别是 Standard(0)/Physical(1)（位置仅作诊断输出）
try:
    scene = json.load(open(SCN, encoding='utf-8'))
except Exception as e:  # noqa: BLE001
    die('场景 JSON 解析失败: %s' % e)
mats = []
for ent in scene.get('entities', []):
    comps = ent.get('components', []) or []
    mat = next((c for c in comps if c.get('type') == 'asset.PbrMaterial'), None)
    if mat is None:
        continue
    raw = mat.get('shading_model', 0)
    if isinstance(raw, str):
        names = ['Standard', 'Physical', 'Basic', 'Lambert', 'Phong',
                 'Toon', 'Normal', 'Matcap', 'Depth', 'Shadow']
        model = names.index(raw.split('::')[-1].strip())
    else:
        model = int(raw)
    pos = (0.0, 0.0, 0.0)
    for c in comps:
        if c.get('type') == 'scene.Transform3D':
            p = (c.get('local') or {}).get('position') or {}
            pos = (float(p.get('x', 0.0)), float(p.get('y', 0.0)), float(p.get('z', 0.0)))
            break
    mats.append({'model': model, 'name': ent.get('name', '?'), 'pos': pos})
print('material_physical_e2e: 场景材质=%s'
      % [(m['name'], m['model'], m['pos']) for m in mats])
if 1 not in [m['model'] for m in mats]:
    die('场景缺 shading_model=1（Physical）对象')
if 0 not in [m['model'] for m in mats]:
    die('场景缺 shading_model=0（Standard）对象')

# ---- 每个角度独立识别 Physical 侧：iridescence 让球面色相分布变宽（hue_bins 多），
#      Standard（灰球 + 白色高光）色相单一。识别不确定时用“配对枚举”兜底。----
def identify_physical(ms, label):
    # 候选：色相多样性高者优先；次看平均彩度
    a_score = (ms[0]['hue_bins'], ms[0]['mean_chroma'])
    b_score = (ms[1]['hue_bins'], ms[1]['mean_chroma'])
    if a_score != b_score:
        idx = 0 if a_score > b_score else 1
        print('material_physical_e2e: %s 识别 Physical=区域%d（hue_bins=%d/%d, chroma=%.1f/%.1f）'
              % (label, idx, ms[0]['hue_bins'], ms[1]['hue_bins'],
                 ms[0]['mean_chroma'], ms[1]['mean_chroma']))
        return idx
    return None


errs = []
pa = identify_physical(A, '角度A')
pb = identify_physical(B, '角度B')
if pa is None or pb is None:
    # 兜底：枚举 4 种配对，取 “Physical hueΔ - Standard hueΔ” 最大者
    best = None
    for ca in (0, 1):
        for cb in (0, 1):
            hp = hue_delta(A[ca], B[cb])
            hs = hue_delta(A[1 - ca], B[1 - cb])
            if hp is None or hs is None:
                continue
            score = hp - hs
            if best is None or score > best[0]:
                best = (score, ca, cb)
    if best is None:
        die('无法从两个角度识别 Physical 侧（色相不可靠）')
    pa, pb = best[1], best[2]
    print('material_physical_e2e: 色相多样性不足以识别，按配对枚举取最佳 A%d→B%d' % (pa, pb))

# ① 每个角度：两对象必须显著不同（Physical vs Standard；亮度/色度/RGB/色相任一维度）
for side, ms in (('A', A), ('B', B)):
    d_l = abs(ms[0]['mean_l'] - ms[1]['mean_l'])
    d_c = chroma_dist(ms[0], ms[1])
    d_rgb = rgb_dist(ms[0], ms[1])
    hd = hue_delta(ms[0], ms[1])
    print('material_physical_e2e: %s 两对象差异 mean_l=%.1f chroma=%.2f rgb=%.2f hue=%s'
          % (side, d_l, d_c, d_rgb, ('%.0f°' % hd) if hd is not None else 'n/a'))
    if not (d_l >= MIN_LUM_DIFF or d_c >= MIN_CHROMA_DIFF or d_rgb >= MIN_COLOR_DIFF
            or (hd is not None and hd >= MIN_HUE_DIFF)):
        errs.append('%s 角度下 Physical/Standard 差异过小（mean_l=%.1f chroma=%.2f rgb=%.2f；'
                    '需 mean_l≥%.1f 或 chroma≥%.2f 或 rgb≥%.2f 或 hue≥%.0f°）'
                    % (side, d_l, d_c, d_rgb, MIN_LUM_DIFF, MIN_CHROMA_DIFF,
                       MIN_COLOR_DIFF, MIN_HUE_DIFF))

# ② Physical 区域内部色相多样性（iridescence 在球面展开）；Standard 应远更单一
for side, ms, idx in (('A', A, pa), ('B', B, pb)):
    ph, st = ms[idx], ms[1 - idx]
    print('material_physical_e2e: %s Physical hue_bins=%d / Standard hue_bins=%d'
          % (side, ph['hue_bins'], st['hue_bins']))
    if ph['hue_bins'] < MIN_PHYS_HUE_BINS:
        errs.append('%s Physical 区域色相分布过窄（hue_bins=%d，需 ≥%d）——'
                    'iridescence 可能未生效' % (side, ph['hue_bins'], MIN_PHYS_HUE_BINS))
    if ph['hue_bins'] <= st['hue_bins']:
        errs.append('%s Physical 色相多样性(=%d) 未超过 Standard(=%d)——'
                    '两者难以区分或映射识别错误' % (side, ph['hue_bins'], st['hue_bins']))

# ③ 两角度之间：Physical 平均色相变化 > Standard 的对应变化（iridescence 视角偏色）
hp = hue_delta(A[pa], B[pb])
hs = hue_delta(A[1 - pa], B[1 - pb])
print('material_physical_e2e: 视角色相变化 Physical hueΔ=%s / Standard hueΔ=%s'
      % (('%.0f°' % hp) if hp is not None else 'n/a',
         ('%.0f°' % hs) if hs is not None else 'n/a'))
if hp is None or hs is None:
    errs.append('色相不可靠，无法判定 iridescence 视角偏色')
else:
    if hp < MIN_PHYS_HUE_SHIFT:
        errs.append('Physical 两角度色相变化过小（Δ=%.0f°，需 ≥%.0f°）——'
                    'iridescence 视角依赖可能未生效' % (hp, MIN_PHYS_HUE_SHIFT))
    if hs > MAX_STD_HUE_SHIFT:
        errs.append('Standard 两角度色相变化过大（Δ=%.0f°，需 ≤%.0f°）——'
                    '说明灯光/相机变化本身很大，无法把差异归因于 iridescence'
                    % (hs, MAX_STD_HUE_SHIFT))
    if hp < hs + MIN_HUE_DIFF_DELTA:
        errs.append('Physical 色相变化(Δ=%.0f°) 未显著大于 Standard(Δ=%.0f°)，'
                    '差值需 ≥%.0f°' % (hp, hs, MIN_HUE_DIFF_DELTA))

if errs:
    for e in errs:
        print('material_physical_e2e: FAIL ' + e, file=sys.stderr)
    sys.exit(1)
print('material_physical_e2e: Physical vs Standard 差异 + 色相多样性 + 两角度 iridescence 偏色断言通过')
PY

echo "material_physical_e2e: PASS（Physical/Standard 对照 + 色相多样性 + 两角度 iridescence 偏色）"
