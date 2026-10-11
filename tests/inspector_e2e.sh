#!/usr/bin/env bash
# Inspector 端到端（plans/17）：独立进程 attach 运行中的游戏，复用控制面 JSON-RPC。
#   ① 端点：游戏侧 MAQUE_INSPECT 起 TCP 端点，inspector 能连上
#   ② 只读：变更类方法被拒绝（-32001）并计数；只读方法正常
#   ③ 反射：Scene.info / Scene.entities / Types.describe 经 TCP 返回
#   ④ 独立性：游戏不因 inspector 断开而退出（attach/detach 无侵入）
#   ⑤ UDS：uds: 传输同样可用
set -u
INSPECTOR="$1"; PLAYER="$2"; SCENE="$3"; WORK="$4"
rm -rf "$WORK"; mkdir -p "$WORK"
fail() { echo "inspector_e2e: FAIL $*" >&2; exit 1; }

PORT=$(( (RANDOM % 2000) + 21000 ))
UDSPATH="$WORK/maque.sock"

start_game() {   # start_game <log> <spec>
  MAQUE_HEADLESS=1 MAQUE_INSPECT="$2" MAQUE_MAX_FRAMES=100000 \
    "$PLAYER" "$SCENE" > "$1" 2>&1 &
  GAME_PID=$!
  for _ in $(seq 1 60); do
    grep -q 'control endpoint' "$1" && return 0
    sleep 0.1
  done
  return 1
}
# 优雅终止：SIGTERM → 最多等 3s → SIGKILL（绝不用裸 wait：PID 异常时会永久阻塞）
stop_game() {
  [ -n "${GAME_PID:-}" ] || return 0
  kill "$GAME_PID" 2>/dev/null
  for _ in $(seq 1 30); do
    kill -0 "$GAME_PID" 2>/dev/null || { wait "$GAME_PID" 2>/dev/null; GAME_PID=""; return 0; }
    sleep 0.1
  done
  kill -9 "$GAME_PID" 2>/dev/null
  wait "$GAME_PID" 2>/dev/null
  GAME_PID=""
}

# ---- ① TCP 端点 + ③ 反射 ----
start_game "$WORK/game.log" "$PORT" || fail "游戏未启动端点"
grep -q "tcp://127.0.0.1:$PORT (read-only)" "$WORK/game.log" || fail "端点未按只读启动"
"$INSPECTOR" --connect "$PORT" --headless Control.info Scene.info Types.describe > "$WORK/tcp.log" 2>&1 \
  || fail "inspector headless 失败（见 $WORK/tcp.log）"
grep -q '"read_only": *true' "$WORK/tcp.log" || fail "Control.info 未报告只读"
grep -q '"title": *"sokoban"' "$WORK/tcp.log" || fail "Scene.info 未返回场景标题"
grep -q 'Scene.info' "$WORK/tcp.log" || true
grep -q '"structs"\|"types"\|"name"' "$WORK/tcp.log" || fail "Types.describe 无内容"

# ---- ② 只读：变更类方法必须被拒绝并计数 ----
"$INSPECTOR" --connect "$PORT" --headless 'Scene.set|{"id":0,"path":"name","value":"x"}' > "$WORK/set.log" 2>&1 || true
grep -q 'read-only' "$WORK/set.log" || fail "只读会话未拒绝 Scene.set"
"$INSPECTOR" --connect "$PORT" --headless Control.info > "$WORK/info2.log" 2>&1
REJ=$(grep -o '"rejected": *[0-9]*' "$WORK/info2.log" | head -1 | grep -o '[0-9]*')
[ "${REJ:-0}" -ge 1 ] || fail "被拒计数未增加（rejected=$REJ）"

# ---- ③b 字体图集与画面抓帧（分块 base64 传输；inspector 面板的数据通路）----
"$INSPECTOR" --connect "$PORT" --headless Font.info > "$WORK/font.log" 2>&1 || fail "Font.info 失败"
grep -q '"atlases"' "$WORK/font.log" || fail "Font.info 未返回图集列表"
SIZE=$(grep -o '"size": *[0-9.]*' "$WORK/font.log" | head -1 | grep -o '[0-9.]*')
IDX=$(grep -o '"index": *[0-9]*' "$WORK/font.log" | head -1 | grep -o '[0-9]*')
[ -n "$SIZE" ] || fail "Font.info 未返回字号"
"$INSPECTOR" --connect "$PORT" --headless "Font.glyphs|{\"index\":$IDX,\"size\":$SIZE}" > "$WORK/glyphs.log" 2>&1
grep -q '"cp"' "$WORK/glyphs.log" || fail "Font.glyphs 未返回字形"
grep -q '"ax"' "$WORK/glyphs.log" || fail "Font.glyphs 未返回图集矩形"
# 图集分块：首块应带 PPM 头（base64 的 'UDY' = "P6\n"）
"$INSPECTOR" --connect "$PORT" --headless "Font.atlas|{\"index\":$IDX,\"size\":$SIZE,\"offset\":0,\"max\":64}" > "$WORK/atlas.log" 2>&1
grep -q '"total"' "$WORK/atlas.log" || fail "Font.atlas 未返回总长"
grep -q '"data":"UDY' "$WORK/atlas.log" || fail "Font.atlas 首块不是 PPM 头（base64 P6）"
"$INSPECTOR" --connect "$PORT" --headless 'Capture.screen|{"offset":0,"max":64,"max_dim":480}' > "$WORK/screen.log" 2>&1
grep -q '"data":"UDY' "$WORK/screen.log" || fail "Capture.screen 未返回 PPM"
grep -q '"w": *[1-9]' "$WORK/screen.log" || fail "Capture.screen 尺寸异常"

# ---- ④ 独立性：断开后游戏仍在运行 ----
kill -0 "$GAME_PID" 2>/dev/null || fail "游戏进程意外退出"
"$INSPECTOR" --connect "$PORT" --headless Scene.entities > "$WORK/ents.log" 2>&1
grep -q '"result"' "$WORK/ents.log" || fail "Scene.entities 无结果"
kill -0 "$GAME_PID" 2>/dev/null || fail "inspector 运行后游戏进程消失"
stop_game

# ---- ⑤ UDS 传输 ----
start_game "$WORK/game_uds.log" "uds:$UDSPATH" || fail "UDS 端点未启动"
grep -q "uds://$UDSPATH" "$WORK/game_uds.log" || fail "UDS 地址不符"
"$INSPECTOR" --connect "uds:$UDSPATH" --headless Control.info > "$WORK/uds.log" 2>&1 \
  || fail "UDS 连接失败（见 $WORK/uds.log）"
grep -q '"endpoint": *"uds://' "$WORK/uds.log" || fail "UDS Control.info 异常"
grep -q '"read_only": *true' "$WORK/uds.log" || fail "UDS 会话未只读"
stop_game
[ -S "$UDSPATH" ] && fail "UDS socket 未清理" || true

echo "inspector_e2e: ok（TCP/UDS 端点、只读拒绝与计数、反射查询、attach/detach 无侵入）"
exit 0
