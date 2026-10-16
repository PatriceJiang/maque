#!/usr/bin/env bash
# 绑定面一致性（docs/21 §6、docs/14）：**JS 与 Lua 都能调用全部引擎 api**。
#   ① 由 tools/gen_binding_probe.js 从 maque_schema.json（单一真相）生成两份等价探针
#   ② 同一场景分别在 js / lua 后端跑 → 两侧 missing 都必须为 0、checked 必须相同（且等于生成数）
# 这抓的是"某一侧被静默丢弃/注册方式未实现"一类问题（例：`(string)->string` 曾被生成器丢弃）。
set -u
PLAYER="$1"; PROBE_DIR="$2"; WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK"
cp "$PROBE_DIR"/bindings_probe.js "$PROBE_DIR"/bindings_probe.lua "$WORK/"
export MAQUE_HEADLESS=1

fail() { echo "bindings_e2e: FAIL $*" >&2; exit 1; }

# 最小场景（场景只提供页面；脚本自己 quit）
printf '{"title":"bindings probe","script":"%s","width":320,"height":240,"seed":1,"entities":[]}\n' \
       "bindings_probe.js"  > "$WORK/probe_js.scn"
printf '{"title":"bindings probe lua","script":"%s","width":320,"height":240,"seed":1,"entities":[]}\n' \
       "bindings_probe.lua" > "$WORK/probe_lua.scn"

run() {   # run <log> <scene> [env…]
    local log="$1" scene="$2"; shift 2
    env MAQUE_MAX_FRAMES=5 "$@" timeout 60 "$PLAYER" "$scene" > "$log" 2>&1 || true
    grep -o 'bindings_probe missing=[0-9]* checked=[0-9]*' "$log" | tail -1
}

JS_LINE=$(run "$WORK/js.log" "$WORK/probe_js.scn")
LUA_LINE=$(run "$WORK/lua.log" "$WORK/probe_lua.scn")
[ -n "$JS_LINE" ] || fail "JS 探针未输出结果（见 $WORK/js.log）"
[ -n "$LUA_LINE" ] || fail "Lua 探针未输出结果（见 $WORK/lua.log）"

JS_MISSING=$(printf '%s' "$JS_LINE" | sed 's/.*missing=\([0-9]*\).*/\1/')
LUA_MISSING=$(printf '%s' "$LUA_LINE" | sed 's/.*missing=\([0-9]*\).*/\1/')
JS_CHECKED=$(printf '%s' "$JS_LINE" | sed 's/.*checked=\([0-9]*\).*/\1/')
LUA_CHECKED=$(printf '%s' "$LUA_LINE" | sed 's/.*checked=\([0-9]*\).*/\1/')

[ "$JS_MISSING" = "0" ] || { grep -o 'bindings_probe missing=.*' "$WORK/js.log" | tail -1 >&2; fail "JS 侧缺少绑定"; }
[ "$LUA_MISSING" = "0" ] || { grep -o 'bindings_probe missing=.*' "$WORK/lua.log" | tail -1 >&2; fail "Lua 侧缺少绑定"; }
[ "$JS_CHECKED" = "$LUA_CHECKED" ] || fail "JS/Lua 检查数不一致（$JS_CHECKED vs $LUA_CHECKED）"
[ "$JS_CHECKED" -ge 100 ] || fail "探针数量异常（$JS_CHECKED）——生成器是否漏了命名空间？"

echo "bindings_e2e: ok（JS/Lua 各 $JS_CHECKED 个 api 全部可调用）"
