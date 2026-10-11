#!/usr/bin/env bash
# 输入模拟器端到端（docs/19 / docs/10 D3）：场景文件 → 确定性输入流；
#   ① MAQUE_SIM=<file.sim.json> 回放场景：指针/滚轮/键/文本 → 脚本回调；坐标空间映射正确
#   ② 确定性：两跑 world_trail 一致
#   ③ **录制/回放等价**：带模拟输入的会话录制后，回放（不带模拟器）得到同一 world_trail
#      —— 证明"输入即数据"：模拟输入与设备输入走同一路径、同样被记录
#   ④ 控制面：Input.sim_load / sim_status / sim_events / sim_stop（AI/工具驱动）
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "sim_input_e2e: skip（无显示环境：设备输入帧只在有窗口时消费）"
  exit 77                                   # CTest SKIP_RETURN_CODE
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/tests/data/sim/ball_clicks.sim.json"
rm -rf "$WORK"; mkdir -p "$WORK"

fail() { echo "sim_input_e2e: FAIL $*" >&2; exit 1; }
run() {   # run <log> [env...] -- 固定窗口与世界尺寸
  local log="$1"; shift
  env MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine MAQUE_VIEWS=1 MAQUE_KEEP_OPEN=1 \
      "$@" "$PLAYER" "$SCENE" > "$log" 2>&1 || true
}

# ---- ① 场景文件回放 ----
run "$WORK/a.log" MAQUE_SIM="$SIM" MAQUE_MAX_FRAMES=120
grep -q "sim '$SIM' loaded" "$WORK/a.log" || fail "场景未载入"
grep -q 'ball: click #1 spawn.*at (40,60)'  "$WORK/a.log" || fail "第 1 次点击坐标映射不符"
grep -q 'ball: click #2 spawn.*at (240,180)' "$WORK/a.log" || fail "第 2 次点击坐标映射不符"
grep -q 'balls=3' "$WORK/a.log" || fail "球数应为 3"

# ---- ② 确定性：两跑一致 ----
run "$WORK/b.log" MAQUE_SIM="$SIM" MAQUE_MAX_FRAMES=120
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/a.log" | head -1)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/b.log" | head -1)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "两跑不一致：$T1 != $T2"

# ---- ③ 录制/回放等价（模拟输入 → 记录 → 回放）----
run "$WORK/rec.log" MAQUE_SIM="$SIM" MAQUE_RECORD="$WORK/input.json" MAQUE_MAX_FRAMES=120
[ -s "$WORK/input.json" ] || fail "未产生录制日志"
grep -q 'recorded input log' "$WORK/rec.log" || fail "未记录输入日志"
run "$WORK/rep.log" MAQUE_REPLAY="$WORK/input.json" MAQUE_MAX_FRAMES=120
grep -q 'replay' "$WORK/rep.log" || fail "未进入回放模式"
T3=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/rep.log" | head -1)
[ -n "$T3" ] && [ "$T3" = "$T1" ] || fail "回放轨迹与录制不一致：$T3 != $T1"

# ---- ④ 控制面：Input.sim_*（无窗口亦可：注入走 pending_inject）----
cat > "$WORK/req.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Input.sim_load","params":{"json":"{\"space\":\"world\",\"events\":[{\"frame\":3,\"type\":\"key_down\",\"key\":65},{\"frame\":5,\"type\":\"click\",\"x\":120,\"y\":90}]}"}}
{"jsonrpc":"2.0","id":2,"method":"Input.sim_status"}
{"jsonrpc":"2.0","id":3,"method":"Input.sim_events"}
{"jsonrpc":"2.0","id":4,"method":"Runtime.step","params":{"frames":8}}
{"jsonrpc":"2.0","id":5,"method":"Input.sim_status"}
{"jsonrpc":"2.0","id":6,"method":"Input.sim_stop"}
{"jsonrpc":"2.0","id":7,"method":"Input.sim_status"}
EOF
MAQUE_CONTROL=- MAQUE_HEADLESS=1 MAQUE_MAX_FRAMES=40 \
  "$PLAYER" "$SCENE" < "$WORK/req.txt" > "$WORK/ctl.log" 2>&1 || true
grep -q '"events":3' "$WORK/ctl.log" || fail "sim_load 事件数应为 3（key_down + click 展开 2）"
grep -q '"cursor":0' "$WORK/ctl.log" || fail "sim_status 初始游标应为 0"
grep -q '"finished":true' "$WORK/ctl.log" || fail "步进后应 finished"
grep -q '"was_active":true' "$WORK/ctl.log" || fail "sim_stop 应报告此前处于活动状态"
grep -q '"active":false' "$WORK/ctl.log" || fail "sim_stop 后应停止"
grep -q '"kind":4' "$WORK/ctl.log" || fail "sim_events 应包含 key_down(kind=4)"

echo "sim_input_e2e: ok（场景回放/确定性/录制回放等价/控制面 全部通过；trail=$T1）"
exit 0
