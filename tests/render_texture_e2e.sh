#!/usr/bin/env bash
# RenderTexture 端到端：相机节点 -> GPU 离屏纹理 -> 同帧 Sprite 采样；同一目标按需回读截图。
set -u
PLAYER="$1"
SCENE="$2"
WORK="$3"
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -n "${PY:-}" ] || { echo "render_texture_e2e: skip（无 Python）"; exit 77; }
fail() { echo "render_texture_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_WINDOW=0x0
export MAQUE_PRESENT=off
export MAQUE_RT_SHOT="$WORK/camera.ppm"

timeout 90 "$PLAYER" --shot "$WORK/main.ppm" --shot-frame 2 "$SCENE" >"$WORK/run.log" 2>&1 \
  || { cat "$WORK/run.log"; fail "player 运行失败"; }
grep -q "RenderTexture 'rendertexture://0/minimap' 160x120" "$WORK/run.log" \
  || { cat "$WORK/run.log"; fail "RenderTexture 未创建"; }
grep -q "render texture=rendertexture://0/minimap" "$WORK/run.log" \
  || { cat "$WORK/run.log"; fail "camera_texture 返回值错误"; }
grep -q "camera shot .*160x120 camera=" "$WORK/run.log" \
  || { cat "$WORK/run.log"; fail "camera_capture 未落盘"; }
[ -s "$WORK/main.ppm" ] || fail "主画面截图缺失"
[ -s "$WORK/camera.ppm" ] || fail "相机截图缺失"

"$PY" - "$WORK/main.ppm" "$WORK/camera.ppm" <<'PY' || exit 1
import sys

def ppm(path):
    d = open(path, 'rb').read()
    magic, size, maxv, px = d.split(b'\n', 3)
    assert magic == b'P6' and maxv == b'255', path
    w, h = map(int, size.split())
    assert len(px) == w*h*3, (path, w, h, len(px))
    return w, h, px

def at(img, x, y):
    w, h, p = img
    i = (y*w+x)*3
    return tuple(p[i:i+3])

main = ppm(sys.argv[1]); cam = ppm(sys.argv[2])
assert main[:2] == (320, 240), main[:2]
assert cam[:2] == (160, 120), cam[:2]
# 相机中心看到 captured_red；outside_green 位于相机水平视野之外。
cr = at(cam, 80, 60)
assert cr[0] > 220 and cr[1] < 60 and cr[2] < 60, ('camera center', cr)
bg = at(cam, 150, 10)
assert max(bg) < 40, ('camera background', bg)
# 主画面 monitor 中心直接采样本帧 RenderTexture，同样为红色。
mr = at(main, 235, 175)
assert mr[0] > 200 and mr[1] < 80 and mr[2] < 80, ('monitor center', mr)
# 主世界相机未被 RenderTexture 相机抢占；右上绿色对象仍在原位。
g = at(main, 260, 65)
assert g[1] > 180 and g[0] < 80, ('main camera', g)
print('render_texture_e2e: pixels ok', cr, mr, g)
PY

SCENE3D="$(dirname "$SCENE")/render_texture_3d.scn"
timeout 90 "$PLAYER" --shot "$WORK/main3d.ppm" --shot-frame 2 "$SCENE3D" >"$WORK/run3d.log" 2>&1 \
  || { cat "$WORK/run3d.log"; fail "Camera3D RenderTexture 运行失败"; }
grep -q "RenderTexture 'rendertexture://0/camera3d' 64x64.*camera=3d" "$WORK/run3d.log" \
  || { cat "$WORK/run3d.log"; fail "Camera3D RenderTexture 未创建"; }
"$PY" - "$WORK/main3d.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read(); _, size, _, px = d.split(b'\n', 3)
w, h = map(int, size.split()); assert (w, h) == (320, 240)
i = ((h//2)*w + w//2)*3
r, g, b = px[i:i+3]
assert r > 180 and g < 70 and b < 90, (r, g, b)
print('render_texture_e2e: camera3d center', (r, g, b))
PY

echo "render_texture_e2e: ok（Camera2D/Camera3D/Texture/screenshot）"
