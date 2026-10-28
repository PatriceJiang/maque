#!/usr/bin/env bash
# 运行时直读 glTF 的 **KHR_animation_pointer** 端到端（plans/24 P6 部分）。
#
# 背景：runtime 的 C++ 直读加载器支持 KHR_animation_pointer 的**材质数值属性**子集：
# `/materials/<i>/...` 的 baseColor / metallic / roughness / emissive / alphaCutoff / KHR 材质扩展因子等，
# 逐帧采样后更新 `mq_renderer_pbr_material`（节点指针/贴图指针通道跳过）。
# 该夹具用一条 pointer 通道动画改 `pbrMetallicRoughness.baseColorFactor`，颜色 红→绿。
# `MAQUE_RENDER3D_INFO=1` 时装配期打印：
#   `runtime: gltf anim '<clip>' channels=<N> duration=<sec>`
#   `runtime: gltf model '<path>' primitives=... materials=... anims=1 ...`
#   `runtime: shot[i] '<path>' 640x360 frame=<N>`（--shot-seq 每帧一行）
#
# 夹具（tests/data/gltf/runtime_pointer/，Lead 已生成）：
#   pointer.gltf  三角形 + unlit 材质 `anim_mat`（baseColor 红 (0.9,0.15,0.1,1)）；
#                 animation `color_anim` 一条 pointer 通道
#                 `/materials/0/pbrMetallicRoughness/baseColorFactor`，时间 0→1，红→绿 (0.1,0.9,0.2,1)。
#   pointer.scn   640x360/views1、camera pos(0,0.1,3)、fov45、1 实体 MeshRenderer.mesh_path=pointer.gltf。
#
# 断言（`--shot-seq --shot-frames 1,40` 拍两帧）：
#   - 日志含 `runtime: gltf anim 'color_anim' channels=1`；两帧 shot 行都在；无 gltf 加载失败/ASan；
#   - 两帧 PPM 都是 640x360、nonbg>0.03、center>500（几何相同，只有材质颜色变）；
#   - 中心像素颜色（±20 容差）：f1 红主导（r-g>20）、f40 绿主导（g-r>20）
#     —— 实测 f1 (240,116,90) / f40 (160,213,114)；
#   - 两帧 PPM 逐字节差异 >1000 字节（实测 70296）→ pointer 通道的逐帧采样/材质更新真的生效。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_pointer）；$3 = 工作目录
# 无显示环境可跑：--shot-seq 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_pointer_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_pointer_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_pointer_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_pointer_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN="$FIXDIR/pointer.scn"
for f in "$SCN" "$FIXDIR/pointer.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_pointer_e2e: skip（缺夹具 $f）"; exit 77; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
# 清掉可能干扰离屏渲染/诊断的环境；强制无窗口 + 纯离屏
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_ENV_IBL \
      MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO MAQUE_WINDOW \
      MAQUE_WINDOW_EVENT_AT MAQUE_ANIM MAQUE_SKIN MAQUE_MORPH 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# PPM 检查器：结构 / 中心像素颜色 / 逐字节差异
cat >"$WORK/ppm_check.py" <<'PY'
import sys

BG_TOL = 25          # 与渲染清屏色的 L1 距离阈值（与 render3d_scene_e2e 一致）
COLOR_TOL = 20       # 中心像素主色相对其它通道的最小差值（允许采样/量化误差）


def load(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{path}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{path}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


def cmd_struct(path, label, min_nonbg, min_center):
    w, h, px = load(path)
    assert (w, h) == (640, 360), f'{label}: 尺寸 {w}x{h} != 设计分辨率 640x360'
    def rgb(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]
    bg = rgb(2, 2)
    diff = lambda c: abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2])
    nonbg = center = 0
    for y in range(h):
        for x in range(w):
            if diff(rgb(x, y)) > BG_TOL:
                nonbg += 1
                if w // 3 <= x < w * 2 // 3 and h // 3 <= y < h * 2 // 3:
                    center += 1
    ratio = nonbg / (w * h)
    print(f'gltf_runtime_pointer_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > min_center):
        print(f'gltf_runtime_pointer_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, center>{min_center})', file=sys.stderr)
        return 1
    return 0


def cmd_center(path, label, dominant):
    """中心像素主色断言：dominant='r' → r 明显大于 g；'g' → g 明显大于 r。"""
    w, h, px = load(path)
    i = ((h // 2) * w + (w // 2)) * 3
    r, g, b = px[i], px[i + 1], px[i + 2]
    print(f'gltf_runtime_pointer_e2e: {label} center_px=({r},{g},{b})')
    if dominant == 'r':
        ok = r - g > COLOR_TOL
    elif dominant == 'g':
        ok = g - r > COLOR_TOL
    else:
        raise SystemExit(f'未知主色 {dominant}')
    if not ok:
        print(f'gltf_runtime_pointer_e2e: FAIL {label}: 中心像素主色不是 {dominant} '
              f'(r={r}, g={g}, tol={COLOR_TOL})', file=sys.stderr)
        return 1
    return 0


def cmd_diff_bytes(a_path, b_path, min_bytes):
    wa, ha, a = load(a_path)
    wb, hb, b = load(b_path)
    assert (wa, ha) == (wb, hb), f'两图尺寸不同 {wa}x{ha} vs {wb}x{hb}'
    da = open(a_path, 'rb').read()
    db = open(b_path, 'rb').read()
    assert len(da) == len(db), f'两 PPM 文件字节数不同 {len(da)} vs {len(db)}'
    nd = sum(1 for i in range(len(da)) if da[i] != db[i])
    print(f'gltf_runtime_pointer_e2e: f1 vs f40 差异字节={nd} / {len(da)}')
    if nd <= int(min_bytes):
        print(f'gltf_runtime_pointer_e2e: FAIL pointer 动画帧差异过小（{nd} <= {min_bytes}）：'
              f'材质指针采样/更新可能未生效', file=sys.stderr)
        return 1
    return 0


def main():
    cmd = sys.argv[1]
    if cmd == 'struct':
        return cmd_struct(sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5]))
    if cmd == 'center':
        return cmd_center(sys.argv[2], sys.argv[3], sys.argv[4])
    if cmd == 'diffbytes':
        return cmd_diff_bytes(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    raise SystemExit(f'未知模式 {cmd}')


if __name__ == '__main__':
    sys.exit(main())
PY
CHECK() { "$PY" "$WORK/ppm_check.py" "$@"; }

# ---- 拍 1/40 两帧（pointer 动画 0→1s，第 1 帧≈红、第 40 帧≈绿）----
timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot-seq "$WORK/f{}.ppm" --shot-frames 1,40 \
    "$SCN" >"$WORK/pointer.log" 2>&1 || { cat "$WORK/pointer.log"; fail "pointer 场景渲染失败"; }
LOG="$WORK/pointer.log"

# 公共日志断言：两帧 shot、无加载失败、无 ASan
for n in 1 40; do
    grep -Eq "runtime: shot\[[0-9]*\] .*frame=$n" "$LOG" \
        || { cat "$LOG"; fail "缺第 $n 帧的 shot 日志（--shot-seq 未写出？）"; }
    [ -f "$WORK/f$n.ppm" ] || { cat "$LOG"; fail "第 $n 帧 PPM 不存在: $WORK/f$n.ppm"; }
done
if grep -q "gltf 加载失败" "$LOG"; then cat "$LOG"; fail "gltf 加载失败"; fi
if grep -q "AddressSanitizer" "$LOG"; then cat "$LOG"; fail "ASan 报告（崩溃/越界）"; fi
# KHR_animation_pointer：pointer 通道被识别为 1 条动画通道
grep -Eq "runtime: gltf anim 'color_anim' channels=1" "$LOG" \
    || { cat "$LOG"; fail "期望 'gltf anim 'color_anim' channels=1'（pointer 通道未被识别？）"; }

# 两帧像素结构（几何相同，只是材质颜色在变）
CHECK struct "$WORK/f1.ppm" "f1" 0.03 500 || exit 1
CHECK struct "$WORK/f40.ppm" "f40" 0.03 500 || exit 1
# 中心像素颜色：pointer 动画把 baseColorFactor 从红插值到绿
CHECK center "$WORK/f1.ppm" "f1" r || exit 1
CHECK center "$WORK/f40.ppm" "f40" g || exit 1
# pointer 通道逐帧采样真的生效：两帧必须明显不同（实测 70296 字节）
CHECK diffbytes "$WORK/f1.ppm" "$WORK/f40.ppm" 1000 || exit 1

echo "gltf_runtime_pointer_e2e: PASS"
