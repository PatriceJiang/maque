#!/usr/bin/env bash
# 内存泄漏回归：JS 根（P0 单 VM 模块/实例）反复 spawn/unload Lua 子场景（旧路径，每实例一个 Lua VM）。
# 断言：脚本/节点内存进入平台期（frame 190 → 800 不再增长），而不是随实例数线性增长；
#       800 次 churn 不触发实例上限、不出现脚本错误。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

# shellcheck source=maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "mixed_lang_mem_e2e: FAIL $*" >&2; exit 1; }

SCN_DIR="$(dirname "$SCENE")"
cp -R "$SCN_DIR"/. "$WORK"/
CHURN="$WORK/churn.scn"
[ -f "$CHURN" ] || fail "missing $CHURN"

cat > "$WORK/req.jsonl" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":10}}
{"jsonrpc":"2.0","id":2,"method":"Perf.get"}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":180}}
{"jsonrpc":"2.0","id":4,"method":"Perf.get"}
{"jsonrpc":"2.0","id":5,"method":"Runtime.step","params":{"frames":200}}
{"jsonrpc":"2.0","id":6,"method":"Perf.get"}
{"jsonrpc":"2.0","id":7,"method":"Runtime.step","params":{"frames":410}}
{"jsonrpc":"2.0","id":8,"method":"Perf.get"}
EOF

MAQUE_CONTROL=- "$PLAYER" "$CHURN" < "$WORK/req.jsonl" > "$WORK/run.log" 2>&1
RC=$?
[ "$RC" -eq 0 ] || { tail -80 "$WORK/run.log"; fail "player exit=$RC"; }

grep -q "scene instance limit reached" "$WORK/run.log" && {
    tail -80 "$WORK/run.log"; fail "churn 触发实例上限（卸载未回收槽位）";
}
grep -q "MIX_CHURN_FAIL" "$WORK/run.log" && {
    tail -80 "$WORK/run.log"; fail "churn 过程中 scene_instantiate 失败";
}
grep -q "script error" "$WORK/run.log" && {
    tail -80 "$WORK/run.log"; fail "churn 过程中出现脚本错误";
}

"$PY" - "$WORK/run.log" <<'PY' || { tail -80 "$WORK/run.log"; fail "内存断言失败"; }
import json, re, sys

resps = {}
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    line = line.strip()
    if not line.startswith('{"jsonrpc"'):
        continue
    o = json.loads(line)
    resps[o.get('id')] = o.get('result') or {}

need = (2, 4, 6, 8)
for k in need:
    assert k in resps, f'缺 Perf.get 响应 {k}: {sorted(resps)}'
    assert 'metrics' in resps[k], resps[k]

def m(k):
    return resps[k]['metrics']

for k in need:
    metrics = m(k)
    for key in ('mem.script', 'mem.node', 'mem.script.allocs', 'mem.script.frees', 'mem.tracked'):
        assert key in metrics, f'缺 {key}'

start, mid, near, end = m(2), m(4), m(6), m(8)
f0, f1, f2, f3 = (int(resps[k].get('frame', -1)) for k in (1, 3, 5, 7))
assert f0 == 10 and f1 == 190 and f2 == 390 and f3 == 800, (f0, f1, f2, f3)

# 平台期：190 → 800 帧又做了 610 次 spawn/unload，内存不应继续增长。
script_growth = float(end['mem.script']) - float(mid['mem.script'])
node_growth = float(end['mem.node']) - float(mid['mem.node'])
assert script_growth < 8192, (
    f'脚本内存仍在增长：{float(mid["mem.script"]):.0f} → {float(end["mem.script"]):.0f} B '
    f'(+{script_growth:.0f})')
assert node_growth < 8192, (
    f'节点内存仍在增长：{float(mid["mem.node"]):.0f} → {float(end["mem.node"]):.0f} B '
    f'(+{node_growth:.0f})')

lines = [l for l in open(sys.argv[1], encoding='utf-8', errors='replace') if 'MIX_CHURN frame=800' in l]
assert lines, '缺 frame=800 的 churn 汇总'
match = re.search(r'spawned=(\d+) failed=(\d+) count=(\d+)', lines[-1])
assert match, lines[-1]
spawned, failed, count = map(int, match.groups())
assert failed == 0, f'churn 失败 {failed} 次'
assert spawned >= 790, f'800 帧应完成约 799 次实例化，实际 {spawned}'
assert count <= 2, f'结束时只剩根实例/一个 pending 子实例，实际 {count}'

print(f'mixed_lang_mem_e2e: 内存平台期 ok '
      f'(script {float(mid["mem.script"]):.0f}->{float(end["mem.script"]):.0f} B, '
      f'node {float(mid["mem.node"]):.0f}->{float(end["mem.node"]):.0f} B, '
      f'spawned={spawned})')
PY

echo "mixed_lang_mem_e2e: ok（800 次 spawn/unload，脚本/节点内存进入平台期）"
exit 0
