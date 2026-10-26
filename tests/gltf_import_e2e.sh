#!/usr/bin/env bash
# glTF 离线导入端到端（plans/18-pbr-3d.md 方向 2 / M4b；plans/19 材质迁移）。
#   夹具 tests/data/gltf/triangle.gltf（内嵌 base64 buffer）→ tools/gltf_import.py
#   断言：顶点/索引/包围盒/材质字段正确、48B 顶点布局一致、两次导入逐字节一致（确定性）、
#         --name 覆盖输出名、错误输入返回非 0、无扩展材质不写任何扩展默认字段。
#   夹具 tests/data/gltf/extensions.gltf（三角网格 + 内嵌 1x1 PNG）：
#     KHR_materials_{unlit,clearcoat,sheen,iridescence,specular,ior,anisotropy,
#     emissive_strength} → asset.PbrMaterial 字段值 + shading_model(Physical/Basic) +
#     贴图搬运 + 两次导入逐字节一致；unlit 材质不得带 physical 扩展字段。
#   旧资产零漂移：cube.gltf 重新生成 → scenes/pbr/assets/cube.mesh.json 逐字节一致。
#   可选（MAQUE_TOYCAR_SRC=<含 ToyCar.gltf 的源目录>，源模型不入库）：ToyCar 三个
#     mesh.json 零漂移；未设置时跳过（ToyCar 源模型有 KHR 扩展，更新入库后可用此钩子校验）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
TOOL="$1"
FIXTURE="$2"
WORK="$3"
[ -n "${PY:-}" ] || { echo "gltf_import_e2e: skip（无 python3）"; exit 77; }
[ -f "$TOOL" ] || { echo "gltf_import_e2e: skip（缺 $TOOL）"; exit 77; }
[ -f "$FIXTURE" ] || { echo "gltf_import_e2e: skip（缺夹具 $FIXTURE）"; exit 77; }
ROOT="$(cd "$(dirname "$TOOL")/.." && pwd)"
FIXDIR="$(cd "$(dirname "$FIXTURE")" && pwd)"
fail() { echo "gltf_import_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/a" "$WORK/b" "$WORK/c"

"$PY" "$TOOL" "$FIXTURE" --out "$WORK/a" >"$WORK/a.log" 2>&1 || { cat "$WORK/a.log"; fail "导入失败"; }
grep -q "gltf_import: OK meshes=1 textures=0" "$WORK/a.log" || { cat "$WORK/a.log"; fail "导入统计不符"; }
[ -f "$WORK/a/triangle.mesh.json" ] || fail "缺 triangle.mesh.json"

"$PY" - "$WORK/a/triangle.mesh.json" <<'PY' || exit 1
import json, sys
m = json.load(open(sys.argv[1], encoding="utf-8"))
assert m["vertex_stride"] == 48 and m["vertex_layout"] == 0, m["vertex_stride"]
assert len(m["vertices"]) == 3, len(m["vertices"])
for v in m["vertices"]:
    assert len(v) == 12, len(v)
assert m["indices"] == [0, 1, 2], m["indices"]
assert m["bounds_min"] == [0, 0, 0] and m["bounds_max"] == [1, 1, 0], (m["bounds_min"], m["bounds_max"])
# 顶点 0：pos(0,0,0) normal(0,0,1) tangent(1,0,0,1) uv(0,0)；顶点 1：pos(1,0,0) uv(1,0)
assert m["vertices"][0][:6] == [0, 0, 0, 0, 0, 1], m["vertices"][0][:6]
assert m["vertices"][1][:3] == [1, 0, 0] and m["vertices"][1][10:] == [1, 0], m["vertices"][1]
mat = m["materials"][0]
assert mat["type"] == "asset.PbrMaterial"
assert mat["metallic"] == 0.75 and mat["roughness"] == 0.25, (mat["metallic"], mat["roughness"])
bc = mat["base_color"]
assert abs(bc["r"] - 0.8) < 1e-6 and abs(bc["g"] - 0.6) < 1e-6 and abs(bc["b"] - 0.2) < 1e-6, bc
assert mat["double_sided"] is False and mat["alpha_mode"] == 0
# 无扩展材质：材质键集合必须与迁移前完全一致（不写任何扩展默认字段，旧资产零漂移）
assert set(mat) == {"type", "name", "base_color", "metallic", "roughness", "emissive",
                    "double_sided", "alpha_mode", "alpha_cutoff"}, sorted(mat)
print("gltf_import_e2e: 结构断言 OK")
PY
[ $? -eq 0 ] || exit 1

# 确定性：两次导入逐字节一致
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/b" >/dev/null 2>&1 || fail "第二次导入失败"
cmp -s "$WORK/a/triangle.mesh.json" "$WORK/b/triangle.mesh.json" || fail "两次导入结果不一致（违反确定性）"
echo "gltf_import_e2e: 确定性 OK"

# --name 覆盖输出基名
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/c" --name hero >/dev/null 2>&1 || fail "--name 运行失败"
[ -f "$WORK/c/hero.mesh.json" ] || fail "--name 未生效"
echo "gltf_import_e2e: --name OK"

# 错误输入 → 非 0
if "$PY" "$TOOL" "$WORK/does_not_exist.gltf" --out "$WORK/c" >/dev/null 2>&1; then
    fail "错误输入应返回非 0"
fi
echo "gltf_import_e2e: 错误路径 OK"

# ---- KHR 材质扩展（plans/19 材质迁移）----
EXT_FIXTURE="$FIXDIR/extensions.gltf"
[ -f "$EXT_FIXTURE" ] || fail "缺夹具 $EXT_FIXTURE"
mkdir -p "$WORK/ext" "$WORK/ext2"
"$PY" "$TOOL" "$EXT_FIXTURE" --out "$WORK/ext" >"$WORK/ext.log" 2>&1 \
    || { cat "$WORK/ext.log"; fail "扩展夹具导入失败"; }
grep -q "gltf_import: OK meshes=2 textures=1" "$WORK/ext.log" \
    || { cat "$WORK/ext.log"; fail "扩展夹具导入统计不符（期望 meshes=2 textures=1）"; }
[ -f "$WORK/ext/extensions_0_0.mesh.json" ] || fail "缺 extensions_0_0.mesh.json"
[ -f "$WORK/ext/extensions_1_0.mesh.json" ] || fail "缺 extensions_1_0.mesh.json"
[ -f "$WORK/ext/textures/image0.png" ] || fail "扩展夹具贴图未搬运"

"$PY" - "$WORK/ext" <<'PY' || exit 1
import json, sys
d = sys.argv[1]
mat = json.load(open(d + "/extensions_0_0.mesh.json", encoding="utf-8"))["materials"][0]
# 有 physical 扩展 → shading_model=1 (Physical)
assert mat["shading_model"] == 1 and isinstance(mat["shading_model"], int), mat.get("shading_model")
# KHR_materials_clearcoat
assert abs(mat["clearcoat"] - 0.8) < 1e-9, mat.get("clearcoat")
assert abs(mat["clearcoat_roughness"] - 0.12) < 1e-9, mat.get("clearcoat_roughness")
assert mat["clearcoat_normal_texture"] == "textures/image0.png", mat.get("clearcoat_normal_texture")
assert abs(mat["clearcoat_normal_scale"] - 0.7) < 1e-9, mat.get("clearcoat_normal_scale")
# KHR_materials_sheen（sheen 无独立开关 → 扩展存在即 1）
assert abs(mat["sheen"] - 1.0) < 1e-9, mat.get("sheen")
assert mat["sheen_color"] == {"x": 0.2, "y": 0.4, "z": 0.9}, mat.get("sheen_color")
assert abs(mat["sheen_roughness"] - 0.25) < 1e-9, mat.get("sheen_roughness")
# KHR_materials_iridescence
assert abs(mat["iridescence"] - 0.6) < 1e-9, mat.get("iridescence")
assert abs(mat["iridescence_ior"] - 1.4) < 1e-9, mat.get("iridescence_ior")
assert abs(mat["iridescence_thickness_min"] - 150.0) < 1e-9, mat.get("iridescence_thickness_min")
assert abs(mat["iridescence_thickness_max"] - 500.0) < 1e-9, mat.get("iridescence_thickness_max")
# KHR_materials_specular
assert abs(mat["specular_intensity"] - 0.7) < 1e-9, mat.get("specular_intensity")
assert mat["specular"] == {"x": 0.9, "y": 0.95, "z": 1.0}, mat.get("specular")
# KHR_materials_ior
assert abs(mat["ior"] - 1.7) < 1e-9, mat.get("ior")
# KHR_materials_anisotropy
assert abs(mat["anisotropy"] - 0.45) < 1e-9, mat.get("anisotropy")
assert abs(mat["anisotropy_rotation"] - 1.2) < 1e-9, mat.get("anisotropy_rotation")
# KHR_materials_emissive_strength
assert abs(mat["emissive_intensity"] - 3.5) < 1e-9, mat.get("emissive_intensity")
# 无 glTF 来源的字段不写（"非默认才写"契约）
for k in ("opacity", "side", "flat_shading", "env_intensity", "normal_scale_y", "shininess",
          "toon_steps", "depth_packing", "matcap_texture", "gradient_texture"):
    assert k not in mat, (k, mat[k])
# KHR_materials_unlit → shading_model=2 (Basic)，且不带任何 physical 扩展字段
u = json.load(open(d + "/extensions_1_0.mesh.json", encoding="utf-8"))["materials"][0]
assert u["shading_model"] == 2, u.get("shading_model")
for k in ("clearcoat", "clearcoat_roughness", "clearcoat_normal_texture", "clearcoat_normal_scale",
          "sheen", "sheen_color", "sheen_roughness", "iridescence", "iridescence_ior",
          "iridescence_thickness_min", "iridescence_thickness_max", "specular",
          "specular_intensity", "ior", "anisotropy", "anisotropy_rotation", "emissive_intensity"):
    assert k not in u, (k, u[k])
# 贴图搬运：内嵌 1x1 PNG 原样落盘
png = open(d + "/textures/image0.png", "rb").read()
assert png[:8] == b"\x89PNG\r\n\x1a\n", png[:8]
# KHR_texture_transform（plans/19）：unlit 的 baseColorTexture scale=[2,3] offset=[0.125,0.25]
assert u.get("uv_scale") == {"x": 2.0, "y": 3.0}, u.get("uv_scale")
assert u.get("uv_offset") == {"x": 0.125, "y": 0.25}, u.get("uv_offset")
# 无变换的 physical 材质不写 uv 字段（非默认才写）
assert "uv_scale" not in mat and "normal_uv_scale" not in mat
print("gltf_import_e2e: 扩展材质映射 OK")
PY
[ $? -eq 0 ] || exit 1

# 扩展夹具确定性：两次导入逐字节一致
"$PY" "$TOOL" "$EXT_FIXTURE" --out "$WORK/ext2" >/dev/null 2>&1 || fail "扩展夹具第二次导入失败"
cmp -s "$WORK/ext/extensions_0_0.mesh.json" "$WORK/ext2/extensions_0_0.mesh.json" \
    || fail "扩展夹具两次导入不一致（violates determinism）"
cmp -s "$WORK/ext/extensions_1_0.mesh.json" "$WORK/ext2/extensions_1_0.mesh.json" \
    || fail "扩展夹具两次导入不一致（violates determinism）"
echo "gltf_import_e2e: 扩展夹具确定性 OK"

# ---- 已入库资产零漂移（无扩展材质）：cube.gltf → scenes/pbr/assets/cube.mesh.json ----
CUBE_FIXTURE="$FIXDIR/cube.gltf"
CUBE_ASSET="$ROOT/scenes/pbr/assets/cube.mesh.json"
if [ -f "$CUBE_FIXTURE" ] && [ -f "$CUBE_ASSET" ]; then
    mkdir -p "$WORK/cube"
    "$PY" "$TOOL" "$CUBE_FIXTURE" --out "$WORK/cube" >/dev/null 2>&1 || fail "cube.gltf 导入失败"
    cmp -s "$WORK/cube/cube.mesh.json" "$CUBE_ASSET" \
        || fail "已入库 cube 资产漂移（扩展映射不得影响无扩展材质输出）"
    echo "gltf_import_e2e: cube 旧资产零漂移 OK"
fi

# ---- 可选：ToyCar 三个 mesh.json 零漂移（源模型不入库，用 MAQUE_TOYCAR_SRC 提供源目录）----
if [ -n "${MAQUE_TOYCAR_SRC:-}" ] && [ -f "$MAQUE_TOYCAR_SRC/ToyCar.gltf" ]; then
    mkdir -p "$WORK/toycar"
    "$PY" "$TOOL" "$MAQUE_TOYCAR_SRC/ToyCar.gltf" --out "$WORK/toycar" --name ToyCar --compact \
        >/dev/null 2>&1 || fail "ToyCar 源导入失败"
    for n in ToyCar_0_0 ToyCar_1_0 ToyCar_2_0; do
        cmp -s "$WORK/toycar/$n.mesh.json" "$ROOT/scenes/khronos/ToyCar/assets/$n.mesh.json" \
            || fail "ToyCar 资产漂移: $n"
    done
    echo "gltf_import_e2e: ToyCar 零漂移 OK"
else
    echo "gltf_import_e2e: ToyCar 零漂移 skip（未设 MAQUE_TOYCAR_SRC）"
fi

echo "gltf_import_e2e: PASS"
