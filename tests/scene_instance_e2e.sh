#!/usr/bin/env bash
# 嵌套场景实例端到端（docs/32）：每个 .scn 自带行为（脚本/生命周期/事件作用域）。
#
# 判定链：
#   ① 静态嵌套：parent.scn 里两个 SceneInstance 各创建 1 个独立实例（inst=1/2），
#      子脚本 on_start 在父 on_start **之前**（后序）执行；
#   ② 每实例 VM/状态隔离：只给 inst=1 发 ping → 只有 inst=1 的 pings 增加，inst=2 仍为 0；
#   ③ 事件作用域：两个实例各在自己的 Body 上 node_emit(selfping) → 各只收到 1 次（全局广播会是 4 次）；
#      子发 from_child 冒泡到父实例的壳节点 → 父收到 2 次（跨实例边界）；
#   ④ 动态实例化：父脚本 scene_instantiate(-1,"child.scn") → 帧边界构建，inst=3 出现；
#      scene_unload 后 on_destroy 派发、句柄失效、计数回落；
#   ⑤ 控制面 Scene.instances 能看到实例树（root + 2 子）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

# shellcheck source=maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "scene_instance_e2e: FAIL $*" >&2; exit 1; }

SCN_DIR="$(dirname "$SCENE")"
cp "$SCN_DIR/parent.scn" "$SCN_DIR/child.scn" "$SCN_DIR/parent.js" "$SCN_DIR/child.js" "$WORK/"
SCENE_RUN="$WORK/parent.scn"

# ---- ①-④ 运行 12 帧，解析脚本日志 ----
timeout 60 "$PLAYER" --frames 12 "$SCENE_RUN" > "$WORK/run.log" 2>&1
RC=$?
[ "$RC" -eq 0 ] || { cat "$WORK/run.log"; fail "player exit=$RC"; }

grep -q "scene instance #1 '/child.scn' built" "$WORK/run.log" || { cat "$WORK/run.log"; fail "未实例化子场景 #1"; }
grep -q "scene instance #2 '/child.scn' built" "$WORK/run.log" || { cat "$WORK/run.log"; fail "未实例化子场景 #2"; }

"$PY" - "$WORK/run.log" <<'PY' || fail "实例行为断言失败"
import re, sys

log = [l for l in open(sys.argv[1], encoding='utf-8', errors='replace') if '[game]' in l]
def lines(tag):
    return [l for l in log if tag in l]

# ① 后序 on_start：两个子实例 START 都在父 START 之前
idx = {l: i for i, l in enumerate(log)}
starts = [i for i, l in enumerate(log) if 'CHILD_START' in l]
parent_start = next((i for i, l in enumerate(log) if 'PARENT_START' in l), None)
assert parent_start is not None, '缺 PARENT_START'
assert len(starts) >= 2, f'子实例 on_start 次数不足: {starts}'
assert all(i < parent_start for i in starts[:2]), '子 on_start 应在父 on_start 之前（后序）'
assert 'PARENT_START children=3' in log[parent_start], log[parent_start].strip()
insts = {int(m.group(1)) for l in log for m in [re.search(r'CHILD_START inst=(\d+)', l)] if m}
assert {1, 2}.issubset(insts), f'静态实例 id 不齐: {insts}'
assert 'PARENT_STATIC=1,2' in '\n'.join(log), '静态实例顺序/列表异常'

# ② 状态隔离：只有 inst=1 收到 ping
pings = [l for l in log if 'CHILD_PING' in l]
assert len(pings) == 1, f'ping 应只到 inst=1，实际: {pings}'
assert 'inst=1' in pings[0] and 'pings=1' in pings[0], pings[0].strip()
assert not any('CHILD_PING inst=2' in l for l in log), 'ping 串到 inst=2'

# ③ 同实例内自派发：两个实例各收到自己的 1 次（共 2 次；广播会 4 次）
selfs = lines('CHILD_SELF')
assert len(selfs) == 2, f'selfping 应各 1 次共 2 次，实际 {len(selfs)}: {selfs}'
assert {int(m.group(1)) for l in selfs for m in [re.search(r'inst=(\d+)', l)] if m} == {1, 2}

# 跨实例冒泡：父收到 2 次（Left/Right 各一次）
cross = lines('PARENT_CHILD_EVENT')
assert len(cross) == 2, f'父应收到 2 次子事件，实际 {len(cross)}: {cross}'
targets = {int(m.group(1)) for l in cross for m in [re.search(r'target=(\d+)', l)] if m}
assert len(targets) == 2, f'父收到的 target 应来自两个壳，实际 {targets}'

# ④ 动态实例化/卸载
assert any('PARENT_SPAWN dyn=3' in l for l in log), 'scene_instantiate 未返回 3'
assert any('CHILD_START inst=3' in l for l in log), '动态实例未 on_start'
dest = lines('CHILD_DESTROY')
assert any('inst=1' in l for l in dest), f'静态实例 1 未 on_destroy: {dest}'
assert any('inst=3' in l for l in dest), f'动态实例 3 未 on_destroy: {dest}'
assert any('CHILD_REPORT inst=2 pings=0' in l for l in log), 'inst=2 的独立状态（pings=0）不符'
assert any('PARENT_FINAL children=2' in l for l in log), '卸载后 active 实例数应为 2（root+inst2）'
print('scene_instance_e2e: 日志断言 ok')
PY

# ---- ⑤ 控制面：装配完成后能看到实例树 ----
cat > "$WORK/req.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.instances"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE_RUN" < "$WORK/req.txt" > "$WORK/ctl.log" 2>&1
"$PY" - "$WORK/ctl.log" <<'PY' || { cat "$WORK/ctl.log"; fail "Scene.instances 断言失败"; }
import json, sys
raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
line = next((l for l in raw.splitlines() if l.lstrip().startswith('{"jsonrpc"')), None)
assert line, raw[-2000:]
resp = json.loads(line)
assert 'result' in resp, resp
res = resp['result']
assert res.get('ok') is True, res
assert res.get('count') == 3, res
ids = [i['id'] for i in res['instances']]
assert ids == [0, 1, 2], f'实例树应为 [0,1,2]，实际 {ids}'
child_src = [i for i in res['instances'] if i['source'] == '/child.scn']
assert len(child_src) == 2 and all(i['state'] == 1 for i in child_src), res
assert all(i['parent'] == 0 for i in child_src), res
print('scene_instance_e2e: 控制面 ok')
PY

echo "scene_instance_e2e: ok"
exit 0
