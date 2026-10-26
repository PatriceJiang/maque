#!/usr/bin/env bash
# 材质着色模型端到端（plans/19-threejs-materials.md）：
#   场景 material_models.scn 把 three.js 的 9 个材质族映射为 asset.PbrMaterial.shading_model
#   （0 Standard / 1 Physical / 2 Basic / 3 Lambert / 4 Phong / 5 Toon / 6 Normal /
#     7 Matcap / 8 Depth），每个模型一个对象。
#   ShadowMaterial（9）无遮挡时透明、在画廊里没有可断言的非背景像素 → 不放进画廊，
#   由 tests/material_shadow_e2e.sh（Shadow 地面 + 开/关阴影像素差）覆盖。
#
#   本用例离屏截一张 PPM → 连通域聚类出 9 个对象 → 解析场景 JSON 的 shading_model +
#   Transform3D 位置得到「屏幕阅读顺序 → 模型」映射 → 逐对象做**结构性 + 对照组**断言
#   （容忍跨 GPU 差异，但对“材质模型没生效”敏感）：
#     ① 9 个对象都在（区域数 = 材质数 = 9，模型集合 = 0..8；另由 runtime 日志核对 9 个材质被提取）
#     ② Basic 无光照：内部亮度方差小（纯色平坦），且与背景明显不同
#     ③ Lambert 漫反射：亮度方差显著大于 Basic（有明暗渐变）
#     ④ Phong 高光：最大亮度明显高于 Lambert（Blinn-Phong 高光）
#     ⑤ Toon 分级：亮度直方图集中（占比≥5% 的 8 宽亮度档 ≤ 5 档），且有分档而非全平
#     ⑥ Normal 法线可视化：RGB 三通道都有明显变化，且色相与 Basic 明显不同
#     ⑦ Matcap：有明暗渐变；且偏冷（b>r）或色相与 Basic 明显不同
#     ⑧ Depth 灰度：接近灰度（|r-g|、|g-b| 小）且与背景可区分
#     ⑨ Standard / Physical：区域非空且亮度合理
#   场景文件由并行的 scenes 任务产出：不存在 → SKIP 77（不阻塞 Lead 构建/全量 ctest）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "material_models_e2e: skip（无显示环境）"
    exit 77
fi
[ -f "$SCN" ] || { echo "material_models_e2e: skip（缺 $SCN —— 材质画廊场景尚未产出）"; exit 77; }
[ "${HAVE_PY:-0}" = 1 ] || { echo "material_models_e2e: skip（无 python3，无法做像素断言）"; exit 77; }

fail() { echo "material_models_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

# 纯离屏 + 不建窗口（CI/无显示环境）；清掉可能干扰相机/尺寸/呈现的外部开关。
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_WINDOW MAQUE_CAPTURE MAQUE_CAPTURE_WGPU \
      MAQUE_CAPTURE_FRAMES MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ \
      MAQUE_ORBIT MAQUE_SS MAQUE_VIEWS MAQUE_SHADOW 2>/dev/null || true
export MAQUE_PRESENT=off MAQUE_WINDOW=0x0 MAQUE_RENDER3D_INFO=1

timeout 120 "$PLAYER" --shot "$WORK/models.ppm" --shot-frame 1 "$SCN" >"$WORK/models.txt" 2>&1 \
    || { cat "$WORK/models.txt"; fail "材质画廊渲染失败"; }
grep -q "3d scene cams=" "$WORK/models.txt" \
    || { cat "$WORK/models.txt"; fail "未走 3D 渲染路径（render.* 组件提取失败）"; }
grep -q "runtime: shot" "$WORK/models.txt" \
    || { cat "$WORK/models.txt"; fail "截图未写入（--shot 未生效）"; }
# runtime 侧旁证：MAQUE_RENDER3D_INFO 会为每个材质打印 model=<id>；核对 9 个都被提取且 id 覆盖 0..8。
if grep -q "3d material node=" "$WORK/models.txt"; then
    NMAT=$(grep -oE "3d material node=[0-9]+ model=[0-9]+" "$WORK/models.txt" | sort -u | wc -l | tr -d ' ')
    [ "$NMAT" = "9" ] || { cat "$WORK/models.txt"; fail "runtime 提取的材质数量应为 9（实际 $NMAT）"; }
    MODELS=$(grep -oE "model=[0-9]+" "$WORK/models.txt" | sort -u | tr '\n' ' ')
    [ "$MODELS" = "model=0 model=1 model=2 model=3 model=4 model=5 model=6 model=7 model=8 " ] \
        || { cat "$WORK/models.txt"; fail "runtime 提取的模型集合不是 0..8（实际: $MODELS）"; }
fi

"$PY" - "$WORK/models.ppm" "$SCN" <<'PY' || exit 1
# 材质画廊像素断言。输入：PPM（--shot 输出）+ 场景 JSON（取 shading_model 与对象位置，
# 作为「区域 → 模型」映射的 oracle，不依赖 runtime 新增任何查询接口）。
import json
import math
import sys
from collections import Counter

PPM, SCN = sys.argv[1], sys.argv[2]
# 画廊对象数 = 9（0..8）；Shadow(9) 不参与画廊（见文件头注释）。
EXPECT_MODELS = list(range(9))

# ---- 阈值：全部为「结构性 + 对照组」，跨 GPU/着色差异留足余量（括号=实测参考值）----
TH_BG_CANDIDATES = (25, 18, 12, 8, 6)   # 背景分割：从干净到灵敏依次尝试
MIN_AREA = 120                          # 单个对象最少像素数（实测 1900+）
MIN_OBJECTS = 9                         # 画廊对象数
BASIC_MAX_STD = 8.0                     # Basic 纯色平坦：亮度标准差上限（实测 0.0）
LAMBERT_DELTA_STD = 10.0                # Lambert 相对 Basic 的方差增量下限（实测 +26.8）
LAMBERT_MIN_STD = 12.0                  # Lambert 自身方差下限（确实有光照渐变）
PHONG_MAX_MARGIN = 8.0                  # Phong 最大亮度相对 Lambert 的下限增量（实测 +33）
PHONG_P98_MARGIN = 4.0                  # Phong 高光区 p98 相对 Lambert 的增量下限（实测 +17）
TOON_MAX_BINS = 6                       # Toon：占比≥5% 的 8 宽亮度档数上限
                                        # （toon_steps=4 + 边界/AA 档；实测 5，退化成 Lambert 时 7 → 仍可区分）
TOON_MIN_STD = 8.0                      # Toon 应确有分档明暗（不是全平）
NORMAL_MIN_CH_STD = 5.0                 # Normal：R/G/B 三通道标准差下限（实测 49.7/45.9/16.2）
NORMAL_MIN_HUE_DIFF = 20.0              # Normal：与 Basic 的加权色相角差下限（实测 179°）
MATCAP_DELTA_STD = 8.0                  # Matcap：相对 Basic 的亮度方差增量下限（实测 +55.4）
MATCAP_BLUE_MARGIN = 2.0                # Matcap：b>r 的“偏冷”下限
MATCAP_HUE_DIFF = 20.0                  # Matcap：与 Basic 色相角差下限（冷色 matcap + 近白冷基色）
DEPTH_GRAY_TOL = 6                      # Depth：|r-g|、|g-b| 视为灰度的容差
DEPTH_GRAY_FRAC = 0.80                  # Depth：满足灰度容差的像素占比下限（实测 1.00）
STD_MIN_L, STD_MAX_L = 8.0, 250.0       # Standard/Physical 平均亮度合理区间

MODEL_NAMES = ['Standard', 'Physical', 'Basic', 'Lambert', 'Phong',
               'Toon', 'Normal', 'Matcap', 'Depth', 'Shadow']


def die(msg):
    print('material_models_e2e: FAIL ' + msg, file=sys.stderr)
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


def components(mask, w, h, min_area):
    """8 邻接连通域 → 每个对象一个连通域（相邻对象即使只隔 1px 背景也不会粘连）。"""
    seen = [False] * (w * h)
    comps = []
    for start in range(w * h):
        if not mask[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = True
        pix = []
        while stack:
            i = stack.pop()
            pix.append(i)
            y, x = divmod(i, w)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dy == 0 and dx == 0:
                        continue
                    ny, nx = y + dy, x + dx
                    if 0 <= nx < w and 0 <= ny < h:
                        j = ny * w + nx
                        if mask[j] and not seen[j]:
                            seen[j] = True
                            stack.append(j)
        if len(pix) >= min_area:
            xs = [i % w for i in pix]
            ys = [i // w for i in pix]
            comps.append({'pix': pix, 'x0': min(xs), 'x1': max(xs),
                          'y0': min(ys), 'y1': max(ys), 'n': len(pix)})
    return comps


def sort_row_major(comps):
    """按行带（上→下）再按 x（左→右）排序；行容差吸收不同对象高度差。"""
    comps = sorted(comps, key=lambda c: ((c['y0'] + c['y1']) / 2.0, c['x0']))
    rows, out = [], []
    for c in comps:
        yc = (c['y0'] + c['y1']) / 2.0
        if rows and abs(yc - rows[-1][0]) <= 20.0:
            rows[-1][1].append(c)
        else:
            rows.append([yc, [c]])
    for _, rs in rows:
        out.extend(sorted(rs, key=lambda c: c['x0']))
    return out


def metrics(px, comp):
    pix = [px[i] for i in comp['pix']]
    n = len(pix)
    lums = [0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in pix]
    mean_l = sum(lums) / n
    std_l = (sum((v - mean_l) ** 2 for v in lums) / n) ** 0.5
    sl = sorted(lums)
    mr = sum(p[0] for p in pix) / n
    mg = sum(p[1] for p in pix) / n
    mb = sum(p[2] for p in pix) / n
    sr = (sum((p[0] - mr) ** 2 for p in pix) / n) ** 0.5
    sg = (sum((p[1] - mg) ** 2 for p in pix) / n) ** 0.5
    sb = (sum((p[2] - mb) ** 2 for p in pix) / n) ** 0.5
    chromas = [max(p) - min(p) for p in pix]
    mean_chroma = sum(chromas) / n
    gray_frac = sum(1 for p in pix
                    if abs(p[0] - p[1]) <= DEPTH_GRAY_TOL and abs(p[1] - p[2]) <= DEPTH_GRAY_TOL) / n
    bins = Counter(int(v // 8) for v in lums)
    sig_bins = sum(1 for c in bins.values() if c / n >= 0.05)
    # 加权色相（按彩度加权；彩度太低时记 None）
    sx = sy = 0.0
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
        rad = math.radians((hh * 60.0) % 360.0)
        sx += c * math.cos(rad)
        sy += c * math.sin(rad)
    hue = math.degrees(math.atan2(sy, sx)) % 360.0 if (abs(sx) + abs(sy)) > 0 else None
    return dict(n=n, mean_l=mean_l, std_l=std_l, max_l=sl[-1],
                p98=sl[min(n - 1, int(n * 0.98))], p99=sl[min(n - 1, int(n * 0.99))],
                mr=mr, mg=mg, mb=mb, sr=sr, sg=sg, sb=sb,
                mean_chroma=mean_chroma, gray_frac=gray_frac, sig_bins=sig_bins, hue=hue,
                bbox=(comp['x0'], comp['y0'], comp['x1'], comp['y1']))


def hue_diff(a, b):
    if a is None or b is None:
        return None
    d = abs(a - b) % 360.0
    return min(d, 360.0 - d)


w, h, px = load_ppm(PPM)

# 背景 = 边框像素众数（场景要求 clear_color 平色、与对象不同）
border = [px[x] for x in range(w)] + [px[(h - 1) * w + x] for x in range(w)]
border += [px[y * w] for y in range(h)] + [px[y * w + (w - 1)] for y in range(h)]
bg = Counter(border).most_common(1)[0][0]
bg_l = 0.299 * bg[0] + 0.587 * bg[1] + 0.114 * bg[2]

# ---- 场景 JSON oracle：9 个 PbrMaterial 对象 + shading_model + 世界位置 ----
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
        key = raw.split('::')[-1].strip()
        if key not in MODEL_NAMES:
            die('未知 shading_model 枚举名: %r' % raw)
        model = MODEL_NAMES.index(key)
    else:
        model = int(raw)
    pos = (0.0, 0.0, 0.0)
    for c in comps:
        if c.get('type') == 'scene.Transform3D':
            p = (c.get('local') or {}).get('position') or {}
            pos = (float(p.get('x', 0.0)), float(p.get('y', 0.0)), float(p.get('z', 0.0)))
            break
    mats.append({'model': model, 'name': ent.get('name', '?'), 'pos': pos})

print('material_models_e2e: %dx%d bg=%s 场景材质对象=%d 模型=%s'
      % (w, h, tuple(bg), len(mats), [m['model'] for m in mats]))
if len(mats) != len(EXPECT_MODELS):
    die('场景应包含 %d 个 asset.PbrMaterial 对象（实际 %d；Shadow 模型不放在画廊，'
        '由 material_shadow_e2e 覆盖）' % (len(EXPECT_MODELS), len(mats)))
if sorted(m['model'] for m in mats) != EXPECT_MODELS:
    die('对象的 shading_model 必须覆盖 0..8（实际 %s）' % sorted(m['model'] for m in mats))

# 期望屏幕顺序：按世界坐标分行（world y 大 → 屏幕上），行内按 x 从左到右。
# 单行场景即“x 升序”；两行场景即“上行 0-4、下行 5-8”（row-major），与场景契约一致。
rows = []
for m in sorted(mats, key=lambda m: -m['pos'][1]):
    if rows and abs(m['pos'][1] - rows[-1][0]) <= 0.2:
        rows[-1][1].append(m)
    else:
        rows.append([m['pos'][1], [m]])
expected = []
for _, rs in rows:
    expected.extend(sorted(rs, key=lambda m: m['pos'][0]))
expected_models = [m['model'] for m in expected]
print('material_models_e2e: 期望屏幕顺序 → 模型 %s（行数=%d）' % (expected_models, len(rows)))
if expected_models != EXPECT_MODELS:
    print('material_models_e2e: WARN 场景位置排序后的模型序列不是 0..8，'
          '映射可能不准（继续执行，断言失败信息会暴露）', file=sys.stderr)

# ---- 图像聚类：依次尝试背景阈值，直到连通域数量 == 9 ----
comps = None
chosen_th = None
tried = []
for th in TH_BG_CANDIDATES:
    mask = [cdiff(p, bg) > th for p in px]
    cs = sort_row_major(components(mask, w, h, MIN_AREA))
    tried.append((th, len(cs)))
    if len(cs) == len(expected):
        comps, chosen_th = cs, th
        break
if comps is None:
    th, n = max(tried, key=lambda t: t[1])
    mask = [cdiff(p, bg) > th for p in px]
    cs = sort_row_major(components(mask, w, h, MIN_AREA))
    detail = '; '.join('阈值%d→%d区域' % t for t in tried)
    die('聚类未得到 %d 个对象连通域（%s）；最大区域 bbox=%s。可能对象粘连/过小/与背景同色'
        % (len(expected), detail, [(c['x0'], c['y0'], c['x1'], c['y1']) for c in cs]))
mask = [cdiff(p, bg) > chosen_th for p in px]
print('material_models_e2e: 背景分割阈值=%d，定位到 %d 个对象区域' % (chosen_th, len(comps)))

M = {}
for comp, m in zip(comps, expected):
    st = metrics(px, comp)
    M[m['model']] = st
    print('material_models_e2e: 区域 model=%-8s name=%-20s bbox=(%3d,%3d)-(%3d,%3d) '
          'area=%4d mean_l=%6.1f std_l=%5.1f p98=%6.1f max_l=%6.1f '
          'rgb=(%5.1f,%5.1f,%5.1f) chstd=(%4.1f,%4.1f,%4.1f) sig_bins=%d hue=%s '
          'chroma=%5.1f gray=%.2f'
          % (MODEL_NAMES[m['model']], str(m['name'])[:20],
             st['bbox'][0], st['bbox'][1], st['bbox'][2], st['bbox'][3], st['n'],
             st['mean_l'], st['std_l'], st['p98'], st['max_l'],
             st['mr'], st['mg'], st['mb'], st['sr'], st['sg'], st['sb'], st['sig_bins'],
             ('%.0f' % st['hue']) if st['hue'] is not None else 'n/a',
             st['mean_chroma'], st['gray_frac']))

errs = []


def chk(cond, msg):
    if not cond:
        errs.append(msg)


# ② Basic：无光照 → 内部平坦（方差小），且与背景明显不同（着色可读）
b = M[2]
chk(b['std_l'] <= BASIC_MAX_STD,
    'Basic 应为无光照纯色（std_l=%.1f 应 ≤ %.1f；若对象带贴图/受光照会超标）'
    % (b['std_l'], BASIC_MAX_STD))
chk(cdiff((b['mr'], b['mg'], b['mb']), bg) > 25,
    'Basic 区域与背景区分不明显（mean_rgb 距 bg=%d）' % cdiff((b['mr'], b['mg'], b['mb']), bg))

# ③ Lambert：漫反射明暗渐变 → 方差显著大于 Basic
lam = M[3]
chk(lam['std_l'] >= b['std_l'] + LAMBERT_DELTA_STD and lam['std_l'] >= LAMBERT_MIN_STD,
    'Lambert 应有光照渐变（std_l=%.1f，需 ≥ Basic(%.1f)+%.1f 且 ≥ %.1f）'
    % (lam['std_l'], b['std_l'], LAMBERT_DELTA_STD, LAMBERT_MIN_STD))

# ④ Phong：高光 → 最大亮度明显高于 Lambert
ph = M[4]
chk(ph['max_l'] >= lam['max_l'] + PHONG_MAX_MARGIN,
    'Phong 应有高光（max_l=%.1f，需 ≥ Lambert(%.1f)+%.1f）'
    % (ph['max_l'], lam['max_l'], PHONG_MAX_MARGIN))
chk(ph['p98'] >= lam['p98'] + PHONG_P98_MARGIN,
    'Phong 高光区域偏窄（p98=%.1f 需 ≥ Lambert(%.1f)+%.1f）'
    % (ph['p98'], lam['p98'], PHONG_P98_MARGIN))

# ⑤ Toon：NdotL 分级 → 亮度直方图集中（≤ toon_steps + 边界余量），且确有分档
toon = M[5]
chk(toon['sig_bins'] <= TOON_MAX_BINS,
    'Toon 亮度应分级集中（占比≥5%% 的 8 宽亮度档=%d，需 ≤ %d；直方图太散说明没走分级）'
    % (toon['sig_bins'], TOON_MAX_BINS))
chk(toon['std_l'] >= TOON_MIN_STD,
    'Toon 区域应有明暗分档（std_l=%.1f 需 ≥ %.1f；全平说明没有光照/分档）'
    % (toon['std_l'], TOON_MIN_STD))

# ⑥ Normal：法线编码 → RGB 三通道都有变化；整体色相与 Basic 明显不同
nm = M[6]
chk(nm['sr'] >= NORMAL_MIN_CH_STD and nm['sg'] >= NORMAL_MIN_CH_STD
    and nm['sb'] >= NORMAL_MIN_CH_STD,
    'Normal 区域 RGB 三通道都应有变化（chstd=(%.1f,%.1f,%.1f)，需都 ≥ %.1f）'
    % (nm['sr'], nm['sg'], nm['sb'], NORMAL_MIN_CH_STD))
nhd = hue_diff(nm['hue'], b['hue'])
chk(nhd is not None and nhd >= NORMAL_MIN_HUE_DIFF,
    'Normal 区域色相应与 Basic 明显不同（差 %s°，需 ≥ %.0f°）'
    % (('%.0f' % nhd) if nhd is not None else 'n/a', NORMAL_MIN_HUE_DIFF))

# ⑦ Matcap：贴图光斑 → 有明暗渐变；且偏冷（b>r）或色相与 Basic 明显不同
mc = M[7]
chk(mc['std_l'] >= b['std_l'] + MATCAP_DELTA_STD,
    'Matcap 应有明暗渐变（std_l=%.1f，需 ≥ Basic(%.1f)+%.1f）'
    % (mc['std_l'], b['std_l'], MATCAP_DELTA_STD))
mhd = hue_diff(mc['hue'], b['hue'])
cool = mc['mb'] >= mc['mr'] + MATCAP_BLUE_MARGIN
chk(cool or (mhd is not None and mhd >= MATCAP_HUE_DIFF),
    'Matcap 应与 Basic 明显不同：偏冷 b-r=%.1f（需 ≥ %.1f）或色相差 ≥ %.0f°'
    '（实际 b=%.1f r=%.1f，色相差=%s°）'
    % (mc['mb'] - mc['mr'], MATCAP_BLUE_MARGIN, MATCAP_HUE_DIFF, mc['mb'], mc['mr'],
       ('%.0f' % mhd) if mhd is not None else 'n/a'))

# ⑧ Depth：深度可视化 → 接近灰度（|r-g|、|g-b| 小），且与背景可区分
dp = M[8]
chk(dp['gray_frac'] >= DEPTH_GRAY_FRAC,
    'Depth 区域应接近灰度（灰度像素占比=%.2f，需 ≥ %.2f）'
    % (dp['gray_frac'], DEPTH_GRAY_FRAC))
chk(cdiff((dp['mr'], dp['mg'], dp['mb']), bg) > 25,
    'Depth 区域与背景区分不明显（mean_rgb 距 bg=%d）'
    % cdiff((dp['mr'], dp['mg'], dp['mb']), bg))

# ⑨ Standard / Physical：非空 + 亮度合理（老路径 / 全特性对照）
for model, label in ((0, 'Standard'), (1, 'Physical')):
    s = M[model]
    chk(s['n'] >= MIN_AREA, '%s 区域过小/为空（area=%d）' % (label, s['n']))
    chk(STD_MIN_L <= s['mean_l'] <= STD_MAX_L,
        '%s 平均亮度不合理（mean_l=%.1f，需在 [%.0f, %.0f]）'
        % (label, s['mean_l'], STD_MIN_L, STD_MAX_L))

if errs:
    for e in errs:
        print('material_models_e2e: FAIL ' + e, file=sys.stderr)
    print('material_models_e2e: 区域明细见上方每行输出（对象粘连/映射错误时先看 bbox）',
          file=sys.stderr)
    sys.exit(1)
print('material_models_e2e: 9 个着色模型区域断言全部通过')
PY

echo "material_models_e2e: PASS（9 个 shading_model：Basic/Lambert/Phong/Toon/Normal/Matcap/Depth/Standard/Physical）"
