#!/usr/bin/env bash
# 静态网格合并端到端（plans/36 / assessment P0 内容管线）：
#   tools/merge_static_meshes.py 把两个 cube 实例按 TRS 烘焙成一个 .mesh.json →
#   运行时 load_mesh 成功（mesh>=0）、场景只有 1 个 MeshRenderer/draw、画面可见、两次一致。
# 参数：<player> <cube.mesh.json> <workdir>
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
CUBE="$2"
WORK="$3"
REPO_ROOT="${MAQUE_REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
TOOL="$REPO_ROOT/tools/merge_static_meshes.py"

fail() { echo "merge_static_meshes_e2e: FAIL $*" >&2; exit 1; }
[ -f "$CUBE" ] || { echo "merge_static_meshes_e2e: skip（缺 $CUBE）"; exit 77; }
[ -f "$TOOL" ] || fail "缺 $TOOL"

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_CAPTURE MAQUE_CAPTURE_WGPU \
      MAQUE_WINDOW MAQUE_PRESENT MAQUE_IMGUI MAQUE_RENDER3D_INFO 2>/dev/null || true

cat > "$WORK/manifest.json" <<EOF
{
  "name": "merged_cubes",
  "groups": [
    { "meshes": [
      { "path": "$CUBE", "position": [-1.0, 0.0, 0.0] },
      { "path": "$CUBE", "position": [1.0, 0.0, 0.0], "rotation_deg": [0.0, 45.0, 0.0], "scale": [1.0, 2.0, 1.0] }
    ] }
  ]
}
EOF
"$PY" "$TOOL" "$WORK/manifest.json" --out "$WORK" > "$WORK/merge.log" 2>&1 \
    || { cat "$WORK/merge.log"; fail "合并工具失败"; }
[ -f "$WORK/merged_cubes.mesh.json" ] || { cat "$WORK/merge.log"; fail "没有输出 mesh"; }

# 几何自检：48 顶点 / 72 索引 / 包围盒包含两个实例
"$PY" - "$WORK/merged_cubes.mesh.json" <<'PY' || fail "合并结果几何断言失败"
import json, sys
d = json.load(open(sys.argv[1], encoding='utf-8'))
assert d['vertex_layout'] == 0 and d['vertex_stride'] == 48, d['vertex_stride']
assert len(d['vertices']) == 48, len(d['vertices'])
assert len(d['indices']) == 72, len(d['indices'])
assert max(d['indices']) < len(d['vertices'])
bmin, bmax = d['bounds_min'], d['bounds_max']
assert bmin[0] < -1.55 and bmax[0] > 1.8, (bmin, bmax)
assert bmin[1] < -1.15 and bmax[1] > 1.15, (bmin, bmax)
PY

cat > "$WORK/merged.scn" <<'EOF'
{
  "title": "merge_static_meshes",
  "width": 320,
  "height": 240,
  "views": 1,
  "entities": [
    { "name": "camera", "components": [
      { "type": "scene.Transform3D", "local": { "position": { "x": 0.0, "y": 0.0, "z": 6.0 } } },
      { "type": "render.Camera3D", "fov_y": 50.0, "near": 0.1, "far": 100.0,
        "clear_color": { "r": 0.05, "g": 0.06, "b": 0.08, "a": 1.0 } } ] },
    { "name": "sun", "components": [
      { "type": "scene.Transform3D", "local": { "rotation": { "x": -0.6, "y": -0.4, "z": 0.0 } } },
      { "type": "render.Light", "intensity": 2.0 } ] },
    { "name": "merged", "components": [
      { "type": "scene.Transform3D" },
      { "type": "render.MeshRenderer", "mesh_path": "merged_cubes.mesh.json" },
      { "type": "asset.PbrMaterial", "base_color": { "r": 0.9, "g": 0.55, "b": 0.25, "a": 1.0 },
        "roughness": 0.45 } ] }
  ]
}
EOF

run_shot() {  # <ppm>
    timeout 180 "$PLAYER" --shot "$1" --shot-frame 2 --frames 3 "$WORK/merged.scn" \
        >"$1.log" 2>&1 || { cat "$1.log"; fail "渲染失败"; }
    grep -q "runtime: shot" "$1.log" || { cat "$1.log"; fail "截图未写入"; }
}
run_shot "$WORK/a.ppm"
MAQUE_RENDER3D_INFO=1 timeout 180 "$PLAYER" --shot "$WORK/b.ppm" --shot-frame 2 --frames 3 \
    "$WORK/merged.scn" > "$WORK/b.log" 2>&1 || { cat "$WORK/b.log"; fail "渲染失败 2"; }
grep -q "runtime: 3d scene cams=1 meshes=1" "$WORK/b.log" \
    || { cat "$WORK/b.log"; fail "合并场景应为 1 个 MeshRenderer"; }
grep -q "mesh renderer node=.*path='merged_cubes.mesh.json' → mesh=-1" "$WORK/b.log" \
    && { cat "$WORK/b.log"; fail "merged mesh 载入失败（mesh=-1）"; }
grep -q "mesh renderer node=.*path='merged_cubes.mesh.json' → mesh=[0-9]" "$WORK/b.log" \
    || { cat "$WORK/b.log"; fail "没有 mesh 载入成功日志"; }
cmp -s "$WORK/a.ppm" "$WORK/b.ppm" || fail "两次渲染不一致（非确定性）"

"$PY" - "$WORK/a.ppm" <<'PY' || fail "画面可见性断言失败"
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
w, h = map(int, dims.split())
base = px[0:3]
n = 0
for i in range(0, len(px), 3):
    if abs(px[i]-base[0]) + abs(px[i+1]-base[1]) + abs(px[i+2]-base[2]) > 30:
        n += 1
assert n / (w * h) > 0.02, n / (w * h)
PY

echo "merge_static_meshes_e2e: PASS（合批 1 draw、几何/包围盒/确定性/可见）"
