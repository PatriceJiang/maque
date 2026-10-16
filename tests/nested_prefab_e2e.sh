#!/usr/bin/env bash
# Prefab **嵌套**实例化端到端（docs/12 §5/§7）。
#
# 为什么单独一条：`prefab_e2e.sh` 覆盖的是**单层**展开（star.prefab.json）与 bake/幂等/循环引用，
# 但 docs/12 §7 要求"禁止循环实例化"隐含了"允许多层嵌套"—— 而嵌套此前**没有测试**。
# 嵌套是坦克这类游戏的刚需：`turret.prefab.json` 里挂 `bullet.prefab.json`，
# 放两个 turret 实例就应得到 2 个根 + 2×(3 自有 + 1 内层) = 10 个实体。
#
# 判定链：
#   ① 外层实例 2 次、内层 bullet **各一次共 2 次**（不是 0 次也不是 4 次）；
#   ② 实体总数 10；
#   ③ 内层 Shell 的 parent 是**外层实例子树的 Loaded**（不是根）——证明真的挂在嵌套树上；
#   ④ 不该误报循环引用 / 嵌套过深。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

# shellcheck source=tests/maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "nested_prefab_e2e: FAIL $*" >&2; exit 1; }
RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }

SCN_DIR="$(dirname "$SCENE")"
[ -f "$SCN_DIR/turret.prefab.json" ] || fail "缺少 turret.prefab.json（测试前置）"
[ -f "$SCN_DIR/bullet.prefab.json" ] || fail "缺少 bullet.prefab.json（测试前置）"

cat > "$WORK/req1.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.entities"}
EOF
MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$WORK/req1.txt" > "$WORK/out1.txt" 2>&1

ENTS=$(RESP "$WORK/out1.txt" 1)
[ -n "$ENTS" ] || { cat "$WORK/out1.txt"; fail "Scene.entities 无响应"; }

# ① 外层 2 次、内层 2 次
OUTER=$(grep -c "prefab 'turret.prefab.json' instantiated" "$WORK/out1.txt" || true)
[ "$OUTER" -eq 2 ] || fail "外层 turret 应实例化 2 次，实际 $OUTER"
INNER=$(grep -c "prefab 'bullet.prefab.json' instantiated" "$WORK/out1.txt" || true)
[ "$INNER" -eq 2 ] || fail "内层 bullet 应实例化 2 次（每个 turret 一次），实际 $INNER"

# ④ 不该报警
grep -q "cyclic prefab reference" "$WORK/out1.txt" && fail "误报循环引用"
grep -q "prefab nesting too deep" "$WORK/out1.txt" && fail "误报嵌套过深"

# ② + ③ 实体总数与嵌套挂载关系
# ⚠️ Scene.entities 的 JSON 是**多行**的（实体多时不会挤在一行）→ 不能只 grep 单行，
# 必须把整个响应交给 Python 逐块解析（否则拿到残缺 JSON，报 "Extra data"）。
if ! "$PY" - "$WORK/out1.txt" <<'PY'
import json, re, sys

raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
# 从整个输出里抠出 id=1 的响应体：允许跨行（用非贪婪 + 平衡括号粗略截取）
m = re.search(r'\{"jsonrpc":"2\.0","id":1,"result":', raw)
if not m:
    print('找不到 id=1 的响应', file=sys.stderr)
    sys.exit(2)

dec = json.JSONDecoder()
# 跳过前缀，从 result 的值开始解析（raw_decode 会停在值结束处，不受后续内容干扰）
idx = m.end()
ents, _ = dec.raw_decode(raw, idx)

by_id = {e['id']: e for e in ents}

assert len(ents) == 10, f'期望 10 个实体，实际 {len(ents)}: {[e["name"] for e in ents]}'

def chain(eid):
    """从实体到根的父链（名字），带环保护。"""
    out, seen, cur = [], set(), eid
    while cur is not None and cur != -1 and cur not in seen:
        seen.add(cur)
        e = by_id.get(cur)
        if not e:
            break
        out.append(e['name'])
        cur = e.get('parent', -1)
    return out

shells = [e for e in ents if e['name'] == 'Shell']
assert len(shells) == 2, f'期望 2 个内层 Shell，实际 {len(shells)}'

for s in shells:
    ch = chain(s['id'])
    assert ch[0] == 'Shell', f'父链异常: {ch}'
    assert 'Loaded' in ch, f'Shell 的父链里没有 Loaded（内层没挂到外层实例子树）: {ch}'
    assert len(ch) >= 3, f'Shell 的父链太短，像是被平铺了: {ch}'
    top = ch[-1]
    assert top in ('TurretA', 'TurretB'), f'Shell 的顶层父节点应是 Turret 实例，实际 {top}: {ch}'

roots = {e['name'] for e in ents if e.get('parent', -1) == -1}
assert 'Shell' not in roots, 'Shell 出现在根层 —— 内层被平铺展开了'
assert roots == {'TurretA', 'TurretB'}, f'根层应为两个 Turret，实际 {roots}'

print(f'nested_prefab_e2e: 嵌套树 ok（2 实例，各自子树内含 Shell；父链示例={chain(shells[0]["id"])}）')
PY
then
    fail "嵌套结构与实体数断言失败"
fi

echo "nested_prefab_e2e: ok (entities=10, outer=$OUTER, inner=$INNER)"
exit 0
