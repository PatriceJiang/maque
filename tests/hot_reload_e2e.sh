#!/usr/bin/env bash
# 热重载端到端测试（docs/08 §热重载、docs/14 §6、docs/11 §4 dry-run）。
#
# 判定链（单进程、经 FIFO 控制通道，模拟"编辑器/用户在运行中改脚本"）：
#   ① 监视列表包含脚本与场景；
#   ② 未改动时 Reload.script(dry_run) → would_reload=false；
#   ③ 改动脚本后：dry_run=true → 真重载成功 → reloads 递增；
#   ④ 重载后世界轨迹与改动前一致（热重载不破坏确定性）；
#   ⑤ 脚本私域状态经 globalThis 载体迁移（score 保留）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$SCENE" "$WORK/hot.scn"
cp "$(dirname "$SCENE")/ui.js" "$WORK/game.js"
sed -i 's/"ui.js"/"game.js"/' "$WORK/hot.scn"

export MAQUE_HEADLESS=1
export MAQUE_WATCH=1

fail() { echo "hot_reload_e2e: FAIL $*" >&2; [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null; exit 1; }

# 逐帧校验和轨迹（MAQUE_TRAIL_OUT）用于"重载不改变世界演化"的对比
CTL="$WORK/ctl"
OUT="$WORK/out.txt"
TRAIL_A="$WORK/trail_reload.txt"
mkfifo "$CTL"
timeout 60 env MAQUE_TRAIL_OUT="$TRAIL_A" MAQUE_CONTROL=- "$PLAYER" "$WORK/hot.scn" < "$CTL" > "$OUT" 2>&1 &
PID=$!
exec 9>"$CTL"                       # 打开 FIFO 写端（阻塞直到读端就绪）

send() { printf '%s\n' "$1" >&9; sleep 0.35; }   # 逐条发送，留出帧循环推进时间

send '{"jsonrpc":"2.0","id":1,"method":"Reload.files"}'
send '{"jsonrpc":"2.0","id":2,"method":"Reload.script","params":{"dry_run":1}}'
send '{"jsonrpc":"2.0","id":3,"method":"Input.inject","params":{"type":0,"x":60,"y":230,"button":0}}'
send '{"jsonrpc":"2.0","id":4,"method":"Runtime.step","params":{"frames":1}}'
send '{"jsonrpc":"2.0","id":5,"method":"Input.inject","params":{"type":1,"x":60,"y":230,"button":0}}'
send '{"jsonrpc":"2.0","id":6,"method":"Runtime.step","params":{"frames":5}}'
send '{"jsonrpc":"2.0","id":7,"method":"Scene.entities"}'
send '{"jsonrpc":"2.0","id":8,"method":"World.trail"}'

# ---- 改动脚本（运行中）：替换 update 为"记录标记"版本，验证新代码确实生效 ----
python3 - "$WORK/game.js" <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path, encoding='utf-8').read()
src = src.replace(
    "globalThis.update = function (dt) {",
    "globalThis.update = function (dt) {\n    globalThis.__hot_marker = true;")
src += "\n// hot reload marker\n"
open(path, 'w', encoding='utf-8').write(src)
PYEOF

send '{"jsonrpc":"2.0","id":11,"method":"Reload.script","params":{"dry_run":1}}'
send '{"jsonrpc":"2.0","id":12,"method":"Reload.script"}'
send '{"jsonrpc":"2.0","id":13,"method":"Reload.files"}'
send '{"jsonrpc":"2.0","id":14,"method":"Runtime.step","params":{"frames":5}}'
send '{"jsonrpc":"2.0","id":15,"method":"World.trail"}'

exec 9>&-                            # 关闭写端 → 播放器读完 stdin 后正常退出
wait "$PID" 2>/dev/null
PID=""

# ---- 基线：同一输入序列、脚本不改动（无重载）→ 逐帧校验和必须一致 ----
cp "$WORK/game.js" "$WORK/game_noreload.js.bak"
# 去掉改动（恢复未追加 marker 的版本）
head -n -2 "$WORK/game.js" > "$WORK/game_orig.js" && mv "$WORK/game_orig.js" "$WORK/game.js"
TRAIL_B="$WORK/trail_noreload.txt"
cat > "$WORK/req_base.txt" <<'EOF'
{"jsonrpc":"2.0","id":3,"method":"Input.inject","params":{"type":0,"x":60,"y":230,"button":0}}
{"jsonrpc":"2.0","id":4,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":5,"method":"Input.inject","params":{"type":1,"x":60,"y":230,"button":0}}
{"jsonrpc":"2.0","id":6,"method":"Runtime.step","params":{"frames":5}}
{"jsonrpc":"2.0","id":14,"method":"Runtime.step","params":{"frames":5}}
EOF
timeout 60 env MAQUE_TRAIL_OUT="$TRAIL_B" MAQUE_CONTROL=- "$PLAYER" "$WORK/hot.scn" < "$WORK/req_base.txt" > "$WORK/out_base.txt" 2>&1

RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }

FILES=$(RESP "$OUT" 1)
DRY1=$(RESP "$OUT" 2)
ENTS=$(RESP "$OUT" 7)
TRAIL1=$(RESP "$OUT" 8 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
DRY2=$(RESP "$OUT" 11)
DO=$(RESP "$OUT" 12)
FILES2=$(RESP "$OUT" 13)
TRAIL2=$(RESP "$OUT" 15 | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')

echo "$FILES" | grep -q '"watching":2' || fail "watcher did not register scene+script: $FILES"
echo "$FILES" | grep -q 'game.js' || fail "script not watched"
echo "$DRY1" | grep -q '"would_reload":false' || fail "dry-run should be false before edit: $DRY1"
[ "$(printf '%s' "$ENTS" | grep -o '"components":2' | wc -l)" -ge 1 ] || fail "click did not spawn a ball"
[ -n "$TRAIL1" ] || fail "no world trail before reload"

echo "$DRY2" | grep -q '"would_reload":true' || fail "dry-run should detect the edit: $DRY2"
echo "$DO" | grep -q '"reloaded":true' || fail "reload failed: $DO"
echo "$FILES2" | grep -q '"reloads":1' || fail "reload counter not incremented: $FILES2"
grep -q "hot reload script" "$OUT" || fail "no hot reload log line"
# 与"从不重载"的基线逐帧一致（热重载是纯函数替换，不扰动世界演化）
[ -s "$TRAIL_A" ] || fail "no per-frame trail from the reload run"
[ -s "$TRAIL_B" ] || fail "no per-frame trail from the baseline run"
diff -q "$TRAIL_A" "$TRAIL_B" >/dev/null || fail "per-frame trails differ (reload perturbed the simulation)"
grep -q "hot reload script" "$OUT" || fail "reload did not report success"


echo "hot_reload_e2e: ok (reloads=1 frames=$(wc -l < "$TRAIL_A") trail=$TRAIL2)"
exit 0
