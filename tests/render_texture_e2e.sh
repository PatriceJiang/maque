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

# 热重载（MAQUE_WATCH / Reload.scene）：Camera3D RenderTexture + glTF 必须能销毁重建。
# 回归点：
#   ① 根场景是 Engine 内嵌的 scene_store，旧实现 reload_scene() 对它 `delete scene` ——
#      对非堆地址调用 operator delete（ASan: attempting free on address which was not
#      malloc()-ed；Release: 堆损坏 0xC0000374）。reload 后的堆场景必须由 scene_owned 持有。
#   ② 相机 RenderTexture 的目标/URI 别名/renderer 句柄随旧世界释放；新目标不得让
#      resource_bytes 增长（泄漏一个 128x128 RT 都会体现在 vram 上）。
RELOAD_IN="$WORK/reload.in"
cat >"$RELOAD_IN" <<'JSON'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":2,"method":"Reload.scene","params":{}}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":4,"method":"Reload.scene","params":{}}
{"jsonrpc":"2.0","id":5,"method":"Runtime.step","params":{"frames":30}}
JSON
MAQUE_CONTROL=- MAQUE_TRACE_STATS=1 timeout 90 "$PLAYER" "$SCENE3D" <"$RELOAD_IN" >"$WORK/reload.log" 2>&1 \
  || { cat "$WORK/reload.log"; fail "RenderTexture 场景热重载崩溃（bad-free/堆损坏）"; }
[ "$(grep -c '"reloaded":true' "$WORK/reload.log")" -eq 2 ] \
  || { cat "$WORK/reload.log"; fail "Reload.scene 未成功执行两次"; }
VRAM_LIST="$(grep -o 'vram_bytes=[0-9]*' "$WORK/reload.log" | cut -d= -f2 | sort -u | tr '\n' ' ')"
[ "$(echo "$VRAM_LIST" | wc -w)" -eq 1 ] \
  || { cat "$WORK/reload.log"; fail "热重载后 VRAM 变化（纹理/网格泄漏）：$VRAM_LIST"; }
echo "render_texture_e2e: hot reload x2 vram_bytes=$VRAM_LIST"

# depth/MSAA 附件缓存（docs/bugs/004）：RenderTexture 128² 与主目标 320×240 交替时，
# 旧实现是 Device 级单槽 → 每帧至少 2 次 depth 重建（wgpu 直呈下还表现为进程内存持续增长）。
# 修复后 60 帧内 depth 只应为“不同尺寸的个数”建一次；阈值 8 给窗口/离屏切换留余量。
env MAQUE_TRACE_GPU_ALLOC=1 timeout 90 "$PLAYER" --frames 60 "$SCENE3D" >"$WORK/attach.log" 2>&1 \
  || { cat "$WORK/attach.log"; fail "附件缓存诊断运行失败"; }
DEPTH_CREATES=$(grep -c 'gfx: gpu attachment create depth ' "$WORK/attach.log" || true)
MSAA_CREATES=$(grep -c 'gfx: gpu attachment create msaa ' "$WORK/attach.log" || true)
[ "${DEPTH_CREATES:-0}" -le 8 ] \
  || { cat "$WORK/attach.log"; fail "60 帧 depth 附件重建 ${DEPTH_CREATES} 次（应只按尺寸建一次）"; }
[ "${MSAA_CREATES:-0}" -le 4 ] \
  || { cat "$WORK/attach.log"; fail "60 帧 MSAA 附件重建 ${MSAA_CREATES} 次"; }
echo "render_texture_e2e: attachment cache depth=${DEPTH_CREATES} msaa=${MSAA_CREATES} (60 frames)"

# ---- 逐目标 MSAA（docs/33 §格式、采样与限制）----
# 主目标与每张 RenderTexture 各算一份 frame3d.msaa_samples：
#   · 组件字段 render.RenderTexture.msaa_samples → 只有那张纹理变 4x；
#   · F4/MAQUE_ENGINE_OPTIONS 的 rt_msaa → 全部相机离屏目标一起变，主输出不受影响；
#   · 优先级：rt_msaa > msaa（面板） > MAQUE_MSAA > 组件字段 > scn.Scene.msaa_samples。
# 多采样只是 3D pass 的临时附件（pass 末 resolve 回单采样纹理），所以物理目标仍可被采样/回读。
SCENEMS="$(dirname "$SCENE")/render_texture_msaa.scn"
# 两份场景必须只差 duck 的 msaa_samples（防漂移：基线一改，这里的像素对照就失去意义）。
"$PY" - "$SCENE3D" "$SCENEMS" <<'PY' || exit 1
import json, sys

def raw(path):
    return json.load(open(path, encoding='utf-8'))

def norm(d):
    d = json.loads(json.dumps(d))
    d.pop('title', None)
    for ent in d.get('entities', []):
        for c in ent.get('components', []):
            if c.get('type') == 'render.RenderTexture':
                c.pop('msaa_samples', None)
    return d

a, b = (raw(p) for p in sys.argv[1:3])
assert norm(a) == norm(b), 'render_texture_msaa.scn 与 render_texture_3d.scn 已漂移（除 msaa_samples 外须一致）'
rts = [c for e in b['entities'] if e['name'] == 'camera_duck'
       for c in e['components'] if c.get('type') == 'render.RenderTexture']
assert rts and rts[0].get('msaa_samples') == 4, ('duck RT 应写 msaa_samples=4', rts)
print('render_texture_e2e: per-target MSAA 场景 = 基线 + duck msaa_samples=4')
PY
env MAQUE_TRACE_GPU_ALLOC=1 timeout 90 "$PLAYER" --shot "$WORK/msaa_rt.ppm" --shot-frame 2 "$SCENEMS" >"$WORK/runms.log" 2>&1 \
  || { cat "$WORK/runms.log"; fail "逐目标 MSAA 场景运行失败（附件/管线样本数不匹配？）"; }
grep -q 'gfx: gpu attachment create msaa 128x128 ' "$WORK/runms.log" \
  || { cat "$WORK/runms.log"; fail "duck RT 的 4x 附件未创建（组件 msaa_samples 未生效）"; }
grep -q 'gfx: gpu attachment create msaa 320x240 ' "$WORK/runms.log" \
  && { cat "$WORK/runms.log"; fail "主目标未开 MSAA，却给它建了 4x 附件"; }
# 只有 duck 变（边缘抗锯齿），fox 与主画面逐像素不变 → 证明样本数是「逐目标」而非全局。
"$PY" - "$WORK/main3d.ppm" "$WORK/msaa_rt.ppm" <<'PY' || exit 1
import sys

def ppm(path):
    d = open(path, 'rb').read()
    magic, size, maxv, px = d.split(b'\n', 3)
    assert magic == b'P6' and maxv == b'255', path
    w, h = map(int, size.split())
    assert len(px) == w * h * 3, (path, w, h, len(px))
    return w, h, px

a, b = ppm(sys.argv[1]), ppm(sys.argv[2])
assert a[:2] == b[:2] == (320, 240)

def diff(x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * a[0] + x) * 3
            if sum(abs(a[2][i + k] - b[2][i + k]) for k in range(3)) > 30:
                n += 1
    return n

duck = diff(20, 56, 148, 184)
fox = diff(172, 56, 300, 184)
outside = diff(0, 190, 320, 240)
assert duck > 20, ('duck RT 区域无抗锯齿差异（RT msaa_samples 未生效？）', duck)
assert fox == 0, ('fox RT 不该被 duck 的 MSAA 影响', fox)
assert outside == 0, ('主画面不该被 RT 的 MSAA 影响', outside)
print('render_texture_e2e: per-target MSAA duck-diff=', duck, 'fox-diff=', fox, 'main-diff=', outside)
PY

# rt_msaa=4：基线场景（两个 RT 都没有组件字段）→ 两张相机纹理一起 4x，主输出仍 1x。
env MAQUE_TRACE_GPU_ALLOC=1 MAQUE_RENDER3D_INFO=1 MAQUE_ENGINE_OPTIONS="rt_msaa=4" \
  timeout 90 "$PLAYER" --shot "$WORK/rt_msaa_on.ppm" --shot-frame 2 "$SCENE3D" >"$WORK/runms_on.log" 2>&1 \
  || { cat "$WORK/runms_on.log"; fail "MAQUE_ENGINE_OPTIONS=rt_msaa=4 运行失败"; }
grep -qF "msaa=scene rt_msaa=4x" "$WORK/runms_on.log" \
  || { cat "$WORK/runms_on.log"; fail "rt_msaa 未被 MAQUE_ENGINE_OPTIONS 解析（缺 env 日志）"; }
grep -q 'gfx: gpu attachment create msaa 128x128 ' "$WORK/runms_on.log" \
  || { cat "$WORK/runms_on.log"; fail "rt_msaa=4 未给相机纹理建 4x 附件"; }
grep -q 'gfx: gpu attachment create msaa 320x240 ' "$WORK/runms_on.log" \
  && { cat "$WORK/runms_on.log"; fail "rt_msaa 只该作用于相机离屏目标，主输出被改动了"; }
# rt_msaa=1：更专门的开关赢过组件字段 → duck 回到与基线逐像素一致，且完全没有多采样附件。
env MAQUE_TRACE_GPU_ALLOC=1 MAQUE_ENGINE_OPTIONS="rt_msaa=1" \
  timeout 90 "$PLAYER" --shot "$WORK/rt_msaa_off.ppm" --shot-frame 2 "$SCENEMS" >"$WORK/runms_off.log" 2>&1 \
  || { cat "$WORK/runms_off.log"; fail "MAQUE_ENGINE_OPTIONS=rt_msaa=1 运行失败"; }
grep -q 'gfx: gpu attachment create msaa ' "$WORK/runms_off.log" \
  && { cat "$WORK/runms_off.log"; fail "rt_msaa=1 应压过 duck 的 msaa_samples=4（仍建了 4x 附件）"; }
"$PY" - "$WORK/main3d.ppm" "$WORK/rt_msaa_off.ppm" "$WORK/rt_msaa_on.ppm" <<'PY' || exit 1
import sys

def ppm(path):
    d = open(path, 'rb').read()
    _, size, _, px = d.split(b'\n', 3)
    w, h = map(int, size.split())
    return w, h, px

base, off, on = (ppm(p) for p in sys.argv[1:4])

def diff(a, b, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * a[0] + x) * 3
            if sum(abs(a[2][i + k] - b[2][i + k]) for k in range(3)) > 30:
                n += 1
    return n

forced_off = diff(base, off, 20, 56, 148, 184)
both_on_duck = diff(base, on, 20, 56, 148, 184)
both_on_fox = diff(base, on, 172, 56, 300, 184)
assert forced_off == 0, ('rt_msaa=1 未压过组件 msaa_samples=4', forced_off)
assert both_on_duck > 20 and both_on_fox > 20, ('rt_msaa=4 应同时改变两张相机纹理', both_on_duck, both_on_fox)
print('render_texture_e2e: rt_msaa 覆盖 off→', forced_off, 'px on→ duck', both_on_duck, 'fox', both_on_fox)
PY
echo "render_texture_e2e: per-target MSAA ok（duck 单张 4x / rt_msaa 全局开关 / 主输出不受影响）"

# 2D 相机目标不吃 MSAA（2D 图元层恒单采样）：强开 rt_msaa=4 必须明确打印一次「无效」，
# 不建多采样附件、画面逐像素不变 —— 免得让人以为小地图开了抗锯齿。
env MAQUE_TRACE_GPU_ALLOC=1 MAQUE_ENGINE_OPTIONS="rt_msaa=4" \
  timeout 90 "$PLAYER" --shot "$WORK/rt2d_msaa.ppm" --shot-frame 5 "$SCENE2D" >"$WORK/run2d_msaa.log" 2>&1 \
  || { cat "$WORK/run2d_msaa.log"; fail "2D RenderTexture + rt_msaa=4 运行失败"; }
grep -qE "2D 相机目标.*MSAA 无效" "$WORK/run2d_msaa.log" \
  || { cat "$WORK/run2d_msaa.log"; fail "2D 目标的 MSAA 请求未被明确拒绝（缺提示）"; }
RT2D_MSAA_CREATES=$(grep -c 'gfx: gpu attachment create msaa ' "$WORK/run2d_msaa.log" || true)
[ "${RT2D_MSAA_CREATES:-0}" -eq 0 ] \
  || { cat "$WORK/run2d_msaa.log"; fail "2D 相机目标不该建多采样附件"; }
"$PY" - "$WORK/rot_a.ppm" "$WORK/rt2d_msaa.ppm" <<'PY' || exit 1
import sys

def ppm(path):
    d = open(path, 'rb').read()
    _, size, _, px = d.split(b'\n', 3)
    w, h = map(int, size.split())
    return w, h, px

a, b = ppm(sys.argv[1]), ppm(sys.argv[2])
assert a[:2] == b[:2]
same = a[2] == b[2]
assert same, '2D 目标在 rt_msaa=4 下画面被改动了（应完全忽略）'
print('render_texture_e2e: 2D RenderTexture 忽略 MSAA（逐像素一致 + 明确提示）')
PY
echo "render_texture_e2e: 2D 目标 MSAA 无效路径 ok"

# shader 精灵 → 2D RenderTexture → cube 面（docs/13 §场景自定义 shader 精灵）：
# 两个 render.ShaderSprite 先渲染进 rendertexture://sprites，再以 Basic（unlit）材质贴到 cube 正面。
SCENESS="$(dirname "$SCENE")/shader-sprites-cube.scn"
ROOT_E2E="$(cd "$(dirname "$0")/.." && pwd)"
timeout 120 "$PLAYER" --shot "$WORK/ss.ppm" --shot-frame 2 "$SCENESS" >"$WORK/runss.log" 2>&1 \
  || { cat "$WORK/runss.log"; fail "shader sprite + RenderTexture + cube 运行失败"; }
grep -q "RenderTexture 'rendertexture://0/sprites' 640x360.*camera=2d" "$WORK/runss.log" \
  || { cat "$WORK/runss.log"; fail "shader sprite 的 2D RenderTexture 未创建"; }
[ "$(grep -c 'shader sprite .* loaded' "$WORK/runss.log")" -ge 2 ] \
  || { cat "$WORK/runss.log"; fail "应加载 2 个 shader 精灵"; }
grep -qiE "shader sprite.*(失败|未找到|error|missing|not found)" "$WORK/runss.log" \
  && { cat "$WORK/runss.log"; fail "shader 精灵加载/编译报错"; }
# VFS 禁止 ".." 跨场景引用 → 场景要带 shader 副本；这条断言防止两份副本漂移。
for n in mandelbulb seascape; do
  cmp -s "$ROOT_E2E/scenes/shadertoy/$n.wgsl" \
         "$ROOT_E2E/scenes/render_texture/assets/shaders/$n.wgsl" \
    || fail "shader 副本漂移：scenes/render_texture/assets/shaders/$n.wgsl"
done
"$PY" - "$WORK/ss.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, size, maxv, px = d.split(b'\n', 3)
assert magic == b'P6' and maxv == b'255', magic
w, h = map(int, size.split())
assert (w, h) == (480, 360), (w, h)
def at(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]
def lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
# cube 正面的屏幕里同时有暖色（mandelbulb）与冷色（seascape）→ 两个 shader 精灵都在 RT 里。
warm = cool = 0
for y in range(40, 320):
    for x in range(w):
        r, g, b = at(x, y)
        if r - b > 30 and r > 90:
            warm += 1
        elif b - r > 15 and b > 90:
            cool += 1
# 顶部必须是背景：精灵只出现在 RT 里，不能以 2D overlay 铺在主画面。
top_max = max(lum(at(x, y)) for y in range(0, 40) for x in range(0, w, 2))
bg = at(5, 5)
caption = sum(1 for y in range(328, h) for x in range(w) if lum(at(x, y)) > 110)
print('render_texture_e2e: cube screen shader sprites warm=', warm, 'cool=', cool,
      'top_max=', round(top_max, 1), 'bg=', bg, 'caption=', caption)
assert warm > 800 and cool > 3000, ('RT 屏幕暖/冷像素不足', warm, cool)
assert top_max < 40, ('顶部不是背景', top_max)
assert max(bg) < 40, ('角落不是背景', bg)
assert caption > 150, ('标题文字缺失', caption)
PY
echo "render_texture_e2e: shader sprites -> RenderTexture -> cube face ok"

echo "render_texture_e2e: ok（Camera2D text rotation + Camera3D duck/fox + Texture/screenshot + hot reload + shader sprite cube）"
