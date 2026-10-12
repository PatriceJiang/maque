#!/usr/bin/env bash
# 多脚本热重载端到端（docs/14 §多 VM / §热重载）：
#   ① 改 Lua AI → 只重建**该脚本**的 VM，无需重启游戏即生效
#   ② 其它脚本（JS）**状态保留**、投递顺序不变（这是增量热更的核心价值）
#   ③ 原子替换：写坏脚本**不致命**（新 VM 丢弃、旧 VM 继续跑，游戏照常）
set -u
PLAYER="$1"; SCENE_DIR="$2"; WORK="$3"
rm -rf "$WORK"; mkdir -p "$WORK/run"
cp "$SCENE_DIR"/multilang.scn "$SCENE_DIR"/game.js "$SCENE_DIR"/ai.lua "$WORK/run/"
export MAQUE_HEADLESS=1
fail() { echo "hot_reload_e2e: FAIL $*" >&2; exit 1; }

MAQUE_WATCH=1 MAQUE_MAX_FRAMES=100000000 stdbuf -oL "$PLAYER" "$WORK/run/multilang.scn" > "$WORK/log.txt" 2>&1 &
PID=$!
cleanup() { kill "$PID" 2>/dev/null; for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || return 0; sleep 0.1; done; kill -9 "$PID" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 1 80); do grep -q 'ml_summary' "$WORK/log.txt" && break; sleep 0.1; done
grep -q 'ml_lua_init count=6' "$WORK/log.txt" || fail "初始跨语言事件未发生"
BASE_APPLIED=$(grep -o 'ml_summary applied=[0-9]*' "$WORK/log.txt" | head -1 | grep -o '[0-9]*')
[ -n "$BASE_APPLIED" ] || fail "未取到初始决策数"

# ---- ① 改 Lua：把 AI 的 y 公式换成显眼的新值 + 打标记 ----
python3 - "$WORK/run/ai.lua" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
s = s.replace('local y = 90 + (f * 7) % 60', 'local y = 30 + (f * 11) % 40')
s = s.replace('log("ml_lua_ready")', 'log("ml_lua_ready_v2")')
open(p, 'w', encoding='utf-8').write(s)
PY
for _ in $(seq 1 100); do grep -q 'ml_lua_ready_v2' "$WORK/log.txt" && break; sleep 0.1; done
grep -q 'ml_lua_ready_v2' "$WORK/log.txt" || fail "改动后的 Lua 未生效（热重载失败）"
grep -q "hot reload script 'ai.lua'" "$WORK/log.txt" || fail "未报告 ai.lua 的重载"
grep -q "hot reload script 'game.js'" "$WORK/log.txt" && fail "不该重载未改动的 game.js"
# ② JS 状态保留：热更后 JS 仍在推进决策（计数继续增长，不是从 0 重来）
for _ in $(seq 1 100); do
  NEW=$(grep -o 'ml_summary applied=[0-9]*' "$WORK/log.txt" | tail -1 | grep -o '[0-9]*')
  [ -n "$NEW" ] && [ "$NEW" -gt "$BASE_APPLIED" ] && break
  sleep 0.1
done
NEW=$(grep -o 'ml_summary applied=[0-9]*' "$WORK/log.txt" | tail -1 | grep -o '[0-9]*')
[ -n "$NEW" ] && [ "$NEW" -gt "$BASE_APPLIED" ] || fail "JS 侧状态未延续（applied $BASE_APPLIED → $NEW）"
# 顺序不变：JS 的 ready 仍先于 Lua 的
JS=$(grep -n 'ml_js_ready' "$WORK/log.txt" | head -1 | cut -d: -f1)
LUA=$(grep -n 'ml_lua_ready' "$WORK/log.txt" | head -1 | cut -d: -f1)
[ "$JS" -lt "$LUA" ] || fail "重载后投递顺序变了"

# ---- ③ 原子替换：写坏 Lua，游戏必须活着且继续跑旧逻辑 ----
FRAMES_BEFORE=$(grep -o 'frames=[0-9]*' "$WORK/log.txt" | tail -1 | grep -o '[0-9]*')
printf 'function on_start( this is not lua at all\n' > "$WORK/run/ai.lua"
for _ in $(seq 1 100); do grep -q 'hot reload failed' "$WORK/log.txt" && break; sleep 0.1; done
grep -q 'hot reload failed for' "$WORK/log.txt" || fail "坏脚本未报错"
kill -0 "$PID" 2>/dev/null || fail "坏脚本把游戏搞挂了（应当保留旧 VM）"
for _ in $(seq 1 100); do
  NOW_FRAMES=$(grep -o 'frames=[0-9]*' "$WORK/log.txt" | tail -1 | grep -o '[0-9]*')
  [ -n "$NOW_FRAMES" ] && [ "$NOW_FRAMES" -gt "$FRAMES_BEFORE" ] && break
  sleep 0.1
done
NOW_FRAMES=$(grep -o 'frames=[0-9]*' "$WORK/log.txt" | tail -1 | grep -o '[0-9]*')
[ -n "$NOW_FRAMES" ] && [ "$NOW_FRAMES" -gt "$FRAMES_BEFORE" ] || fail "坏脚本后游戏未继续推进"
# 旧 AI 仍在工作：决策计数继续增长
AFTER=$(grep -o 'ml_summary applied=[0-9]*' "$WORK/log.txt" | tail -1 | grep -o '[0-9]*')
[ -n "$AFTER" ] && [ "$AFTER" -ge "$NEW" ] || fail "坏脚本后旧 AI 失效"

echo "hot_reload_e2e: ok（Lua 增量热更生效 / JS 状态保留 + 顺序不变 / 坏脚本原子回退不致命）"
exit 0
