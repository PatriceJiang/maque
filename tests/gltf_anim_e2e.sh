#!/usr/bin/env bash
# glTF 关键帧动画端到端（plans/22 §S2）：
#   夹具 tests/data/gltf/animated.gltf（pivot→spinner，rotation/translation LINEAR + scale STEP）
#   ① 结构：.clip.json 3 轨道（node/path/interpolation 正确）、.scn 有 anim_player（scn.AnimationPlayer.clips）。
#   ② 渲染：--shot-seq 第 1/20/40 帧 PPM **逐一不同**（真的在动）；同帧两次逐字节一致（确定性）。
#   ③ 日志：clip 加载、anim_pose 采样、无 mesh=-1。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
IMPORTER="$2"
WORK="$3"
[ -n "${PY:-}" ] || { echo "gltf_anim_e2e: skip（无 python3）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_anim_e2e: skip（缺 player）"; exit 77; }
[ -f "$IMPORTER" ] || { echo "gltf_anim_e2e: skip（缺 importer）"; exit 77; }
ROOT="$(cd "$(dirname "$IMPORTER")/.." && pwd)"
FIX="$ROOT/tests/data/gltf/animated.gltf"
[ -f "$FIX" ] || { echo "gltf_anim_e2e: skip（缺夹具 $FIX）"; exit 77; }
fail() { echo "gltf_anim_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/scn"
export PYTHONIOENCODING=utf-8
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_WINDOW MAQUE_ANIM MAQUE_SKIN MAQUE_SHOT MAQUE_SHOT_SEQ 2>/dev/null || true

"$PY" "$IMPORTER" "$FIX" --out "$WORK/scn" --name Animated --scene --compact >"$WORK/import.log" 2>&1 \
    || { cat "$WORK/import.log"; fail "导入失败"; }

"$PY" - "$WORK/scn" <<'PY' || exit 1
import json, os, sys
d = sys.argv[1]
scn = json.load(open(os.path.join(d, "Animated.scn"), encoding="utf-8"))
clip = json.load(open(os.path.join(d, "assets", "anim", "spin.clip.json"), encoding="utf-8"))
assert clip["duration"] == 1.0, clip["duration"]
tracks = clip["tracks"]
assert len(tracks) == 3, len(tracks)
by = {(t["node"], t["path"]): t for t in tracks}
assert ("pivot", "rotation") in by and by[("pivot", "rotation")]["interpolation"] == "LINEAR"
assert ("spinner", "translation") in by and by[("spinner", "translation")]["interpolation"] == "LINEAR"
assert ("spinner", "scale") in by and by[("spinner", "scale")]["interpolation"] == "STEP"
# 层级场景：pivot → spinner（含 mesh 子实体）
players = []
def walk(ents):
    for e in ents:
        for c in e.get("components", []):
            if c.get("type") == "scn.AnimationPlayer":
                players.append(c)
        walk(e.get("children", []))
walk(scn["entities"])
assert len(players) == 1, len(players)
p = players[0]
assert p.get("clips") == ["assets/anim/spin.clip.json"], p.get("clips")
assert p.get("playing") is True and p.get("loop") is True
print("gltf_anim_e2e: 结构断言 OK")
PY

export MAQUE_PRESENT=off MAQUE_WINDOW=0x0 MAQUE_RENDER3D_INFO=1
timeout 90 "$PLAYER" --shot-seq "$WORK/anim_f{}.ppm" --shot-frames 1,20,40 "$WORK/scn/Animated.scn" \
    >"$WORK/render.log" 2>&1 || { cat "$WORK/render.log"; fail "动画渲染失败"; }
grep -q "clip 'assets/anim/spin.clip.json' tracks=3" "$WORK/render.log" || { cat "$WORK/render.log"; fail "clip 未加载"; }
grep -q "anim entity=" "$WORK/render.log" || { cat "$WORK/render.log"; fail "没有动画采样（anim entity= 缺失）"; }
if grep -q "mesh=-1" "$WORK/render.log"; then cat "$WORK/render.log"; fail "网格加载失败（mesh=-1）"; fi
for f in 1 20 40; do [ -f "$WORK/anim_f$f.ppm" ] || fail "缺帧 $f"; done
H1=$(sha256sum "$WORK/anim_f1.ppm" | cut -d' ' -f1)
H20=$(sha256sum "$WORK/anim_f20.ppm" | cut -d' ' -f1)
H40=$(sha256sum "$WORK/anim_f40.ppm" | cut -d' ' -f1)
[ "$H1" != "$H20" ] && [ "$H20" != "$H40" ] && [ "$H1" != "$H40" ] || fail "帧间画面相同（动画没有生效）"
echo "gltf_anim_e2e: 帧 1/20/40 各不相同 OK"

# 同帧两次逐字节一致（确定性；--shot-frame 20）
timeout 90 "$PLAYER" --shot "$WORK/frame20_again.ppm" --shot-frame 20 "$WORK/scn/Animated.scn" \
    >"$WORK/render2.log" 2>&1 || { cat "$WORK/render2.log"; fail "重复渲染失败"; }
cmp -s "$WORK/anim_f20.ppm" "$WORK/frame20_again.ppm" || fail "同帧两次截图不一致（确定性）"
echo "gltf_anim_e2e: 确定性 OK"

"$PY" - "$WORK/anim_f1.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], "rb").read()
magic, dims, _max, px = d.split(b"\n", 3)
assert magic == b"P6", magic
w, h = map(int, dims.split())
bg = px[0:3]
nonbg = sum(1 for i in range(0, len(px), 3)
            if abs(px[i]-bg[0]) + abs(px[i+1]-bg[1]) + abs(px[i+2]-bg[2]) > 25)
ratio = nonbg / (w * h)
print(f"gltf_anim_e2e: {w}x{h} nonbg={ratio:.3f}")
assert ratio > 0.005, ratio
PY
echo "gltf_anim_e2e: ① 结构 ② 动画帧 ③ 确定性 OK"
