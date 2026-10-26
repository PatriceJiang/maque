#!/usr/bin/env bash
# glTF 离线导入端到端（plans/18-pbr-3d.md 方向 2 / M4b；plans/19 材质迁移）。
#   夹具 tests/data/gltf/triangle.gltf（内嵌 base64 buffer）→ tools/gltf_import.py
#   断言：顶点/索引/包围盒/材质字段正确、48B 顶点布局一致、两次导入逐字节一致（确定性）、
#         --name 覆盖输出名、错误输入返回非 0；M1 修复后每个材质都写 side
#         （doubleSided=true→2 / false→0，运行时剔除只认 side）。
#   夹具 tests/data/gltf/extensions.gltf（三角网格 + 内嵌 1x1 PNG）：
#     KHR_materials_{unlit,clearcoat,sheen,iridescence,specular,ior,anisotropy,
#     emissive_strength} → asset.PbrMaterial 字段值 + shading_model(Physical/Basic) +
#     贴图搬运 + 两次导入逐字节一致；unlit 材质不得带 physical 扩展字段。
#   已入库资产漂移基线：cube.gltf 重新生成 → scenes/pbr/assets/cube.mesh.json 逐字节一致
#     （M1 后材质新增 "side" 属一次性预期变更，基线已同步重新生成）。
#   顶点布局 v1 + per-texture sampler + transmission 近似（task-11）：
#     夹具 tests/data/gltf/vertex_uv1.gltf（COLOR_0 normalized ubyte VEC4 + TEXCOORD_1 +
#     clamp/nearest 与 mirror sampler + 第二套 UV 贴图 + transmission 材质）：
#       · 有 COLOR_0/uv1 贴图的 primitive → vertex_layout=1、vertex_stride=56、14 float/顶点
#         （pos3|normal3|color4|uv0_2|uv1_2），颜色归一化、uv1 真读（不是复制 uv0）；
#       · 材质 uv1_mask（bit0 base / bit3 normal 使用 TEXCOORD_1）；
#       · 贴图路径后缀 `#wrap=clamp,clamp&filter=nearest` / `#wrap=mirror,repeat`，默认 sampler 无后缀；
#       · 无 COLOR_0/uv1 的 primitive 仍 layout=0/12 float（旧资产零漂移）；
#       · transmission factor=0.5 → alpha_mode=2、opacity=0.575（近似）。
#   相机 CLI：--camera-position/--camera-target/--camera-fov/--camera-distance-scale（仅 --scene）。
#   KHR 扩展材质贴图槽（plans/20 第 2 轮，task-13）：
#     夹具 tests/data/gltf/ext_material_slots.gltf（一个材质同时带 clearcoat/sheen/iridescence/
#     anisotropy 扩展 + 3 个 texture 复用同一 1x1 PNG、不同 sampler）：
#       · clearcoatTexture→clearcoat_texture、clearcoatRoughnessTexture→clearcoat_roughness_texture、
#         sheenColorTexture→sheen_color_texture、sheenRoughnessTexture→sheen_roughness_texture、
#         iridescenceTexture→iridescence_texture、iridescenceThicknessTexture→
#         iridescence_thickness_texture、anisotropyTexture→anisotropy_texture 全部映射；
#       · 复用 sampler 后缀（clamp/clamp+nearest、mirror/repeat、默认无后缀）；
#         specularTexture/specularColorTexture 仍未支持（gap 文案在迁移脚本，Lead 更新）。
#   KHR_lights_punctual + KHR_node_visibility（plans/20，task-14）：
#     夹具 tests/data/gltf/lights_visibility.gltf（3 方向光 + 3 点光 + 3 聚光 = 9 盏；
#     隐藏节点子树含 mesh 与灯光）：
#       · → 8 盏上限（方向光优先，第 9 盏聚光被丢弃并 warning）；有灯光时不再追加默认太阳；
#       · Light 实体 type=0/1/2、position/rotation 用节点世界变换（方向 = 节点 -Z）、
#         spot 内/外锥角（弧度→度）、point/spot range（缺省 0=无限远）；
#       · 强度观感归一化：directional=2.6×intensity；point/spot=2.6×(d_ref²+1)×intensity；
#       · KHR_node_visibility=false 节点及整棵子树（hidden_mesh/hidden_light）不生成实体；
#       · 无灯光模型仍生成默认太阳（旧行为零漂移）。
#   可选（MAQUE_TOYCAR_SRC=<含 ToyCar.gltf 的源目录>，源模型不入库）：ToyCar 三个
#     mesh.json 逐字节一致；未设置时跳过（ToyCar 源模型有 KHR 扩展，更新入库后可用此钩子校验）。
set -u
# Windows: Python redirected stdout/stderr defaults to locale (GBK); force UTF-8 for assertions.
export PYTHONIOENCODING=utf-8
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
# M1：doubleSided=false → side=0（MaterialSide::Front，运行时剔除背面）
assert mat["side"] == 0, mat.get("side")
# 无扩展材质：键集合 = 基础字段 + M1 的 side（不写任何扩展默认字段）
assert set(mat) == {"type", "name", "base_color", "metallic", "roughness", "emissive",
                    "double_sided", "side", "alpha_mode", "alpha_cutoff"}, sorted(mat)
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
# M1：源材质 doubleSided=true → side=2（MaterialSide::Double）
assert mat["side"] == 2, mat.get("side")
assert mat["double_sided"] is True
# 无 glTF 来源的字段不写（"非默认才写"契约；side 是例外：每个材质都写）
for k in ("opacity", "flat_shading", "env_intensity", "normal_scale_y", "shininess",
          "toon_steps", "depth_packing", "matcap_texture", "gradient_texture"):
    assert k not in mat, (k, mat[k])
# KHR_materials_unlit → shading_model=2 (Basic)，且不带任何 physical 扩展字段
u = json.load(open(d + "/extensions_1_0.mesh.json", encoding="utf-8"))["materials"][0]
assert u["shading_model"] == 2, u.get("shading_model")
# M1：unlit 源材质未写 doubleSided（默认 false）→ side=0
assert u["side"] == 0, u.get("side")
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

# ---- KHR 扩展材质贴图槽（plans/20 第 2 轮）：clearcoat/sheen/iridescence/anisotropy 共 7 槽 ----
SLOT_FIXTURE="$FIXDIR/ext_material_slots.gltf"
[ -f "$SLOT_FIXTURE" ] || fail "缺夹具 $SLOT_FIXTURE"
mkdir -p "$WORK/slots" "$WORK/slots2"
"$PY" "$TOOL" "$SLOT_FIXTURE" --out "$WORK/slots" >"$WORK/slots.log" 2>&1 \
    || { cat "$WORK/slots.log"; fail "扩展贴图槽夹具导入失败"; }
grep -q "gltf_import: OK meshes=1 textures=1" "$WORK/slots.log" \
    || { cat "$WORK/slots.log"; fail "扩展贴图槽夹具导入统计不符（期望 meshes=1 textures=1）"; }
[ -f "$WORK/slots/ext_material_slots.mesh.json" ] || fail "缺 ext_material_slots.mesh.json"
[ -f "$WORK/slots/textures/image0.png" ] || fail "扩展贴图槽夹具贴图未搬运"

"$PY" - "$WORK/slots" <<'PY' || exit 1
import json, sys
d = sys.argv[1]
m = json.load(open(d + "/ext_material_slots.mesh.json", encoding="utf-8"))
mat = m["materials"][0]
# 7 个扩展贴图槽全部映射（同一 image、不同 sampler → 后缀不同；默认 sampler 无后缀）
assert mat["clearcoat_texture"] == "textures/image0.png#wrap=clamp,clamp&filter=nearest", mat["clearcoat_texture"]
assert mat["clearcoat_roughness_texture"] == "textures/image0.png#wrap=mirror,repeat", mat["clearcoat_roughness_texture"]
assert mat["sheen_color_texture"] == "textures/image0.png#wrap=clamp,clamp&filter=nearest", mat["sheen_color_texture"]
assert mat["sheen_roughness_texture"] == "textures/image0.png", mat["sheen_roughness_texture"]
assert mat["iridescence_texture"] == "textures/image0.png#wrap=clamp,clamp&filter=nearest", mat["iridescence_texture"]
assert mat["iridescence_thickness_texture"] == "textures/image0.png", mat["iridescence_thickness_texture"]
assert mat["anisotropy_texture"] == "textures/image0.png#wrap=mirror,repeat", mat["anisotropy_texture"]
# 既有 clearcoat normal 槽仍工作（mirror sampler + scale）
assert mat["clearcoat_normal_texture"] == "textures/image0.png#wrap=mirror,repeat", mat["clearcoat_normal_texture"]
assert abs(mat["clearcoat_normal_scale"] - 0.8) < 1e-9
# 扩展因子与 Physical 映射不变
assert mat["shading_model"] == 1, mat.get("shading_model")
assert mat["clearcoat"] == 1.0 and abs(mat["clearcoat_roughness"] - 0.2) < 1e-9
assert mat["sheen_color"] == {"x": 0.8, "y": 0.2, "z": 0.1}
assert mat["anisotropy"] == 1.0 and mat["iridescence"] == 1.0
# 网格仍是旧布局（无 COLOR_0/uv1 贴图）
assert m["vertex_layout"] == 0 and m["vertex_stride"] == 48, (m["vertex_layout"], m["vertex_stride"])
print("gltf_import_e2e: 扩展贴图槽 OK")
PY
[ $? -eq 0 ] || exit 1

# 扩展贴图槽确定性：两次导入逐字节一致
"$PY" "$TOOL" "$SLOT_FIXTURE" --out "$WORK/slots2" >/dev/null 2>&1 || fail "扩展贴图槽第二次导入失败"
cmp -s "$WORK/slots/ext_material_slots.mesh.json" "$WORK/slots2/ext_material_slots.mesh.json" \
    || fail "扩展贴图槽两次导入不一致（违反确定性）"
echo "gltf_import_e2e: 扩展贴图槽确定性 OK"

# ---- KHR_lights_punctual 灯光 + KHR_node_visibility 隐藏节点（--scene，task-14）----
LIGHT_FIXTURE="$FIXDIR/lights_visibility.gltf"
[ -f "$LIGHT_FIXTURE" ] || fail "缺夹具 $LIGHT_FIXTURE"
mkdir -p "$WORK/lights" "$WORK/lights2"
"$PY" "$TOOL" "$LIGHT_FIXTURE" --out "$WORK/lights" --scene --compact >"$WORK/lights.log" 2>&1 \
    || { cat "$WORK/lights.log"; fail "灯光/可见性夹具导入失败"; }
[ -f "$WORK/lights/lights_visibility.scn" ] || fail "缺 lights_visibility.scn"
grep -Eq "KHR_lights_punctual.*> 8" "$WORK/lights.log" \
    || { cat "$WORK/lights.log"; fail "灯光上限告警缺失（期望 9>8 丢弃 1 盏）"; }

"$PY" - "$WORK/lights" <<'PY' || exit 1
import json, math, sys
d = sys.argv[1]
scn = json.load(open(d + "/lights_visibility.scn", encoding="utf-8"))
names = [e["name"] for e in scn["entities"]]
def comp(e, key):
    return next((c for c in e["components"] if key in c), None)
def t3(e):
    return next(c for c in e["components"] if c.get("type") == "scene.Transform3D")["local"]
def light(name):
    return comp(next(x for x in lights if x["name"] == name), "intensity")
lights = [e for e in scn["entities"] if comp(e, "intensity") is not None]
meshes = [e for e in scn["entities"] if comp(e, "mesh_path") is not None]
# 9 盏源灯光 → 8 盏上限（方向光优先，最后 1 盏 spot_c 被丢）；隐藏子树里的 mesh/灯都不生成
assert len(lights) == 8, len(lights)
assert [light(e["name"])["type"] for e in lights] == [0, 0, 0, 1, 1, 1, 2, 2], \
    [light(e["name"])["type"] for e in lights]
assert len(meshes) == 1 and meshes[0]["name"] == "mesh_node_0", [m["name"] for m in meshes]
assert all(not n.startswith("hidden") for n in names), names
assert "sun" not in names, names            # 有 glTF 灯光 → 不再追加默认太阳
assert "spot_c_light" not in names, names   # 第 9 盏（非方向光）被丢弃
# 方向 = 节点 -Z（与 runtime L.dir=-R[8..10] 约定一致）：dir_a=+Y、dir_b=-X、dir_c=-Z
def m4r(rot):
    cx, sx = math.cos(rot[0]), math.sin(rot[0])
    cy, sy = math.cos(rot[1]), math.sin(rot[1])
    cz, sz = math.cos(rot[2]), math.sin(rot[2])
    return [cy*cz, cy*sz, -sy,
            sx*sy*cz - cx*sz, sx*sy*sz + cx*cz, sx*cy,
            cx*sy*cz + sx*sz, cx*sy*sz - sx*cz, cx*cy]
def dir_of(name):
    r = t3(next(x for x in lights if x["name"] == name))["rotation"]
    m = m4r([r["x"], r["y"], r["z"]])
    return [-m[6], -m[7], -m[8]]
def close(a, b, eps=1e-6):
    return all(abs(x-y) < eps for x, y in zip(a, b))
assert close(dir_of("dir_a_light"), [0.0, 1.0, 0.0]), dir_of("dir_a_light")
assert close(dir_of("dir_b_light"), [-1.0, 0.0, 0.0]), dir_of("dir_b_light")
assert close(dir_of("dir_c_light"), [0.0, 0.0, -1.0]), dir_of("dir_c_light")
# 位置按节点世界变换
def pos_of(name):
    p = t3(next(x for x in lights if x["name"] == name))["position"]
    return [p["x"], p["y"], p["z"]]
assert close(pos_of("point_a_light"), [1.0, 2.0, 3.0]), pos_of("point_a_light")
assert close(pos_of("spot_a_light"), [0.0, 1.0, 2.0]), pos_of("spot_a_light")
# 强度观感归一化：directional=2.6×gltf；point/spot=2.6×(d_ref²+1)×gltf
# 夹具 mesh 包围盒 (0,0,0)..(1,1,0) → radius=√0.5 → d_ref² = 0.125
P = 2.6 * (0.125 + 1.0)
assert abs(light("dir_a_light")["intensity"] - 2.6) < 1e-12, light("dir_a_light")
assert abs(light("dir_b_light")["intensity"] - 5.2) < 1e-12
assert abs(light("point_a_light")["intensity"] - P * 2.0) < 1e-9
assert abs(light("point_b_light")["intensity"] - P * 0.5) < 1e-9
# range：显式保留；缺省写 0（引擎语义 = 无限远）
assert light("point_a_light")["range"] == 4.0
assert light("point_b_light")["range"] == 0.0
assert light("spot_a_light")["range"] == 6.0
# spot 内外锥角（glTF 弧度半角 → 引擎度）；缺省 inner=0 / outer=π/4
sa, sb = light("spot_a_light"), light("spot_b_light")
assert abs(sa["inner_angle"] - math.degrees(0.3)) < 1e-9 and abs(sa["outer_angle"] - math.degrees(0.6)) < 1e-9
assert sb["inner_angle"] == 0.0 and abs(sb["outer_angle"] - 45.0) < 1e-9
print("gltf_import_e2e: 灯光/可见性导入 OK")
PY
[ $? -eq 0 ] || exit 1

# 灯光夹具确定性：两次导入逐字节一致（scn + assets）
"$PY" "$TOOL" "$LIGHT_FIXTURE" --out "$WORK/lights2" --scene --compact >/dev/null 2>&1 \
    || fail "灯光夹具第二次导入失败"
cmp -s "$WORK/lights/lights_visibility.scn" "$WORK/lights2/lights_visibility.scn" \
    || fail "灯光夹具 scn 两次导入不一致（违反确定性）"
cmp -s "$WORK/lights/assets/lights_visibility.mesh.json" "$WORK/lights2/assets/lights_visibility.mesh.json" \
    || fail "灯光夹具 mesh 两次导入不一致（违反确定性）"
echo "gltf_import_e2e: 灯光夹具确定性 OK"

# 无灯光模型仍生成默认太阳（旧行为零漂移；triangle 夹具无 KHR_lights_punctual）
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/lights/sun_case" --scene >/dev/null 2>&1 \
    || fail "默认太阳场景生成失败"
"$PY" - "$WORK/lights/sun_case/triangle.scn" <<'PY' || exit 1
import json, sys
scn = json.load(open(sys.argv[1], encoding="utf-8"))
names = [e["name"] for e in scn["entities"]]
assert "sun" in names and "camera" in names, names
assert sum(1 for e in scn["entities"] if any("intensity" in c for c in e["components"])) == 1, names
print("gltf_import_e2e: 无灯光模型默认太阳 OK")
PY
[ $? -eq 0 ] || exit 1

# ---- 顶点布局 v1（COLOR_0 / TEXCOORD_1）+ per-texture sampler + transmission 近似 ----
V1_FIXTURE="$FIXDIR/vertex_uv1.gltf"
[ -f "$V1_FIXTURE" ] || fail "缺夹具 $V1_FIXTURE"
mkdir -p "$WORK/v1" "$WORK/v1b" "$WORK/v1c"
"$PY" "$TOOL" "$V1_FIXTURE" --out "$WORK/v1" >"$WORK/v1.log" 2>&1 \
    || { cat "$WORK/v1.log"; fail "布局 v1 夹具导入失败"; }
grep -q "gltf_import: OK meshes=2 textures=1" "$WORK/v1.log" \
    || { cat "$WORK/v1.log"; fail "布局 v1 夹具导入统计不符（期望 meshes=2 textures=1）"; }
[ -f "$WORK/v1/vertex_uv1.mesh.json" ] || fail "缺 vertex_uv1.mesh.json"
[ -f "$WORK/v1/vertex_uv1_0_1.mesh.json" ] || fail "缺 vertex_uv1_0_1.mesh.json"
[ -f "$WORK/v1/textures/image0.png" ] || fail "布局 v1 夹具贴图未搬运"

"$PY" - "$WORK/v1" <<'PY' || exit 1
import json, sys
d = sys.argv[1]
m = json.load(open(d + "/vertex_uv1.mesh.json", encoding="utf-8"))
# layout v1：14 float/顶点、stride 56B；顺序 pos3|normal3|color4|uv0_2|uv1_2
assert m["vertex_layout"] == 1 and m["vertex_stride"] == 56, (m["vertex_layout"], m["vertex_stride"])
assert len(m["vertices"]) == 3, len(m["vertices"])
for v in m["vertices"]:
    assert len(v) == 14, len(v)
assert m["indices"] == [0, 1, 2], m["indices"]
v0 = m["vertices"][0]
assert v0[:6] == [0, 0, 0, 0, 0, 1], v0[:6]
# COLOR_0（normalized ubyte VEC4）→ [1,0,0,1]（除以 255）
assert [round(x, 6) for x in v0[6:10]] == [1, 0, 0, 1], v0[6:10]
# uv0 / uv1 各自读取（uv1 不是复制 uv0）
assert abs(v0[10] - 0.1) < 1e-6 and abs(v0[11] - 0.2) < 1e-6, v0[10:12]
assert abs(v0[12] - 0.7) < 1e-6 and abs(v0[13] - 0.8) < 1e-6, v0[12:14]
mat = m["materials"][0]
# uv1_mask：bit0=base、bit3=normal 使用 TEXCOORD_1（emissive/occlusion 用 uv0）
assert mat.get("uv1_mask") == (1 << 0) | (1 << 3), mat.get("uv1_mask")
# per-texture sampler 后缀（仅非默认；默认 sampler 无后缀）
assert mat["base_color_texture"] == "textures/image0.png#wrap=clamp,clamp&filter=nearest", mat["base_color_texture"]
assert mat["normal_texture"] == "textures/image0.png#wrap=clamp,clamp&filter=nearest", mat["normal_texture"]
assert mat["emissive_texture"] == "textures/image0.png#wrap=mirror,repeat", mat["emissive_texture"]
assert mat["occlusion_texture"] == "textures/image0.png", mat["occlusion_texture"]
# 无 COLOR_0 / 无 uv1 贴图的 primitive 仍走旧布局（12 float/48B，逐字节零漂移路径）
m0 = json.load(open(d + "/vertex_uv1_0_1.mesh.json", encoding="utf-8"))
assert m0["vertex_layout"] == 0 and m0["vertex_stride"] == 48, (m0["vertex_layout"], m0["vertex_stride"])
for v in m0["vertices"]:
    assert len(v) == 12, len(v)
# KHR_materials_transmission 近似：alpha_mode=2；opacity=clamp(1-0.85*0.5,0.15,1)=0.575
g = m0["materials"][0]
assert g["alpha_mode"] == 2, g.get("alpha_mode")
assert abs(g["opacity"] - 0.575) < 1e-9, g.get("opacity")
assert "uv1_mask" not in g, g.get("uv1_mask")
print("gltf_import_e2e: 布局 v1 / sampler / transmission OK")
PY
[ $? -eq 0 ] || exit 1

# 布局 v1 确定性：同输入两次导入（默认 + compact）逐字节一致
"$PY" "$TOOL" "$V1_FIXTURE" --out "$WORK/v1b" >/dev/null 2>&1 || fail "布局 v1 第二次导入失败"
cmp -s "$WORK/v1/vertex_uv1.mesh.json" "$WORK/v1b/vertex_uv1.mesh.json" \
    || fail "布局 v1 两次导入不一致（违反确定性）"
cmp -s "$WORK/v1/vertex_uv1_0_1.mesh.json" "$WORK/v1b/vertex_uv1_0_1.mesh.json" \
    || fail "布局 v1 两次导入不一致（prim1，违反确定性）"
"$PY" "$TOOL" "$V1_FIXTURE" --out "$WORK/v1c" --compact >/dev/null 2>&1 || fail "布局 v1 compact 导入失败"
[ -f "$WORK/v1c/vertex_uv1.mesh.json" ] || fail "布局 v1 compact 未产出"
echo "gltf_import_e2e: 布局 v1 确定性 OK"

# ---- 相机 CLI（--scene）：显式机位 / FOV / 自动拟合距离倍率（task-11）----
mkdir -p "$WORK/cam_a" "$WORK/cam_b" "$WORK/cam_auto1" "$WORK/cam_auto2" "$WORK/cam_auto3"
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/cam_a" --scene \
    --camera-position "3,2,1" --camera-target "0,0.5,0" --camera-fov 30 >"$WORK/cam_a.log" 2>&1 \
    || { cat "$WORK/cam_a.log"; fail "相机 CLI（显式机位）失败"; }
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/cam_b" --scene \
    --camera-position "3,2,1" --camera-target "0,0,0" --camera-fov 30 >/dev/null 2>&1 \
    || fail "相机 CLI（target 变体）失败"
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/cam_auto1" --scene >/dev/null 2>&1 || fail "相机 CLI（默认）失败"
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/cam_auto2" --scene --camera-distance-scale 2 >/dev/null 2>&1 \
    || fail "相机 CLI（distance-scale）失败"
"$PY" "$TOOL" "$FIXTURE" --out "$WORK/cam_auto3" --scene --camera-distance-scale 1 >/dev/null 2>&1 \
    || fail "相机 CLI（distance-scale=1）失败"
cmp -s "$WORK/cam_auto1/triangle.scn" "$WORK/cam_auto3/triangle.scn" \
    || fail "默认取景与 --camera-distance-scale 1 应逐字节一致（默认零漂移）"
# CLI 参数校验：无 --scene 时应报错（不静默忽略）
if "$PY" "$TOOL" "$FIXTURE" --out "$WORK/c" --camera-fov 30 >/dev/null 2>&1; then
    fail "相机 CLI 参数在无 --scene 时应报错"
fi
echo "gltf_import_e2e: 相机 CLI 参数校验 OK"

"$PY" - "$WORK" <<'PY' || exit 1
import json, math, sys
w = sys.argv[1]
def cam(d):
    scn = json.load(open(d + "/triangle.scn", encoding="utf-8"))
    e = next(x for x in scn["entities"] if x["name"] == "camera")
    tr = next(c for c in e["components"] if c["type"] == "scene.Transform3D")["local"]
    c = next(c for c in e["components"] if c["type"] == "render.Camera3D")
    return [tr["position"]["x"], tr["position"]["y"], tr["position"]["z"]], \
           [tr["rotation"]["x"], tr["rotation"]["y"]], c

p, r, c = cam(w + "/cam_a")
assert [round(v, 6) for v in p] == [3.0, 2.0, 1.0], p
assert c["fov_y"] == 30.0, c["fov_y"]
# 朝向 = look_rotation：pitch=asin(dy)，yaw=atan2(-dx,-dz)，eye(3,2,1)→target(0,0.5,0)
d3 = [-3.0, -1.5, -1.0]
ln = math.sqrt(sum(v * v for v in d3))
d3 = [v / ln for v in d3]
want = (math.asin(d3[1]), math.atan2(-d3[0], -d3[2]))
assert abs(r[0] - want[0]) < 1e-9 and abs(r[1] - want[1]) < 1e-9, (r, want)
# --camera-target 生效：target 不同 → 朝向不同
_, r0, _ = cam(w + "/cam_b")
assert abs(r[0] - r0[0]) > 1e-6 or abs(r[1] - r0[1]) > 1e-6, (r, r0)
# 自动拟合 + distance-scale：相机到包围盒中心距离恰为 2×；默认 fov 45
p1, _, c1 = cam(w + "/cam_auto1")
p2, _, c2 = cam(w + "/cam_auto2")
ctr = [0.5, 0.5, 0.0]          # triangle.gltf 包围盒 [0,0,0]..[1,1,0]
d1 = math.dist(p1, ctr)
d2 = math.dist(p2, ctr)
assert abs(d2 / d1 - 2.0) < 1e-9, (d1, d2)
assert abs(c2["fov_y"] - 45.0) < 1e-9, c2["fov_y"]
print("gltf_import_e2e: 相机 CLI OK")
PY
[ $? -eq 0 ] || exit 1
echo "gltf_import_e2e: 相机 CLI 显式机位/取景倍率 OK"

# ---- 已入库资产漂移基线：cube.gltf → scenes/pbr/assets/cube.mesh.json ----
CUBE_FIXTURE="$FIXDIR/cube.gltf"
CUBE_ASSET="$ROOT/scenes/pbr/assets/cube.mesh.json"
if [ -f "$CUBE_FIXTURE" ] && [ -f "$CUBE_ASSET" ]; then
    mkdir -p "$WORK/cube"
    "$PY" "$TOOL" "$CUBE_FIXTURE" --out "$WORK/cube" >/dev/null 2>&1 || fail "cube.gltf 导入失败"
    cmp -s "$WORK/cube/cube.mesh.json" "$CUBE_ASSET" \
        || fail "已入库 cube 资产漂移（基线需随 importer 变更重新生成）"
    echo "gltf_import_e2e: cube 资产基线一致 OK"
fi

# ---- 可选：ToyCar 三个 mesh.json 漂移基线（源模型不入库，用 MAQUE_TOYCAR_SRC 提供源目录）----
if [ -n "${MAQUE_TOYCAR_SRC:-}" ] && [ -f "$MAQUE_TOYCAR_SRC/ToyCar.gltf" ]; then
    mkdir -p "$WORK/toycar"
    "$PY" "$TOOL" "$MAQUE_TOYCAR_SRC/ToyCar.gltf" --out "$WORK/toycar" --name ToyCar --compact \
        >/dev/null 2>&1 || fail "ToyCar 源导入失败"
    for n in ToyCar_0_0 ToyCar_1_0 ToyCar_2_0; do
        cmp -s "$WORK/toycar/$n.mesh.json" "$ROOT/scenes/khronos/ToyCar/assets/$n.mesh.json" \
            || fail "ToyCar 资产漂移: $n"
    done
    echo "gltf_import_e2e: ToyCar 资产基线一致 OK"
else
    echo "gltf_import_e2e: ToyCar 资产基线 skip（未设 MAQUE_TOYCAR_SRC）"
fi

echo "gltf_import_e2e: PASS"
