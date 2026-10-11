#!/usr/bin/env bash
# 动画 / 粒子端到端回归（docs/15 §2/§4、docs/10 §5）。
#
# 判定链：
#   ① AnimationPlayer 播放头随时间推进，关键帧线性采样写回目标字段（radius 6 → 22 → 6）；
#   ② 粒子发射器参与绘制（draws 增加）；
#   ③ 逐帧校验和两跑一致（确定性；粒子走引擎 RNG）；
#   ④ 快照 → 前进 → restore 后校验和回到快照点（回滚含物理/粒子状态）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "anim_e2e: FAIL $*" >&2; exit 1; }
RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }

run() {
  # $1=请求文件 $2=输出文件 $3=轨迹文件（可空）
  MAQUE_TRAIL_OUT="${3-}" MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$1" > "$2" 2>&1
}

cat > "$WORK/req1.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Perf.get"}
{"jsonrpc":"2.0","id":2,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.radius"}}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":4,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.radius"}}
{"jsonrpc":"2.0","id":5,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":6,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.radius"}}
{"jsonrpc":"2.0","id":7,"method":"Scene.get","params":{"id":1,"path":"time"}}
{"jsonrpc":"2.0","id":8,"method":"Perf.get"}
{"jsonrpc":"2.0","id":9,"method":"World.snapshot"}
{"jsonrpc":"2.0","id":10,"method":"Runtime.step","params":{"frames":20}}
{"jsonrpc":"2.0","id":11,"method":"World.trail"}
{"jsonrpc":"2.0","id":12,"method":"World.restore"}
{"jsonrpc":"2.0","id":13,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":14,"method":"World.checksum"}
{"jsonrpc":"2.0","id":15,"method":"World.trail"}
EOF
run "$WORK/req1.txt" "$WORK/out1.txt" "$WORK/trail1.txt"

R2=$(RESP "$WORK/out1.txt" 2 | grep -o '"value":[0-9.]*' | cut -d: -f2)
R4=$(RESP "$WORK/out1.txt" 4 | grep -o '"value":[0-9.]*' | cut -d: -f2)
R6=$(RESP "$WORK/out1.txt" 6 | grep -o '"value":[0-9.]*' | cut -d: -f2)
T7=$(RESP "$WORK/out1.txt" 7 | grep -o '"value":[0-9.]*' | cut -d: -f2)
DRAWS=$(RESP "$WORK/out1.txt" 8 | grep -o '"draws":[0-9.]*' | cut -d: -f2)
SNAP=$(RESP "$WORK/out1.txt" 9)
# 回滚后应与"从同一点直接前进"给出同一世界（逐帧轨迹前缀比较）
SUM2=$(RESP "$WORK/out1.txt" 14 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')

grep -q "clip 'fade.anim.json' loaded" "$WORK/out1.txt" || fail "animation clip was not loaded"
[ -n "$R2" ] && [ -n "$R4" ] && [ -n "$R6" ] || fail "Scene.get did not return radius values"
# 关键帧：t=0 → 6，t=1s（60 帧）→ 22；t=0.5s → 约 14
awk -v a="$R2" -v b="$R4" -v c="$R6" 'BEGIN { exit !(a > 5.9 && a < 6.1 && b > 13.5 && b < 14.5 && c > 21.5 && c < 22.5) }' \
  || fail "keyframe sampling wrong: t0=$R2 t1=$R4 t2=$R6"
awk -v t="$T7" 'BEGIN { exit !(t > 0.99 && t < 1.01) }' || fail "playback head wrong: $T7"
awk -v d="$DRAWS" 'BEGIN { exit !(d > 1) }' || fail "particles not drawn (draws=$DRAWS)"
echo "$SNAP" | grep -q '"ok":true' || fail "World.snapshot failed: $SNAP"
[ -n "$SUM2" ] || fail "World.checksum after restore returned nothing"
grep -qE "hot reload|DESYNC" "$WORK/out1.txt" && fail "unexpected reload/desync"

# ---- ③ 回滚保真：快照@60 → 前进 20 → 回滚 → 前进 40 == 直接前进 100 ----
cat > "$WORK/req_plain.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":100}}
{"jsonrpc":"2.0","id":8,"method":"World.checksum"}
EOF
cat > "$WORK/req_rollback.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":60}}
{"jsonrpc":"2.0","id":6,"method":"World.snapshot"}
{"jsonrpc":"2.0","id":7,"method":"Runtime.step","params":{"frames":20}}
{"jsonrpc":"2.0","id":9,"method":"World.restore"}
{"jsonrpc":"2.0","id":10,"method":"Runtime.step","params":{"frames":40}}
{"jsonrpc":"2.0","id":11,"method":"World.checksum"}
EOF
run "$WORK/req_plain.txt" "$WORK/out_plain.txt"
run "$WORK/req_rollback.txt" "$WORK/out_rb.txt"
PLAIN=$(RESP "$WORK/out_plain.txt" 8 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
ROLL=$(RESP "$WORK/out_rb.txt" 11 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
[ -n "$PLAIN" ] && [ "$PLAIN" = "$ROLL" ] || fail "rollback fidelity broken: plain=$PLAIN rollback=$ROLL"

# ---- ④ 确定性：同一输入两跑逐帧校验和一致（粒子/动画/物理都入校验和）----
run "$WORK/req1.txt" "$WORK/out2.txt" "$WORK/trail2.txt"
TRAIL1=$(RESP "$WORK/out1.txt" 15)
TRAIL2=$(RESP "$WORK/out2.txt" 15)
[ -n "$TRAIL1" ] && [ "$TRAIL1" = "$TRAIL2" ] || fail "run-to-run determinism broken: $TRAIL1 != $TRAIL2"
[ -s "$WORK/trail1.txt" ] || fail "no per-frame trail"
diff -q "$WORK/trail1.txt" "$WORK/trail2.txt" >/dev/null || fail "per-frame trails differ between runs"

echo "anim_e2e: ok (radius=$R2→$R4→$R6, draws=$DRAWS, frames=$(wc -l < "$WORK/trail1.txt"))"
exit 0
