#!/usr/bin/env bash
# 运行时直读 glTF 的 **KHR_materials_variants** 端到端（plans/24 P6 部分）。
#
# 背景：runtime 的 C++ 直读加载器解析顶层 `KHR_materials_variants` 的名称表与 primitive
# 的 `mappings`（变体索引 → 材质下标），场景侧用 `render.MeshRenderer.variant`（string）**按名字**
# 选择变体：空 = 默认材质；命中 = 用 mapping 指定的材质替换该 primitive 的材质。
# `MAQUE_RENDER3D_INFO=1` 时装配期打印：
#   `runtime: gltf model '<path>' primitives=... materials=... ...`
#   `runtime: gltf lights=... cameras=... radius=... variants=<N>`（N = 顶层变体数）
#
# 夹具（tests/data/gltf/runtime_variants/，Lead 已生成）：
#   variants.gltf  三角形 + 2 个 unlit 材质（material0 `default_red` 红、material1 `variant_green` 绿）
#                  + primitive mapping（变体 0 `green` → material 1）；顶层 variants=[{name:"green"}]。
#   default.scn    无 `variant` 字段 → 默认红色材质。
#   green.scn      `render.MeshRenderer.variant = "green"` → 绿色材质。
#   两场景同为 640x360/views1、camera pos(0,0.1,3)、fov45（几何/相机/清屏色完全相同）。
#
# 断言：
#   - 两个场景都加载：`gltf model 'variants.gltf' primitives=1 materials=2` + `variants=1`；
#     `3d frame0 draws=1`；无 `runtime: gltf 加载失败`、无 ASan、截图存在；
#   - 两张 PPM 都 nonbg>0.03 且 center>500（几何相同 → 非背景占比一致）；
#   - **中心像素颜色**：default 红分量显著 > 绿分量；green 绿分量显著 > 红分量
#     （compare 留 ±20 容差；unlit 材质 → 颜色是 0.9/0.15 之类的平色，容差足够）；
#     default 实测 (243,108,89) 红、green 实测 (89,243,124) 绿；
#   - **default vs green 的 PPM 必须不同**：逐字节差 > 1000 字节（实测 70296）→ 变体选择真的生效。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_variants）；$3 = 工作目录
# 无显示环境可跑：--shot 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_variants_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_variants_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_variants_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_variants_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN_DEFAULT="$FIXDIR/default.scn"
SCN_GREEN="$FIXDIR/green.scn"
for f in "$SCN_DEFAULT" "$SCN_GREEN" "$FIXDIR/variants.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_variants_e2e: skip（缺夹具 $f）"; exit 77; }
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
COLOR_TOL = 20       # 中心像素主色相对其它通道的最小差值（允许光照/量化误差）


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
    print(f'gltf_runtime_variants_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > min_center):
        print(f'gltf_runtime_variants_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, center>{min_center})', file=sys.stderr)
        return 1
    return 0


def cmd_center(path, label, dominant):
    """中心像素主色断言：dominant='r' → r 明显大于 g；'g' → g 明显大于 r。"""
    w, h, px = load(path)
    i = ((h // 2) * w + (w // 2)) * 3
    r, g, b = px[i], px[i + 1], px[i + 2]
    print(f'gltf_runtime_variants_e2e: {label} center_px=({r},{g},{b})')
    if dominant == 'r':
        ok = r - g > COLOR_TOL
    elif dominant == 'g':
        ok = g - r > COLOR_TOL
    else:
        raise SystemExit(f'未知主色 {dominant}')
    if not ok:
        print(f'gltf_runtime_variants_e2e: FAIL {label}: 中心像素主色不是 {dominant} '
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
    print(f'gltf_runtime_variants_e2e: default vs green 差异字节={nd} / {len(da)}')
    if nd <= int(min_bytes):
        print(f'gltf_runtime_variants_e2e: FAIL 变体渲染差异过小（{nd} <= {min_bytes}）：'
              f'variant 选择可能未生效', file=sys.stderr)
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

# glTF 装配期公共断言：模型统计 + 顶层变体数 + 一个 draw
check_variants_log() {   # check_variants_log <log> <label>
    local log="$1" label="$2"
    grep -Eq "runtime: gltf model 'variants\.gltf' primitives=1 materials=2" "$log" \
        || { cat "$log"; fail "$label: 期望 'gltf model 'variants.gltf' primitives=1 materials=2'"; }
    grep -Eq "runtime: gltf lights=0 cameras=0 .*variants=1" "$log" \
        || { cat "$log"; fail "$label: 期望变体表 'variants=1'（KHR_materials_variants 顶层 1 个）"; }
    grep -Eq "runtime: 3d frame0 draws=1" "$log" \
        || { cat "$log"; fail "$label: 期望 '3d frame0 draws=1'"; }
}

# ---- ① default.scn（无 variant → 默认红色材质）----
render "$SCN_DEFAULT" "$WORK/default.ppm" "$WORK/default.log" \
    || { cat "$WORK/default.log"; fail "default 场景渲染失败"; }
check_ok_log "$WORK/default.log" "default"
check_variants_log "$WORK/default.log" "default"
CHECK struct "$WORK/default.ppm" "default" 0.03 500 || exit 1
CHECK center "$WORK/default.ppm" "default" r || exit 1
echo "gltf_runtime_variants_e2e: ① default.scn（无 variant → 默认红材质）OK"

# ---- ② green.scn（render.MeshRenderer.variant="green" → 变体绿色材质）----
render "$SCN_GREEN" "$WORK/green.ppm" "$WORK/green.log" \
    || { cat "$WORK/green.log"; fail "green 场景渲染失败"; }
check_ok_log "$WORK/green.log" "green"
check_variants_log "$WORK/green.log" "green"
CHECK struct "$WORK/green.ppm" "green" 0.03 500 || exit 1
CHECK center "$WORK/green.ppm" "green" g || exit 1
echo "gltf_runtime_variants_e2e: ② green.scn（variant=\"green\" → 变体绿材质）OK"

# ---- ③ variant 选择真的生效：两张 PPM 必须明显不同（> 1000 字节；实测 70296）----
# 同几何/同相机/同清屏色，唯一差异 = 材质（红 vs 绿）+ 中心像素颜色已在上两步分别断言。
CHECK diffbytes "$WORK/default.ppm" "$WORK/green.ppm" 1000 || exit 1
echo "gltf_runtime_variants_e2e: ③ default vs green 像素差异（variant 选择生效）OK"

echo "gltf_runtime_variants_e2e: PASS"
