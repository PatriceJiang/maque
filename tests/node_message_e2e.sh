#!/usr/bin/env bash
# 跨脚本消息（不同节点 + 嵌套子场景）端到端：
#   场景 scenes/messages/message_nodes.scn 有 3 个根场景脚本（3 个 VM），各自“负责”不同节点：
#     orchestrator.js → Root（汇总）· alpha.js → Alpha/AlphaSensor · beta.js → Beta
#   另有 Responder 节点通过 scn.SceneInstance 加载子场景 scenes/messages/responder.scn，
#   子场景带自己的脚本 responder.js（独立 VM/生命周期）。
#
# 判定链（所有消息只走节点事件 / scene_emit，不共享脚本变量）：
#   ① 同一个事件名 ping 发给 Alpha / Beta 两个不同节点 → 各自脚本的处理器各命中 1 次；
#      把 Beta 的事件名发给 Alpha（Alpha 没注册）→ 0 命中（目标节点决定路由）。
#   ② Alpha 的处理器把消息转给 Beta 的节点（beta_forward）→ beta.js 处理 → 回到 Root 的
#      orchestrator.js；同时 Alpha 也直接回 Root（跨脚本 A→B→hub 与 A→hub 两条链）。
#   ③ 发给子节点 AlphaSensor 的事件冒泡到父节点 Alpha → alpha.js 的处理器命中，
#      target = 处理器注册节点 Alpha（不是最初 emit 的 AlphaSensor），载荷保持不变。
#   ④ 父 → 子场景：scene_emit(sub_instance, 'sub_ping', 40) → responder.js；
#      子 → 父两条：scene_emit(0, 'sub_pong', 41)（显式跨实例）与
#      node_emit_number(scene_self_root(), 'sub_node', 42)（从挂载壳冒泡到父场景处理器）。
#   ④b 全局消息总线：同一 (bus, subject, event) 同时命中根脚本函数回调与子场景脚本；
#      子场景 msg_publish('from_child') → 根脚本字符串 handler；msg_unsubscribe 后根回调消失、
#      子场景 listener 仍在（hits 2 → 1）。
#   ⑤ node_off(Alpha, 'ping') 只摘掉 Alpha 节点上的注册：Alpha 再发 0 命中，Beta 仍 1 命中。
#   ⑥ 根 VM 的 on_start 顺序 = scripts 声明顺序；子场景 on_start 在父 on_start 之前；
#      没有脚本错误；3 个根脚本的总线 = 3 VM，子场景另算 1 个 VM。
#   ⑦ 确定性：同 seed 两跑 [game] 日志序列与 world_trail 完全一致。
set -u
. "$(dirname "$0")/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "node_message_e2e: FAIL $*" >&2; exit 1; }

[ -f "$SCENE" ] || fail "missing scene: $SCENE"

run_once() {   # run_once <log>
    local out="$1"
    timeout 60 "$PLAYER" --frames 8 "$SCENE" > "$out" 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || { tail -60 "$out"; fail "player exit=$rc"; }
}

run_once "$WORK/run1.log"
run_once "$WORK/run2.log"

grep -q 'scripts=3' "$WORK/run1.log" || fail "根场景未装载 3 个脚本"
grep -q 'multi-script bus with 3 VM' "$WORK/run1.log" || fail "根场景未创建 3 个 VM 的总线"
grep -q 'script backend .js. for .orchestrator.js.' "$WORK/run1.log" || fail "orchestrator.js 未装载"
grep -q 'script backend .js. for .alpha.js.' "$WORK/run1.log" || fail "alpha.js 未装载"
grep -q 'script backend .js. for .beta.js.' "$WORK/run1.log" || fail "beta.js 未装载"
grep -q "scene instance #1 '/responder.scn' built" "$WORK/run1.log" || fail "子场景未实例化"
grep -q "script backend 'js' for 'responder.js' (instance 1)" "$WORK/run1.log" || fail "子场景脚本未装载"
grep -q 'script error' "$WORK/run1.log" && { cat "$WORK/run1.log"; fail "出现脚本错误"; }
grep -q 'node_on rejected' "$WORK/run1.log" && { cat "$WORK/run1.log"; fail "node_on 被拒绝"; }

"$PY" - "$WORK/run1.log" <<'PY' || fail "消息日志断言失败"
import re, sys

raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
log = [l for l in raw.splitlines() if '[game]' in l]

def lines(tag):
    return [l for l in log if tag in l]

def one(tag):
    got = lines(tag)
    assert len(got) == 1, f'{tag} 应恰好 1 条，实际 {len(got)}: {got}'
    return got[0]

def num(line, key):
    m = re.search(rf'\b{key}=(-?\d+(?:\.\d+)?)', line)
    assert m, f'{key} 不在行内: {line}'
    return float(m.group(1))

# ⑥ 子场景先装配（后序 on_start），再是根脚本的声明顺序
sub_ready = one('MSG_SUB_READY')
orch = one('MSG_ORCH_READY')
alpha = one('MSG_ALPHA_READY')
beta = one('MSG_BETA_READY')
assert log.index(sub_ready) < log.index(orch) < log.index(alpha) < log.index(beta), \
    'on_start 顺序应为 sub → orchestrator → alpha → beta'

# 节点/实例 id（后续断言用；不依赖场景里的硬编码）
A = int(num(orch, 'a'))
B = int(num(orch, 'b'))
S = int(num(orch, 's'))
SUB = int(num(orch, 'sub'))
SHELL = int(num(orch, 'shell'))
assert len({A, B, S, SUB, SHELL}) == 5, (A, B, S, SUB, SHELL)
assert int(num(orch, 'instances')) == 2, orch
assert int(num(sub_ready, 'inst')) == SUB, sub_ready
assert int(num(sub_ready, 'root')) == SHELL, sub_ready
assert SUB > 0 and SHELL >= 0, (SUB, SHELL)
assert int(num(alpha, 'a')) == A and int(num(alpha, 'b')) == B, alpha
assert int(num(beta, 'b')) == B, beta

# 全局消息总线的 listener（子场景先 on_start，先拿到较小 id；根脚本然后订阅两个）
SUB_BUS = int(num(sub_ready, 'bus_listener'))
ROOT_BUS = int(num(orch, 'bus_listener'))
ROOT_BUS2 = int(num(orch, 'bus_listener2'))
assert SUB_BUS > 0 and ROOT_BUS > 0 and ROOT_BUS2 > 0, (SUB_BUS, ROOT_BUS, ROOT_BUS2)
assert len({SUB_BUS, ROOT_BUS, ROOT_BUS2}) == 3, (SUB_BUS, ROOT_BUS, ROOT_BUS2)

# ① 同一个事件名 ping 发到两个节点：两个脚本各处理一次，目标 = 各自节点
a_ping = one('MSG_ALPHA_PING')
assert int(num(a_ping, 'target')) == A and num(a_ping, 'value') == 10, a_ping
b_pings = lines('MSG_BETA_PING')
assert len(b_pings) == 2, f'Beta 应收到 2 次 ping（直接 + node_off 后），实际: {b_pings}'
assert all(int(num(l, 'target')) == B for l in b_pings), b_pings
assert sorted(num(l, 'value') for l in b_pings) == [20, 21], b_pings

# ② Alpha → Beta → Root 的跨脚本链，以及 Alpha → Root 的直接回复
forward = one('MSG_BETA_FORWARD')
assert int(num(forward, 'target')) == B and num(forward, 'value') == 11, forward
pongs = lines('MSG_ORCH_PONG')
assert len(pongs) == 3, pongs
assert sorted(num(l, 'value') for l in pongs) == [120, 121, 211], pongs
hub_alpha = one('MSG_ORCH_ALPHA')
assert num(hub_alpha, 'value') == 100, hub_alpha

direct = one('MSG_ORCH_EMIT phase=direct')
assert num(direct, 'alpha') == 1 and num(direct, 'beta') == 1, direct
assert num(direct, 'wrong') == 0, direct

# ③ 子节点事件冒泡到 Alpha（target 是处理器注册节点）
sensor = one('MSG_ALPHA_SENSOR')
assert int(num(sensor, 'target')) == A and num(sensor, 'value') == 30, sensor
sensor_emit = one('MSG_ORCH_EMIT phase=sensor')
assert num(sensor_emit, 'hits') == 1, sensor_emit
reply = one('MSG_ORCH_SENSOR_REPLY')
assert num(reply, 'value') == 31, reply

# ④ 父 → 子场景（scene_emit 到子实例）→ 子 → 父（scene_emit(0) + 壳节点冒泡）
sub_emit = one('MSG_ORCH_SUB_EMIT')
assert num(sub_emit, 'hits') == 1, sub_emit
sub_ping = one('MSG_SUB_PING')
assert num(sub_ping, 'target') == 0 and num(sub_ping, 'value') == 40, sub_ping
sub_pong = one('MSG_ORCH_SUB_PONG')
assert num(sub_pong, 'value') == 41, sub_pong
assert one('MSG_SUB_PONG_SEND').strip().endswith('=1'), '子 → 根 scene_emit 未命中'
sub_node = one('MSG_ORCH_SUB_NODE')
assert int(num(sub_node, 'target')) == SHELL and num(sub_node, 'value') == 42, sub_node
assert one('MSG_SUB_NODE_SEND').strip().endswith('=1'), '子 → 父节点事件未命中'

# ④b 全局消息总线：同一 (bus, subject, event) 同时命中根脚本（函数回调）与子场景脚本；
#     子场景发布 from_child → 根脚本的字符串 handler；unsubscribe 后根回调不再触发。
first = one('MSG_ORCH_BUS_PUB phase=first')
assert num(first, 'hits') == 2, first
root_ping = one('MSG_BUS_ROOT_PING')
assert num(root_ping, 'a') == 7 and num(root_ping, 'b') == 8 and num(root_ping, 'c') == 0, root_ping
sub_pings = lines('MSG_BUS_SUB_PING')
assert len(sub_pings) == 2, f'子场景应收到 2 次全局 ping，实际: {sub_pings}'
assert sorted((num(l, 'a'), num(l, 'b')) for l in sub_pings) == [(7, 8), (9, 10)], sub_pings
child_pub = one('MSG_SUB_BUS_PUB')
assert num(child_pub, 'hits') == 1, child_pub
root_child = one('MSG_BUS_ROOT_FROM_CHILD')
assert num(root_child, 'a') == 5 and num(root_child, 'b') == 6, root_child
unsub = one('MSG_ORCH_BUS_UNSUB')
assert unsub.rstrip().endswith('=1'), unsub
after = one('MSG_ORCH_BUS_PUB phase=after_unsub')
assert num(after, 'hits') == 1, after
assert len(lines('MSG_BUS_ROOT_PING')) == 1, 'unsubscribe 后根脚本回调仍被触发'

# ⑤ node_off 只影响 Alpha 节点：Alpha 0 命中、Beta 仍 1 命中
off = one('MSG_ORCH_OFF')
assert num(off, 'removed') == 1, off
assert num(off, 'alpha_after') == 0, off
assert num(off, 'beta_after') == 1, off
assert len(lines('MSG_ALPHA_PING')) == 1, 'node_off 后 Alpha 又收到了 ping'
assert len(lines('MSG_BETA_PING')) == 2, 'node_off 不应影响 Beta 的注册'

# ⑥ 汇总
summary = one('MSG_ORCH_SUMMARY')
assert num(summary, 'alpha') == 1 and num(summary, 'pongs') == 3, summary
assert num(summary, 'sensor') == 1 and num(summary, 'sub_pong') == 1, summary
assert num(summary, 'sub_node') == 1, summary
assert num(summary, 'bus_ping') == 1 and num(summary, 'bus_child') == 1, summary

# 事件没有串到另一个节点：Beta 不处理 Alpha 的 ping，Alpha 不处理 Beta 的 ping；
# Beta 节点上注册的 alpha_ping 哨兵也不应收到发给 Root 的 alpha_ping。
assert not lines('MSG_BETA_SNOOP'), 'Beta 节点上的 alpha_ping 哨兵被串扰'
assert not any('MSG_BETA_PING' in l and int(num(l, 'target')) == A for l in log), 'Beta 串到 Alpha 节点'
assert not any('MSG_ALPHA_PING' in l and int(num(l, 'target')) == B for l in log), 'Alpha 串到 Beta 节点'
# 子场景是独立实例：父场景的 alpha/beta ping 不应命中 responder.js。
assert not any('MSG_SUB_PING' in l and num(l, 'value') in (10, 20) for l in log), '父场景事件串到子场景'
print('node_message_e2e: 消息断言 ok')
PY

# ⑦ 确定性：两跑日志序列 + world_trail
grep '\[game\]' "$WORK/run1.log" > "$WORK/seq1.txt"
grep '\[game\]' "$WORK/run2.log" > "$WORK/seq2.txt"
cmp -s "$WORK/seq1.txt" "$WORK/seq2.txt" || {
    diff -u "$WORK/seq1.txt" "$WORK/seq2.txt" | head -40
    fail "两跑消息序列不一致（确定性被破坏）"
}
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/run1.log" | tail -1)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/run2.log" | tail -1)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "两跑 world_trail 不一致：'$T1' != '$T2'"

echo "node_message_e2e: ok（根 3 脚本 + 子场景脚本 / 双向跨脚本消息 / 冒泡 / node_off / 确定性；$T1）"
exit 0
