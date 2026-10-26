#!/usr/bin/env bash
# glTF 离线导入端到端（plans/18-pbr-3d.md 方向 2 / M4b）：
#   夹具 tests/data/gltf/triangle.gltf（内嵌 base64 buffer）→ tools/gltf_import.py
#   断言：顶点/索引/包围盒/材质字段正确、48B 顶点布局一致、两次导入逐字节一致（确定性）、
#         --name 覆盖输出名、错误输入返回非 0。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
TOOL="$1"
FIXTURE="$2"
WORK="$3"
[ -n "${PY:-}" ] || { echo "gltf_import_e2e: skip（无 python3）"; exit 77; }
[ -f "$TOOL" ] || { echo "gltf_import_e2e: skip（缺 $TOOL）"; exit 77; }
[ -f "$FIXTURE" ] || { echo "gltf_import_e2e: skip（缺夹具 $FIXTURE）"; exit 77; }
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
echo "gltf_import_e2e: PASS"
