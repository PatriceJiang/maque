#!/usr/bin/env bash
# Prefab **平级展开**（`source` 简写）端到端。
#
# 为什么需要：`PrefabInstance` 组件把 prefab 展开成**自己的子实体**（套一层壳），
# 适合"整体变换/显隐一个复合对象"；但坦克那类**平级重复对象**（20 个履带印、5 个残骸）
# 本来是各自独立的根级实体 —— 用组件会改变层级（`entity_find` 语义变化）并白增 25 个空壳实体。
# 为此加了实体级 `source` 简写：**纯展开指令，不 spawn 壳实体**，prefab 的根直接成为兄弟。
#
# 判定链：
#   ① 3 个 `source` 简写 → 各展开出 1 个根实体，**没有**壳实体（实体名恰好是 dot + 前缀）；
#   ② `name_prefix` 生效：展开名 = **前缀 + prefab 内实体名**（adot/bdot/cdot）——
#      坦克用的就是这条规则（prefab 里叫 `track` + 前缀 `1` → `track1`）；
#      不同实例必须能区分，否则同名实体在场景里 `entity_find` 只会命中第一个；
#   ③ 展开出的实体是**根级**（parent == -1），即真的平级、不是挂在某个壳下；
#   ④ 对照组：`PrefabInstance` 组件仍然**套壳**（`Attached` 的子实体），两种语义并存不冲突。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

# shellcheck source=tests/maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "inline_prefab_e2e: FAIL $*" >&2; exit 1; }

cat > "$WORK/req1.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.entities"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req1.txt" > "$WORK/out1.txt" 2>&1

grep -q "prefab not found" "$WORK/out1.txt" && fail "prefab 未找到"

if ! "$PY" - "$WORK/out1.txt" <<'PY'
import json, re, sys

raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = re.search(r'\{"jsonrpc":"2\.0","id":1,"result":', raw)
if not m:
    print('找不到 id=1 响应', file=sys.stderr); sys.exit(2)
ents, _ = json.JSONDecoder().raw_decode(raw, m.end())
by_id = {e['id']: e for e in ents}
names = [e['name'] for e in ents]

# ① 3 个简写各展开 1 个；Attached 组件展开 1 个子实体；加 Marker/Attached 自身
for want in ('adot', 'bdot', 'cdot'):
    assert want in names, f'缺少展开出的实体 {want}（name_prefix 未生效？）names={names}'
# ② 名字互不相同
assert len(names) == len(set(names)), f'出现同名实体（name_prefix 没区分开）: {names}'

# ③ 简写展开出的实体必须是**根级**
for want in ('adot', 'bdot', 'cdot'):
    e = next(x for x in ents if x['name'] == want)
    assert e.get('parent', -1) == -1, f'{want} 不是根级（parent={e.get("parent")}）—— 说明套了壳'

# 无壳实体：未加前缀的 `dot` **不应存在**（它只应作为 Attached 的子实体，见 ④）
# 根级不应出现未加前缀的 `dot`（它只应由 Attached 的 PrefabInstance 展开成子实体，见 ④）
root_names = {x['name'] for x in ents if x.get('parent', -1) == -1}
assert 'dot' not in root_names, f'根级出现未加前缀的 dot —— 前缀没生效或展开了两次: {root_names}'

# ④ 对照组：PrefabInstance 组件仍然套壳（展开为 Attached 的子实体）
att = next((x for x in ents if x['name'] == 'Attached'), None)
assert att is not None, '缺少 Attached 实体'
children = [x for x in ents if x.get('parent') == att['id']]
assert len(children) == 1, f'Attached 应有 1 个子实体（组件语义=套壳），实际 {len(children)}'
assert children[0]['name'] == 'dot', f'组件展开的子实体名应为 dot，实际 {children[0]["name"]}'

print(f'inline_prefab_e2e: ok（{len(ents)} 实体）'
      f' 简写={[n for n in names if n in ("adot","bdot","cdot")]}'
      f' 组件子实体={children[0]["name"]}')
PY
then
    cat "$WORK/out1.txt"; fail "平级展开断言失败"
fi

echo "inline_prefab_e2e: ok"
exit 0
