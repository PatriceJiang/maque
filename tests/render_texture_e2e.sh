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
grep -q "RenderTexture 'rendertexture://0/duck' 128x128.*camera=3d" "$WORK/run3d.log" \
  || { cat "$WORK/run3d.log"; fail "Duck Camera3D RenderTexture 未创建"; }
grep -q "RenderTexture 'rendertexture://0/fox' 128x128.*camera=3d" "$WORK/run3d.log" \
  || { cat "$WORK/run3d.log"; fail "Fox Camera3D RenderTexture 未创建"; }
"$PY" - "$WORK/main3d.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read(); _, size, _, px = d.split(b'\n', 3)
w, h = map(int, size.split()); assert (w, h) == (320, 240)
def at(x, y):
    i = (y * w + x) * 3
    return tuple(px[i:i+3])
def colors(x0, y0, x1, y1):
    return {at(x, y) for y in range(y0, y1) for x in range(x0, x1)}
def count(x0, y0, x1, y1, pred):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if pred(at(x, y)))
# 单像素会落在模型暗部（Fox 的腿/阴影），用区域颜色计数判定各自 RT 的内容。
duck_yellow = count(20, 56, 148, 184, lambda c: c[0] > 150 and c[1] > 120 and c[2] < 90)
fox_orange = count(172, 56, 300, 184, lambda c: c[0] > 150 and 60 < c[1] < 200 and c[2] < 110)
duck = at(84, 120)
fox = at(236, 120)
bg = at(160, 120)
assert duck_yellow > 300, ('duck yellow px', duck_yellow)
assert fox_orange > 300, ('fox orange px', fox_orange)
assert max(bg) < 30, ('main background', bg)
assert len(colors(20, 56, 148, 184)) > 300, 'Duck RT 画面颜色过少'
assert len(colors(172, 56, 300, 184)) > 300, 'Fox RT 画面颜色过少'
print('render_texture_e2e: camera3d duck/fox', duck, fox, 'px', duck_yellow, fox_orange, 'bg', bg)
PY

# 2D 相机 + 文本旋转：同一 RT 目标在 5/40 帧的 monitor 区域必须变化（Label 节点旋转写进纹理）。
SCENE2D="$(dirname "$SCENE")/render_texture_2d.scn"
timeout 90 "$PLAYER" --shot "$WORK/rot_a.ppm" --shot-frame 5 "$SCENE2D" >"$WORK/run2d_a.log" 2>&1 \
  || { cat "$WORK/run2d_a.log"; fail "2D text rotation frame 5 运行失败"; }
timeout 90 "$PLAYER" --shot "$WORK/rot_b.ppm" --shot-frame 40 "$SCENE2D" >"$WORK/run2d_b.log" 2>&1 \
  || { cat "$WORK/run2d_b.log"; fail "2D text rotation frame 40 运行失败"; }
grep -q "RenderTexture 'rendertexture://0/text' 160x160.*camera=2d" "$WORK/run2d_a.log" \
  || { cat "$WORK/run2d_a.log"; fail "2D text RenderTexture 未创建"; }
grep -q "render texture 2d=rendertexture://0/text" "$WORK/run2d_a.log" \
  || { cat "$WORK/run2d_a.log"; fail "2D camera_texture 返回值错误"; }
"$PY" - "$WORK/rot_a.ppm" "$WORK/rot_b.ppm" <<'PY' || exit 1
import sys

def ppm(path):
    d = open(path, 'rb').read()
    magic, size, maxv, px = d.split(b'\n', 3)
    assert magic == b'P6' and maxv == b'255', path
    w, h = map(int, size.split())
    assert len(px) == w*h*3, (path, w, h, len(px))
    return w, h, px

a = ppm(sys.argv[1]); b = ppm(sys.argv[2])
assert a[:2] == (320, 240) and b[:2] == (320, 240)

def diff(x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * a[0] + x) * 3
            if sum(abs(a[2][i+k] - b[2][i+k]) for k in range(3)) > 30:
                n += 1
    return n

# monitor 中心 = world (236,128) -> 屏幕 (236,112)，160x160 → x 156..316 / y 32..192。
d = diff(156, 32, 316, 192)
assert d > 2000, ('2d text rotation 未生效（monitor 区域无变化）', d)
print('render_texture_e2e: 2d text rotation diff px', d)
PY

echo "render_texture_e2e: ok（Camera2D text rotation + Camera3D duck/fox + Texture/screenshot）"
