#!/usr/bin/env bash
# 3D 渲染基础端到端（plans/18-pbr-3d.md M2）：
#   场景用**已声明的 render.* 组件**（scene.Transform3D + render.Camera3D/MeshRenderer/Light）。
#   ① --shot：depth + 3D pass + Lambert 光照 + 透视相机 → PPM 结构断言（非背景占比/中心可见/顶部为空）
#   ② MAQUE_PRESENT=wgpu：同一 3D 场景走 wgpu 交换链直接呈现（attach 成功 + 交换链抓帧非空）
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "render3d_scene_e2e: skip（无显示环境）"
    exit 77
fi
[ -f "$SCN" ] || { echo "render3d_scene_e2e: skip（缺 $SCN）"; exit 77; }
fail() { echo "render3d_scene_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ 2>/dev/null || true

# ---- ① 离屏 3D 渲染（--shot）----
timeout 90 "$PLAYER" --shot "$WORK/scene.ppm" --frames 2 "$SCN" >"$WORK/out.txt" 2>&1 \
    || { cat "$WORK/out.txt"; fail "3D 场景渲染失败"; }
grep -q "3d scene cams=" "$WORK/out.txt" || { cat "$WORK/out.txt"; fail "未识别到 3D 组件（render.* 提取失败）"; }
grep -q "runtime: shot" "$WORK/out.txt" || { cat "$WORK/out.txt"; fail "截图未写入"; }

"$PY" - "$WORK/scene.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
def rgb(x, y):
    i = (y * w + x) * 3
    return px[i:i + 3]
def diff(a, b):
    return abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2])
bg = rgb(2, 2)
nonbg = 0
for y in range(h):
    for x in range(w):
        if diff(rgb(x, y), bg) > 25:
            nonbg += 1
ratio = nonbg / (w * h)
# 中心区域（3D 物体）必须有内容
center = 0
for y in range(h // 3, h * 2 // 3):
    for x in range(w // 3, w * 2 // 3):
        if diff(rgb(x, y), bg) > 25:
            center += 1
# 顶部 15% 应为背景（相机俯视下没有几何覆盖）
top = 0
for y in range(max(1, h // 7)):
    for x in range(0, w, 4):
        if diff(rgb(x, y), bg) > 25:
            top += 1
top_ratio = top / max(1, (h // 7) * (w // 4))
# 不同面亮度/材质色 → 颜色多样性（区分 Lambert 明暗面）
colors = set()
for y in range(h // 4, h * 3 // 4, 3):
    for x in range(w // 4, w * 3 // 4, 3):
        colors.add(tuple(rgb(x, y)))
print(f'render3d_scene_e2e: {w}x{h} bg={tuple(bg)} nonbg={ratio:.3f} center={center} top={top_ratio:.3f} colors={len(colors)}')
ok = (0.05 < ratio < 0.6) and center > 500 and top_ratio < 0.02 and len(colors) >= 5
sys.exit(0 if ok else 1)
PY
echo "render3d_scene_e2e: ① 离屏 3D（depth/相机/Lambert/透视）OK"

# ---- ② 3D + wgpu 交换链直接呈现 ----
MAQUE_PRESENT=wgpu MAQUE_CAPTURE_WGPU="$WORK/direct" MAQUE_CAPTURE_FRAMES=1 \
    timeout 90 "$PLAYER" --frames 4 "$SCN" >"$WORK/direct.txt" 2>&1 \
    || { cat "$WORK/direct.txt"; fail "3D 直呈运行失败"; }
grep -q "direct present attach_window .*rc=0" "$WORK/direct.txt" \
    || { cat "$WORK/direct.txt"; fail "attach_window 未成功"; }
grep -q "capture(wgpu surface)" "$WORK/direct.txt" || { cat "$WORK/direct.txt"; fail "未抓到交换链帧"; }
if grep -q "present via renderer=" "$WORK/direct.txt"; then fail "直呈模式仍走了 SDL 呈现"; fi
"$PY" - "$WORK/direct_0.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
ratio = nonbg / (w * h)
print(f'render3d_scene_e2e: 直呈交换链帧 {w}x{h} nonbg={ratio:.3f}')
sys.exit(0 if ratio > 0.05 else 1)
PY
echo "render3d_scene_e2e: ② 3D wgpu 交换链直接呈现 OK"

# ---- ③ PBR 材质（M3）：同基色/金属度、粗糙度 0.12（光滑）vs 0.9（粗糙）----
# 期望：光滑金属几乎只剩窄高光+环境项（整体偏暗）；粗糙金属把高光摊开到整面（更亮）。
MAT_SCN="${SCN%cube.scn}materials.scn"
if [ -f "$MAT_SCN" ]; then
    timeout 90 "$PLAYER" --shot "$WORK/mat.ppm" --frames 2 "$MAT_SCN" >"$WORK/mat.txt" 2>&1 \
        || { cat "$WORK/mat.txt"; fail "PBR 材质场景渲染失败"; }
    grep -q "3d scene" "$WORK/mat.txt" || { cat "$WORK/mat.txt"; fail "材质场景无 3D 内容"; }
    "$PY" - "$WORK/mat.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
def lum(i):
    return 0.299*px[i] + 0.587*px[i+1] + 0.114*px[i+2]
def stats(lo, hi):
    s = n = 0
    for y in range(h):
        for x in range(lo, hi):
            v = lum((y*w+x)*3)
            if v > 30:
                s += v
                n += 1
    return s / max(1, n), n
lm, ln = stats(0, w // 2)
rm, rn = stats(w // 2, w)
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
print(f'render3d_scene_e2e: PBR smooth_mean={lm:.1f} rough_mean={rm:.1f} nonbg={nonbg/(w*h):.3f}')
# 粗糙度对比：粗糙面把能量摊开 → 平均亮度显著高于光滑面
ok = (ln > 500 and rn > 500 and rm > lm + 15 and nonbg / (w*h) > 0.05)
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: ③ PBR 材质（GGX/tonemap，粗糙度对比）OK"
fi
echo "render3d_scene_e2e: PASS"
