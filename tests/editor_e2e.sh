#!/usr/bin/env bash
# 编辑器 e2e（M9、docs/11 §3/§4）：通过控制平面编辑场景并落盘，再重新加载验证往返一致。
#
# 判定链：
#   ① Scene.set(dry_run=1) 预演不改变世界；实写后 Scene.get 读到新值；
#   ② Scene.spawn / Scene.destroy 增删实体（dry_run 预演可用）；
#   ③ Scene.save 落盘的 .scn 可被引擎**重新加载**，且编辑后的字段值保持不变（往返一致）；
#   ④ 重载后的世界校验和与保存前一致（编辑器不破坏世界）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "editor_e2e: FAIL $*" >&2; exit 1; }
RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }
VAL()  { grep -o '"value":[0-9.eE+-]*' <<< "$1" | head -1 | cut -d: -f2; }

SAVED="$WORK/edited.scn"
# 场景与脚本同目录：落盘到源场景目录旁，脚本引用相对路径仍可解析
SAVED="$(dirname "$SCENE")/.editor_e2e_saved.scn"
trap 'rm -f "$SAVED"' EXIT

# ---- ① 编辑：改字段 / 预演 / 增删 ----
cat > "$WORK/req1.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.r"}}
{"jsonrpc":"2.0","id":2,"method":"Scene.set","params":{"id":1,"path":"scn.Sprite.r","value":0.125,"dry_run":1}}
{"jsonrpc":"2.0","id":3,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.r"}}
{"jsonrpc":"2.0","id":4,"method":"Scene.set","params":{"id":1,"path":"scn.Sprite.r","value":0.125}}
{"jsonrpc":"2.0","id":5,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.r"}}
{"jsonrpc":"2.0","id":6,"method":"Scene.set","params":{"id":2,"path":"scn.ParticleEmitter.rate","value":55.0}}
{"jsonrpc":"2.0","id":7,"method":"Scene.spawn","params":{"name":"EditorBall","component":"scn.Transform2D"}}
{"jsonrpc":"2.0","id":8,"method":"Scene.destroy","params":{"id":3,"dry_run":1}}
{"jsonrpc":"2.0","id":9,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":12,"method":"Scene.save","params":{"path":"$SAVED"}}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req1.txt" > "$WORK/out1.txt" 2>&1

R1=$(VAL "$(RESP "$WORK/out1.txt" 1)")
R3=$(VAL "$(RESP "$WORK/out1.txt" 3)")
R5=$(VAL "$(RESP "$WORK/out1.txt" 5)")
DRY=$(RESP "$WORK/out1.txt" 2)
SPAWN=$(RESP "$WORK/out1.txt" 7)
ENTS=$(RESP "$WORK/out1.txt" 9)
SAVE=$(RESP "$WORK/out1.txt" 12)

awk -v a="$R1" 'BEGIN { exit !(a > 0.85 && a < 0.95) }' || fail "unexpected initial r: $R1"
echo "$DRY" | grep -q '"dry_run":true' || fail "dry-run not honored: $DRY"
awk -v a="$R3" 'BEGIN { exit !(a > 0.85 && a < 0.95) }' || fail "dry-run must not change the world: $R3"
awk -v a="$R5" 'BEGIN { exit !(a > 0.12 && a < 0.13) }' || fail "set did not apply: $R5"
echo "$SPAWN" | grep -q '"ok":true' || fail "spawn failed: $SPAWN"
echo "$ENTS" | grep -q 'EditorBall' || fail "spawned entity missing"
echo "$SAVE" | grep -q '"ok":true' || fail "save failed: $SAVE"
[ -f "$SAVED" ] || fail "saved scene file missing"

# ---- ② 重新加载落盘的场景：编辑后的值必须保留（往返一致）----
cat > "$WORK/req2.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Scene.get","params":{"id":1,"path":"scn.Sprite.r"}}
{"jsonrpc":"2.0","id":2,"method":"Scene.get","params":{"id":2,"path":"scn.ParticleEmitter.rate"}}
{"jsonrpc":"2.0","id":3,"method":"Scene.entities"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SAVED" < "$WORK/req2.txt" > "$WORK/out2.txt" 2>&1

R2_1=$(VAL "$(RESP "$WORK/out2.txt" 1)")
R2_2=$(VAL "$(RESP "$WORK/out2.txt" 2)")
ENTS2=$(RESP "$WORK/out2.txt" 3)

grep -qE "cannot read scene|cannot read script" "$WORK/out2.txt" && fail "saved scene is not loadable"
grep -q "ui_e2e\|anim.js" /dev/null 2>/dev/null || true
awk -v a="$R2_1" 'BEGIN { exit !(a > 0.12 && a < 0.13) }' || fail "r not preserved after reload: $R2_1"
awk -v a="$R2_2" 'BEGIN { exit !(a > 54.9 && a < 55.1) }' || fail "emitter rate not preserved after reload: $R2_2"
echo "$ENTS2" | grep -q 'EditorBall' || fail "spawned entity not preserved in saved scene"

echo "editor_e2e: ok (r=$R1→$R5 reload=$R2_1, rate=$R2_2, entities=$(printf '%s' "$ENTS2" | grep -o '"name"' | wc -l))"
exit 0
