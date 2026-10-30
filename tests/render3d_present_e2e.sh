#!/usr/bin/env bash
# 3D/PBR M1 直接呈现端到端（plans/18-pbr-3d.md §4.1、§5 #10）：
#   ① MAQUE_PRESENT=wgpu：renderer 建 wgpu surface + swapchain **直接呈现**
#      （不建 SDL_Renderer、不走 CPU readback→SDL 的旧呈现路径）；
#   ② 交换链纹理带 CopySrc → MAQUE_CAPTURE_WGPU 抓 present 前的整帧 PPM，
#      断言画面确实画上去了（非纯背景）—— 这是"直接呈现真的出图"的硬证据；
#   ③ 同一环境下 --shot 仍走离屏回读（截图/CI 像素断言路径不受直呈影响）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "render3d_present_e2e: skip（无显示环境）"
    exit 77
fi
[ -f "$SCN" ] || { echo "render3d_present_e2e: skip（缺 $SCN）"; exit 77; }

fail() { echo "render3d_present_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_RENDER_SCALE \
      MAQUE_RENDER3D_INFO 2>/dev/null || true

# ---- ① + ②：wgpu 交换链直接呈现 + 抓交换链帧 ----
MAQUE_PRESENT=wgpu MAQUE_WINDOW=640x480 MAQUE_RENDER3D_INFO=1 \
    MAQUE_CAPTURE_WGPU="$WORK/direct" MAQUE_CAPTURE_FRAMES=2 \
    timeout 90 "$PLAYER" --frames 5 "$SCN" >"$WORK/out.txt" 2>&1 \
    || { cat "$WORK/out.txt"; fail "wgpu 直呈运行失败（exit != 0）"; }
grep -q "direct present attach_window .*rc=0" "$WORK/out.txt" \
    || { cat "$WORK/out.txt"; fail "attach_window 未成功（surface/swapchain 建不起来）"; }
if grep -q "present via renderer=" "$WORK/out.txt"; then fail "直呈模式仍走了 SDL 呈现路径"; fi
grep -Eq "present path direct-to-surface [0-9]+x[0-9]+ \(blit=0\)" "$WORK/out.txt" \
    || { cat "$WORK/out.txt"; fail "渲染分辨率等于 surface 时仍未走无 blit 直呈"; }
grep -q "capture(wgpu surface)" "$WORK/out.txt" \
    || { cat "$WORK/out.txt"; fail "未抓到直呈帧（surface 不支持 CopySrc？）"; }
[ -s "$WORK/direct_0.ppm" ] || fail "缺直呈帧 PPM"

"$PY" - "$WORK/direct_0.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _maxv, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
bg = px[0:3]
diff = 0
for i in range(0, len(px), 3):
    if abs(px[i] - bg[0]) + abs(px[i + 1] - bg[1]) + abs(px[i + 2] - bg[2]) > 30:
        diff += 1
# cube.scn 设计 16:9，测试窗口 4:3：Camera3D 必须与 2D Canvas 共用居中的 16:9 viewport。
content_h = min(h, int(round(w * 9 / 16)))
bar = max(0, (h - content_h) // 2)
def rgb(x, y):
    i = (y * w + x) * 3
    return tuple(px[i:i+3])
if bar > 1:
    assert max(rgb(w // 2, bar // 2)) <= 2, ('top bar not black', rgb(w // 2, bar // 2), w, h)
    assert max(rgb(w // 2, h - 1 - bar // 2)) <= 2, ('bottom bar not black', rgb(w // 2, h - 1 - bar // 2), w, h)
    assert sum(rgb(w // 2, bar)) > 10, ('content starts black', rgb(w // 2, bar), bar)
print(f'render3d_present_e2e: 交换链帧 {w}x{h} bars={bar}px bg={tuple(bg)} non-bg={diff}')
sys.exit(0 if diff > 500 else 1)
PY
echo "render3d_present_e2e: ① wgpu swapchain 直接呈现 + 帧内容 OK"

# ---- ③：render_scale 改变目标尺寸时才被动启用 blit ----
MAQUE_PRESENT=wgpu MAQUE_WINDOW=640x480 MAQUE_RENDER_SCALE=0.5 MAQUE_RENDER3D_INFO=1 \
    MAQUE_CAPTURE_WGPU="$WORK/scaled" MAQUE_CAPTURE_FRAMES=1 \
    timeout 90 "$PLAYER" --frames 3 "$SCN" >"$WORK/scaled.txt" 2>&1 \
    || { cat "$WORK/scaled.txt"; fail "render_scale=0.5 运行失败"; }
grep -Eq "present path scaled [0-9]+x[0-9]+ -> [0-9]+x[0-9]+ \(blit=1\)" "$WORK/scaled.txt" \
    || { cat "$WORK/scaled.txt"; fail "目标与 surface 不同却未启用条件 blit"; }
[ -s "$WORK/scaled_0.ppm" ] || fail "条件 blit 未产生 surface 抓帧"
"$PY" - "$WORK/scaled_0.ppm" <<'PY' || exit 1
import sys
raw = open(sys.argv[1], 'rb').read()
magic, dims, _maxv, px = raw.split(b'\n', 3)
assert magic == b'P6'
w, h = map(int, dims.split())
nonblack = sum(1 for i in range(0, len(px), 3) if px[i] + px[i+1] + px[i+2] > 12)
assert nonblack > w * h * 0.2, (w, h, nonblack)
print(f'render3d_present_e2e: 条件 blit surface {w}x{h} nonblack={nonblack}')
PY
echo "render3d_present_e2e: ② 条件 blit surface 像素 OK"

# ---- ④：直呈环境下 --shot 仍可离屏回读（截图路径） ----
MAQUE_PRESENT=wgpu MAQUE_WINDOW=640x480 timeout 90 "$PLAYER" --shot "$WORK/shot.png" --frames 5 "$SCN" \
    >"$WORK/shot.txt" 2>&1 \
    || { cat "$WORK/shot.txt"; fail "--shot 在直呈模式下失败"; }
grep -q "runtime: shot" "$WORK/shot.txt" || { cat "$WORK/shot.txt"; fail "截图未写入"; }
[ -s "$WORK/shot.png" ] || fail "截图 PNG 为空"
echo "render3d_present_e2e: ③ 直呈模式下 --shot 离屏回读 OK"
echo "render3d_present_e2e: PASS"
