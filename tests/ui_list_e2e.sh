#!/usr/bin/env bash
# ui.ListView：大文本滚动 + 视口裁剪（headless PPM）。
#   ① 滚轮在视口内：帧 1 与帧 8 的差异全部落在 ListView 矩形内
#   ② 视口外（含视口下沿）必须完全不变 → 内容没有泄漏
#   ③ 指针在视口外滚轮：两帧逐字节一致
#   ④ 视口内确实有文字（非背景像素足够多）
set -u
. "$(dirname "$0")/maque_python.sh"
PLAYER="$1"; WORK="$2"; mkdir -p "$WORK"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCENE="$ROOT/scenes/ui_list/list.scn"
PPM="$WORK/ui_list"

run_seq() {
    local sim="$1"; local prefix="$2"
    env MAQUE_HEADLESS=1 MAQUE_SIM="$sim" "$PLAYER" \
        --shot-seq "${prefix}_{}.ppm" --shot-frames 1,8 "$SCENE" >"$WORK/ui_list.log" 2>&1 \
        || { cat "$WORK/ui_list.log"; echo "ui_list_e2e: player failed"; exit 1; }
}

run_seq "$ROOT/tests/data/sim/ui_list_scroll.sim.json" "$PPM"
[ -s "${PPM}_1.ppm" ] && [ -s "${PPM}_8.ppm" ] || { echo "ui_list_e2e: missing shot"; exit 1; }
"$PY" - "${PPM}_1.ppm" "${PPM}_8.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
# ListView: node (32,32) + Control 576x270 → screen x 32..608, y 58..328
def inside(x, y): return 32 <= x < 608 and 58 <= y < 328
diff_in = diff_out = text_px = 0
for y in range(h):
    row = y * w * 3
    for x in range(w):
        i = row + x * 3
        if a[i:i+3] != b[i:i+3]:
            if inside(x, y): diff_in += 1
            else: diff_out += 1
        if inside(x, y) and tuple(a[i:i+3]) != (6, 9, 14): text_px += 1
if diff_in < 1000:
    print(f'ui_list_e2e: 滚轮后视口内变化太少 {diff_in}')
    sys.exit(1)
if diff_out != 0:
    print(f'ui_list_e2e: 视口外泄漏 {diff_out} px')
    sys.exit(1)
if text_px < 500:
    print(f'ui_list_e2e: 视口内文字像素太少 {text_px}')
    sys.exit(1)
# 视口下沿外的一小条必须是背景（防“只是整体平移、没裁剪”）
for y in range(329, 335):
    for x in range(32, 608):
        i = (y * w + x) * 3
        if tuple(a[i:i+3]) != (11, 14, 19):
            print(f'ui_list_e2e: 视口下沿泄漏 ({x},{y}) {tuple(a[i:i+3])}')
            sys.exit(1)
print(f'ui_list_e2e: scroll in={diff_in} out={diff_out} text={text_px}')
PY
run_seq "$ROOT/tests/data/sim/ui_list_scroll_outside.sim.json" "$WORK/ui_list_outside"
cmp -s "${WORK}/ui_list_outside_1.ppm" "${WORK}/ui_list_outside_8.ppm" \
    || { echo "ui_list_e2e: 视口外滚轮仍滚动了内容"; exit 1; }
echo "ui_list_e2e: ok"
