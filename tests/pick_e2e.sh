#!/usr/bin/env bash
# 2D raycast + 3D 拾取端到端（docs/15 §3、docs/19 §4）：
#   ① scenes/pick/pick.scn          内建 cube：屏幕点选（正投影，设计坐标精确可算）+ 世界射线 + 距离限制
#   ② scenes/pick/ray2d.scn         2D 物理：盒命中 / 未命中 / max_distance / 起点在内
#   ③ scenes/pbr/pick_imported.scn  .mesh.json 与 glTF 直读：节点旋转 45° 后的 OBB 边缘点选
#      （旋转断言：不旋转时这些点落在本地 AABB 外 → 能抓"忽略节点世界矩阵"的回归）
#   ④ 控制面 Scene.pick / Scene.raycast（与脚本同一实现；先 Runtime.step 成帧，相机缓存才有值）
# 全程 headless（脚本内 quit()，不依赖 DISPLAY）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
PICK_DIR="$2"
PBR_DIR="$3"
WORK="$4"

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "pick_e2e: FAIL $*" >&2; exit 1; }

run() {  # run <log> <frames> <scene>
    local log="$1" frames="$2" scene="$3"
    [ -f "$scene" ] || fail "缺场景 $scene"
    timeout 90 env MAQUE_MAX_FRAMES="$frames" "$PLAYER" "$scene" > "$log" 2>&1 || true
    [ -s "$log" ] || fail "没有输出（$scene）"
    grep -q "AddressSanitizer\|runtime error:" "$log" && { cat "$log"; fail "sanitizer 报错（$scene）"; }
    return 0
}

# ---- ① 内建 cube：屏幕点选 + 世界射线 ----
run "$WORK/pick.log" 20 "$PICK_DIR/pick.scn"
LINE=$(grep -o '\[pick\] ok=.*' "$WORK/pick.log" | tail -1)
[ -n "$LINE" ] || { cat "$WORK/pick.log"; fail "没有 [pick] 结果（脚本未跑/quit 太早）"; }
case "$LINE" in
    "[pick] ok=1 "*) ;;
    *) fail "内建 cube 拾取结果不符：$LINE" ;;
esac
grep -q 'rayA=[0-9-]*' "$WORK/pick.log" || fail "缺少 world raycast 明细"

# ---- ② 2D 物理 raycast ----
run "$WORK/ray2d.log" 20 "$PICK_DIR/ray2d.scn"
LINE=$(grep -o '\[ray2d\] ok=.*' "$WORK/ray2d.log" | tail -1)
[ -n "$LINE" ] || { cat "$WORK/ray2d.log"; fail "没有 [ray2d] 结果"; }
case "$LINE" in
    "[ray2d] ok=1 "*) ;;
    *) fail "2D raycast 结果不符：$LINE" ;;
esac

# ---- ③ 导入资产（.mesh.json / glTF）----
run "$WORK/imported.log" 20 "$PBR_DIR/pick_imported.scn"
LINE=$(grep -o '\[pick_imported\] ok=.*' "$WORK/imported.log" | tail -1)
[ -n "$LINE" ] || { cat "$WORK/imported.log"; fail "没有 [pick_imported] 结果"; }
case "$LINE" in
    "[pick_imported] ok=1 "*) ;;
    *) fail "导入资产拾取结果不符：$LINE" ;;
esac
grep -q "gltf assets" "$WORK/imported.log" || fail "glTF 模型未装配"

# ---- ④ 透视相机（覆盖 NDC w 齐次除法）----
run "$WORK/persp.log" 20 "$PICK_DIR/perspective.scn"
LINE=$(grep -o '\[pick_persp\] ok=.*' "$WORK/persp.log" | tail -1)
[ -n "$LINE" ] || { cat "$WORK/persp.log"; fail "没有 [pick_persp] 结果"; }
case "$LINE" in
    "[pick_persp] ok=1 "*) ;;
    *) fail "透视相机拾取结果不符：$LINE" ;;
esac

# ---- ⑤ 控制面（JSON-RPC）：Scene.pick / Scene.raycast ----
MAQUE_CONTROL=- timeout 90 env "$PLAYER" "$PICK_DIR/pick.scn" > "$WORK/ctl.log" 2>&1 <<'JSON' || fail "控制面运行失败"
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":2,"method":"Scene.pick","params":{"x":100,"y":120}}
{"jsonrpc":"2.0","id":3,"method":"Scene.pick","params":{"x":160,"y":120}}
{"jsonrpc":"2.0","id":4,"method":"Scene.raycast","params":{"ox":0,"oy":0,"oz":5,"dx":1,"dy":0,"dz":-5,"max_distance":100}}
JSON
"$PY" - "$WORK/ctl.log" <<'PY' || exit 1
import json, sys
resp = {}
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    line = line.strip()
    if not line.startswith('{"jsonrpc"'):
        continue
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        continue
    resp[d.get('id')] = d.get('result')
bad = []
if resp.get(1, {}).get('frame') != 2:
    bad.append(f"Runtime.step: {resp.get(1)}")
if resp.get(2, {}).get('name') != 'cube_a':
    bad.append(f"Scene.pick(100,120): {resp.get(2)}")
if resp.get(3, {}).get('entity') != -1:
    bad.append(f"Scene.pick(160,120): {resp.get(3)}")
if resp.get(4, {}).get('name') != 'cube_b':
    bad.append(f"Scene.raycast: {resp.get(4)}")
for b in bad:
    print('pick_e2e:', b)
sys.exit(1 if bad else 0)
PY

echo "pick_e2e: ok（正交/透视屏幕射线 / 世界射线 / 2D 物理 raycast / .mesh.json / glTF / 旋转 OBB / 控制面）"
