#!/usr/bin/env bash
# 运行时直读 glTF 的 **P4 材质 KHR 扩展映射**端到端（plans/24 P4）。
#
# 背景：runtime 的 C++ 直读加载器把 glTF 材质扩展映射到 `mq_renderer_pbr_material`：
#   KHR_materials_clearcoat/sheen/iridescence/specular/ior/anisotropy/emissive_strength、
#   KHR_materials_transmission/volume/volume_scatter/dispersion/diffuse_transmission，
#   以及扩展贴图槽 textures3[7] / textures4[2] / textures5[1] / textures6[2] / textures7[1] + flags2 presence。
#   `MAQUE_RENDER3D_INFO=1` 时每个材质打印一行：
#   `runtime: gltf material[i] '<name>' shading=<0 Standard|1 Physical|2 Basic> clearcoat=... sheen=...
#    iridescence=... specular=... ior=... anisotropy=... transmission=... volume=...
#    diffuse_transmission=... tex3=<true/false> tex4=<a>/<b> tex5=<id> tex6=<a>/<b> tex7=<id>`
#
# 夹具（从 tests/data/gltf/ 原样复制，data: URI 内嵌、自包含）：
#   extensions.gltf        material0 'physical'：clearcoat 0.8 / sheen / iridescence 0.6 /
#                          specular 0.7 / ior 1.7 / anisotropy 0.45（shading=1）；
#                          material1 'unlit'（KHR_materials_unlit → shading=2）；两 primitive 各一个三角形。
#   ext_material_slots.gltf 单个材质带 7 个扩展贴图槽（clearcoat/sheen/iridescence/anisotropy）
#                          + KHR_materials_specular 两槽 → 期望 tex3=true、tex4 两个非 -1 句柄、
#                          贴图上传 ≥3（同一 image + 不同 sampler/sRGB → 多个纹理句柄）。
#   diffuse_volume.gltf     material0 'milky'：KHR_materials_diffuse_transmission=0.5（tex6 两槽有句柄）；
#                           material1 'amber'：KHR_materials_transmission=1.0 + volume。
#
# 断言（每个场景：无 `runtime: gltf 加载失败`、无 ASan、截图存在 + PPM 结构非背景/中心有内容）：
#   A. extensions：model materials=2/textures>=1；material[0] 'physical' shading=1 且
#      clearcoat≈0.8 / sheen=1 / iridescence=0.6 / specular=0.7 / ior=1.7 / anisotropy=0.45；
#      material[1] 'unlit' shading=2；贴图上传 ≥2。
#   B. ext_material_slots：tex3=true 且 tex4=<a>/<b> 两个非负句柄；贴图上传 ≥3。
#   C. diffuse_volume：material[0] 'milky' diffuse_transmission=0.500 且 tex6 两槽非 -1；
#      material[1] 'amber' transmission=1.000。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_material）；$3 = 工作目录
# 无显示环境可跑：全程 --shot 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_material_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_material_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_material_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_material_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN_EXT="$FIXDIR/extensions.scn"
SCN_SLOTS="$FIXDIR/ext_material_slots.scn"
SCN_DV="$FIXDIR/diffuse_volume.scn"
for f in "$SCN_EXT" "$SCN_SLOTS" "$SCN_DV" \
         "$FIXDIR/extensions.gltf" "$FIXDIR/ext_material_slots.gltf" "$FIXDIR/diffuse_volume.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_material_e2e: skip（缺夹具 $f）"; exit 77; }
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
    print(f'gltf_runtime_material_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}')
    if not (min_nonbg < ratio < 0.85 and center > 500):
        print(f'gltf_runtime_material_e2e: FAIL {label}: PPM 结构断言不满足 '
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

tex_uploads() { grep -c "runtime: gltf texture" "$1" || true; }

# ---- A. KHR 材质扩展因子（extensions.gltf）----
render "$SCN_EXT" "$WORK/extensions.ppm" "$WORK/extensions.log" \
    || { cat "$WORK/extensions.log"; fail "A extensions: 渲染失败"; }
check_ok_log "$WORK/extensions.log" "A extensions"
grep -Eq "runtime: gltf model .*primitives=2 .*materials=2 .*textures=[1-9][0-9]*" "$WORK/extensions.log" \
    || { cat "$WORK/extensions.log"; fail "A extensions: model 日志不符（期望 primitives=2 materials=2 textures>=1）"; }
# material0 physical：shading=1 + 六类扩展因子（日志固定 3 位小数，正则容忍 2~3 位）
grep -Eq "runtime: gltf material\[0\] 'physical' shading=1 clearcoat=0\.8[0-9]* sheen=1\.0[0-9]* iridescence=0\.60[0-9]* specular=0\.70[0-9]* ior=1\.70[0-9]* anisotropy=0\.450[0-9]*" \
    "$WORK/extensions.log" \
    || { cat "$WORK/extensions.log"; fail "A extensions: material[0] 'physical' 扩展因子映射不符（clearcoat/sheen/iridescence/specular/ior/anisotropy）"; }
# material1 unlit → shading=2 (Basic)
grep -Eq "runtime: gltf material\[1\] 'unlit' shading=2" "$WORK/extensions.log" \
    || { cat "$WORK/extensions.log"; fail "A extensions: material[1] 'unlit' 未映射为 shading=2(Basic)"; }
# 贴图：normal(sRGB=0) + unlit baseColor(sRGB=1) 至少两次上传
[ "$(tex_uploads "$WORK/extensions.log")" -ge 2 ] \
    || { cat "$WORK/extensions.log"; fail "A extensions: 贴图上传不足（期望 >=2）"; }
CHECK "$WORK/extensions.ppm" "A extensions" 0.03 || exit 1
echo "gltf_runtime_material_e2e: A extensions（physical 六扩展因子 + unlit shading=2 + 贴图）OK"

# ---- B. 扩展贴图槽（ext_material_slots.gltf）----
render "$SCN_SLOTS" "$WORK/ext_material_slots.ppm" "$WORK/ext_material_slots.log" \
    || { cat "$WORK/ext_material_slots.log"; fail "B ext_material_slots: 渲染失败"; }
check_ok_log "$WORK/ext_material_slots.log" "B ext_material_slots"
grep -Eq "runtime: gltf model .*materials=1 .*textures=[1-9][0-9]*" "$WORK/ext_material_slots.log" \
    || { cat "$WORK/ext_material_slots.log"; fail "B ext_material_slots: model 日志不符（期望 materials=1 textures>=1）"; }
# 7 个 textures3 扩展槽任一存在 → tex3=true；KHR_materials_specular 两槽 → tex4 两个非 -1 句柄
grep -Eq "runtime: gltf material\[0\] 'ext_slots' shading=1 .*tex3=true tex4=[0-9]+/[0-9]+" \
    "$WORK/ext_material_slots.log" \
    || { cat "$WORK/ext_material_slots.log"; fail "B ext_material_slots: tex3/tex4 扩展贴图槽未映射（期望 tex3=true tex4=<a>/<b>）"; }
# specularFactor 0.65 也应映射（同材质）
grep -Eq "runtime: gltf material\[0\] 'ext_slots' .*specular=0\.65[0-9]*" "$WORK/ext_material_slots.log" \
    || { cat "$WORK/ext_material_slots.log"; fail "B ext_material_slots: specularFactor 未映射（期望 0.65）"; }
# 同一 image 因 sampler/sRGB 组合不同 → 期望 >=3 张贴图上传
[ "$(tex_uploads "$WORK/ext_material_slots.log")" -ge 3 ] \
    || { cat "$WORK/ext_material_slots.log"; fail "B ext_material_slots: 贴图上传不足（期望 >=3）"; }
CHECK "$WORK/ext_material_slots.ppm" "B ext_material_slots" 0.03 || exit 1
echo "gltf_runtime_material_e2e: B ext_material_slots（tex3 + tex4 双槽 + 贴图上传>=3）OK"

# ---- C. diffuse_transmission / transmission / volume（diffuse_volume.gltf）----
render "$SCN_DV" "$WORK/diffuse_volume.ppm" "$WORK/diffuse_volume.log" \
    || { cat "$WORK/diffuse_volume.log"; fail "C diffuse_volume: 渲染失败"; }
check_ok_log "$WORK/diffuse_volume.log" "C diffuse_volume"
grep -Eq "runtime: gltf model .*materials=2 .*textures=[1-9][0-9]*" "$WORK/diffuse_volume.log" \
    || { cat "$WORK/diffuse_volume.log"; fail "C diffuse_volume: model 日志不符（期望 materials=2 textures>=1）"; }
# material0 milky：diffuse_transmission 0.5 + tex6 两槽（透射率 A / 透射色 RGB）都有句柄
grep -Eq "runtime: gltf material\[0\] 'milky' .*diffuse_transmission=0\.50[0-9]* .*tex6=[0-9]+/[0-9]+" \
    "$WORK/diffuse_volume.log" \
    || { cat "$WORK/diffuse_volume.log"; fail "C diffuse_volume: material[0] 'milky' 的 diffuse_transmission/tex6 映射不符"; }
# material1 amber：KHR_materials_transmission factor=1.0
grep -Eq "runtime: gltf material\[1\] 'amber' .*transmission=1\.0[0-9]*" "$WORK/diffuse_volume.log" \
    || { cat "$WORK/diffuse_volume.log"; fail "C diffuse_volume: material[1] 'amber' 的 transmission 映射不符（期望 1.000）"; }
CHECK "$WORK/diffuse_volume.ppm" "C diffuse_volume" 0.03 || exit 1
echo "gltf_runtime_material_e2e: C diffuse_volume（diffuse_transmission 0.5 + tex6 双槽 + transmission 1.0）OK"

echo "gltf_runtime_material_e2e: PASS"
