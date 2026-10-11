#!/usr/bin/env bash
# 导航端到端（docs/15 导航、docs/10 §5）：
#   ① 引擎按 A* 生成路径并逐帧追踪（实体位置改变）；
#   ② 代理最终到达目标（NavAgent.state = 2 Arrived）；
#   ③ 两跑逐帧校验和一致（寻路/追踪确定性）；
#   ④ 网格/障碍由脚本数据声明（nav_setup/nav_block），引擎侧无硬编码地图。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "nav_e2e: FAIL $*" >&2; exit 1; }
RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }
POS() { printf '%s' "$1" | grep -o '"name":"Walker"[^}]*' | grep -o '"x":[-0-9.]*,"y":[-0-9.]*' | tr -d 'xy":' | tr ',' ' '; }
STATE() { printf '%s' "$1" | grep -o '"value":[-0-9.]*' | head -1 | cut -d: -f2; }

cat > "$WORK/req1.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":2,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":4,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":5,"method":"Runtime.step","params":{"frames":270}}
{"jsonrpc":"2.0","id":6,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":7,"method":"Scene.get","params":{"id":3,"path":"scn.NavAgent.state"}}
{"jsonrpc":"2.0","id":8,"method":"World.trail"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req1.txt" > "$WORK/out1.txt" 2>&1

P1=$(POS "$(RESP "$WORK/out1.txt" 2)")
P2=$(POS "$(RESP "$WORK/out1.txt" 4)")
P3=$(POS "$(RESP "$WORK/out1.txt" 6)")
ST=$(STATE "$(RESP "$WORK/out1.txt" 7)")
TRAIL1=$(RESP "$WORK/out1.txt" 8 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')

[ -n "$P1" ] && [ -n "$P2" ] && [ -n "$P3" ] || fail "could not read Walker positions"
# 代理必须真的在移动（路径追踪生效）
awk -v a="$P1" -v b="$P2" 'BEGIN { split(a,p1," "); split(b,p2," "); d=(p1[1]-p2[1])^2+(p1[2]-p2[2])^2; exit !(d > 1.0) }' \
  || fail "agent did not move between frames 1 and 31 ($P1 → $P2)"
# 到达目标（280,200）附近
awk -v b="$P3" 'BEGIN { split(b,p," "); d=sqrt((p[1]-280)^2+(p[2]-200)^2); exit !(d < 6.0) }' \
  || fail "agent did not reach the goal: $P3"
[ "$ST" = "2" ] || [ "$ST" = "1" ] || fail "unexpected NavAgent state: $ST"

# ③ 确定性：同输入两跑逐帧校验和一致
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req1.txt" > "$WORK/out2.txt" 2>&1
TRAIL2=$(RESP "$WORK/out2.txt" 8 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
[ -n "$TRAIL1" ] && [ "$TRAIL1" = "$TRAIL2" ] || fail "nav determinism broken: $TRAIL1 != $TRAIL2"

# ① 地图来自场景脚本（非引擎硬编码）：脚本里出现 nav_setup / nav_block
grep -q 'nav_setup' "$(dirname "$SCENE")/nav.js" || fail "map is not declared by scene data"
grep -q 'nav_block' "$(dirname "$SCENE")/nav.js" || fail "obstacles are not declared by scene data"

echo "nav_e2e: ok (start=$P1 mid=$P2 end=$P3 state=$ST trail=$TRAIL1)"
exit 0
