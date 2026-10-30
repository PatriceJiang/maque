#!/usr/bin/env bash
# 脚本热重载的节点事件注册回归（docs/14 §热重载）：
#   · 热重载 = 新 VM + 对新 VM 派发 on_start（脚本借机重新 node_on 注册处理器）；
#   · 旧 VM 的注册必须先按 (实例, 脚本槽位) 摘掉 —— 否则 `node_events` 每次重载都追加一份：
#     同一事件被调用 N 次（功能错误），NodeHandler 的两个 std::string 也随重载次数无界增长（泄漏）。
#
# 判定：让 handler 打印 `ping f=<frame>`（帧号单调），一个 emit 只应产生一条；
#       若同一帧出现多条 → 处理器被重复注册。用 MAQUE_WATCH 触发（追加注释改 hash），
#       不依赖 FIFO，Windows 也能跑。
set -u
PLAYER="$1"
WORK="$2"
fail() { echo "script_reload_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"

cat >"$WORK/s.scn" <<'JSON'
{ "title": "script_reload", "script": "s.js", "width": 320, "height": 240, "views": 1,
  "entities": [
    { "name": "emitter", "components": [
      { "type": "scn.Transform2D", "x": 160.0, "y": 120.0 },
      { "type": "scn.Sprite", "width": 32.0, "height": 32.0, "r": 0.9, "g": 0.9, "b": 0.2 } ] }
  ] }
JSON

cat >"$WORK/s.js" <<'JS'
'use strict';
let n = 0;
function on_start() { node_on(0, "ping", "on_ping"); log("ready"); }
function on_ping() { log("ping f=" + director_frame()); }
function update(dt) { n = n + 1; if (n % 10 === 0) node_emit(0, "ping"); }
JS

export MAQUE_WINDOW=0x0
export MAQUE_PRESENT=off
export MAQUE_WATCH=1
export MAQUE_PACE=1
export MAQUE_MAX_FRAMES=240

timeout 60 "$PLAYER" "$WORK/s.scn" >"$WORK/run.log" 2>&1 &
PID=$!

# 等 watcher 就绪再改文件（否则追加可能发生在初始 stamp 之前 → 永远不触发）。
for _ in $(seq 1 100); do grep -q 'hot reload watching' "$WORK/run.log" && break; sleep 0.1; done
if ! grep -q 'hot reload watching' "$WORK/run.log"; then
  kill "$PID" 2>/dev/null
  tail -20 "$WORK/run.log"
  fail "watcher 未启动"
fi
sleep 0.4
printf '\n// reload 1\n' >>"$WORK/s.js"
sleep 0.8
printf '// reload 2\n' >>"$WORK/s.js"
wait "$PID" || fail "player 退出码非 0"

grep -q "hot reload instance #0 script 's.js'" "$WORK/run.log" \
  || { tail -20 "$WORK/run.log"; fail "watcher 未触发热重载（用例没有真正跑到回归点）"; }

DUPS=$(grep -o 'ping f=[0-9]*' "$WORK/run.log" | sort | uniq -d | tr '\n' ' ')
[ -z "$DUPS" ] || { cat "$WORK/run.log"; fail "node_on 热重载后重复派发（同帧重复: $DUPS）"; }

PINGS=$(grep -c 'ping f=' "$WORK/run.log")
[ "${PINGS:-0}" -ge 4 ] || { tail -20 "$WORK/run.log"; fail "ping 次数过少（$PINGS）——脚本/事件未运行"; }

echo "script_reload_e2e: ok（reloads=$(grep -c 'hot reload instance' "$WORK/run.log") pings=$PINGS）"
