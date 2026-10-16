#!/usr/bin/env bash
# 多语言脚本协作端到端（docs/14 §多 VM、docs/19 §事件）：
#   ① 一个场景装载两个语言的脚本（scripts: ["game.js","ai.lua"]），两个后端都就绪
#   ② 投递顺序 = 声明顺序（JS 的 on_start 先于 Lua）
#   ③ **跨语言事件**：JS emit → Lua 处理器；Lua emit → JS 处理器（只靠处理器名路由，无额外协议）
#   ④ 确定性：同输入两跑 world_trail 与日志序列完全一致（多 VM 不破坏可回放）
#   ⑤ 宿主 → 脚本：控制面 Scene.emit 触发脚本处理器（与脚本内 emit 同一条路径）
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）
PLAYER="$1"; INSPECTOR="$2"; SCENE="$3"; WORK="$4"
rm -rf "$WORK"; mkdir -p "$WORK"
fail() { echo "multilang_e2e: FAIL $*" >&2; exit 1; }

run_demo() {   # run_demo <log> [额外 env...]
  local log="$1"; shift
  env "$@" MAQUE_HEADLESS=1 MAQUE_MAX_FRAMES=160 "$PLAYER" "$SCENE" > "$log" 2>&1 || true
}

# ---- ①②③ 跨语言装载与双向事件 ----
run_demo "$WORK/a.log"
A="$WORK/a.log"
grep -q 'scripts=2' "$A" || fail "场景未声明两个脚本（scripts=2）"
grep -q "script backend 'js' for 'game.js'" "$A" || fail "JS 后端未装载"
grep -q "script backend 'lua' for 'ai.lua'" "$A" || fail "Lua 后端未装载"

# 声明顺序：JS 的 on_start 先于 Lua 的
JS_LINE=$(grep -n 'ml_js_ready' "$A" | head -1 | cut -d: -f1)
LUA_LINE=$(grep -n 'ml_lua_ready' "$A" | head -1 | cut -d: -f1)
[ -n "$JS_LINE" ] && [ -n "$LUA_LINE" ] || fail "两语言 on_start 未都执行"
[ "$JS_LINE" -lt "$LUA_LINE" ] || fail "投递顺序与声明顺序不一致（JS=$JS_LINE Lua=$LUA_LINE）"

# 跨语言双向：JS→Lua（ml_lua_init 由 JS 的 ai_init 触发）、Lua→JS（ml_js_got_ai_ready 由 Lua 的 ai_ready 触发）
grep -q 'ml_lua_init count=6' "$A" || fail "JS → Lua 事件未送达"
grep -q 'ml_js_got_ai_ready count=6' "$A" || fail "Lua → JS 事件未送达"
grep -qE 'ml_summary applied=[1-9]' "$A" || fail "跨语言往返未产生决策（applied=0）"

# ---- ④ 确定性：两跑一致（多 VM 不得破坏可回放）----
run_demo "$WORK/b.log"
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$A" | head -1)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/b.log" | head -1)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "两跑 world_trail 不一致（$T1 != $T2）"
L1=$(grep -oE 'ml_[a-z_]+[^"]*' "$A" | tr '\n' '|')
L2=$(grep -oE 'ml_[a-z_]+[^"]*' "$WORK/b.log" | tr '\n' '|')
[ "$L1" = "$L2" ] || fail "两跑跨语言事件序列不一致"

# ---- ⑤ 宿主 → 脚本：控制面 Scene.emit ----
PORT=$(( (RANDOM % 2000) + 23000 ))
# Scene.emit 是**变更类**方法（control_mutating）→ 只读会话会被拒（-32001，权限模型生效）；
# 这里用 MAQUE_INSPECT_RW=1 开启编辑权限来验证宿主 → 脚本通路。
MAQUE_HEADLESS=1 MAQUE_INSPECT="$PORT" MAQUE_INSPECT_RW=1 MAQUE_MAX_FRAMES=100000 "$PLAYER" "$SCENE" > "$WORK/game.log" 2>&1 &
GAME_PID=$!
for _ in $(seq 1 60); do grep -q 'control endpoint' "$WORK/game.log" && break; sleep 0.1; done
grep -q 'control endpoint' "$WORK/game.log" || { kill "$GAME_PID" 2>/dev/null; fail "游戏未启动端点"; }

# 找到 Status 实体（事件 target）；由 inspector 的 headless 模式查询
ENTS=$("$INSPECTOR" --connect "$PORT" --headless Scene.entities 2>/dev/null | tail -1)
SID=$(printf '%s' "$ENTS" | "$PY" -c "
import json,sys
d=json.loads(sys.stdin.read())['result']
for e in d:
    if e.get('name')=='Status': print(e['id']); break
")
[ -n "$SID" ] || { kill "$GAME_PID" 2>/dev/null; fail "未找到 Status 实体"; }
# 行为断言（不依赖脚本打日志的频率）：宿主 emit 一个**特定值** →
# Lua 处理器算出 y = 90 + (value*7) % 60 → 发 ai_target → JS 把某个实体移到该 y。
VALUE=9999
EXPECT_Y=$(( 90 + (VALUE * 7) % 60 ))
EMIT=$("$INSPECTOR" --connect "$PORT" --headless "Scene.emit|{\"id\":$SID,\"event\":\"ai_tick\",\"value\":$VALUE}" 2>/dev/null | tail -1)
printf '%s' "$EMIT" | grep -q '"handlers":[1-9]' || { kill "$GAME_PID" 2>/dev/null; fail "Scene.emit 未命中脚本处理器（$EMIT）"; }

# 等若干帧让"Lua 决策 → JS 应用"落地，再查实体 y 是否出现期望值（宿主 → Lua → JS → 世界状态 全链路）
OK=0
for _ in $(seq 1 40); do
  ENTS2=$("$INSPECTOR" --connect "$PORT" --headless Scene.entities 2>/dev/null | tail -1)
  if printf '%s' "$ENTS2" | "$PY" -c "
import json,sys
d=json.loads(sys.stdin.read())['result']
want=float('$EXPECT_Y')
sys.exit(0 if any(abs(e.get('y',0)-want) < 0.5 for e in d) else 1)"; then OK=1; break; fi
  sleep 0.1
done
[ "$OK" = 1 ] || { kill "$GAME_PID" 2>/dev/null; fail "宿主事件未产生世界状态变化（期望某实体 y=$EXPECT_Y）"; }

kill "$GAME_PID" 2>/dev/null
for _ in $(seq 1 30); do kill -0 "$GAME_PID" 2>/dev/null || break; sleep 0.1; done
kill -9 "$GAME_PID" 2>/dev/null

echo "multilang_e2e: ok（双语言装载 / 声明顺序投递 / 双向跨语言事件 / 确定性 / 宿主 emit；trail=$T1）"
exit 0
