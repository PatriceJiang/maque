#!/usr/bin/env bash
# 后处理 PostEffect 端到端（docs/13 §后处理；docs/28 F4 引擎选项面板「后处理」一节）
#
# 覆盖的是"主输出阶段后处理"这条 GPU 通路（gfx 内建 shader 'post'）：
#   ① 默认关闭 = 零回归：`post=0` 与不设任何选项的两张 PPM **逐字节一致**；
#      只开总开关（post=1，效果全关、曝光 1、无 tone map）时画面近似不变（identity 重采样）。
#   ② 曝光 / ACES 色调映射：亮度按曝光方向变化（说明 pass 真的跑了，不是死代码）。
#   ③ 暗角：四角均值明显变暗，中心区域基本不变（只压边缘）。
#   ④ 灰度/色彩分级：3D 区域的彩色像素几乎消失；基线里彩色像素很多（对照组有鉴别力）。
#   ⑤ Bloom 泛光：3D 区域暗背景上出现亮度抬升的像素（光晕），基线没有；
#      同选项两次运行必须**逐字节一致**（确定性）。
#   ⑥ 色差：径向 RGB 偏移使画面与基线显著不同。
#   ⑦ 作用范围 = **默认只叠加 3D 层**：同一帧里的 2D Sprite 像素与基线**逐字节相同**；
#      纯 2D 场景在默认范围下逐字节不变，`post_2d=1` 才整帧处理（灰度生效）。
#   ⑧ F4 面板 + 后处理共存（打开面板不崩、日志可见）；全程无 ASan。
#
# 无显示环境可跑：`MAQUE_WINDOW=0x0 MAQUE_PRESENT=off` + `--shot`（离屏回读路径）。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player 可执行文件；$2 = 仓库根（CMAKE_SOURCE_DIR）；$3 = 工作目录
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="${1:-}"
ROOT="${2:-}"
WORK="${3:-}"

[ -n "${PY:-}" ] || { echo "post_effect_e2e: skip（无可用 Python 3，无法解析 PPM）"; exit 77; }
[ -n "$PLAYER" ] && [ -f "$PLAYER" ] \
    || { echo "post_effect_e2e: skip（缺 player 可执行文件: ${PLAYER:-<未给出>}）"; exit 77; }
[ -n "$ROOT" ] && [ -d "$ROOT" ] \
    || { echo "post_effect_e2e: skip（缺仓库根目录: ${ROOT:-<未给出>}）"; exit 77; }
[ -n "$WORK" ] || { echo "post_effect_e2e: skip（未给出工作目录）"; exit 77; }

fail() { echo "post_effect_e2e: FAIL $*" >&2; exit 1; }

check_clean() {
    if grep -q "AddressSanitizer" "$1" 2>/dev/null; then
        cat "$1" >&2
        fail "日志出现 AddressSanitizer 报告（$1）"
    fi
}

# 运行一次 player：$1=日志，$2=失败描述，其余为 player 参数（全部加 timeout 90）。
run() {
    local log="$1" desc="$2"; shift 2
    timeout 90 "$PLAYER" "$@" >"$log" 2>&1 || { cat "$log" >&2; fail "$desc"; }
    check_clean "$log"
}

rm -rf "$WORK"; mkdir -p "$WORK" || fail "无法创建工作目录 $WORK"

# 干扰项全清掉，再强制「无窗口 + 纯离屏」（CI / 无显示环境可用）。
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_CAPTURE_EVERY MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_IMGUI_SCALE MAQUE_IMGUI_FONT_SIZE \
      MAQUE_SHOT MAQUE_SHOT_FRAME MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_SHOT_EVERY MAQUE_SHOT_COUNT \
      MAQUE_ENV_IBL MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO \
      MAQUE_WINDOW MAQUE_WINDOW_EVENT_AT MAQUE_WINDOW_MAXIMIZED MAQUE_ENGINE_OPTIONS MAQUE_KEEP_OPEN \
      MAQUE_TITLE MAQUE_VIEWS MAQUE_SIM MAQUE_SIM_KEY_AT MAQUE_SIM_CLICK MAQUE_DPI_SCALE \
      MAQUE_TRACE_INPUT MAQUE_TRACE_STATS 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# 场景：深色 clear + 一盏方向光 + 一个**高亮橙色** emissive cube（Bloom 的亮源 /
# 灰度测试的彩色源）+ 一个蓝色 2D Sprite（验证后处理作用在 3D+2D 合成后的整帧上）。
SCN="$WORK/post_effect_e2e.scn"
cat >"$SCN" <<'JSON'
{
  "title": "post_effect_e2e",
  "width": 640,
  "height": 360,
  "views": 1,
  "entities": [
    {
      "name": "camera",
      "components": [
        { "type": "scene.Transform3D", "local": {
            "position": { "x": 0.0, "y": 1.0, "z": 4.4 },
            "rotation": { "x": -0.22, "y": 0.0, "z": 0.0 } } },
        { "type": "render.Camera3D", "fov_y": 55.0, "near": 0.1, "far": 100.0,
          "clear_color": { "r": 0.02, "g": 0.02, "b": 0.03, "a": 1.0 } }
      ]
    },
    {
      "name": "sun",
      "components": [
        { "type": "scene.Transform3D" },
        { "type": "render.Light", "type": 0, "intensity": 2.0,
          "color": { "r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0 } }
      ]
    },
    {
      "name": "cube",
      "components": [
        { "type": "scene.Transform3D", "local": {
            "position": { "x": 0.0, "y": 0.0, "z": 0.0 },
            "rotation": { "x": 0.0, "y": 0.45, "z": 0.0 },
            "scale": { "x": 1.0, "y": 1.0, "z": 1.0 } } },
        { "type": "render.MeshRenderer" },
        { "type": "asset.PbrMaterial",
          "base_color": { "r": 1.0, "g": 0.55, "b": 0.22, "a": 1.0 },
          "emissive": { "x": 0.85, "y": 0.42, "z": 0.12 },
          "emissive_intensity": 2.0,
          "metallic": 0.0,
          "roughness": 0.55 }
      ]
    },
    {
      "name": "badge",
      "components": [
        { "type": "scene.Transform2D", "x": -150.0, "y": -95.0 },
        { "type": "scn.Sprite", "radius": 34.0, "r": 0.15, "g": 0.5, "b": 1.0 }
      ]
    }
  ]
}
JSON
[ -s "$SCN" ] || fail "测试场景写入失败: $SCN"

echo "post_effect_e2e: ① 默认关闭零回归 / identity"
MAQUE_ENGINE_OPTIONS="" run "$WORK/base.log" "基线运行失败" \
    --frames 3 --shot "$WORK/base.ppm" "$SCN"
[ -s "$WORK/base.ppm" ] || fail "基线截图未生成"
MAQUE_ENGINE_OPTIONS="post=0" run "$WORK/off.log" "post=0 运行失败" \
    --frames 3 --shot "$WORK/off.ppm" "$SCN"
grep -qF "engine option post=off" "$WORK/off.log" \
    || { cat "$WORK/off.log" >&2; fail "post=0 缺 'runtime: engine option post=off' 日志"; }
cmp -s "$WORK/base.ppm" "$WORK/off.ppm" \
    || fail "post=0 应与默认（不设选项）逐字节一致（默认关闭不得改变画面）"

MAQUE_ENGINE_OPTIONS="post=1" run "$WORK/identity.log" "post=1（identity）运行失败" \
    --frames 3 --shot "$WORK/identity.ppm" "$SCN"
grep -qF "engine option post=on effects=- exposure=1.00 tonemap=none" "$WORK/identity.log" \
    || { cat "$WORK/identity.log" >&2; fail "post=1 缺后处理状态日志（effects=- / exposure=1.00 / tonemap=none）"; }

echo "post_effect_e2e: ② 曝光 / ACES 色调映射"
MAQUE_ENGINE_OPTIONS="post=1,tonemap=aces,exposure=2.0" \
    run "$WORK/aces.log" "tonemap=aces,exposure=2.0 运行失败" \
    --frames 3 --shot "$WORK/aces.ppm" "$SCN"
grep -qF "engine option post=on effects=- exposure=2.00 tonemap=aces" "$WORK/aces.log" \
    || { cat "$WORK/aces.log" >&2; fail "tonemap=aces 缺状态日志（应含 tonemap=aces / exposure=2.00）"; }
MAQUE_ENGINE_OPTIONS="post=1,exposure=0.2" \
    run "$WORK/dim.log" "exposure=0.2 运行失败" \
    --frames 3 --shot "$WORK/dim.ppm" "$SCN"
grep -qF "engine option post=on effects=- exposure=0.20 tonemap=none" "$WORK/dim.log" \
    || fail "exposure=0.2 缺状态日志（应含 exposure=0.20）"

echo "post_effect_e2e: ③ 暗角 / ④ 灰度 / ⑤ Bloom / ⑥ 色差"
MAQUE_ENGINE_OPTIONS="post=1,vignette=1,vignette_intensity=0.9" \
    run "$WORK/vignette.log" "暗角运行失败" --frames 3 --shot "$WORK/vignette.ppm" "$SCN"
grep -qF "engine option post=on effects=vignette" "$WORK/vignette.log" \
    || fail "暗角缺状态日志（effects=vignette）"

MAQUE_ENGINE_OPTIONS="post=1,grade=1,gray=1" \
    run "$WORK/gray.log" "灰度运行失败" --frames 3 --shot "$WORK/gray.ppm" "$SCN"
grep -qF "engine option post=on effects=grade" "$WORK/gray.log" \
    || fail "灰度缺状态日志（effects=grade）"

MAQUE_ENGINE_OPTIONS="post=1,bloom=1,bloom_intensity=2.0,bloom_threshold=0.35" \
    run "$WORK/bloom.log" "Bloom 运行失败" --frames 3 --shot "$WORK/bloom.ppm" "$SCN"
grep -qF "engine option post=on effects=bloom" "$WORK/bloom.log" \
    || fail "Bloom 缺状态日志（effects=bloom）"
# 确定性：同选项第二次运行必须逐字节一致。
MAQUE_ENGINE_OPTIONS="post=1,bloom=1,bloom_intensity=2.0,bloom_threshold=0.35" \
    run "$WORK/bloom2.log" "Bloom 第二次运行失败" --frames 3 --shot "$WORK/bloom2.ppm" "$SCN"
cmp -s "$WORK/bloom.ppm" "$WORK/bloom2.ppm" \
    || fail "同一后处理选项两次运行应逐字节一致（非确定性）"

MAQUE_ENGINE_OPTIONS="post=1,chromatic=1,chromatic_strength=0.02" \
    run "$WORK/chromatic.log" "色差运行失败" --frames 3 --shot "$WORK/chromatic.ppm" "$SCN"
grep -qF "engine option post=on effects=chromatic" "$WORK/chromatic.log" \
    || fail "色差缺状态日志（effects=chromatic）"

"$PY" - "$WORK/base.ppm" "$WORK/identity.ppm" "$WORK/aces.ppm" "$WORK/dim.ppm" \
    "$WORK/vignette.ppm" "$WORK/gray.ppm" "$WORK/bloom.ppm" "$WORK/chromatic.ppm" <<'PY' \
    || fail "后处理像素断言不满足"
import sys


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素长度不符'
    return w, h, px


images = [load(p) for p in sys.argv[1:]]
w, h = images[0][0], images[0][1]
assert (w, h) == (640, 360), f'期望 640x360，实际 {w}x{h}'
assert all(im[:2] == (w, h) for im in images)
base, identity, aces, dim, vignette, gray, bloom, chromatic = [im[2] for im in images]


def diff(a, b, threshold=8):
    n = 0
    for i in range(0, len(a), 3):
        if (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])) > threshold:
            n += 1
    return n


def luma(px, i):
    return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]


def mean_region(px, x0, y0, x1, y1):
    total = 0.0
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * w + x) * 3
            total += luma(px, i)
            n += 1
    return total / n


def mean_all(px):
    return sum(luma(px, i) for i in range(0, len(px), 3)) / (w * h)


# 2D 小徽章（蓝色 Sprite）在基线里的像素掩码 —— 从**基线图像**自校准，
# 不依赖世界→屏幕映射（窗口/画布适配会改变映射，硬编码会误判成"UI 被后处理改了"）。
sprite_idx = [i for i in range(0, len(base), 3)
              if base[i + 2] > 140 and base[i + 2] - base[i] > 60 and base[i + 2] - base[i + 1] > 40]
assert len(sprite_idx) > 200, f'基线里找不到 2D 徽章（蓝像素 {len(sprite_idx)}）'
sx = [(i // 3) % w for i in sprite_idx]
sy = [(i // 3) // w for i in sprite_idx]
SPRITE = (max(0, min(sx) - 8), max(0, min(sy) - 8), min(w, max(sx) + 9), min(h, max(sy) + 9))


def outside_bbox(x, y):
    return not (SPRITE[0] <= x < SPRITE[2] and SPRITE[1] <= y < SPRITE[3])


def diff_sprite(px):
    """2D 徽章掩码内的逐像素差异数（0 = UI 完全没被后处理碰过）。"""
    n = 0
    for i in sprite_idx:
        if px[i] != base[i] or px[i + 1] != base[i + 1] or px[i + 2] != base[i + 2]:
            n += 1
    return n


def colorful_outside(px):
    """3D 区域内（排除 2D 徽章）的彩色像素数：默认后处理范围只到 3D 层。"""
    n = 0
    for y in range(h):
        for x in range(w):
            if not outside_bbox(x, y):
                continue
            i = (y * w + x) * 3
            if luma(px, i) < 24:
                continue
            hi = max(px[i], px[i + 1], px[i + 2])
            lo = min(px[i], px[i + 1], px[i + 2])
            if hi - lo > 8:
                n += 1
    return n


# ① identity：只开总开关（效果全关/曝光 1/无 tonemap）→ 与基线近似相同（允许线性重采样的 ±1 舍入）。
identity_diff = diff(base, identity, threshold=3)
# ② 曝光 / ACES：3D 区域平均亮度必须按曝光方向变化（pass 真的执行了，且调参有单调性）。
base_mean = mean_all(base)
aces_shift = mean_all(aces) - base_mean
dim_shift = mean_all(dim) - base_mean
aces_diff = diff(base, aces, threshold=8)
# ③ 暗角：四角明显变暗，中心基本不变。
corner_base = mean_region(base, 0, 0, 48, 48)
corner_vig = mean_region(vignette, 0, 0, 48, 48)
center_base = mean_region(base, w // 2 - 40, h // 2 - 40, w // 2 + 40, h // 2 + 40)
center_vig = mean_region(vignette, w // 2 - 40, h // 2 - 40, w // 2 + 40, h // 2 + 40)
# ④ 灰度：**3D 区域**的彩色像素几乎消失；2D 徽章保持原样（默认 include_2d=0）。
base_colorful = colorful_outside(base)
gray_colorful = colorful_outside(gray)
# ⑤ Bloom：暗背景上出现"比基线更亮"的像素（光晕）；只统计 3D 区域。
halo = 0
for y in range(h):
    for x in range(w):
        if not outside_bbox(x, y):
            continue
        i = (y * w + x) * 3
        if luma(base, i) < 55 and (luma(bloom, i) - luma(base, i)) > 10:
            halo += 1
# ⑥ 色差：整帧差异显著（径向 RGB 偏移在物体边缘最明显）。
chromatic_diff = diff(base, chromatic, threshold=8)
bloom_diff = diff(base, bloom, threshold=8)
# ⑦ **2D/UI 默认不受后处理影响**：徽章像素与基线逐字节相同（所有效果都一样）。
sprite_diffs = {
    'aces': diff_sprite(aces),
    'vignette': diff_sprite(vignette),
    'gray': diff_sprite(gray),
    'bloom': diff_sprite(bloom),
    'chromatic': diff_sprite(chromatic),
}

print(f'post_effect_e2e: identity差异={identity_diff} 曝光 基线均值={base_mean:.2f} '
      f'aces({aces_shift:+.2f}) dim({dim_shift:+.2f}) aces差异像素={aces_diff} '
      f'暗角 角={corner_base:.1f}->{corner_vig:.1f} 中心={center_base:.1f}->{center_vig:.1f} '
      f'灰度(3D) 彩色像素 {base_colorful}->{gray_colorful} '
      f'bloom 光晕={halo} (差异={bloom_diff}) chromatic差异={chromatic_diff} '
      f'2D徽章差异={sprite_diffs}')

ok = (identity_diff < 2000
      and dim_shift < -2.0 and aces_shift > 1.0 and aces_diff > 2000
      and corner_vig < corner_base * 0.6 and corner_vig < corner_base - 2.0
      and abs(center_vig - center_base) < max(4.0, center_base * 0.10)
      and base_colorful > 3000 and gray_colorful < base_colorful * 0.02
      and halo > 800
      and chromatic_diff > 800
      and bloom_diff > 800
      and all(v == 0 for v in sprite_diffs.values()))
sys.exit(0 if ok else 1)
PY

echo "post_effect_e2e: ⑦ 纯 2D 场景默认不处理；post_2d=1 才整帧处理"
SCN2D="$WORK/post_effect_2d.scn"
cat >"$SCN2D" <<'JSON'
{
  "title": "post_effect_2d",
  "width": 640,
  "height": 360,
  "views": 1,
  "entities": [
    {
      "name": "badge",
      "components": [
        { "type": "scene.Transform2D", "x": 0.0, "y": 0.0 },
        { "type": "scn.Sprite", "radius": 70.0, "r": 1.0, "g": 0.45, "b": 0.15 }
      ]
    }
  ]
}
JSON
[ -s "$SCN2D" ] || fail "2D 测试场景写入失败: $SCN2D"
run "$WORK/2d_base.log" "2D 基线运行失败" --frames 3 --shot "$WORK/2d_base.ppm" "$SCN2D"
# 默认范围只看 3D：纯 2D 场景即便开了后处理也**逐字节不变**（不建中间目标、不跑 pass）。
MAQUE_ENGINE_OPTIONS="post=1,grade=1,gray=1" \
    run "$WORK/2d_gray.log" "2D 灰度运行失败" --frames 3 --shot "$WORK/2d_gray.ppm" "$SCN2D"
grep -qF "engine option post=on effects=grade" "$WORK/2d_gray.log" \
    || fail "2D 灰度缺状态日志（effects=grade）"
grep -qF "scope=3d" "$WORK/2d_gray.log" \
    || fail "默认后处理作用范围应为 3d（日志缺 scope=3d）"
cmp -s "$WORK/2d_base.ppm" "$WORK/2d_gray.ppm" \
    || fail "默认（scope=3d）下纯 2D 场景不应被后处理改变（应逐字节一致）"
# 显式 post_2d=1：整帧（含 2D/UI）处理。
MAQUE_ENGINE_OPTIONS="post=1,post_2d=1,grade=1,gray=1" \
    run "$WORK/2d_gray_all.log" "2D 灰度（post_2d=1）运行失败" --frames 3 --shot "$WORK/2d_gray_all.ppm" "$SCN2D"
grep -qF "scope=all" "$WORK/2d_gray_all.log" \
    || fail "post_2d=1 缺状态日志（应含 scope=all）"

"$PY" - "$WORK/2d_base.ppm" "$WORK/2d_gray_all.ppm" <<'PY' \
    || fail "纯 2D 场景（post_2d=1）的后处理像素断言不满足"
import sys


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素长度不符'
    return w, h, px


images = [load(p) for p in sys.argv[1:]]
w, h = images[0][0], images[0][1]
assert (w, h) == (640, 360)
base, gray_all = [im[2] for im in images]


def colorful(px):
    n = 0
    for i in range(0, len(px), 3):
        hi = max(px[i], px[i + 1], px[i + 2])
        lo = min(px[i], px[i + 1], px[i + 2])
        if hi - lo > 20:
            n += 1
    return n


base_colorful = colorful(base)
gray_colorful = colorful(gray_all)
print(f'post_effect_e2e: 2D（post_2d=1）彩色像素 {base_colorful}->{gray_colorful}')
sys.exit(0 if (base_colorful > 2000 and gray_colorful < base_colorful * 0.05) else 1)
PY

echo "post_effect_e2e: ⑧ F4 面板 + 后处理共存（IMGUI 面板打开时不崩、画面仍非空）"
MAQUE_IMGUI=1 MAQUE_ENGINE_OPTIONS="post=1,bloom=1,vignette=1" \
    MAQUE_SIM_KEY_AT="5:1004" run "$WORK/panel.log" "F4 + 后处理运行失败" \
    --shot "$WORK/panel.ppm" --shot-frame 25 --frames 40 "$SCN"
[ -s "$WORK/panel.ppm" ] || fail "F4 + 后处理截图未生成"
grep -qF "engine options panel ON" "$WORK/panel.log" \
    || { cat "$WORK/panel.log" >&2; fail "F4 面板未打开（缺 'engine options panel ON'）"; }
grep -qF "engine option post=on effects=bloom,vignette" "$WORK/panel.log" \
    || fail "F4 + 后处理缺状态日志（effects=bloom,vignette）"

echo "post_effect_e2e: ok（默认关闭零回归 / identity / 曝光·ACES / 暗角 / 灰度 / Bloom / 色差 / 2D 默认不受影响 / post_2d=1 整帧 / 确定性 / F4 共存 / 无 ASan）"
