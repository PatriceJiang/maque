#!/usr/bin/env bash
# ui.ListView：滚动 + 视口裁剪（headless PPM）。
# 用法：ui_list_e2e.sh <player> <work> [scene]   （默认 scenes/ui_list/list.scn）
#   ① 滚轮在视口内：帧 1 与帧 8 的差异全部落在 ListView 矩形内
#   ② 视口外（含视口下沿）必须完全不变 → 内容没有泄漏
#   ③ 指针在视口外滚轮：两帧逐字节一致
#   ④ 视口内有文字/子控件像素，滚动条 thumb 存在
#   ⑤ 拖拽滚动同样只改变视口内像素
set -u
. "$(dirname "$0")/maque_python.sh"
PLAYER="$1"; WORK="$2"; mkdir -p "$WORK"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCENE="${3:-$ROOT/scenes/ui_list/list.scn}"
PPM="$WORK/ui_list"

run_seq() {
    local sim="$1"; local prefix="$2"; local frames="${3:-1,8}"
    env MAQUE_HEADLESS=1 MAQUE_SIM="$sim" "$PLAYER" \
        --shot-seq "${prefix}_{}.ppm" --shot-frames "$frames" "$SCENE" >"$WORK/ui_list.log" 2>&1 \
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
# 滚轮后滚动条 thumb 必须存在（右侧 x≈590..604 应有亮色像素，不能只有 track/背景）
bar_px = sum(1 for y in range(60, 320) for x in range(590, 605)
             if sum(a[(y * w + x) * 3:(y * w + x) * 3 + 3]) > 300)
if bar_px < 100:
    print(f'ui_list_e2e: 滚动条 thumb 像素太少 {bar_px}')
    sys.exit(1)
# 视口下沿外的一小条必须是背景（防“只是整体平移、没裁剪”）
for y in range(329, 335):
    for x in range(32, 608):
        i = (y * w + x) * 3
        if tuple(a[i:i+3]) != (11, 14, 19):
            print(f'ui_list_e2e: 视口下沿泄漏 ({x},{y}) {tuple(a[i:i+3])}')
            sys.exit(1)
print(f'ui_list_e2e: scroll in={diff_in} out={diff_out} text={text_px} bar={bar_px}')
PY
run_seq "$ROOT/tests/data/sim/ui_list_scroll_outside.sim.json" "$WORK/ui_list_outside"
cmp -s "${WORK}/ui_list_outside_1.ppm" "${WORK}/ui_list_outside_8.ppm" \
    || { echo "ui_list_e2e: 视口外滚轮仍滚动了内容"; exit 1; }

# 拖拽滚动：在视口内按下并向上拖，内容变化仍必须全部落在视口内。
run_seq "$ROOT/tests/data/sim/ui_list_drag.sim.json" "$WORK/ui_list_drag"
"$PY" - "${WORK}/ui_list_drag_1.ppm" "${WORK}/ui_list_drag_8.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
inside = outside = 0
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if a[i:i+3] == b[i:i+3]: continue
        if 32 <= x < 608 and 58 <= y < 328: inside += 1
        else: outside += 1
if inside < 1000 or outside:
    print(f'ui_list_e2e: drag inside={inside} outside={outside}')
    sys.exit(1)
print(f'ui_list_e2e: drag in={inside} out={outside}')
PY

# 滚动条可触摸：按在 thumb 上往下拖，内容应滚动且不泄漏。
run_seq "$ROOT/tests/data/sim/ui_list_scrollbar.sim.json" "$WORK/ui_list_bar"
"$PY" - "${WORK}/ui_list_bar_1.ppm" "${WORK}/ui_list_bar_8.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
inside = outside = 0
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if a[i:i+3] == b[i:i+3]: continue
        if 32 <= x < 608 and 58 <= y < 328: inside += 1
        else: outside += 1
if inside < 1000 or outside:
    print(f'ui_list_e2e: scrollbar inside={inside} outside={outside}')
    sys.exit(1)
print(f'ui_list_e2e: scrollbar in={inside} out={outside}')
PY

# 惯性：释放后继续滑动（帧 8 与帧 20 仍有差异），且不泄漏到视口外。
run_seq "$ROOT/tests/data/sim/ui_list_fling.sim.json" "$WORK/ui_list_fling" "1,8,20"
"$PY" - "${WORK}/ui_list_fling_8.ppm" "${WORK}/ui_list_fling_20.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
inside = outside = 0
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if a[i:i+3] == b[i:i+3]: continue
        if 32 <= x < 608 and 58 <= y < 328: inside += 1
        else: outside += 1
if inside < 1000 or outside:
    print(f'ui_list_e2e: inertia inside={inside} outside={outside}')
    sys.exit(1)
print(f'ui_list_e2e: inertia in={inside} out={outside}')
PY

# 子节点列表的剔除：20 行只有视口附近的项应提交（无剔除会 >1000 个图元）。
if [[ "$SCENE" == *ui_list/items.scn ]]; then
    trace=$(env MAQUE_HEADLESS=1 MAQUE_TRACE_BATCH=1 "$PLAYER" --frames 2 "$SCENE" 2>&1)
    items=$(printf '%s\n' "$trace" | sed -n 's/.*\[ui_batch\] items=\([0-9][0-9]*\) .*/\1/p' | tail -1)
    [ -n "$items" ] || { echo "ui_list_e2e: 没有 ui_batch 统计"; exit 1; }
    [ "$items" -lt 700 ] || { echo "ui_list_e2e: 视口外子节点未剔除 items=$items"; exit 1; }
    echo "ui_list_e2e: culled items=$items"
fi
echo "ui_list_e2e: ok"
