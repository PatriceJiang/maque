#!/usr/bin/env bash
# 运行时直读 glTF 的 **sparse accessor** 端到端（glTF 2.0 §5.1.2）：
# runtime 在读完 base accessor 数据后用 sparse.indices/values 覆盖指定元素。
#
# 夹具（tests/data/gltf/runtime_sparse/）：
#   sparse.gltf  单个三角形（POSITION 3 顶点、indices [0,1,2]、unlit 橙色材质，data: URI 自包含）；
#                POSITION 带 sparse：count=1，把**顶点 2** 的 y 从 0.5 覆盖为 1.5
#                （base 顶点：(-0.8,-0.5,0) / (0.8,-0.5,0) / (0,0.5,0)）。
#   base.gltf    与 sparse.gltf **逐字节同一份 buffer / 同一份 indices / 同一材质**，
#                仅删掉 POSITION.sparse 并把 accessor.max 从 y=1.5 改为 y=0.5（确定性生成）。
#   sparse.scn / base.scn：640x360/views1；camera pos(0,0.3,4) 正对三角形 + 方向光（重复 "type" 键）
#                + 1 实体 Transform3D+MeshRenderer.mesh_path=sparse.gltf / base.gltf。
#
# 断言：
#   - 两个场景都能加载：日志含 `gltf model 'sparse.gltf' primitives=1` / `... 'base.gltf' ...`，
#     且 `3d frame0 draws=1`；无 `gltf 加载失败` / ASan / 截图缺失；
#   - 两张 PPM 都 nonbg>0.02 且 center>300（sparse 三角形被拉高 → 面积/中心内容都变大）；
#   - **sparse 与 base 的 PPM 必须不同**：逐字节比较差异 > 1000 字节
#     （Lead 实测 28380 字节；证明 sparse 覆盖真的生效，而非"按 base 渲染"）。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_sparse）；$3 = 工作目录
# 无显示环境可跑：--shot 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_sparse_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_sparse_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_sparse_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_sparse_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN_SPARSE="$FIXDIR/sparse.scn"
SCN_BASE="$FIXDIR/base.scn"
for f in "$SCN_SPARSE" "$SCN_BASE" "$FIXDIR/sparse.gltf" "$FIXDIR/base.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_sparse_e2e: skip（缺夹具 $f）"; exit 77; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
# 清掉可能干扰离屏渲染/诊断的环境；强制无窗口 + 纯离屏
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_ENV_IBL \
      MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO MAQUE_WINDOW \
      MAQUE_WINDOW_EVENT_AT MAQUE_ANIM MAQUE_SKIN MAQUE_MORPH 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# PPM 检查器：结构断言 + 逐字节差异计数
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
    print(f'gltf_runtime_sparse_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > min_center):
        print(f'gltf_runtime_sparse_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, center>{min_center})', file=sys.stderr)
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
    print(f'gltf_runtime_sparse_e2e: sparse vs base 差异字节={nd} / {len(da)}')
    if nd <= int(min_bytes):
        print(f'gltf_runtime_sparse_e2e: FAIL sparse 与 base 渲染差异过小 '
              f'({nd} <= {min_bytes})：sparse 覆盖可能未生效', file=sys.stderr)
        return 1
    return 0


def main():
    cmd = sys.argv[1]
    if cmd == 'struct':
        return cmd_struct(sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5]))
    if cmd == 'diffbytes':
        return cmd_diff_bytes(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    raise SystemExit(f'未知模式 {cmd}')


if __name__ == '__main__':
    sys.exit(main())
PY
CHECK() { "$PY" "$WORK/ppm_check.py" "$@"; }

# 成功渲染的公共日志断言：有 shot、无加载失败、无 ASan
check_ok_log() {   # check_ok_log <log> <label>
    local log="$1" label="$2"
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "$label: 截图未写入"; }
    if grep -q "gltf 加载失败" "$log"; then cat "$log"; fail "$label: gltf 加载失败"; fi
    if grep -q "AddressSanitizer" "$log"; then cat "$log"; fail "$label: ASan 报告（崩溃/越界）"; fi
}

render() {   # render <scene> <out.ppm> <log>
    timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$2" --frames 2 "$1" >"$3" 2>&1
}

# ---- ① sparse.gltf（POSITION 带 sparse 覆盖）----
render "$SCN_SPARSE" "$WORK/sparse.ppm" "$WORK/sparse.log" \
    || { cat "$WORK/sparse.log"; fail "sparse 场景渲染失败"; }
check_ok_log "$WORK/sparse.log" "sparse"
grep -Eq "runtime: gltf model 'sparse\.gltf' primitives=1" "$WORK/sparse.log" \
    || { cat "$WORK/sparse.log"; fail "sparse: 缺 'gltf model 'sparse.gltf' primitives=1'"; }
grep -Eq "runtime: 3d frame0 draws=1" "$WORK/sparse.log" \
    || { cat "$WORK/sparse.log"; fail "sparse: 期望 '3d frame0 draws=1'"; }
CHECK struct "$WORK/sparse.ppm" "sparse" 0.02 300 || exit 1
echo "gltf_runtime_sparse_e2e: ① sparse.gltf 加载/渲染 OK"

# ---- ② base.gltf（同一几何，无 sparse 覆盖）----
render "$SCN_BASE" "$WORK/base.ppm" "$WORK/base.log" \
    || { cat "$WORK/base.log"; fail "base 场景渲染失败"; }
check_ok_log "$WORK/base.log" "base"
grep -Eq "runtime: gltf model 'base\.gltf' primitives=1" "$WORK/base.log" \
    || { cat "$WORK/base.log"; fail "base: 缺 'gltf model 'base.gltf' primitives=1'"; }
grep -Eq "runtime: 3d frame0 draws=1" "$WORK/base.log" \
    || { cat "$WORK/base.log"; fail "base: 期望 '3d frame0 draws=1'"; }
CHECK struct "$WORK/base.ppm" "base" 0.02 300 || exit 1
echo "gltf_runtime_sparse_e2e: ② base.gltf（无 sparse 对照）加载/渲染 OK"

# ---- ③ sparse 覆盖真的生效：两张 PPM 必须明显不同（> 1000 字节）----
# 同一 buffer / indices / 材质 / 相机，唯一差异 = 顶点 2 的 y（0.5 vs 1.5）→ 三角形轮廓变高。
# Lead 实测差异 28380 字节；这里只要求 >1000（留足实现/驱动差异余量）。
CHECK diffbytes "$WORK/sparse.ppm" "$WORK/base.ppm" 1000 || exit 1
echo "gltf_runtime_sparse_e2e: ③ sparse vs base 像素差异（sparse 覆盖生效）OK"

echo "gltf_runtime_sparse_e2e: PASS"
