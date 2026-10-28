#!/usr/bin/env bash
# 骨骼蒙皮端到端（plans/22-gltf-animation-implementation.md S1）：
#   夹具 tests/data/gltf/skinned.gltf（2 关节 + JOINTS_0/WEIGHTS_0 + IBM）→ importer 层级场景
#   ① 结构：.mesh.json layout=2/stride=72、joints/weights 等长且权重和≈1；.skel.json joint_names
#      全部命中场景实体名；蒙皮 mesh 实体是**场景根**且 local=identity + skin_path。
#   ② 渲染：bind pose（MAQUE_ANIM=0）非背景占比 > 阈值、日志 skins=1、无 mesh=-1/回退静态。
#   ③ 不变量：MAQUE_SKIN=0（identity 调色板）与默认调色板截图**逐字节一致**（palette≈I）。
#   ④ 确定性：同帧两次截图逐字节一致。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
IMPORTER="$2"
WORK="$3"
[ -n "${PY:-}" ] || { echo "gltf_skin_e2e: skip（无 python3）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_skin_e2e: skip（缺 player）"; exit 77; }
[ -f "$IMPORTER" ] || { echo "gltf_skin_e2e: skip（缺 importer）"; exit 77; }
ROOT="$(cd "$(dirname "$IMPORTER")/.." && pwd)"
FIX="$ROOT/tests/data/gltf/skinned.gltf"
[ -f "$FIX" ] || { echo "gltf_skin_e2e: skip（缺夹具 $FIX）"; exit 77; }
fail() { echo "gltf_skin_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/scn"
export PYTHONIOENCODING=utf-8
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_WINDOW MAQUE_SKIN MAQUE_ANIM MAQUE_SHOT MAQUE_SHOT_SEQ 2>/dev/null || true

"$PY" "$IMPORTER" "$FIX" --out "$WORK/scn" --name Skinned --scene --compact >"$WORK/import.log" 2>&1 \
    || { cat "$WORK/import.log"; fail "导入失败"; }
grep -q "hierarchy nodes=" "$WORK/import.log" || { cat "$WORK/import.log"; fail "未走层级场景输出"; }

"$PY" - "$WORK/scn" <<'PY' || exit 1
import json, os, sys
d = sys.argv[1]
scn = json.load(open(os.path.join(d, "Skinned.scn"), encoding="utf-8"))
mesh = json.load(open(os.path.join(d, "assets", "Skinned.mesh.json"), encoding="utf-8"))
skel = json.load(open(os.path.join(d, "assets", "skin", "Skinned.skel.json"), encoding="utf-8"))
assert mesh["vertex_layout"] == 2 and mesh["vertex_stride"] == 72, (mesh["vertex_layout"], mesh["vertex_stride"])
n = len(mesh["vertices"])
assert len(mesh["joints"]) == n and len(mesh["weights"]) == n
for w in mesh["weights"]:
    assert abs(sum(w) - 1.0) < 1e-4, w
for j in mesh["joints"]:
    assert all(0 <= x < len(skel["joint_names"]) for x in j), j
assert len(skel["joint_names"]) == len(skel["inverse_bind"]) == 2
# 场景实体名（含嵌套 children）
names = set()
def walk(ents):
    for e in ents:
        names.add(e.get("name"))
        walk(e.get("children", []))
walk(scn["entities"])
for jn in skel["joint_names"]:
    assert jn in names, ("骨架关节名不在场景实体中", jn)
# 蒙皮 mesh 实体：场景根 + identity + skin_path
skin_ents = [e for e in scn["entities"] if any(c.get("type") == "render.MeshRenderer" and c.get("skin_path") for c in e.get("components", []))]
assert len(skin_ents) == 1, len(skin_ents)
e = skin_ents[0]
t = [c for c in e["components"] if c.get("type") == "scene.Transform3D"][0]["local"]
assert t["position"] == {"x": 0.0, "y": 0.0, "z": 0.0} and t["scale"] == {"x": 1.0, "y": 1.0, "z": 1.0}, t
print("gltf_skin_e2e: 结构断言 OK")
PY

export MAQUE_PRESENT=off MAQUE_WINDOW=0x0 MAQUE_ANIM=0 MAQUE_RENDER3D_INFO=1
timeout 90 "$PLAYER" --shot "$WORK/default.ppm" --frames 2 "$WORK/scn/Skinned.scn" >"$WORK/render.log" 2>&1 \
    || { cat "$WORK/render.log"; fail "默认蒙皮渲染失败"; }
grep -q "skins=1" "$WORK/render.log" || { cat "$WORK/render.log"; fail "没有蒙皮 draw（skins=1 缺失）"; }
grep -q "skeleton 'assets/skin/Skinned.skel.json' joints=2" "$WORK/render.log" || { cat "$WORK/render.log"; fail "骨架未加载"; }
if grep -q "mesh=-1" "$WORK/render.log"; then cat "$WORK/render.log"; fail "网格加载失败（mesh=-1）"; fi
if grep -q "回退静态" "$WORK/render.log"; then cat "$WORK/render.log"; fail "蒙皮回退静态"; fi

MAQUE_SKIN=0 timeout 90 "$PLAYER" --shot "$WORK/noskin.ppm" --frames 2 "$WORK/scn/Skinned.scn" >"$WORK/noskin.log" 2>&1 \
    || { cat "$WORK/noskin.log"; fail "MAQUE_SKIN=0 渲染失败"; }
cmp -s "$WORK/default.ppm" "$WORK/noskin.ppm" || fail "bind pose 不变量失败（palette≠I：默认与 MAQUE_SKIN=0 截图不一致）"
echo "gltf_skin_e2e: bind pose palette≈I OK"

timeout 90 "$PLAYER" --shot "$WORK/default2.ppm" --frames 2 "$WORK/scn/Skinned.scn" >"$WORK/render2.log" 2>&1 \
    || { cat "$WORK/render2.log"; fail "重复渲染失败"; }
cmp -s "$WORK/default.ppm" "$WORK/default2.ppm" || fail "同帧两次截图不一致（确定性）"

"$PY" - "$WORK/default.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], "rb").read()
magic, dims, _max, px = d.split(b"\n", 3)
assert magic == b"P6", magic
w, h = map(int, dims.split())
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
ratio = nonbg / (w * h)
print(f"gltf_skin_e2e: {w}x{h} nonbg={ratio:.3f}")
assert ratio > 0.01, ratio
PY
echo "gltf_skin_e2e: ① 结构 ② 渲染 ③ bind pose 不变量 ④ 确定性 OK"
