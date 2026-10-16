#!/usr/bin/env bash
# Lua 脚本后端端到端（docs/14「统一后端 ABI」）：
#   ① 语言选择：场景脚本为 .lua → 运行时选 lua 后端（并打印所选后端）
#   ② 功能对等：Lua 脚本可调用同一套宿主 API（log/entity_*/spawn_ball/math.*）、
#      可接收事件回调（on_pointer_down），行为与 js 版一致
#   ③ **跨语言一致性**：同一份语义的 js 版与 lua 版场景，在相同模拟输入下
#      逐帧演化完全一致（帧数 + world_trail 相同）—— 这是后端抽象最强的正确性证据
#   ④ 确定性：lua 场景两跑 world_trail 一致
set -u

PLAYER="$1"
WORK="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/tests/data/sim/ball_clicks.sim.json"

rm -rf "$WORK"; mkdir -p "$WORK"
# ⚠️ 必须保留真窗口：`MAQUE_WINDOW=800x600` 提供**窗口→世界坐标映射**，
# 本测试的点击坐标断言（`at (40,180)` / `at (240,60)`）正是验证这条映射
# （实测：改成无头后坐标映射不符 → 5/5 稳定失败）。
unset MAQUE_HEADLESS

fail() { echo "lua_e2e: FAIL $*" >&2; exit 1; }
run() {   # run <log> <scene> [env...]
  local log="$1" scene="$2"; shift 2
  env MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine MAQUE_VIEWS=1 MAQUE_KEEP_OPEN=1 \
      "$@" "$PLAYER" "$scene" > "$log" 2>&1 || true
}
trail_of() { grep -o 'world_trail=0x[0-9a-f]*' "$1" | head -1 | cut -d= -f2; }
frames_of() { grep -o 'exited after [0-9]* frames' "$1" | head -1 | grep -o '[0-9]*'; }

# ---- ① + ② Lua 后端被选中，且脚本真的在跑（点击生成球）----
run "$WORK/lua.log" "$ROOT/scenes/lua/lua.scn" MAQUE_SIM="$SIM" MAQUE_MAX_FRAMES=200
grep -q "script backend 'lua'" "$WORK/lua.log" || fail "未选中 lua 后端"
grep -q 'ball: click #1 spawn.*at (40,180)' "$WORK/lua.log" || fail "Lua 脚本未收到点击（坐标映射不符）"
grep -q 'ball: click #2 spawn.*at (240,60)' "$WORK/lua.log" || fail "Lua 脚本第二次点击坐标不符"
grep -q 'balls=3' "$WORK/lua.log" || fail "Lua 侧球数应为 3"
# 宿主 API 命名空间（math.*）与 Lua 标准库共存
grep -q 'math.clamp(5,0,3)=3.0' "$WORK/lua.log" || fail "宿主 math.clamp 不可用或覆盖了标准库"

# ---- ③ 跨语言一致性：js 版 vs lua 版（同场景、同输入）----
run "$WORK/js.log" "$ROOT/scenes/ball.scn" MAQUE_SIM="$SIM" MAQUE_MAX_FRAMES=200
grep -q "script backend 'js'" "$WORK/js.log" || fail "js 场景未选中 js 后端"
TJS=$(trail_of "$WORK/js.log"); TLUA=$(trail_of "$WORK/lua.log")
FJS=$(frames_of "$WORK/js.log"); FLUA=$(frames_of "$WORK/lua.log")
[ -n "$TJS" ] && [ "$TJS" = "$TLUA" ] || fail "跨语言世界演化不一致：js=$TJS lua=$TLUA"
[ -n "$FJS" ] && [ "$FJS" = "$FLUA" ] || fail "跨语言帧数不一致：js=$FJS lua=$FLUA"

# ---- ④ 确定性：lua 两跑一致 ----
run "$WORK/lua2.log" "$ROOT/scenes/lua/lua.scn" MAQUE_SIM="$SIM" MAQUE_MAX_FRAMES=200
T2=$(trail_of "$WORK/lua2.log")
[ -n "$T2" ] && [ "$T2" = "$TLUA" ] || fail "lua 两跑不一致：$T2 != $TLUA"

# ---- ⑤ 脚本错误不致命：坏脚本给出结构化错误且进程正常退出 ----
mkdir -p "$WORK/scene"
sed 's/"script": "ball.lua"/"script": "bad.lua"/' "$ROOT/scenes/lua/lua.scn" > "$WORK/scene/bad.scn"
printf 'function on_start() error("intentional") end\n' > "$WORK/scene/bad.lua"
run "$WORK/bad.log" "$WORK/scene/bad.scn" MAQUE_MAX_FRAMES=20
grep -q 'script:lua' "$WORK/bad.log" || fail "Lua 运行错误未被捕获上报"
grep -q 'intentional' "$WORK/bad.log" || fail "错误信息未包含脚本原因"

echo "lua_e2e: ok（lua 后端 / 宿主 API / 事件回调 / 跨语言一致 trail=$TLUA frames=$FLUA / 确定性 / 错误捕获）"
exit 0
