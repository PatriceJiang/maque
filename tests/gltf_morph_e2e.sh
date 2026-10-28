#!/usr/bin/env bash
# glTF morph 变形目标端到端（plans/22 §S3）：
#   夹具 tests/data/gltf/morph.gltf（2 targets、default_weights [0.5,0.25]、weights 动画）
#   ① 结构：.mesh.json 有 morph.targets/default_weights；clip 有 path=weights 轨道。
#   ② 默认权重生效：把 .mesh.json 的 default_weights 改成 0 → 截图必须变化。
#   ③ weights 动画：第 1/20/40 帧 PPM 各不相同；同帧两次一致（确定性）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
IMPORTER="$2"
WORK="$3"
[ -n "${PY:-}" ] || { echo "gltf_morph_e2e: skip（无 python3）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_morph_e2e: skip（缺 player）"; exit 77; }
[ -f "$IMPORTER" ] || { echo "gltf_morph_e2e: skip（缺 importer）"; exit 77; }
ROOT="$(cd "$(dirname "$IMPORTER")/.." && pwd)"
FIX="$ROOT/tests/data/gltf/morph.gltf"
[ -f "$FIX" ] || { echo "gltf_morph_e2e: skip（缺夹具 $FIX）"; exit 77; }
fail() { echo "gltf_morph_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/scn"
export PYTHONIOENCODING=utf-8
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_WINDOW MAQUE_ANIM MAQUE_SKIN MAQUE_SHOT MAQUE_SHOT_SEQ 2>/dev/null || true

"$PY" "$IMPORTER" "$FIX" --out "$WORK/scn" --name Morph --scene --compact >"$WORK/import.log" 2>&1 \
    || { cat "$WORK/import.log"; fail "导入失败"; }

"$PY" - "$WORK/scn" <<'PY' || exit 1
import json, os, sys
d = sys.argv[1]
mesh = json.load(open(os.path.join(d, "assets", "Morph.mesh.json"), encoding="utf-8"))
assert "morph" in mesh, sorted(mesh)
m = mesh["morph"]
assert len(m["default_weights"]) == 2 and abs(m["default_weights"][0] - 0.5) < 1e-6, m["default_weights"]
assert len(m["targets"]) == 2
n = len(mesh["vertices"])
assert len(m["targets"][0]["positions"]) == n
clip = json.load(open(os.path.join(d, "assets", "anim", "morph_anim.clip.json"), encoding="utf-8"))
assert len(clip["tracks"]) == 1 and clip["tracks"][0]["path"] == "weights", clip["tracks"]
assert len(clip["tracks"][0]["values"][0]) == 2, clip["tracks"][0]["values"][0]
print("gltf_morph_e2e: 结构断言 OK")
PY

export MAQUE_PRESENT=off MAQUE_WINDOW=0x0 MAQUE_RENDER3D_INFO=1
timeout 90 "$PLAYER" --shot-seq "$WORK/morph_f{}.ppm" --shot-frames 1,20,40 "$WORK/scn/Morph.scn" \
    >"$WORK/render.log" 2>&1 || { cat "$WORK/render.log"; fail "morph 渲染失败"; }
grep -q "morph mesh 'assets/Morph.mesh.json' targets=2" "$WORK/render.log" || { cat "$WORK/render.log"; fail "morph 网格未加载"; }
if grep -q "mesh=-1" "$WORK/render.log"; then cat "$WORK/render.log"; fail "网格加载失败（mesh=-1）"; fi
H1=$(sha256sum "$WORK/morph_f1.ppm" | cut -d' ' -f1)
H20=$(sha256sum "$WORK/morph_f20.ppm" | cut -d' ' -f1)
H40=$(sha256sum "$WORK/morph_f40.ppm" | cut -d' ' -f1)
[ "$H1" != "$H20" ] && [ "$H20" != "$H40" ] || fail "weights 动画没有生效（帧相同）"
echo "gltf_morph_e2e: weights 动画帧变化 OK"

# 默认权重生效：复制场景 → default_weights 置 0 → 第 1 帧必须与原来不同
cp -r "$WORK/scn" "$WORK/scn_zero"
"$PY" - "$WORK/scn_zero/assets/Morph.mesh.json" <<'PY' || exit 1
import json, sys
p = sys.argv[1]
m = json.load(open(p, encoding="utf-8"))
m["morph"]["default_weights"] = [0.0, 0.0]
json.dump(m, open(p, "w", encoding="utf-8", newline="\n"), ensure_ascii=False, separators=(",", ":"))
PY
MAQUE_ANIM=0 timeout 90 "$PLAYER" --shot "$WORK/zero.ppm" --frames 2 "$WORK/scn_zero/Morph.scn" \
    >"$WORK/zero.log" 2>&1 || { cat "$WORK/zero.log"; fail "权重 0 渲染失败"; }
MAQUE_ANIM=0 timeout 90 "$PLAYER" --shot "$WORK/def.ppm" --frames 2 "$WORK/scn/Morph.scn" \
    >"$WORK/def.log" 2>&1 || { cat "$WORK/def.log"; fail "默认权重渲染失败"; }
cmp -s "$WORK/zero.ppm" "$WORK/def.ppm" && fail "默认权重未生效（default_weights=0 与 0.5/0.25 截图相同）"
echo "gltf_morph_e2e: 默认权重生效 OK"

MAQUE_ANIM=0 timeout 90 "$PLAYER" --shot "$WORK/def2.ppm" --frames 2 "$WORK/scn/Morph.scn" \
    >"$WORK/def2.log" 2>&1 || { cat "$WORK/def2.log"; fail "重复渲染失败"; }
cmp -s "$WORK/def.ppm" "$WORK/def2.ppm" || fail "同帧两次截图不一致（确定性）"
echo "gltf_morph_e2e: ① 结构 ② weights 动画 ③ 默认权重 ④ 确定性 OK"
