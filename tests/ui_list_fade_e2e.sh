#!/usr/bin/env bash
# ListView 程序化 alpha mask：percent 单位 + 一次性 content_alpha 淡入 + 稳定后零抖动。
set -e
. "$(dirname "$0")/maque_python.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLAYER="${1:-${PLAYER:-$ROOT/build/samples/player/maque_player}}"
WORK="${2:-${TMPDIR:-/tmp}/maque_ui_list_fade.$$}"
SCENE="${3:-$ROOT/scenes/ui_list/fade.scn}"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
run_shot() { env MAQUE_HEADLESS=1 "$PLAYER" --shot "$WORK/$1.ppm" --shot-frame "$2" "$SCENE" >"$WORK/$1.log" 2>&1 || { cat "$WORK/$1.log"; exit 1; }; }
run_shot fade_120 120
run_shot fade_240 240
env MAQUE_HEADLESS=1 MAQUE_UI_ALPHA_MASK=0 "$PLAYER" --shot "$WORK/nomask_120.ppm" --shot-frame 120 "$SCENE" >"$WORK/nomask.log" 2>&1 || { cat "$WORK/nomask.log"; exit 1; }
"$PY" - "$WORK/fade_120.ppm" "$WORK/fade_240.ppm" "$WORK/nomask_120.ppm" <<'PY'
import sys
def load(p):
    d = open(p, 'rb').read(); h = d.split(b'\n', 3)
    w, hh = map(int, h[1].split()); return w, hh, h[3]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2]); _, _, n = load(sys.argv[3])
def px(p, x, y): return tuple(p[(y * w + x) * 3:(y * w + x) * 3 + 3])
# 1s 淡入结束后必须保持稳定（不能持续跳动）。
if a != b:
    d = sum(1 for i in range(0, len(a), 3) if a[i:i+3] != b[i:i+3])
    print(f'ui_list_fade_e2e: 淡入结束后仍在变化 diff={d}')
    sys.exit(1)
# percent 边缘 fade：顶部行必须比关闭 mask 时暗，中间行不变。
top, mid, bar = px(a, 75, 80), px(a, 75, 180), px(a, 601, 90)
ntop, nmid, nbar = px(n, 75, 80), px(n, 75, 180), px(n, 601, 90)
if sum(top) >= sum(ntop) * 0.9:
    print(f'ui_list_fade_e2e: Percent fade 顶部未生效 {top} vs {ntop}')
    sys.exit(1)
if mid != nmid:
    print(f'ui_list_fade_e2e: 中间行被误淡出 {mid} vs {nmid}')
    sys.exit(1)
if bar != nbar:
    print(f'ui_list_fade_e2e: 滚动条被内容 fade 影响 {bar} vs {nbar}')
    sys.exit(1)
print(f'ui_list_fade_e2e: stable, percent top={sum(top)}/{sum(ntop)} mid={mid}')
PY
echo "ui_list_fade_e2e: ok"
