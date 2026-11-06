#!/usr/bin/env bash
# ListView 内 scn.Button：命中后按下态变色，释放后恢复；验证按钮不是只有背景矩形。
set -e
. "$(dirname "$0")/maque_python.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLAYER="${1:-$ROOT/build/samples/player/maque_player}"
WORK="${2:-${TMPDIR:-/tmp}/maque_ui_list_button.$$}"
SCENE="${3:-$ROOT/scenes/ui_list/fade_mix.scn}"
SIM="$ROOT/tests/data/sim/ui_list_button_down.sim.json"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

shot() { local out="$1" frame="$2"; shift 2; env MAQUE_HEADLESS=1 "$@" "$PLAYER" --shot "$WORK/$out.ppm" --shot-frame "$frame" "$SCENE" >"$WORK/$out.log" 2>&1 || { cat "$WORK/$out.log"; exit 1; }; }
shot base 3
shot pressed 3 MAQUE_SIM="$SIM"
shot released 7 MAQUE_SIM="$SIM"
"$PY" - "$WORK/base.ppm" "$WORK/pressed.ppm" "$WORK/released.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); h = d.split(b'\n', 3)
    w, hh = map(int, h[1].split()); return w, hh, h[3]
w, h, base = load(sys.argv[1]); _, _, press = load(sys.argv[2]); _, _, release = load(sys.argv[3])
def px(p, x, y): return tuple(p[(y * w + x) * 3:(y * w + x) * 3 + 3])
b = px(base, 200, 80); p = px(press, 200, 80); r = px(release, 200, 80)
if b == p:
    print(f'ui_list_button_e2e: Button 按下没有变色 {b} vs {p}')
    sys.exit(1)
if b != r:
    print(f'ui_list_button_e2e: Button 释放后没有恢复 {b} vs {r}')
    sys.exit(1)
print(f'ui_list_button_e2e: hit color {b} -> {p} -> {r}')
PY
echo "ui_list_button_e2e: ok"
