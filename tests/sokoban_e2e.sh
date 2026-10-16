#!/usr/bin/env bash
# 推箱子 demo 端到端：键鼠模拟驱动 → 规则/撤销/关卡/确定性。
#   ① 推箱入目标 → 过关（步数/推动次数精确）
#   ② 撞墙被阻挡（状态不变）
#   ③ 推动被阻挡（箱子后面还有箱子）
#   ④ 撤销（Z）回到上一步状态
#   ⑤ 重开（R）与下一关（N）
#   ⑥ 确定性：同一输入序列两跑 world_trail 与状态摘要完全一致
# 无需 DISPLAY：按键事件经输入模拟器注入（与设备事件同一路径）。
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "sokoban_e2e: FAIL $*" >&2; exit 1; }

# 按键码（归一化键码，见 abi/platform.h）：DOWN=9 UP=8 LEFT=6 A=65 W=87 Z=90 N=78 R=82
# 序列（第 1 关 → 第 2 关）：
#   DOWN,DOWN  第一关向下走一格、再撞下墙（②）
#   UP,LEFT    回位后向左推箱入目标 → 过关（① steps=3 pushes=1）
#   N          进入第二关（⑤）
#   A          向左推箱，被后面的箱子挡住（③）
#   W          向上走一格（有效移动）
#   Z          撤销（④ → steps 回到 0）
#   R          重开第二关（⑤）
cat > "$WORK/keys.sim.json" <<'EOF'
{ "name": "sokoban_rules", "events": [
  { "frame": 5,  "type": "key_down", "key": 9  },
  { "frame": 8,  "type": "key_down", "key": 9  },
  { "frame": 12, "type": "key_down", "key": 8  },
  { "frame": 16, "type": "key_down", "key": 6  },
  { "frame": 20, "type": "key_down", "key": 78 },
  { "frame": 24, "type": "key_down", "key": 6  },
  { "frame": 28, "type": "key_down", "key": 87 },
  { "frame": 32, "type": "key_down", "key": 90 },
  { "frame": 36, "type": "key_down", "key": 82 }
] }
EOF

run() {  # run <log> [extra env...]
  local log="$1"; shift
  env "$@" MAQUE_SIM="$WORK/keys.sim.json" MAQUE_MAX_FRAMES=70 "$PLAYER" "$SCENE" > "$log" 2>&1 || true
}

run "$WORK/a.log"
A="$WORK/a.log"

# ---- ① 推箱入目标过关（DOWN,DOWN 撞墙，UP 回位，LEFT 推箱入目标）----
grep -q 'sokoban_done level=0 steps=3 pushes=1' "$A" \
  || fail "未按预期过关（期望 steps=3 pushes=1）"
# ---- ② 撞墙被阻挡 ----
grep -q 'sokoban_block wall' "$A" || fail "撞墙未被阻挡"
# ---- ⑤ 下一关 / ③ 推动被阻挡（第二关两箱相邻）----
grep -q 'sokoban_level .*index=1 boxes=2' "$A" || fail "N 未进入第二关（2 个箱子）"
grep -q 'sokoban_block box' "$A" || fail "推动被另一箱子阻挡未生效"
# ---- ④ 撤销 ----
grep -q 'sokoban_undo steps=0' "$A" || fail "撤销未回到上一步（期望 steps=0）"
# ---- ⑤ 重开 ----
grep -q 'sokoban_restart' "$A" || fail "重开未生效"
# 重开后状态复位（第 60 帧摘要：第二关、步数 0、两箱都在原位）
STATE=$(grep -o 'sokoban_state {.*}' "$A" | tail -1 | sed 's/^sokoban_state //')
"$PY" - "$STATE" <<'PY' || exit 1
import json, sys
s = json.loads(sys.argv[1])
bad = []
if s["level"] != 1: bad.append(f'level={s["level"]}（期望 1）')
if s["steps"] != 0 or s["pushes"] != 0: bad.append(f'steps/pushes={s["steps"]}/{s["pushes"]}（重开后应为 0）')
if len(s["boxes"]) != 2: bad.append(f'boxes={len(s["boxes"])}（期望 2）')
if s["done"]: bad.append('done=true（重开后不应过关）')
if bad:
    print("sokoban_e2e: FAIL " + "; ".join(bad), file=sys.stderr); sys.exit(1)
print(f'  sokoban 状态摘要：{s}')
PY

# ---- ⑥ 确定性：两跑 trail 与状态摘要一致 ----
run "$WORK/b.log"
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$A" | head -1)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/b.log" | head -1)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "两跑 world_trail 不一致：$T1 != $T2"
S1=$(grep -o 'sokoban_state {.*}' "$A" | tail -1)
S2=$(grep -o 'sokoban_state {.*}' "$WORK/b.log" | tail -1)
[ -n "$S1" ] && [ "$S1" = "$S2" ] || fail "两跑状态摘要不一致"

# ---- ⑦ 第三关可加载（关卡数据完整）----
cat > "$WORK/l3.sim.json" <<'EOF'
{ "name": "to_level3", "events": [
  { "frame": 5, "type": "key_down", "key": 78 },
  { "frame": 9, "type": "key_down", "key": 78 } ] }
EOF
MAQUE_SIM="$WORK/l3.sim.json" MAQUE_MAX_FRAMES=20 "$PLAYER" "$SCENE" > "$WORK/c.log" 2>&1 || true
grep -q 'sokoban_level .*index=2 boxes=2' "$WORK/c.log" || fail "第三关未正确加载"

echo "sokoban_e2e: ok（规则/阻挡/撤销/关卡/确定性 全部通过；trail=$T1）"
exit 0
