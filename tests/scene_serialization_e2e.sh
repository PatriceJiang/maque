#!/usr/bin/env bash
# 场景实例序列化 round-trip 端到端（docs/32 §6）。
#
# 判定链：
#   ① `Scene.save` 写出 `.scn` v2：场景描述符全字段（scripts/overlay/env/...）+ 未烘焙实例只写引用；
#   ② 同一世界连续保存两次**逐字节一致**（确定性文本）；
#   ③ `load(save(scene))` 行为等价：12 帧 world_trail 与原始场景完全相同；
#   ④ `Scene.bake_instances` 后保存为自包含（children 落盘、detached=true），重载不再实例化/不再跑子脚本；
#   ⑤ 世界快照 `World.snapshot/restore` 保留实例表（Scene.instances 数量/结构不变）。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

# shellcheck source=maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "scene_serialization_e2e: FAIL $*" >&2; exit 1; }

SCN_DIR="$(dirname "$SCENE")"
cp "$SCN_DIR/parent.scn" "$SCN_DIR/child.scn" "$SCN_DIR/parent.js" "$SCN_DIR/child.js" "$WORK/"
P="$WORK/parent.scn"
A="$WORK/saved_a.scn"
B="$WORK/saved_b.scn"
BAKED="$WORK/baked.scn"
A_Q="$(printf '%s' "$A" | sed 's|\\|/|g')"
B_Q="$(printf '%s' "$B" | sed 's|\\|/|g')"
BAKED_Q="$(printf '%s' "$BAKED" | sed 's|\\|/|g')"

# ---- ① 保存两次 + 快照（控制面：无帧循环，仅装配后请求）----
cat > "$WORK/req_save.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Scene.save","params":{"path":"$A_Q"}}
{"jsonrpc":"2.0","id":2,"method":"Scene.save","params":{"path":"$B_Q"}}
{"jsonrpc":"2.0","id":3,"method":"World.snapshot"}
{"jsonrpc":"2.0","id":4,"method":"World.restore"}
{"jsonrpc":"2.0","id":5,"method":"Scene.instances"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$P" < "$WORK/req_save.txt" > "$WORK/save.log" 2>&1
[ -f "$A" ] && [ -f "$B" ] || { cat "$WORK/save.log"; fail "Scene.save 未落盘"; }
cmp -s "$A" "$B" || fail "两次保存不一致（确定性文本被破坏）"

"$PY" - "$WORK/save.log" "$A" <<'PY' || fail "保存/快照断言失败"
import json, sys
raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
resps = {}
for line in raw.splitlines():
    line = line.strip()
    if not line.startswith('{"jsonrpc"'):
        continue
    try:
        r = json.loads(line)
    except json.JSONDecodeError:
        continue
    resps[r.get('id')] = r
assert resps[1]['result']['ok'] is True, resps[1]
assert resps[2]['result']['ok'] is True, resps[2]
assert resps[3]['result']['ok'] is True, resps[3]          # 快照含实例表（不因实例失败）
assert resps[4]['result']['ok'] is True, resps[4]          # 快照恢复实例表
assert resps[5]['result']['count'] == 3, resps[5]          # root + 2 静态子实例
assert raw.count('CHILD_START') == 2, '快照恢复结构相同却重建了实例 VM（应保留脚本状态、不重复 on_start）'

scn = json.load(open(sys.argv[2], encoding='utf-8'))
assert scn['version'] == 2, scn.get('version')
for k in ('script', 'scripts', 'overlay_script', 'width', 'height', 'seed', 'gravity_y',
          'env_color', 'env_intensity', 'views', 'render_scale', 'msaa_samples'):
    assert k in scn, f'场景描述符丢字段 {k}'
ents = scn['entities']
assert len(ents) == 2, f'未烘焙实例应只写引用，实际 root entities={len(ents)}'
insts = [c for e in ents for c in e['components'] if c['type'] == 'scn.SceneInstance']
assert len(insts) == 2 and all(i['detached'] is False for i in insts), insts
assert all('children' not in e or not e['children'] for e in ents), '未烘焙实例不应落盘子树'
print('save/snapshot assertions ok')
PY

# ---- ③ round-trip 行为等价（12 帧 world_trail 逐位相同）----
trail_of() { grep -o 'world_trail=0x[0-9a-f]*' "$1" | tail -1; }
timeout 60 "$PLAYER" --frames 12 "$P" > "$WORK/orig.log" 2>&1
[ "$?" -eq 0 ] || { cat "$WORK/orig.log"; fail "原场景运行失败"; }
timeout 60 "$PLAYER" --frames 12 "$A" > "$WORK/saved.log" 2>&1
[ "$?" -eq 0 ] || { cat "$WORK/saved.log"; fail "保存后场景运行失败"; }
T1="$(trail_of "$WORK/orig.log")"; T2="$(trail_of "$WORK/saved.log")"
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "round-trip 行为不等价：$T1 != $T2"
grep -q "CHILD_START inst=1" "$WORK/saved.log" || fail "保存后子实例 1 未加载"
grep -q "CHILD_START inst=2" "$WORK/saved.log" || fail "保存后子实例 2 未加载"
grep -q "CHILD_PING inst=1" "$WORK/saved.log" || fail "保存后事件路由异常"

# ---- ④ 烘焙（自包含）----
cat > "$WORK/req_bake.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Scene.bake_instances","params":{"dry_run":0}}
{"jsonrpc":"2.0","id":2,"method":"Scene.save","params":{"path":"$BAKED_Q"}}
EOF
MAQUE_CONTROL=- "$PLAYER" "$P" < "$WORK/req_bake.txt" > "$WORK/bake.log" 2>&1
[ -f "$BAKED" ] || { cat "$WORK/bake.log"; fail "烘焙保存失败"; }
"$PY" - "$BAKED" <<'PY' || fail "烘焙场景内容断言失败"
import json, sys
scn = json.load(open(sys.argv[1], encoding='utf-8'))
insts = [c for e in scn['entities'] for c in e['components'] if c['type'] == 'scn.SceneInstance']
assert len(insts) == 2 and all(i['detached'] is True for i in insts), insts
bodies = [ch for e in scn['entities'] for ch in e.get('children', []) if ch['name'] == 'Body']
assert len(bodies) == 2, f'烘焙后子树未落盘: {bodies}'
print('bake assertions ok')
PY
timeout 60 "$PLAYER" --frames 3 "$BAKED" > "$WORK/baked_run.log" 2>&1
[ "$?" -eq 0 ] || { cat "$WORK/baked_run.log"; fail "烘焙场景运行失败"; }
grep -q "CHILD_START" "$WORK/baked_run.log" && fail "烘焙场景不应再实例化子场景（但出现 CHILD_START）"
cat > "$WORK/req_baked.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.instances"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$BAKED" < "$WORK/req_baked.txt" > "$WORK/baked_ctl.log" 2>&1
"$PY" - "$WORK/baked_ctl.log" <<'PY' || { cat "$WORK/baked_ctl.log"; fail "烘焙场景实例断言失败"; }
import json, sys
line = next(l for l in open(sys.argv[1], encoding='utf-8', errors='replace') if l.strip().startswith('{"jsonrpc"'))
res = json.loads(line)['result']
assert res['count'] == 1, res
print('baked scene has single root instance')
PY

echo "scene_serialization_e2e: ok"
exit 0
