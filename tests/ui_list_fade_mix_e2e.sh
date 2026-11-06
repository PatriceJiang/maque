#!/usr/bin/env bash
# ListView 新场景：Sprite + Button + Label 子节点 + Percent 边缘 fade。
# 断言三类图元都随 mask 变暗，中间行不变，滚动条不变。
set -e
. "$(dirname "$0")/maque_python.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLAYER="${1:-$ROOT/build/samples/player/maque_player}"
WORK="${2:-${TMPDIR:-/tmp}/maque_ui_list_fade_mix.$$}"
SCENE="${3:-$ROOT/scenes/ui_list/fade_mix.scn}"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

run() { local out="$1"; shift; env MAQUE_HEADLESS=1 "$@" "$PLAYER" --shot "$WORK/$out.ppm" --shot-frame 2 "$SCENE" >"$WORK/$out.log" 2>&1 || { cat "$WORK/$out.log"; exit 1; }; }
run fade
run nomask MAQUE_UI_ALPHA_MASK=0
"$PY" - "$WORK/fade.ppm" "$WORK/nomask.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); h = d.split(b'\n', 3)
    w, hh = map(int, h[1].split()); return w, hh, h[3]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
def px(p, x, y): return tuple(p[(y * w + x) * 3:(y * w + x) * 3 + 3])
def darker(p, n, label):
    if sum(p) >= sum(n) * 0.9:
        print(f'ui_list_fade_mix_e2e: {label} 未被 fade 变暗 {p} vs {n}')
        sys.exit(1)
# 顶部 row0：button 背景、sprite 图标、文字都必须变暗。
darker(px(a, 75, 80), px(b, 75, 80), 'top button')
darker(px(a, 50, 84), px(b, 50, 84), 'top sprite')
darker(px(a, 100, 88), px(b, 100, 88), 'top text')
# 中间 row2 不淡出。
for x, y, label in [(75, 180, 'mid button'), (50, 180, 'mid sprite'), (100, 184, 'mid text')]:
    if px(a, x, y) != px(b, x, y):
        print(f'ui_list_fade_mix_e2e: {label} 被误淡出 {px(a,x,y)} vs {px(b,x,y)}')
        sys.exit(1)
# 底部 row4 处于 bottom fade。
darker(px(a, 75, 275), px(b, 75, 275), 'bottom button')
# 滚动条/百分比不受内容 fade 影响。
if px(a, 601, 90) != px(b, 601, 90):
    print('ui_list_fade_mix_e2e: 滚动条被内容 fade 影响')
    sys.exit(1)
print('ui_list_fade_mix_e2e: sprite/button/text fade ok')
PY
echo "ui_list_fade_mix_e2e: ok"
