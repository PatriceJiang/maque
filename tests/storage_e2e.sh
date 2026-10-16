#!/usr/bin/env bash
# 键值存储端到端（docs/25）：跨进程持久 + 会话隔离 + 控制面读写 + **JS/Lua 读同一份存档**。
#   ① 默认模式 file://<场景目录>/.maque（无需配置即落盘）
#   ② 同一 (uid,session) 连跑两次 → 计数递增（跨进程持久）；换 session → 从 1 开始（隔离）
#   ③ JS 与 Lua 用同一套脚本绑定（storage.*）操作同一份存档：JS→JS→Lua 计数 1→2→3
#   ④ 控制面：Storage.info / list / get / put / remove（put/remove 是变更类）
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）
. "$(dirname "set -u")/maque_python.sh"                 # 本仓库内直接跑（tests/ 下的脚本）
[ -f "$MAQUE_REPO_ROOT/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径 ${BASH_SOURCE[0]} 调用时兜底
PLAYER="$(realpath "$1")"; SCENE_DIR="$(realpath "$2")"; WORK="$(realpath -m "$3")"

rm -rf "$WORK"; mkdir -p "$WORK"
cp "$SCENE_DIR"/storage.scn "$SCENE_DIR"/storage_lua.scn "$SCENE_DIR"/storage.js "$SCENE_DIR"/storage.lua "$WORK/"
cd "$WORK"                                   # 场景用相对路径 → 默认存档根 = <CWD>/.maque
export MAQUE_HEADLESS=1 MAQUE_PRESENT=off

fail() { echo "storage_e2e: FAIL $*" >&2; exit 1; }

run_counter() {   # run_counter <log> <scene> [env…]
    local log="$1" scene="$2"; shift 2
    env MAQUE_MAX_FRAMES=4 "$@" timeout 60 "$PLAYER" "$scene" > "$log" 2>&1 || true
    grep -o 'storage_demo count=[0-9]*' "$log" | tail -1
}

# ---- ① 默认根在场景目录旁 + ② 跨进程持久 / 会话隔离 ----
COUNT=$(run_counter "$WORK/a.log" storage.scn MAQUE_UID=alice MAQUE_SESSION=main)
[ "$COUNT" = "storage_demo count=1" ] || fail "首次运行计数应为 1（$COUNT；见 $WORK/a.log）"
grep -q "storage mode='file' root='$WORK/.*\.maque' ns='alice/main'" "$WORK/a.log" \
    || fail "默认根不是 <场景目录>/.maque（$(grep -o "storage mode='.*" "$WORK/a.log" | head -1)）"
[ -f "$WORK/.maque/alice/main/demo/count" ] || fail "存档文件未落在 .maque/（find: $(find "$WORK" -name count))"
[ ! -f "$WORK/.maque/alice/main/demo/count.tmp" ] || fail "残留 .tmp（原子写失败）"

COUNT=$(run_counter "$WORK/b.log" storage.scn MAQUE_UID=alice MAQUE_SESSION=main)
[ "$COUNT" = "storage_demo count=2" ] || fail "同会话第二次运行应递增到 2（$COUNT）"

COUNT=$(run_counter "$WORK/c.log" storage.scn MAQUE_UID=alice MAQUE_SESSION=run-1)
[ "$COUNT" = "storage_demo count=1" ] || fail "换 session 应从 1 开始（$COUNT）"
COUNT=$(run_counter "$WORK/d.log" storage.scn MAQUE_UID=bob MAQUE_SESSION=main)
[ "$COUNT" = "storage_demo count=1" ] || fail "换 uid 应从 1 开始（$COUNT）"

# ---- ③ 跨语言：同一份存档、同一套绑定（Lua 接着 JS 的计数）----
COUNT=$(run_counter "$WORK/e.log" storage_lua.scn MAQUE_UID=alice MAQUE_SESSION=main)
[ "$COUNT" = "storage_demo count=3" ] || fail "Lua 应读到 JS 写下的同一份存档（$COUNT）"

# ---- ④ 控制面：info / list / get / put / remove ----
CTL="$WORK/ctl.txt"
{
  printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"Storage.info"}'
  printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"Storage.get","params":{"key":"demo/count"}}'
  printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"Storage.put","params":{"key":"cloud/slot","value":"42"}}'
  printf '%s\n' '{"jsonrpc":"2.0","id":4,"method":"Storage.list"}'
  printf '%s\n' '{"jsonrpc":"2.0","id":5,"method":"Storage.remove","params":{"key":"cloud/slot"}}'
  printf '%s\n' '{"jsonrpc":"2.0","id":6,"method":"Storage.get","params":{"key":"cloud/slot"}}'
} > "$CTL"
env MAQUE_MAX_FRAMES=5 MAQUE_UID=alice MAQUE_SESSION=main MAQUE_CONTROL=- \
    timeout 60 "$PLAYER" storage.scn < "$CTL" > "$WORK/ctl.log" 2>&1 || true
grep -q '"id":1,"result":{"ok":true,"mode":"file"' "$WORK/ctl.log" || fail "Storage.info 异常"
# 控制面这次跑也会执行场景脚本（计数 +1）→ 断言"读到了持久档里的计数（≥3）"
grep -qE '"id":2,"result":\{"ok":true,"value":"[3-9][0-9]*"\}' "$WORK/ctl.log" || fail "Storage.get 未读到持久计数（$(grep '"id":2' "$WORK/ctl.log" | head -1)）"
grep -q '"id":3,"result":{"ok":true' "$WORK/ctl.log" || fail "Storage.put 失败"
grep -q '"id":4,"result":{"ok":true,"keys":\["cloud/slot","demo/count","demo/last_seen"\]}' "$WORK/ctl.log" \
    || fail "Storage.list 未列出新键（$(grep '"id":4' "$WORK/ctl.log" | head -1)）"
grep -q '"id":5,"result":{"ok":true' "$WORK/ctl.log" || fail "Storage.remove 失败"
grep -q '"id":6,"result":{"ok":false,"error":"notfound"}' "$WORK/ctl.log" || fail "删除后应 notfound"
[ ! -f "$WORK/.maque/alice/main/cloud/slot" ] || fail "控制面删除未真正落盘"

# 只读会话拒绝变更类（D2）
env MAQUE_MAX_FRAMES=200 MAQUE_UID=alice MAQUE_SESSION=main MAQUE_INSPECT=17771 \
    timeout 60 "$PLAYER" storage.scn > "$WORK/ro.log" 2>&1 &
RO_PID=$!
sleep 1
"$PY" - <<'PY' > "$WORK/ro_resp.txt" 2>&1 || true
import json, socket
s = socket.create_connection(("127.0.0.1", 17771), timeout=3)
s.sendall(json.dumps({"jsonrpc":"2.0","id":9,"method":"Storage.put","params":{"key":"x","value":"y"}}).encode() + b"\n")
print(s.recv(4096).decode(errors="replace"))
PY
wait "$RO_PID" 2>/dev/null || true
grep -q -- '-32001' "$WORK/ro_resp.txt" || fail "只读会话应拒绝 Storage.put（-32001；$(cat "$WORK/ro_resp.txt")）"

echo "storage_e2e: ok（持久/隔离/跨语言/控制面/只读拒绝）"
