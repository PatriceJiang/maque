#!/usr/bin/env bash
# 运行时直读 glTF 的 **KTX2 / Basis Universal** 纹理解码端到端。
#
# 背景：image 模块新增 `ktx2_decode.cpp`（vendored `third_party/basisu` 的 transcoder）：
# KTX2 容器的 **ETC1S（BasisLZ）** 与 **UASTC（含 Zstd 超压缩）** 都解码成 RGBA8；
# `mq_image_decode` 按 KTX2 magic 自动分发。glTF parser 支持 `KHR_texture_basisu.source`
# （texture → image）。`MAQUE_RENDER3D_INFO=1` 时装配期打印：
#   `runtime: gltf texture image=0 '<file>.ktx2' srgb=1 -> <纹理句柄>`（句柄应 >= 0，-1 = 失败）
#   `runtime: gltf model '<gltf>' primitives=... materials=... textures=1 ...`
#   `runtime: gltf '<gltf>' image[0] 解码失败（...）` / `ktx2: ...`（失败时才会出现）
#
# 夹具（tests/data/gltf/runtime_ktx2/，Lead 已生成；16x16 红绿棋盘纹理）：
#   cube_ktx2.gltf + checker_etc1s.ktx2 + ktx2.scn   —— ETC1S/BasisLZ 路径
#   cube_uastc.gltf + checker_uastc.ktx2 + uastc.scn —— UASTC + Zstd 路径
#   cube_raw.gltf + checker_raw.ktx2 + raw.scn       —— 未压缩 RGBA（KTX2 容器内的 raw 像素）
#   三个场景均为 640x360/views1、camera pos(0,1,4)、fov45、1 实体 MeshRenderer。
#
# 断言（三个场景各跑一次）：
#   - 都能加载：日志含 `gltf texture image=0 '<...>.ktx2' srgb=1 -> <id>` 且 id>=0
#     （匹配 `-> [0-9]+`，即不是 -1）；model 行 `textures=1`；无 `gltf 加载失败`/ASan；
#   - 日志里没有 `image[0] 解码失败`、没有 `ktx2:` 解码错误；
#   - 每帧 PPM 均 640x360、nonbg>0.03、center>500；
#   - 每帧 PPM 同时存在**红主导**（r-g>20）与**绿主导**（g-r>20）像素
#     —— 棋盘纹理真的解码出来了（不要求与源 PNG 逐像素一致）。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_ktx2）；$3 = 工作目录
# 无显示环境可跑：--shot 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_ktx2_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_ktx2_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_ktx2_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_ktx2_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
for f in "$FIXDIR/ktx2.scn" "$FIXDIR/uastc.scn" "$FIXDIR/raw.scn" \
         "$FIXDIR/cube_ktx2.gltf" "$FIXDIR/cube_uastc.gltf" "$FIXDIR/cube_raw.gltf" \
         "$FIXDIR/checker_etc1s.ktx2" "$FIXDIR/checker_uastc.ktx2" "$FIXDIR/checker_raw.ktx2"; do
    [ -f "$f" ] || { echo "gltf_runtime_ktx2_e2e: skip（缺夹具 $f）"; exit 77; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
# 清掉可能干扰离屏渲染/诊断的环境；强制无窗口 + 纯离屏
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_ENV_IBL \
      MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO MAQUE_WINDOW \
      MAQUE_WINDOW_EVENT_AT MAQUE_ANIM MAQUE_SKIN MAQUE_MORPH 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# PPM 检查器：结构断言 + 红/绿主导像素存在断言
cat >"$WORK/ppm_check.py" <<'PY'
import sys

BG_TOL = 25          # 与渲染清屏色的 L1 距离阈值（与 render3d_scene_e2e 一致）
COLOR_TOL = 20       # 红/绿通道差阈值（光照/压缩误差下仍能分出色块）


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
    print(f'gltf_runtime_ktx2_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > min_center):
        print(f'gltf_runtime_ktx2_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, center>{min_center})', file=sys.stderr)
        return 1
    return 0


def cmd_redgreen(path, label, min_each):
    """棋盘纹理已解码的证据：画面里同时存在红主导与绿主导像素。"""
    w, h, px = load(path)
    red = green = 0
    for i in range(0, w * h * 3, 3):
        r, g = px[i], px[i + 1]
        if r - g > COLOR_TOL:
            red += 1
        elif g - r > COLOR_TOL:
            green += 1
    print(f'gltf_runtime_ktx2_e2e: {label} red_dom_px={red} green_dom_px={green}')
    if red <= int(min_each) or green <= int(min_each):
        print(f'gltf_runtime_ktx2_e2e: FAIL {label}: 红/绿主导像素不足 '
              f'(red={red}, green={green}, 各需>{min_each})：KTX2 棋盘纹理可能没解码出来',
              file=sys.stderr)
        return 1
    return 0


def main():
    cmd = sys.argv[1]
    if cmd == 'struct':
        return cmd_struct(sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5]))
    if cmd == 'redgreen':
        return cmd_redgreen(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    raise SystemExit(f'未知模式 {cmd}')


if __name__ == '__main__':
    sys.exit(main())
PY
CHECK() { "$PY" "$WORK/ppm_check.py" "$@"; }

# 成功渲染的公共日志断言：有 shot、无加载失败、无 ASan、无 KTX2 解码错误
check_common_log() {   # check_common_log <log> <label>
    local log="$1" label="$2"
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "$label: 截图未写入"; }
    if grep -q "gltf 加载失败" "$log"; then cat "$log"; fail "$label: gltf 加载失败"; fi
    if grep -q "AddressSanitizer" "$log"; then cat "$log"; fail "$label: ASan 报告（崩溃/越界）"; fi
    if grep -q "image\[0\] 解码失败" "$log"; then cat "$log"; fail "$label: image[0] 解码失败"; fi
    if grep -q "ktx2:" "$log"; then cat "$log"; fail "$label: ktx2 解码错误"; fi
}

render() {   # render <scene> <out.ppm> <log>
    timeout 180 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$2" --frames 2 "$1" >"$3" 2>&1
}

# 单个 KTX2 场景的完整校验：run_case <scene> <gltf> <ktx2> <ppm 基名> <报告标签>
run_case() {
    local scn="$1" gltf="$2" ktx2="$3" name="$4" label="$5"
    local ppm="$WORK/$name.ppm" log="$WORK/$name.log"
    local ktx2_re="${ktx2//./\\.}" gltf_re="${gltf//./\\.}"
    render "$FIXDIR/$scn" "$ppm" "$log" || { cat "$log"; fail "$label: 渲染失败"; }
    check_common_log "$log" "$label"
    grep -Eq "runtime: gltf texture image=0 '$ktx2_re' srgb=1 -> [0-9]+" "$log" \
        || { cat "$log"; fail "$label: 缺 'gltf texture image=0 ... -> <id>'（id 必须 >=0，-1 表示解码失败）"; }
    grep -Eq "runtime: gltf model '$gltf_re' .*textures=1" "$log" \
        || { cat "$log"; fail "$label: 期望 model 行 'textures=1'"; }
    CHECK struct "$ppm" "$label" 0.03 500 || exit 1
    CHECK redgreen "$ppm" "$label" 100 || exit 1
    echo "gltf_runtime_ktx2_e2e: $label 解码/渲染 OK"
}

# ---- A. ETC1S（BasisLZ）----
run_case "ktx2.scn" "cube_ktx2.gltf" "checker_etc1s.ktx2" "etc1s" "A ETC1S（BasisLZ）"
# ---- B. UASTC + Zstd ----
run_case "uastc.scn" "cube_uastc.gltf" "checker_uastc.ktx2" "uastc" "B UASTC（+ Zstd）"
# ---- C. 未压缩 RGBA ----
run_case "raw.scn" "cube_raw.gltf" "checker_raw.ktx2" "raw" "C raw RGBA（未压缩）"

echo "gltf_runtime_ktx2_e2e: PASS"
