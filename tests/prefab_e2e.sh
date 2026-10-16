#!/usr/bin/env bash
# Prefab 实例化与展开端到端（docs/12 §5、docs/assets、docs/11 §4）：Scene.expand_prefabs（旧名 Scene.bake 为兼容别名）。
#
# 判定链：
#   ① .scn 中的 scn.PrefabInstance 在加载时就地展开为子实体（两个实例 → 实体数 4+2+2=7）；
#   ② Scene.save 落盘的场景可再次加载，且展开内容完整（自包含）；
#   ③ Scene.expand_prefabs 之后实例被标记 detached：再次加载**不再**展开（避免重复）；
#   ④ 展开是幂等的（重复调用不再计数）；旧名 Scene.bake 仍可用（兼容别名）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "prefab_e2e: FAIL $*" >&2; exit 1; }
RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }
COUNT() { printf '%s' "$1" | grep -o '"name"' | wc -l; }

SCN_DIR="$(dirname "$SCENE")"
SAVED="$SCN_DIR/.prefab_e2e_saved.scn"
BAKED="$SCN_DIR/.prefab_e2e_baked.scn"
trap 'rm -f "$SAVED" "$BAKED"' EXIT

# ---- ① 展开 + ② 落盘（自包含）----
cat > "$WORK/req1.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":2,"method":"Scene.expand_prefabs","params":{"dry_run":1}}
{"jsonrpc":"2.0","id":3,"method":"Scene.save","params":{"path":"$SAVED"}}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req1.txt" > "$WORK/out1.txt" 2>&1

ENTS=$(RESP "$WORK/out1.txt" 1)
DRY=$(RESP "$WORK/out1.txt" 2)
SAVE=$(RESP "$WORK/out1.txt" 3)
N=$(COUNT "$ENTS")

grep -q "prefab 'star.prefab.json' instantiated" "$WORK/out1.txt" || fail "prefab was not instantiated"
[ "$N" -eq 7 ] || fail "expected 7 entities after instantiation, got $N"
echo "$DRY" | grep -q '"expanded":2' || fail "dry-run expand should report 2 instances: $DRY"
echo "$SAVE" | grep -q '"ok":true' || fail "save failed: $SAVE"

# ---- ② 重载落盘场景：内容自包含（不再依赖 prefab 源也能得到全部实体）----
cat > "$WORK/req2.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.entities"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SAVED" < "$WORK/req2.txt" > "$WORK/out2.txt" 2>&1
N2=$(COUNT "$(RESP "$WORK/out2.txt" 1)")
grep -qE "cannot read scene" "$WORK/out2.txt" && fail "saved scene not loadable"
[ "$N2" -ge 7 ] || fail "saved scene lost prefab content: $N2 entities"

# ---- ③ 展开后不再展开；④ 幂等 + 旧名别名可用 ----
cat > "$WORK/req3.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Scene.expand_prefabs"}
{"jsonrpc":"2.0","id":2,"method":"Scene.bake"}   # 兼容别名（已弃用）
{"jsonrpc":"2.0","id":3,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":4,"method":"Scene.save","params":{"path":"$BAKED"}}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req3.txt" > "$WORK/out3.txt" 2>&1
B1=$(RESP "$WORK/out3.txt" 1)
B2=$(RESP "$WORK/out3.txt" 2)
echo "$B1" | grep -q '"expanded":2' || fail "first expand should mark 2: $B1"
echo "$B2" | grep -q '"expanded":0' || fail "expand must be idempotent（旧名别名同语义）: $B2"

cat > "$WORK/req4.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.entities"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$BAKED" < "$WORK/req4.txt" > "$WORK/out4.txt" 2>&1
N4=$(COUNT "$(RESP "$WORK/out4.txt" 1)")
grep -q "prefab 'star.prefab.json' instantiated" "$WORK/out4.txt" && fail "baked scene must not re-expand prefab"
[ "$N4" -eq "$N" ] || fail "baked entity count changed: $N4 != $N"

# ---- ⑤ 循环引用防护（docs/12 §7）：自引用 prefab 必须报错而非崩溃 ----
mkdir -p "$WORK/cyc"
cat > "$WORK/cyc/self.prefab.json" <<'JSON'
{ "name": "Self", "entities": [ { "name": "S", "components": [ { "type": "scn.PrefabInstance", "source": "self.prefab.json" } ] } ] }
JSON
cat > "$WORK/cyc/c.scn" <<'JSON'
{ "title": "cyc", "script": "", "width": 64, "height": 64, "entities": [ { "name": "R", "components": [ { "type": "scn.PrefabInstance", "source": "self.prefab.json" } ] } ] }
JSON
timeout 20 env MAQUE_HEADLESS=1 MAQUE_MAX_FRAMES=2 "$PLAYER" "$WORK/cyc/c.scn" > "$WORK/cyc/out.txt" 2>&1
CYC_RC=$?
# 无脚本 quit 时正常退出码为 1；崩溃会是 139（SIGSEGV=128+11）→ 只拒绝信号退出
[ "$CYC_RC" -lt 128 ] || fail "cyclic prefab crashed with signal (exit=$CYC_RC)"
grep -q "cyclic prefab reference" "$WORK/cyc/out.txt" || fail "cyclic prefab was not reported"

echo "prefab_e2e: ok (entities=$N, baked-count=$N4, cycle-guard=ok)"
exit 0
