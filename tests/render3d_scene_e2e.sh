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
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_ENV_IBL 2>/dev/null || true

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
    timeout 90 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/mat.ppm" --frames 2 "$MAT_SCN" >"$WORK/mat.txt" 2>&1 \
        || { cat "$WORK/mat.txt"; fail "PBR 材质场景渲染失败"; }
    grep -q "3d scene" "$WORK/mat.txt" || { cat "$WORK/mat.txt"; fail "材质场景无 3D 内容"; }
    # M4：材质 albedo 贴图真的走了渲染器纹理句柄（builtin:/ 或 VFS → create_texture）
    grep -q "pbr albedo texture" "$WORK/mat.txt" || { cat "$WORK/mat.txt"; fail "albedo 贴图未加载/未绑定"; }
    "$PY" - "$WORK/mat.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
def lum(i):
    return 0.299*px[i] + 0.587*px[i+1] + 0.114*px[i+2]
def stats(lo, hi):
    vals = [lum((y*w+x)*3) for y in range(h) for x in range(lo, hi)]
    vals = [v for v in vals if v > 30]
    if not vals:
        return 0.0, 0, 0.0, 0.0
    mean = sum(vals) / len(vals)
    sd = (sum((v-mean)**2 for v in vals) / len(vals)) ** 0.5
    hot = sum(1 for v in vals if v > 110) / len(vals)
    return mean, len(vals), sd, hot
lm, ln, lsd, lhot = stats(0, w // 2)
rm, rn, rsd, rhot = stats(w // 2, w)
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
print(f'render3d_scene_e2e: PBR smooth_mean={lm:.1f} rough_mean={rm:.1f} '
      f'hot={lhot:.2f}/{rhot:.2f} sd={lsd:.1f}/{rsd:.1f} nonbg={nonbg/(w*h):.3f}')
# analytic studio IBL（plans/19）下：光滑金属 = 锐利环境反射（高对比/低平均），粗糙金属 = 能量摊平
# （高平均/低对比/几乎全部越过中亮阈值）。注：M1 修复法线缺省 + 阴影数学后旧断言已两次更新。
ok = (ln > 500 and rn > 500 and rm > lm + 15 and rhot > 0.9 and rsd < lsd and nonbg / (w*h) > 0.05)
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: ③ PBR 材质（GGX/tonemap，粗糙度对比）OK"
fi

# ---- ④ 离线导入资产闭环（M4c）：glTF → .mesh.json（工具）→ render.MeshRenderer.mesh_path → 渲染 ----
IMPORT_SCN="${SCN%cube.scn}imported.scn"
if [ -f "$IMPORT_SCN" ]; then
    ROOT="$(cd "$(dirname "$0")/.." && pwd)"
    IMPORTED="$ROOT/scenes/pbr/assets/cube.mesh.json"
    [ -f "$IMPORTED" ] || fail "缺已入库导入资产 scenes/pbr/assets/cube.mesh.json"
    timeout 90 "$PY" "$ROOT/tools/gltf_import.py" "$ROOT/tests/data/gltf/cube.gltf" --out "$WORK/import" \
        >"$WORK/import.txt" 2>&1 || { cat "$WORK/import.txt"; fail "glTF 导入失败"; }
    cmp -s "$WORK/import/cube.mesh.json" "$IMPORTED" \
        || fail "已入库资产与 importer 输出不一致（导入资产漂移）"
    ( cd "$ROOT" && timeout 90 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/imported.ppm" --frames 2 "$IMPORT_SCN" \
        >"$WORK/imported.txt" 2>&1 ) || { cat "$WORK/imported.txt"; fail "导入网格场景渲染失败"; }
    grep -q "mesh renderer node=" "$WORK/imported.txt" || { cat "$WORK/imported.txt"; fail "mesh_path 未走加载路径"; }
    # M4d：MR / AO 贴图槽要真正加载并绑定（无贴图时走 1×1 白纹理，不影响标量材质）
    grep -q "pbr mr texture" "$WORK/imported.txt" || { cat "$WORK/imported.txt"; fail "metallic-roughness 贴图未加载"; }
    grep -q "pbr ao texture" "$WORK/imported.txt" || { cat "$WORK/imported.txt"; fail "occlusion 贴图未加载"; }
    if grep -q "→ mesh=-1\|mesh=-1" "$WORK/imported.txt"; then
        cat "$WORK/imported.txt"; fail "mesh_path 指向的 .mesh.json 加载失败（VFS 根 = 场景目录）"
    fi
    grep -q "runtime: shot" "$WORK/imported.txt" || { cat "$WORK/imported.txt"; fail "导入网格场景未截图"; }
    "$PY" - "$WORK/imported.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
ratio = nonbg / (w * h)
print(f'render3d_scene_e2e: imported mesh nonbg={ratio:.3f}')
sys.exit(0 if ratio > 0.03 else 1)
PY
    echo "render3d_scene_e2e: ④ 离线导入网格（glTF→.mesh.json→渲染）OK"
fi

# ---- ⑤ Khronos ToyCar PBR 场景（scenes/khronos/ToyCar，离线导入的复杂 glTF）----
KHRONOS_SCN="$ROOT/scenes/khronos/ToyCar/ToyCar.scn"
if [ -f "$KHRONOS_SCN" ] && [ -f "$ROOT/scenes/khronos/ToyCar/assets/ToyCar_0_0.mesh.json" ]; then
    ( cd "$ROOT" && timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/khronos.ppm" --frames 2 "$KHRONOS_SCN" \
        >"$WORK/khronos.txt" 2>&1 ) || { cat "$WORK/khronos.txt"; fail "Khronos 场景渲染失败"; }
    grep -q "3d scene cams=1 meshes=3 lights=1" "$WORK/khronos.txt" || { cat "$WORK/khronos.txt"; fail "Khronos 场景 3D 内容不符"; }
    grep -q "pbr normal texture" "$WORK/khronos.txt" || { cat "$WORK/khronos.txt"; fail "Khronos 场景法线贴图未加载"; }
    "$PY" - "$WORK/khronos.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
ratio = nonbg / (w * h)
print(f'render3d_scene_e2e: khronos_toycar nonbg={ratio:.3f}')
sys.exit(0 if ratio > 0.05 else 1)
PY
    echo "render3d_scene_e2e: ⑤ Khronos ToyCar（离线导入复杂 glTF + PBR 贴图）OK"
fi

# ---- ⑥ OrbitControls：MAQUE_ORBIT 覆盖相机角度（CI 无真实鼠标，验证环绕参数链路）----
if [ -f "$SCN" ]; then
    timeout 90 env MAQUE_ORBIT="0.0,0.30,4.4" "$PLAYER" --shot "$WORK/orbit_a.ppm" --frames 2 "$SCN" >"$WORK/orbit_a.txt" 2>&1 \
        || { cat "$WORK/orbit_a.txt"; fail "OrbitControls 渲染 A 失败"; }
    timeout 90 env MAQUE_ORBIT="0.9,0.30,4.4" "$PLAYER" --shot "$WORK/orbit_b.ppm" --frames 2 "$SCN" >"$WORK/orbit_b.txt" 2>&1 \
        || { cat "$WORK/orbit_b.txt"; fail "OrbitControls 渲染 B 失败"; }
    "$PY" - "$WORK/orbit_a.ppm" "$WORK/orbit_b.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split()); return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
bg = a[0:3]
def nonbg(px):
    return sum(1 for i in range(0, len(px), 3)
               if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
na, nb = nonbg(a), nonbg(b)
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i]-b[i]) + abs(a[i+1]-b[i+1]) + abs(a[i+2]-b[i+2]) > 30)
print(f'render3d_scene_e2e: orbit yaw 0->0.9 nonbg=({na},{nb}) diff={diff}')
sys.exit(0 if na > 500 and nb > 500 and diff > 3000 else 1)
PY
    echo "render3d_scene_e2e: ⑥ OrbitControls（环绕/缩放参数链路）OK"
fi

# ---- ⑦ 透明材质（M5a）：BLEND 半透明混合 / MASK alpha 丢弃 / 不透明不受影响 ----
TRANS_SCN="$ROOT/scenes/pbr/transparency.scn"
if [ -f "$TRANS_SCN" ]; then
    ( cd "$ROOT" && timeout 90 "$PLAYER" --shot "$WORK/trans.ppm" --frames 2 scenes/pbr/transparency.scn \
        >"$WORK/trans.txt" 2>&1 ) || { cat "$WORK/trans.txt"; fail "透明材质场景渲染失败"; }
    "$PY" - "$WORK/trans.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _m, px = d.split(b'\n', 3)
assert magic == b'P6'
w, h = map(int, dims.split())
def rgb(x, y):
    i = (y * w + x) * 3
    return px[i], px[i+1], px[i+2]
def at(xr):
    return rgb(int(w * xr), h // 2)
red = at(0.33)     # 不透明红 cube
mid = at(0.50)     # 黄 panel(alpha 0.5) 叠在蓝 cube 上 → 同时含红/蓝分量
grn = at(0.66)     # MASK panel(alpha 0.2<0.5) 被丢弃 → 绿色 cube 可见
print(f'render3d_scene_e2e: transparency red={red} mid={mid} green={grn}')
ok = (red[0] > 90 and red[0] > red[1] * 2 and red[0] > red[2] * 2
      and mid[2] > 20 and mid[0] > 20
      and grn[1] > 60 and grn[1] > grn[0] and grn[1] > grn[2])
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: ⑦ 透明材质（BLEND 混合 / MASK 裁剪）OK"
fi

# ---- ⑧ 多光源（M5b）：Point（红）与 Spot（绿）投影到不同 cube ----
LIGHTS_SCN="$ROOT/scenes/pbr/lights.scn"
if [ -f "$LIGHTS_SCN" ]; then
    ( cd "$ROOT" && timeout 90 "$PLAYER" --shot "$WORK/lights.ppm" --frames 2 scenes/pbr/lights.scn \
        >"$WORK/lights.txt" 2>&1 ) || { cat "$WORK/lights.txt"; fail "多光源场景渲染失败"; }
    "$PY" - "$WORK/lights.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _m, px = d.split(b'\n', 3)
assert magic == b'P6'
w, h = map(int, dims.split())
def rgb(cx, cy):
    i = (cy * w + cx) * 3
    return px[i], px[i+1], px[i+2]
left = rgb(int(w*0.34), h//2)
mid = rgb(int(w*0.50), h//2)
right = rgb(int(w*0.66), h//2)
print(f'render3d_scene_e2e: lights point={left} spot_mid={mid} right={right}')
ok = (left[0] > left[1] + 12 and left[0] > left[2] + 12
      and mid[1] > mid[0] + 5 and mid[1] > mid[2] + 8
      and abs(right[0]-right[1]) < 12)
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: ⑧ 多光源（Point/Spot）OK"
fi

# ---- ⑨ 方向光阴影（M5c）：MAQUE_SHADOW=0/1 同场景像素差 ----
SHADOW_SCN="$ROOT/scenes/pbr/shadows.scn"
if [ -f "$SHADOW_SCN" ]; then
    ( cd "$ROOT" && timeout 90 "$PLAYER" --shot "$WORK/shadow_on.ppm" --frames 2 scenes/pbr/shadows.scn \
        >"$WORK/shadow_on.txt" 2>&1 ) || { cat "$WORK/shadow_on.txt"; fail "阴影开启渲染失败"; }
    ( cd "$ROOT" && timeout 90 env MAQUE_SHADOW=0 "$PLAYER" --shot "$WORK/shadow_off.ppm" --frames 2 scenes/pbr/shadows.scn \
        >"$WORK/shadow_off.txt" 2>&1 ) || { cat "$WORK/shadow_off.txt"; fail "阴影关闭渲染失败"; }
    "$PY" - "$WORK/shadow_on.ppm" "$WORK/shadow_off.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); m, dims, _x, px = d.split(b'\n', 3)
    assert m == b'P6'
    w, h = map(int, dims.split()); return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
def lum(px, i):
    return 0.299 * px[i] + 0.587 * px[i+1] + 0.114 * px[i+2]
darker = sum(1 for i in range(0, len(a), 3) if lum(a, i) + 15.0 < lum(b, i))
print(f'render3d_scene_e2e: shadow darker_pixels={darker} of {w*h}')
# M6/plans/19 修复阴影两处数学 bug（light 正交近远平面传反 + shadow uv.y 未翻）后，
# 正确阴影 = cube 投影的紧凑四边形（数百像素），不再是"整个 shadow 视锥误判"（数万像素）。
# 双边界断言：既要有真实投影，也不能回退成大范围误遮挡。
ok = (darker > 300 and darker < w * h // 4)
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: ⑨ 方向光阴影（shadow map + PCF）OK"
fi
# ---- (8) envmap/IBL: MAQUE_ENV_IBL=1 builtin env must differ from analytic default ----
if [ -f "$SCN" ]; then
    ( cd "$ROOT" && timeout 90 env MAQUE_ENV_IBL=1 "$PLAYER" --shot "$WORK/env_on.ppm" --frames 2 "$SCN"         >"$WORK/env_on.txt" 2>&1 ) || { cat "$WORK/env_on.txt"; fail "envmap/IBL forced render failed"; }
    cmp -s "$WORK/scene.ppm" "$WORK/env_on.ppm" && fail "envmap/IBL switch has no effect (env not active)"
    "$PY" - "$WORK/scene.ppm" "$WORK/env_on.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); m, dims, _x, px = d.split(b'\n', 3)
    assert m == b'P6'
    w, h = map(int, dims.split()); return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
def lum(px, i):
    return 0.299 * px[i] + 0.587 * px[i+1] + 0.114 * px[i+2]
diff = sum(abs(a[i]-b[i]) for i in range(0, len(a), 3))
print(f'render3d_scene_e2e: envmap mean_abs_diff={diff / (w*h):.3f}')
sys.exit(0 if diff / (w*h) > 0.02 else 1)
PY
    echo "render3d_scene_e2e: (8) envmap/IBL OK"
fi
# ---- (9) envmap sky background: scenes/pbr/env_ibl.scn (HDR .hdr) must show a bright sky ----
ENV_SCN="$ROOT/scenes/pbr/env_ibl.scn"
if [ -f "$ENV_SCN" ]; then
    ( cd "$ROOT" && timeout 90 "$PLAYER" --shot "$WORK/env_sky.ppm" --frames 2 scenes/pbr/env_ibl.scn \
        >"$WORK/env_sky.txt" 2>&1 ) || { cat "$WORK/env_sky.txt"; fail "HDR env sky render failed"; }
    ( cd "$ROOT" && timeout 90 env MAQUE_ENV_IBL=0 "$PLAYER" --shot "$WORK/env_sky_off.ppm" --frames 2 scenes/pbr/env_ibl.scn \
        >"$WORK/env_sky_off.txt" 2>&1 ) || { cat "$WORK/env_sky_off.txt"; fail "HDR env sky off render failed"; }
    cmp -s "$WORK/env_sky.ppm" "$WORK/env_sky_off.ppm" && fail "HDR env has no effect on env_ibl scene"
    "$PY" - "$WORK/env_sky.ppm" "$WORK/env_sky_off.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); m, dims, _x, px = d.split(b'\n', 3)
    assert m == b'P6'
    w, h = map(int, dims.split()); return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
def corner(px):
    i = (2 * w + 2) * 3
    return px[i], px[i+1], px[i+2]
ca = corner(a); cb = corner(b)
lum = lambda c: 0.299*c[0] + 0.587*c[1] + 0.114*c[2]
print(f'render3d_scene_e2e: env sky corner_on={tuple(ca)} corner_off={tuple(cb)}')
ok = lum(ca) > 120 and lum(ca) > lum(cb) + 60
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: (9) envmap sky background OK"
fi
# ---- (10) 真 PMREM：GGX 粗糙度预过滤 + 漫反射 irradiance（scenes/pbr/env_pmrem.scn）----
PMREM_SCN="$ROOT/scenes/pbr/env_pmrem.scn"
if [ -f "$PMREM_SCN" ]; then
    ( cd "$ROOT" && timeout 120 "$PLAYER" --shot "$WORK/pmrem_on.ppm" --frames 2 scenes/pbr/env_pmrem.scn \
        >"$WORK/pmrem_on.txt" 2>&1 ) || { cat "$WORK/pmrem_on.txt"; fail "PMREM 场景渲染失败"; }
    ( cd "$ROOT" && timeout 120 env MAQUE_ENV_IBL=0 "$PLAYER" --shot "$WORK/pmrem_off.ppm" --frames 2 scenes/pbr/env_pmrem.scn \
        >"$WORK/pmrem_off.txt" 2>&1 ) || { cat "$WORK/pmrem_off.txt"; fail "PMREM analytic 对照渲染失败"; }
    grep -q "env PMREM .*GGX.*irradiance" "$WORK/pmrem_on.txt" \
        || { cat "$WORK/pmrem_on.txt"; fail "PMREM 预过滤日志缺失（.hdr 未走 GGX + irradiance）"; }
    "$PY" - "$WORK/pmrem_on.ppm" "$WORK/pmrem_off.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); m, dims, _x, px = d.split(b'\n', 3)
    assert m == b'P6'
    w, h = map(int, dims.split())
    return w, h, [(px[i], px[i+1], px[i+2]) for i in range(0, len(px), 3)]
def lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
def region(px, w, h, xr0, xr1, yr0, yr1):
    vals = [lum(px[y * w + x]) for y in range(int(h * yr0), int(h * yr1))
            for x in range(int(w * xr0), int(w * xr1))]
    m = sum(vals) / len(vals)
    sd = (sum((v - m) ** 2 for v in vals) / len(vals)) ** 0.5
    return m, sd
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
sky = a[2 * w + 2]
left_m, left_sd = region(a, w, h, .13, .36, .30, .72)
mid_m, _ = region(a, w, h, .38, .62, .30, .72)
right_m, right_sd = region(a, w, h, .64, .87, .30, .72)
mid_off, _ = region(b, w, h, .38, .62, .30, .72)
print(f'render3d_scene_e2e: PMREM sky={sky} metal_mean={left_m:.1f}/{right_m:.1f} '
      f'metal_sd={left_sd:.1f}/{right_sd:.1f} matte_env={mid_m:.1f} matte_analytic={mid_off:.1f}')
# 天空 = HDR env 可见背景；镜面：光滑金属局部对比 > 粗糙金属；
# 漫反射：matte 立方体在 PMREM irradiance 下与 analytic 有可测差异（旧实现 envmap 只影响镜面）。
ok = (lum(sky) > 120 and mid_m > mid_off + 20.0
      and left_sd > right_sd + 3.0 and left_m > 60.0 and right_m > 60.0)
sys.exit(0 if ok else 1)
PY
    echo "render3d_scene_e2e: (10) PMREM（GGX 预过滤 + irradiance + 天空）OK"
fi
# ---- (11) KHR_materials_specular 贴图：specularColorTexture 真正参与电介质 F0 ----
SPEC_SCN="$ROOT/scenes/pbr/material_specular.scn"
SPEC_BASE_SCN="$ROOT/scenes/pbr/material_specular_base.scn"
if [ -f "$SPEC_SCN" ] && [ -f "$SPEC_BASE_SCN" ]; then
    ( cd "$ROOT" && timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/spec_tex.ppm" --frames 2 scenes/pbr/material_specular.scn \
        >"$WORK/spec_tex.txt" 2>&1 ) || { cat "$WORK/spec_tex.txt"; fail "specular 贴图场景渲染失败"; }
    ( cd "$ROOT" && timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/spec_off.ppm" --frames 2 scenes/pbr/material_specular_base.scn \
        >"$WORK/spec_off.txt" 2>&1 ) || { cat "$WORK/spec_off.txt"; fail "specular 对照场景渲染失败"; }
    grep -q "pbr specular texture specular_color_texture.*-> [0-9]" "$WORK/spec_tex.txt" \
        || { cat "$WORK/spec_tex.txt"; fail "specular_color_texture 未加载（runtime 映射缺失）"; }
    grep -q "pbr specular texture" "$WORK/spec_off.txt" && fail "对照场景不应加载 specular 贴图"
    "$PY" - "$WORK/spec_tex.ppm" "$WORK/spec_off.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); m, dims, _x, px = d.split(b'\n', 3)
    assert m == b'P6'
    w, h = map(int, dims.split())
    return w, h, [(px[i], px[i+1], px[i+2]) for i in range(0, len(px), 3)]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
def diff_n(x0, x1, y0, y1):
    n = 0
    for y in range(int(h * y0), int(h * y1)):
        for x in range(int(w * x0), int(w * x1)):
            i = y * w + x
            if sum(abs(a[i][k] - b[i][k]) for k in range(3)) > 12:
                n += 1
    return n
right = diff_n(.62, .82, .25, .80)    # 有 specularColorTexture 的立方体
left = diff_n(.18, .38, .25, .80)     # 相同参数但无贴图的对照立方体
print(f'render3d_scene_e2e: specular tex diff right={right} left={left}')
# checker 贴图按 UV 调制 specularColorFactor → 右侧大片像素亮度变化；左侧无贴图应几乎不变。
sys.exit(0 if right > 1000 and left < 50 else 1)
PY
    echo "render3d_scene_e2e: (11) KHR_materials_specular 贴图 OK"
fi
echo "render3d_scene_e2e: PASS"
