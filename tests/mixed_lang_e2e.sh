#!/usr/bin/env bash
# 混合语言通信端到端（docs/34 P0 新路径 + docs/32 场景实例）：
#   · JS 根场景（包内模块/实例，this 上下文）→ Lua 子实例（旧路径）→ JS 根；
#   · 通信只走 scene_emit(instance, event, value)（事件名 = 处理器名，标量载荷）；
#   · 两个 Lua 子实例各自闭包状态独立；同 seed 两跑 world_trail 与日志序列一致。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

# shellcheck source=maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "mixed_lang_e2e: FAIL $*" >&2; exit 1; }

SCN_DIR="$(dirname "$SCENE")"
cp -R "$SCN_DIR"/. "$WORK"/
RUN="$WORK/mixed.scn"
[ -f "$RUN" ] || fail "missing $RUN"

run_once() {
    local out="$1"
    timeout 60 "$PLAYER" --frames 20 "$RUN" > "$out" 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || { cat "$out"; fail "player exit=$rc"; }
}

run_once "$WORK/run1.log"
run_once "$WORK/run2.log"

"$PY" - "$WORK/run1.log" <<'PY' || fail "通信日志断言失败"
import re, sys

raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
log = [l for l in raw.splitlines() if '[game]' in l]

def one(tag):
    return [l for l in log if tag in l]

host = one('MIX_HOST_START')
assert len(host) == 1, host
assert 'node=-1' in host[0], host[0]
assert 'package=mixed@1.0.0' in host[0], host[0]
assert 'evals=1' in host[0], 'JS 模块在同一 VM 内应只求值一次: ' + host[0]

workers = one('MIX_WORKER_START')
assert len(workers) == 2, workers
assert any('tag=A' in l for l in workers), workers
assert any('tag=B' in l for l in workers), workers

assert one('MIX_HOST_WORKERS=1,2'), one('MIX_HOST_WORKERS')

asks = one('MIX_WORKER_ASK')
assert len(asks) == 2, asks
assert any('tag=A' in l and 'value=100' in l and 'count=1' in l for l in asks), asks
assert any('tag=B' in l and 'value=101' in l and 'count=1' in l for l in asks), asks

replies = one('MIX_HOST_REPLY')
assert len(replies) == 2, replies
values = sorted(int(re.search(r'value=(\d+)', l).group(1)) for l in replies)
assert values == [101, 102], values

summary = one('MIX_HOST_SUMMARY')
assert len(summary) == 1, summary
assert 'asks=2' in summary[0] and 'replies=2' in summary[0], summary[0]
assert 'script error' not in raw, '出现脚本错误'
assert 'module instantiate failed' not in raw, '模块实例化失败'
print('mixed_lang_e2e: 通信断言 ok')
PY

T1="$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/run1.log" | tail -1)"
T2="$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/run2.log" | tail -1)"
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "两跑 world_trail 不一致：'$T1' != '$T2'"

grep '\[game\]' "$WORK/run1.log" > "$WORK/seq1.txt"
grep '\[game\]' "$WORK/run2.log" > "$WORK/seq2.txt"
cmp -s "$WORK/seq1.txt" "$WORK/seq2.txt" || {
    diff -u "$WORK/seq1.txt" "$WORK/seq2.txt" | head -40
    fail "两跑日志序列不一致（确定性被破坏）"
}

echo "mixed_lang_e2e: ok（JS 根 ↔ Lua 子实例双向 scene_emit / 实例状态隔离 / 确定性）"
exit 0
