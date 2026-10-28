#!/usr/bin/env bash
# 运行时直读 glTF 的 **P5 灯光/相机**端到端（plans/24 P5）。
#
# 背景：runtime 的 C++ 直读加载器把 glTF 的 `KHR_lights_punctual` 映射为 frame3d 灯光
# （方向/点/聚光；强度按模型尺度归一化；方向光优先、**上限 8 盏**），并支持 glTF cameras：
# **场景没有** `render.Camera3D` 时采用模型首个 perspective 相机（节点世界矩阵 + yfov/znear/zfar），
# orthographic 忽略。`MAQUE_RENDER3D_INFO=1` 时装配期/首帧打印：
#   `runtime: gltf light[i] node=... type=... intensity=... range=... spot_cos=.../...`
#   `runtime: gltf lights=<N> cameras=<M> radius=...`
#   `runtime: gltf light on entity=<id> node=... type=... intensity=...`
#   `runtime: gltf camera[i] node=... perspective=... yfov=... z=[...]`
#   `runtime: gltf camera entity=<id> node=... fov=... near=... far=...`
#   `runtime: 3d scene cams=... meshes=... lights=... cube_mesh=...`
#   `runtime: 3d frame0 draws=... skins=... morph=... anim_pose=... names=... lights=... mats=...`
#
# 夹具（从 tests/data/gltf/ 原样复制，自包含）：
#   lights_visibility.gltf  13 节点；节点 10 `hidden_parent` 带
#                           `KHR_node_visibility.visible=false`，其子树 = 节点 11 `hidden_mesh`
#                           + 节点 12 `hidden_light`。runtime 直读已支持**静态可见性 + 祖先传播**：
#                           隐藏子树的 mesh primitive / 灯光都**不生成**。因此：
#                             灯光节点实例 = 9（3 directional + 3 point + 3 spot；隐藏的额外
#                             directional 不计）→ frame3d 取前 8 盏（方向光优先 + 上限 8）；
#                             mesh draw = 1（隐藏 mesh 不生成）→ frame0 draws=1 / mats=1。
#                           （visibility 支持前是 lights=10 / draws=2 / mats=2，PPM 不变：
#                             隐藏 mesh 在 x=5 原本就在画面外，隐藏灯原本也是第 10 盏、被 8 盏上限截掉。）
#   camera.gltf             1 个 perspective 相机节点（yfov=0.6 → 34.38°、znear=0.1、zfar=50）
#                           + 1 个三角 mesh 节点。
#
# 断言：
#   A. lights.scn：`gltf model ... primitives=1`（隐藏 mesh 不生成）+ `gltf lights=9 cameras=0`
#      （隐藏灯不生成）+ `gltf light on entity=... type=0 intensity=2.600`
#      + `3d frame0 draws=1 ... lights=8 mats=1`（上限生效 + 隐藏 mesh 不 draw）
#      + 灯光实例日志里**不得出现 node=12（隐藏灯）**；PPM nonbg>0.03 且 center>500
#      （实测 0.0629 / 10353；可见几何与前 8 盏灯在 visibility 支持前后一致，故阈值不变）。
#   B. camera.scn（**不含 render.Camera3D**）：`3d scene cams=0` + `gltf camera[0] ... yfov=0.600`
#      + `gltf camera entity=... fov=34.38 near=0.100 far=50.000`；`3d frame0 draws=1 ... lights=1`；
#      PPM nonbg>0.02 且 center>500。
#   两者：无 `runtime: gltf 加载失败`、无 ASan、截图存在。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_light）；$3 = 工作目录
# 无显示环境可跑：全程 --shot 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_light_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_light_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_light_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_light_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN_LIGHTS="$FIXDIR/lights.scn"
SCN_CAMERA="$FIXDIR/camera.scn"
for f in "$SCN_LIGHTS" "$SCN_CAMERA" "$FIXDIR/lights_visibility.gltf" "$FIXDIR/camera.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_light_e2e: skip（缺夹具 $f）"; exit 77; }
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
    print(f'gltf_runtime_light_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > 500):
        print(f'gltf_runtime_light_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, center>500)', file=sys.stderr)
        return 1
    return 0


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

# ---- A. KHR_lights_punctual + KHR_node_visibility（lights_visibility.gltf）----
render "$SCN_LIGHTS" "$WORK/lights.ppm" "$WORK/lights.log" \
    || { cat "$WORK/lights.log"; fail "A lights: 渲染失败"; }
check_ok_log "$WORK/lights.log" "A lights"
# KHR_node_visibility：节点 10 `hidden_parent`（visible=false）的整棵子树不生成 →
# 隐藏 mesh（node 11）的 primitive 不建（primitives=1，而不是 2）
grep -Eq "runtime: gltf model 'lights_visibility\.gltf' primitives=1 " "$WORK/lights.log" \
    || { cat "$WORK/lights.log"; fail "A lights: 期望 model 'primitives=1'（隐藏 mesh 的 primitive 不应生成）"; }
# 装配期统计：可见灯光节点实例 = 9（不是 10；隐藏子树里那盏 directional 不生成）、0 个相机
grep -Eq "runtime: gltf lights=9 cameras=0" "$WORK/lights.log" \
    || { cat "$WORK/lights.log"; fail "A lights: 期望 'gltf lights=9 cameras=0'（KHR_node_visibility：隐藏灯不应生成）"; }
# 每个灯光实例一行（9 行）
[ "$(grep -c 'runtime: gltf light\[' "$WORK/lights.log")" -ge 9 ] \
    || { cat "$WORK/lights.log"; fail "A lights: 灯光实例日志少于 9 行"; }
# 隐藏灯 node=12 必须**不出现**在灯光实例日志里（可见性沿祖先传播到子树）
if grep -Eq "runtime: gltf light\[[0-9]+\] node=(11|12) " "$WORK/lights.log"; then
    cat "$WORK/lights.log"; fail "A lights: 隐藏子树的 node=12（hidden_light）仍被生成"
fi
# 首盏方向光按模型尺度归一化后 2.600
grep -Eq "runtime: gltf light on entity=[0-9]+ node=[0-9]+ type=0 intensity=2\.600" "$WORK/lights.log" \
    || { cat "$WORK/lights.log"; fail "A lights: 缺 'gltf light on entity=... type=0 intensity=2.600'"; }
# frame3d 灯光上限 8（9 盏 → 截断到 8）；隐藏 mesh 不 draw → draws=1 / mats=1（visibility 前是 2/2）
grep -Eq "runtime: 3d frame0 draws=1 .*lights=8 .*mats=1" "$WORK/lights.log" \
    || { cat "$WORK/lights.log"; fail "A lights: 期望 '3d frame0 draws=1 ... lights=8 ... mats=1'（隐藏 mesh 或灯上限未生效？）"; }
CHECK "$WORK/lights.ppm" "A lights" 0.03 || exit 1
echo "gltf_runtime_light_e2e: A lights（KHR_node_visibility：隐藏 mesh/灯不生成 → lights=9 / draws=1 / mats=1；frame3d 8 上限）OK"

# ---- B. glTF 相机（camera.gltf；场景不含 render.Camera3D）----
render "$SCN_CAMERA" "$WORK/camera.ppm" "$WORK/camera.log" \
    || { cat "$WORK/camera.log"; fail "B camera: 渲染失败"; }
check_ok_log "$WORK/camera.log" "B camera"
# 场景没有 render.Camera3D → cams=0；采用 glTF perspective 相机（yfov=0.6 rad → 34.38°）
grep -Eq "runtime: 3d scene cams=0 meshes=1 lights=1" "$WORK/camera.log" \
    || { cat "$WORK/camera.log"; fail "B camera: 期望 '3d scene cams=0 meshes=1 lights=1'"; }
grep -Eq "runtime: gltf lights=0 cameras=1" "$WORK/camera.log" \
    || { cat "$WORK/camera.log"; fail "B camera: 期望 'gltf lights=0 cameras=1'"; }
grep -Eq "runtime: gltf camera\[0\] node=1 perspective=1 yfov=0\.600 z=\[0\.100,50\.000\]" "$WORK/camera.log" \
    || { cat "$WORK/camera.log"; fail "B camera: 缺 glTF 相机装配日志（perspective=1 yfov=0.600 z=[0.100,50.000]）"; }
grep -Eq "runtime: gltf camera entity=[0-9]+ node=[0-9]+ fov=34\.38 near=0\.100 far=50\.000" "$WORK/camera.log" \
    || { cat "$WORK/camera.log"; fail "B camera: 缺相机采用日志（fov=34.38 near=0.100 far=50.000）"; }
# 只有 mesh 一个 draw、一盏灯（camera.scn 里的场景方向光）
grep -Eq "runtime: 3d frame0 draws=1 .*lights=1" "$WORK/camera.log" \
    || { cat "$WORK/camera.log"; fail "B camera: 期望 '3d frame0 draws=1 ... lights=1'"; }
CHECK "$WORK/camera.ppm" "B camera" 0.02 || exit 1
echo "gltf_runtime_light_e2e: B camera（cams=0 → 采用 glTF camera fov=34.38 / near/far）OK"

echo "gltf_runtime_light_e2e: PASS"
