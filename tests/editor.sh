#!/usr/bin/env bash
# maque 最小编辑器（docs/11 §3/§4、M9）：通过控制平面读写/增删实体并落盘。
#
# 编辑器不复制引擎逻辑——所有编辑都是引擎的稳定调用：
#   Scene.entities / Scene.get / Scene.set(dry_run) / Scene.spawn / Scene.destroy / Scene.save
#
# 用法：
#   tests/editor.sh <player> <scene.scn> [命令...]
# 命令（按顺序执行）：
#   list                         列出实体
#   get <id> <path>              读字段（如 1 scn.Sprite.radius）
#   set <id> <path> <value>      写字段（先 dry_run 预演，再实写）
#   spawn <name> <component>     新建实体（component 如 scn.Sprite，可空）
#   destroy <id>                 删除实体
#   save <path>                  场景落盘
# 无命令时跑一次默认演示：改半径 → 预演 → 落盘 → 列出。
set -u

PLAYER="$1"; SCENE="$2"; shift 2
WORK="$(mktemp -d /tmp/maque_editor.XXXXXX)"
OUT="$WORK/out.txt"; CTL="$WORK/ctl"
mkfifo "$CTL"
export MAQUE_HEADLESS=1

timeout 120 env MAQUE_CONTROL=- "$PLAYER" "$SCENE" < "$CTL" > "$OUT" 2>&1 &
PID=$!
exec 9>"$CTL"

ID=0
req() {
  ID=$((ID + 1))
  printf '%s\n' "{\"jsonrpc\":\"2.0\",\"id\":$ID,\"method\":\"$1\",\"params\":$2}" >&9
  sleep 0.25
  grep -F "\"id\":$ID,\"result\":" "$OUT" | tail -1 | sed 's/^.*"result"://'
}

step() { req Runtime.step '{"frames":1}' >/dev/null; }

cmd_list()   { req Scene.entities '{}'; }
cmd_get()    { req Scene.get "{\"id\":$1,\"path\":\"$2\"}"; }
cmd_set() {
  local id="$1" path="$2" val="$3"
  local preview; preview=$(req Scene.set "{\"id\":$id,\"path\":\"$path\",\"value\":$val,\"dry_run\":1}")
  echo "  预演: $preview"
  req Scene.set "{\"id\":$id,\"path\":\"$path\",\"value\":$val}"
}
cmd_spawn() {
  local comp="${2:-}"
  if [ -n "$comp" ]; then
    req Scene.spawn "{\"name\":\"$1\",\"component\":\"$comp\"}"
  else
    req Scene.spawn "{\"name\":\"$1\"}"
  fi
}
cmd_destroy() { local p; p=$(req Scene.destroy "{\"id\":$1,\"dry_run\":1}"); echo "  预演: $p"; req Scene.destroy "{\"id\":$1}"; }
cmd_save()    { req Scene.save "{\"path\":\"$1\"}"; }
cmd_frame()   { req Runtime.frame '{}'; }

if [ "$#" -eq 0 ]; then
  echo "== 默认编辑演示 =="
  echo "-- 实体列表"; cmd_list
  echo "-- 当前 radius"; cmd_get 1 scn.Sprite.radius
  echo "-- 改 radius（先 dry_run）"; cmd_set 1 scn.Sprite.radius 30.0
  step
  echo "-- 改后 radius"; cmd_get 1 scn.Sprite.radius
  echo "-- 新建实体"; cmd_spawn "EditorBall" scn.Sprite
  step
  echo "-- 删除实体"; cmd_destroy 4
  echo "-- 落盘"; cmd_save "$WORK/edited.scn"
  echo "-- 帧信息"; cmd_frame
else
  while [ "$#" -gt 0 ]; do
    c="$1"; shift
    case "$c" in
      list)    cmd_list ;;
      get)     cmd_get "$1" "$2"; shift 2 ;;
      set)     cmd_set "$1" "$2" "$3"; shift 3 ;;
      spawn)   cmd_spawn "$1" "${2:-}"; shift 2 ;;
      destroy) cmd_destroy "$1"; shift ;;
      save)    cmd_save "$1"; shift ;;
      frame)   cmd_frame ;;
      *) echo "unknown command: $c" >&2; exit 2 ;;
    esac
  done
fi

exec 9>&-
wait "$PID" 2>/dev/null
SAVED="$WORK/edited.scn"
if [ -f "$SAVED" ]; then
  echo "-- 落盘文件（前 12 行）"; head -12 "$SAVED"
fi
rm -rf "$WORK"
exit 0
