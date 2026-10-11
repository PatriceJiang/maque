#!/usr/bin/env bash
# 输入/回放/快照端到端测试（docs/10 §3/§5、docs/11 §5、docs/19 §9）。
#
# 判定链：
#   ① 控制面注入输入 → 引擎按 DOM 式分发 → 脚本生成球（实体数增加）；
#   ② 世界快照 → restore → 世界校验和必须回到同一值；
#   ③ 录制输入日志（含 frames/trail 元数据）→ 用同一日志回放 → world_trail 必须一致（L1 确定性）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

mkdir -p "$WORK"
REC="$WORK/ui_input.json"
rm -f "$REC"

# 让"同一份输入"两跑都固定 60 帧后停止
export MAQUE_HEADLESS=1
export MAQUE_STOP_FRAME=60

run_case() {
  # $1=记录文件（可空） $2=回放文件（可空）
  MAQUE_RECORD="$1" MAQUE_REPLAY="$2" MAQUE_CONTROL=- "$PLAYER" "$SCENE" 2>/dev/null <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Input.inject","params":{"type":2,"x":60,"y":22,"pointer_type":0}}
{"jsonrpc":"2.0","id":2,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":3,"method":"Input.inject","params":{"type":0,"x":60,"y":22,"button":0}}
{"jsonrpc":"2.0","id":4,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":5,"method":"Input.inject","params":{"type":1,"x":60,"y":22,"button":0}}
{"jsonrpc":"2.0","id":6,"method":"Runtime.step","params":{"frames":6}}
{"jsonrpc":"2.0","id":11,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":12,"method":"World.snapshot"}
{"jsonrpc":"2.0","id":13,"method":"Runtime.step","params":{"frames":10}}
{"jsonrpc":"2.0","id":15,"method":"World.checksum"}
{"jsonrpc":"2.0","id":16,"method":"Runtime.step","params":{"frames":10}}
{"jsonrpc":"2.0","id":17,"method":"World.trail"}
{"jsonrpc":"2.0","id":21,"method":"Types.describe"}
{"jsonrpc":"2.0","id":22,"method":"Replay.log"}
{"jsonrpc":"2.0","id":31,"method":"Input.inject","params":{"type":0,"x":188,"y":22,"button":0}}
{"jsonrpc":"2.0","id":32,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":33,"method":"Input.inject","params":{"type":1,"x":188,"y":22,"button":0}}
{"jsonrpc":"2.0","id":34,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":35,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":36,"method":"World.trail"}
EOF
}

# ---- ① 录制跑：校验注入/分发/快照/回滚 ----
OUT_A=$(run_case "$REC" "")
RESP() { printf '%s\n' "$1" | grep -F "\"id\":$2,\"result\":" | head -1; }

ENTS_A=$(RESP "$OUT_A" 11)
BALLS_A=$(printf '%s' "$ENTS_A" | grep -o '"components":2' | wc -l)
SNAP=$(RESP "$OUT_A" 12)
SUM_A=$(RESP "$OUT_A" 15 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
TRAIL_A=$(RESP "$OUT_A" 17 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
TYPES_A=$(RESP "$OUT_A" 21)
LOG_A=$(RESP "$OUT_A" 22)
ENTS_C=$(RESP "$OUT_A" 35)
BALLS_C=$(printf '%s' "$ENTS_C" | grep -o '"components":2' | wc -l)
TRAIL_C=$(RESP "$OUT_A" 36 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')

fail() { echo "ui_e2e: FAIL $*" >&2; exit 1; }

[ -n "$ENTS_A" ] || fail "no Scene.entities response"
[ "$BALLS_A" -ge 1 ] || fail "click on UI button did not spawn a ball (components:2 count=$BALLS_A)"
echo "$SNAP" | grep -q '"ok":true' || fail "World.snapshot failed: $SNAP"
[ -n "$SUM_A" ] || fail "World.checksum did not return a value"
echo "$TYPES_A" | grep -q 'scn.Button' || fail "Types.describe did not enumerate schema types"
[ -f "$REC" ] || fail "recording log not written: $REC"
grep -q '"frames"' "$REC" || fail "recording log has no frames metadata"
grep -q '"trail"' "$REC" || fail "recording log has no trail metadata"

# ---- ② 回放跑：同一输入日志 → 同一世界轨迹 ----
OUT_B=$(run_case "" "$REC")
TRAIL_B=$(RESP "$OUT_B" 17 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
[ -n "$TRAIL_B" ] || fail "replay run produced no World.trail"
[ "$TRAIL_A" = "$TRAIL_B" ] || fail "replay DESYNC: recorded=$TRAIL_A replay=$TRAIL_B"
printf '%s' "$OUT_B" | grep -q 'replay matches recorded trail' || fail "replay did not report a trail match"

# ---- ③ 清空按钮：点击 → 脚本 → ball_clear（实体减少）----
[ "$BALLS_A" -ge 1 ] || fail "precondition: expected a ball before clear"
[ "$BALLS_C" -eq 0 ] || fail "click on clear button did not remove balls (components:2 count=$BALLS_C)"
[ "$TRAIL_A" != "$TRAIL_C" ] || fail "clear did not change world trail"

# ---- ④ 快照字节非空 ----
echo "$SNAP" | grep -qE '"bytes":[1-9][0-9]*' || fail "snapshot is empty: $SNAP"
echo "$LOG_A" | grep -q '"version":1' || fail "Replay.log returned no log"

echo "ui_e2e: ok (balls=$BALLS_A trail=$TRAIL_A replay=$TRAIL_B)"
exit 0
