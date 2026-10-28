#!/usr/bin/env bash
# 运行时直读 glTF 的 **EXT_mesh_gpu_instancing** 端到端（plans/24 P6 部分）。
#
# 背景：runtime 的 C++ 直读加载器对带 `EXT_mesh_gpu_instancing` 的节点，按实例 accessor
# （TRANSLATION / ROTATION / SCALE）**展开成 N 个合成子节点 + N 份 primitive**，
# 每个合成节点的世界 = 原节点世界 × 实例 TRS。`MAQUE_RENDER3D_INFO=1` 时可断言：
#   `runtime: gltf model '<path>' primitives=5 materials=1 textures=0 nodes=6 ...`
#   `runtime: gltf lights=0 cameras=0 radius=...`
#   `runtime: 3d frame0 draws=5 ... lights=1 mats=5`
#
# 夹具（从 tests/data/gltf/ 原样复制）：
#   instancing.gltf  node[0] `inst_node`：translation [10,0,0] + mesh 0（1 个三角形），
#                    带 EXT_mesh_gpu_instancing（5 组 TRS）：实例世界位置
#                    (10,0,0)/(12,0,0)/(14,0,0)/(10,2,0)/(10,0,2)，其中第 2 个实例绕 Y 旋转 90°、
#                    第 3 个实例 scale=2 → 三角形世界范围 x∈[10,16]、y∈[0,3]、z∈[-1,2]。
#                    夹具无材质 → 每个 primitive 用引擎默认材质（mats=5）；无 glTF 灯光/相机。
#
# 断言：
#   - model 日志 `primitives=5 ... nodes=6`（实例展开生效：5 份 primitive + 1 原节点 + 5 合成子节点）；
#   - `3d frame0 draws=5 ... mats=5`（5 个实例各一个 draw）；
#   - PPM nonbg>0.03 且 center>500（机位覆盖 x≈10~16 的实例分布）；
#   - 无 `runtime: gltf 加载失败`、无 ASan、截图存在。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_instancing）；$3 = 工作目录
# 无显示环境可跑：--shot 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_instancing_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_instancing_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_instancing_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_instancing_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN="$FIXDIR/instancing.scn"
for f in "$SCN" "$FIXDIR/instancing.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_instancing_e2e: skip（缺夹具 $f）"; exit 77; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
# 清掉可能干扰离屏渲染/诊断的环境；强制无窗口 + 纯离屏
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_ENV_IBL \
      MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO MAQUE_WINDOW \
      MAQUE_WINDOW_EVENT_AT MAQUE_ANIM MAQUE_SKIN MAQUE_MORPH 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# PPM 结构检查器（非背景占比 + 中心区内容；阈值集中在这里）
cat >"$WORK/ppm_check.py" <<'PY'
import sys

BG_TOL = 25          # 与渲染清屏色的 L1 距离阈值（与 render3d_scene_e2e 一致）


def load(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{path}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{path}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


def main():
    path, label, min_nonbg = sys.argv[1], sys.argv[2], float(sys.argv[3])
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
    print(f'gltf_runtime_instancing_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > 500):
        print(f'gltf_runtime_instancing_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, center>500)', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
PY
CHECK() { "$PY" "$WORK/ppm_check.py" "$@"; }

fail_hint() {   # fail_hint <log> <label> <msg>
    local log="$1" label="$2" msg="$3"
    cat "$log"; fail "$label: $msg"
}

# ---- EXT_mesh_gpu_instancing ----
timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/instancing.ppm" --frames 2 \
    "$SCN" >"$WORK/instancing.log" 2>&1 || { cat "$WORK/instancing.log"; fail "实例化场景渲染失败"; }
LOG="$WORK/instancing.log"
grep -q "runtime: shot" "$LOG" || fail_hint "$LOG" "" "截图未写入"
if grep -q "gltf 加载失败" "$LOG"; then fail_hint "$LOG" "" "gltf 加载失败"; fi
if grep -q "AddressSanitizer" "$LOG"; then fail_hint "$LOG" "" "ASan 报告（崩溃/越界）"; fi

# 实例展开：5 个实例 → 5 份 primitive；节点数 = 1 原节点 + 5 合成子节点 = 6
grep -Eq "runtime: gltf model 'instancing\.gltf' primitives=5 .*nodes=6" "$LOG" \
    || fail_hint "$LOG" "" "model 日志不符（期望 primitives=5 ... nodes=6，实例展开未生效？）"
# 5 个实例各一个 draw、各一份默认材质
grep -Eq "runtime: 3d frame0 draws=5 .*mats=5" "$LOG" \
    || fail_hint "$LOG" "" "期望 '3d frame0 draws=5 ... mats=5'（每个实例一个 draw/material）"
# 夹具本身无 glTF 灯光/相机（场景灯由 .scn 提供，因此 scene lights=1）
grep -Eq "runtime: gltf lights=0 cameras=0" "$LOG" \
    || fail_hint "$LOG" "" "期望 'gltf lights=0 cameras=0'（夹具无 KHR_lights_punctual / cameras）"
grep -Eq "runtime: 3d scene cams=1 meshes=1 lights=1" "$LOG" \
    || fail_hint "$LOG" "" "期望 '3d scene cams=1 meshes=1 lights=1'"
# 像素结构：相机覆盖实例分布（x≈10~16），非背景 >0.03 且中心区有内容
CHECK "$WORK/instancing.ppm" "instancing" 0.03 || exit 1

echo "gltf_runtime_instancing_e2e: PASS"
