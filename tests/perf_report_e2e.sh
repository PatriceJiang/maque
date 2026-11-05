#!/usr/bin/env bash
# perf 报告解析/对比离线端到端（docs/35 §6/§7）：
#   ① `parse --report/--script`：百分比、样本数、DSO、C++ 模板符号、源码行、模块归类；
#   ② 无 script 文本时退回 report 百分比（旧 perf / 无源码行）；
#   ③ report.md 生成表格与模块汇总；
#   ④ `compare` 自比较 = 0；人为制造热点回退 → --fail-on-regression 退出 1。
# 不依赖真实 perf / 权限：只读 tests/data/perf 的固定文本。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

TOOL="$1"
FIXTURES="$2"
WORK="$3"

fail() { echo "perf_report_e2e: FAIL $*" >&2; exit 1; }
skip() { echo "perf_report_e2e: skip（$*）"; exit 77; }

[ "$HAVE_PY" = "1" ] || skip "缺 Python 3"
[ -f "$TOOL" ] || fail "缺工具 $TOOL"
[ -f "$FIXTURES/sample-report.txt" ] || fail "缺 fixture sample-report.txt"
[ -f "$FIXTURES/sample-script.txt" ] || fail "缺 fixture sample-script.txt"

rm -rf "$WORK"; mkdir -p "$WORK"

run_py() { "$PY" "$@"; }

# ---- ① 带 script：源码行 + 模块归类 ----
run_py "$TOOL" parse --report "$FIXTURES/sample-report.txt" \
    --script "$FIXTURES/sample-script.txt" --out "$WORK/full" --top 10 >"$WORK/full.log" 2>&1 \
    || { cat "$WORK/full.log"; fail "parse（带 script）失败"; }
[ -s "$WORK/full/hotspots.json" ] || fail "缺 hotspots.json"
[ -s "$WORK/full/report.md" ] || fail "缺 report.md"
grep -q '## Hotspots' "$WORK/full/report.md" || fail "report.md 缺 Hotspots 段"

run_py - "$WORK/full/hotspots.json" <<'PY' || fail "带 script 的解析结果不符"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["total_samples"] == 20, d["total_samples"]
assert d["source_samples"] == 19, d["source_samples"]
top = d["hotspots"][0]
assert "Engine::step_frame" in top["symbol"], top
assert abs(top["percent"] - 40.0) < 1e-6, top
assert top["module"] == "modules/runtime", top
assert top["source"].endswith("modules/runtime/src/engine/frame.cpp:42"), top
mods = {m["module"]: m for m in d["modules"]}
assert mods["modules/runtime"]["samples"] == 12, mods["modules/runtime"]
assert mods["modules/render/gfx"]["samples"] == 1, mods
assert mods["third_party/stl"]["samples"] == 2, mods
assert any("0x0000000000123456" in h["symbol"] for h in d["hotspots"]), d
print("full ok")
PY

# ---- ② 只有 report：退回百分比/样本列（无源码行）----
run_py "$TOOL" parse --report "$FIXTURES/sample-report.txt" --out "$WORK/report-only" \
    --top 10 >"$WORK/report-only.log" 2>&1 || { cat "$WORK/report-only.log"; fail "parse（仅 report）失败"; }
run_py - "$WORK/report-only/hotspots.json" <<'PY' || fail "仅 report 的解析结果不符"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["total_samples"] == 500, d["total_samples"]
top = d["hotspots"][0]
assert "Engine::step_frame" in top["symbol"], top
assert abs(top["percent"] - 40.0) < 1e-6, top
assert top["samples"] == 200, top
assert top["module"] == "modules/runtime", top
assert top["source"] == "", top
assert any("source lines unavailable" in w for w in d["warnings"]), d["warnings"]
print("report-only ok")
PY

# ---- ③ compare：自比较 0；制造回退 → 1 ----
run_py "$TOOL" compare --baseline "$WORK/full" --candidate "$WORK/full" \
    --threshold 3 >"$WORK/self.log" 2>&1 || { cat "$WORK/self.log"; fail "自比较不应失败"; }
grep -q 'compare: OK' "$WORK/self.log" || { cat "$WORK/self.log"; fail "自比较 verdict != OK"; }

cp -r "$WORK/full" "$WORK/cand"
run_py - "$WORK/cand/hotspots.json" <<'PY' || fail "构造候选失败"
import json, sys
p = sys.argv[1]
d = json.load(open(p))
for h in d["hotspots"]:
    if "Engine::step_frame" in h["symbol"]:
        h["percent"] = 55.0          # 40% → 55%，明显回退
        break
json.dump(d, open(p, "w"), indent=2)
PY

set +e
run_py "$TOOL" compare --baseline "$WORK/full" --candidate "$WORK/cand" \
    --threshold 3 --fail-on-regression >"$WORK/regress.log" 2>&1
RC=$?
set -e
[ "$RC" -eq 1 ] || { cat "$WORK/regress.log"; fail "回退没有被 --fail-on-regression 捕获（rc=$RC）"; }
grep -q 'REGRESSION' "$WORK/regress.log" || { cat "$WORK/regress.log"; fail "compare 没报 REGRESSION"; }

echo "perf_report_e2e: PASS"
