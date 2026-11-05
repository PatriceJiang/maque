#!/usr/bin/env bash
# Linux perf 剖析端到端（docs/35）：
#   ① 构建形态：Perf.profile_status 返回 supported=true（MAQUE_PROFILE=OFF / 非 Linux → SKIP 77）。
#   ② 环境自动录制：MAQUE_PROFILE=1 + MAQUE_PROFILE_FRAMES=N → 帧边界 start/stop，
#      产物 perf.data / hotspots.json / meta.json / report.md；热点含 step_frame。
#   ③ 控制面录制（agent 路径）：profile_start → Runtime.step → profile_mark → profile_stop
#      → profile_report，全程不依赖 shell；marks.jsonl 记录帧号。
#   ④ perf 不存在/无权限：玩家**继续运行**（退出码 0），打印可操作错误，不崩。
#   ⑤ MAQUE_PROFILE=0：零录制、零产物。
#
# 用 tests/data/perf/fake_perf.py 充当 perf：本机/CI 常见 perf_event_paranoid=4，
# 真实 perf 无法采样；fake 让生命周期/解析/控制面在**任何** Linux 构建上可回归。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCN="$2"
WORK="$3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() { echo "profile_e2e: FAIL $*" >&2; exit 1; }
skip() { echo "profile_e2e: skip（$*）"; exit 77; }

[ -f "$SCN" ] || skip "缺场景 $SCN"
[ "$HAVE_PY" = "1" ] || skip "缺 Python 3（fake perf 需要）"

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_PROFILE MAQUE_PROFILE_DIR MAQUE_PROFILE_FRAMES MAQUE_PROFILE_START \
      MAQUE_PROFILE_PERF MAQUE_PROFILE_FREQ MAQUE_PROFILE_CALLGRAPH \
      MAQUE_MAX_FRAMES MAQUE_WINDOW MAQUE_PRESENT MAQUE_CONTROL 2>/dev/null || true

# 构建不支持（MAQUE_PROFILE=OFF）→ SKIP
STATUS=$(MAQUE_CONTROL='{"jsonrpc":"2.0","id":1,"method":"Perf.profile_status"}' \
         MAQUE_WINDOW=0x0 timeout 60 "$PLAYER" "$SCN" 2>/dev/null) || fail "player 退出非 0"
case "$STATUS" in
  *'"supported":true'*) ;;
  *) skip "该构建未编入 perf 剖析（MAQUE_PROFILE=OFF）：$STATUS" ;;
esac

# fake perf 包装器（避免依赖 .py 的可执行位；$PY 可能是 "py -3"）
FAKE="$WORK/fake-perf"
cat > "$FAKE" <<EOF
#!/usr/bin/env bash
exec $PY "$REPO_ROOT/tests/data/perf/fake_perf.py" "\$@"
EOF
chmod +x "$FAKE"

# ---- ② 环境自动录制 ----
AUTO_DIR="$WORK/auto"
env MAQUE_PROFILE=1 MAQUE_PROFILE_PERF="$FAKE" MAQUE_PROFILE_FRAMES=10 \
    MAQUE_PROFILE_FREQ=500 MAQUE_PROFILE_CALLGRAPH=fp MAQUE_PROFILE_DIR="$AUTO_DIR" \
    MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=30 \
    timeout 120 "$PLAYER" "$SCN" >"$WORK/auto.log" 2>&1 || { tail -20 "$WORK/auto.log"; fail "自动录制玩家退出非 0"; }

grep -q 'runtime: \[profile\] perf recording' "$WORK/auto.log" || fail "没有 perf recording 日志"
grep -q 'auto profile started' "$WORK/auto.log" || fail "没有 auto profile started"
grep -q 'runtime: \[profile\] stopped:' "$WORK/auto.log" || fail "没有 stopped 日志"
grep -q 'auto stop at frame 10' "$WORK/auto.log" || fail "没有在第 10 帧自动停止"

for f in perf.data perf-report.txt hotspots.json meta.json; do
  [ -s "$AUTO_DIR/$f" ] || fail "缺产物 $AUTO_DIR/$f"
done
grep -q '"report_ok":true' "$AUTO_DIR/hotspots.json" || fail "hotspots.json report_ok != true"
grep -q 'maque::rt::Engine::step_frame' "$AUTO_DIR/hotspots.json" || fail "热点缺 step_frame"
grep -q '"context":' "$AUTO_DIR/meta.json" || fail "meta.json 缺 context"
grep -q '"scene":' "$AUTO_DIR/meta.json" || fail "meta.json context 缺 scene"
echo "profile_e2e: ② 自动录制 OK（$(wc -c <"$AUTO_DIR/perf.data")B perf.data）"

# ---- ③ 控制面（agent）路径 ----
CTL_DIR="$WORK/control"
CTL_OUT=$(MAQUE_PROFILE_PERF="$FAKE" MAQUE_WINDOW=0x0 MAQUE_CONTROL=- \
    timeout 120 "$PLAYER" "$SCN" 2>/dev/null <<JSON
{"jsonrpc":"2.0","id":1,"method":"Perf.profile_start","params":{"freq":750,"callgraph":"fp","label":"e2e","dir":"$CTL_DIR"}}
{"jsonrpc":"2.0","id":2,"method":"Runtime.step","params":{"frames":6}}
{"jsonrpc":"2.0","id":3,"method":"Perf.profile_mark","params":{"label":"after-6-frames"}}
{"jsonrpc":"2.0","id":4,"method":"Perf.profile_stop","params":{"report":true}}
{"jsonrpc":"2.0","id":5,"method":"Perf.profile_report","params":{"top":3}}
JSON
) || fail "控制面录制玩家退出非 0"

case "$CTL_OUT" in *'"id":1,"result":{"ok":true'*) ;; *) fail "profile_start 失败：$CTL_OUT" ;; esac
case "$CTL_OUT" in *'"state":"recording"'*) ;; *) fail "profile_start 未进入 recording" ;; esac
case "$CTL_OUT" in *'"id":2,"result":{"frame":6'*) ;; *) fail "Runtime.step 结果异常" ;; esac
case "$CTL_OUT" in *'"id":3,"result":{"ok":true,"frame":6'*) ;; *) fail "profile_mark 失败" ;; esac
case "$CTL_OUT" in *'"id":4,"result":{"ok":true,"report":true'*) ;; *) fail "profile_stop 失败" ;; esac
case "$CTL_OUT" in *'"id":5,"result":{"ok":true'*) ;; *) fail "profile_report 失败" ;; esac
case "$CTL_OUT" in *'maque::rt::Engine::step_frame'*) ;; *) fail "profile_report 缺热点" ;; esac
grep -q '"label":"after-6-frames"' "$CTL_DIR/marks.jsonl" || fail "marks.jsonl 缺标记"
[ -s "$CTL_DIR/hotspots.json" ] || fail "控制面路径没有 hotspots.json"
echo "profile_e2e: ③ 控制面路径 OK"

# ---- ③b 无 perf 权限也能用的 bench/compare 回归门 ----
TOOL="$REPO_ROOT/tools/perf/maque_perf.py"
"$PY" "$TOOL" bench --out "$WORK/bench" --frames 30 --reps 1 -- "$PLAYER" "$SCN" \
    >"$WORK/bench.log" 2>&1 || { cat "$WORK/bench.log"; fail "bench 失败"; }
[ -s "$WORK/bench/bench.json" ] || fail "bench 缺 bench.json"
"$PY" "$TOOL" compare --baseline "$WORK/bench" --candidate "$WORK/bench" --fail-on-regression \
    >"$WORK/compare.log" 2>&1 || { cat "$WORK/compare.log"; fail "bench 自比较不应失败"; }
grep -q 'compare: OK' "$WORK/compare.log" || { cat "$WORK/compare.log"; fail "bench 自比较 verdict != OK"; }
echo "profile_e2e: ③b bench/compare OK"

# ---- ④ perf 不可用：可操作错误，游戏继续 ----
env MAQUE_PROFILE=1 MAQUE_PROFILE_PERF="$WORK/does-not-exist" MAQUE_PROFILE_DIR="$WORK/broken" \
    MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=5 \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/broken.log" 2>&1 || fail "perf 缺失时玩家退出码非 0"
grep -q 'runtime: \[profile\] perf 启动失败' "$WORK/broken.log" || fail "perf 缺失时没有清晰错误"
grep -q 'perf not found' "$WORK/broken.log" || fail "perf 缺失错误缺 'perf not found'"
[ -e "$WORK/broken/perf.data" ] && fail "perf 缺失却生成了 perf.data"
echo "profile_e2e: ④ perf 缺失回退 OK"

# ---- ⑤ 关闭：零日志 ----
env MAQUE_PROFILE=0 MAQUE_PROFILE_PERF="$FAKE" MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=5 \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/off.log" 2>&1 || fail "MAQUE_PROFILE=0 玩家退出非 0"
grep -q 'runtime: \[profile\]' "$WORK/off.log" && fail "MAQUE_PROFILE=0 仍输出 profile 日志"
echo "profile_e2e: ⑤ 关闭路径 OK"

# ---- ⑥ 真实 perf（有权限时）：外部 record → report 解析；无权限则跳过该节 ----
REAL_PERF="${MAQUE_PROFILE_REAL_PERF:-$(command -v perf 2>/dev/null || true)}"
REAL_OK=0
if [ -n "$REAL_PERF" ] && "$REAL_PERF" record -o "$WORK/probe.data" -e task-clock -- true >/dev/null 2>&1; then
  REAL_OK=1
fi
if [ "$REAL_OK" = "1" ]; then
  "$PY" "$REPO_ROOT/tools/perf/maque_perf.py" record --perf "$REAL_PERF"       --out "$WORK/real" --frames 30 -- "$PLAYER" "$SCN" >"$WORK/real.log" 2>&1 \
      || { cat "$WORK/real.log"; fail "真实 perf record/report 失败"; }
  "$PY" - "$WORK/real/hotspots.json" <<'PY' || fail "真实 perf 报告没有样本"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["total_samples"] > 0, d
assert d["hotspots"], d
PY
  echo "profile_e2e: ⑥ 真实 perf OK（$(wc -c <"$WORK/real/perf.data")B perf.data）"
else
  echo "profile_e2e: ⑥ 真实 perf 跳过（无 perf_event_paranoid/CAP_PERFMON；fake 已覆盖生命周期）"
fi

echo "profile_e2e: PASS"
