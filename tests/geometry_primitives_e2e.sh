#!/usr/bin/env bash
# 程序化几何图元端到端（plans/27-threejs-feature-migration.md M1-1）：
#   render.MeshRenderer.geometry = Cube/Sphere/Plane/Cylinder/Cone/Torus/Capsule
#   ① --shot 日志：6 个非缺省图元各自 create_mesh（Cube 缺省仍走内建 cube_mesh）
#   ② PPM 结构断言：7 个物体都在、球角是背景/立方体中心是前景、圆锥上窄下宽
#   ③ 同一帧两次渲染逐字节一致
#   ④ 旧场景（scene 完全不写 geometry）不创建程序化图元 → 旧路径零变化
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "geometry_primitives_e2e: skip（无显示环境）"
    exit 77
fi
[ -f "$SCN" ] || { echo "geometry_primitives_e2e: skip（缺 $SCN）"; exit 77; }
fail() { echo "geometry_primitives_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_ENV_IBL MAQUE_RENDER3D_INFO 2>/dev/null || true
export MAQUE_WINDOW=0x0

timeout 90 "$PLAYER" --shot "$WORK/a.ppm" --shot-frame 4 --frames 5 "$SCN" >"$WORK/a.txt" 2>&1 \
    || { cat "$WORK/a.txt"; fail "图元场景渲染失败"; }
grep -q "runtime: shot" "$WORK/a.txt" || { cat "$WORK/a.txt"; fail "截图未写入"; }
grep -q "3d scene cams=1 meshes=7" "$WORK/a.txt" || { cat "$WORK/a.txt"; fail "实体/网格数不符"; }

# 拾取走程序化图元的解析包围盒（不是旧 cube 的 ±0.6）：三个中心命中、空隙未命中。
grep -q "primitive pick 384,240 id=[0-9][0-9]* name=p1_sphere" "$WORK/a.txt" \
    || { cat "$WORK/a.txt"; fail "球心拾取失败"; }
grep -q "primitive pick 640,240 id=[0-9][0-9]* name=p3_cylinder" "$WORK/a.txt" \
    || { cat "$WORK/a.txt"; fail "圆柱中心拾取失败"; }
grep -q "primitive pick 768,213 id=[0-9][0-9]* name=p4_cone" "$WORK/a.txt" \
    || { cat "$WORK/a.txt"; fail "圆锥拾取失败"; }
grep -q "primitive pick 384,39 id=-1" "$WORK/a.txt" \
    || { cat "$WORK/a.txt"; fail "空隙应未命中"; }

# 6 个非缺省图元（Cube 缺省用内建 cube_mesh，不创建程序化网格）
n_prim=$(grep -c "runtime: primitive geometry=" "$WORK/a.txt" || true)
[ "$n_prim" -eq 6 ] || { cat "$WORK/a.txt"; fail "程序化图元数量=$n_prim（期望 6）"; }
for k in 1 2 3 4 5 6; do
    grep -q "runtime: primitive geometry=$k " "$WORK/a.txt" \
        || { cat "$WORK/a.txt"; fail "缺少 geometry=$k 的创建日志"; }
done
grep -q "cube_mesh=0" "$WORK/a.txt" || { cat "$WORK/a.txt"; fail "内建 cube 未创建"; }

"$PY" - "$WORK/a.ppm" <<'PY' || exit 1
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


def fg(x, y):
    return diff(rgb(x, y), bg) > 25


def win(cx, cy, hw, hh):
    n = t = 0
    for y in range(max(0, cy-hh), min(h, cy+hh)):
        for x in range(max(0, cx-hw), min(w, cx+hw)):
            t += 1
            if fg(x, y):
                n += 1
    return n / max(1, t)

# 7 个物体在 x 方向的中心（1280x480、相机 z=7.2/fov40 的固定取景）
centers = [255, 383, 512, 640, 768, 897, 1025]
ratios = [win(c, 240, 45, 45) for c in centers]
total = sum(fg(x, y) for y in range(h) for x in range(0, w, 2)) / (h * (w // 2))
sphere_corner = fg(383 - 40, 200)
cube_center = fg(255, 240)
cone_top = win(768, 215, 35, 25)
cone_bot = win(768, 265, 35, 25)
print(f'geometry_primitives_e2e: {w}x{h} nonbg={total:.3f} win={[round(v, 2) for v in ratios]} '
      f'sphere_corner={sphere_corner} cube_center={cube_center} cone_top/bot={cone_top:.2f}/{cone_bot:.2f}')

ok = True
ok &= 0.04 < total < 0.30
ok &= all(v > 0.30 for v in ratios)
ok &= not sphere_corner          # 球的外接盒角上是背景
ok &= cube_center                # 立方体中心是前景
ok &= cone_top < 0.5 * cone_bot  # 圆锥上窄下宽
sys.exit(0 if ok else 1)
PY
echo "geometry_primitives_e2e: ① 图元创建 + ② 结构断言 OK"

timeout 90 "$PLAYER" --shot "$WORK/b.ppm" --shot-frame 4 --frames 5 "$SCN" >"$WORK/b.txt" 2>&1 \
    || { cat "$WORK/b.txt"; fail "第二次渲染失败"; }
cmp -s "$WORK/a.ppm" "$WORK/b.ppm" || fail "两次渲染不一致（非确定性）"
echo "geometry_primitives_e2e: ③ 确定性 OK"

CUBE="${SCN%primitives.scn}cube.scn"
if [ -f "$CUBE" ]; then
    timeout 90 "$PLAYER" --shot "$WORK/cube.ppm" --frames 2 "$CUBE" >"$WORK/cube.txt" 2>&1 \
        || { cat "$WORK/cube.txt"; fail "旧 cube 场景渲染失败"; }
    if grep -q "runtime: primitive geometry=" "$WORK/cube.txt"; then
        cat "$WORK/cube.txt"; fail "默认 MeshRenderer 不应创建程序化图元（旧路径漂移）"
    fi
    grep -q "cube_mesh=" "$WORK/cube.txt" || { cat "$WORK/cube.txt"; fail "内建 cube 路径缺失"; }
    echo "geometry_primitives_e2e: ④ 旧场景零变化 OK"
else
    echo "geometry_primitives_e2e: ④ 跳过（缺 $CUBE）"
fi

echo "geometry_primitives_e2e: OK"
