#!/usr/bin/env bash
# UI 新功能**独立场景**回归。每个 scene 只测一件事，可按 MODE 单独跑：
#
#   nine_patch.scn       MODE=nine        9-patch 单独：角/边/中心像素 + 9 块合成 1 draw + 图集 A/B
#   mask_normal.scn      MODE=normal      普通 Rect Mask 单独：洞内图片 / 洞外背景 + 2 draws
#   mask_reverse.scn     MODE=reverse     reverse Mask 单独：洞内背景 / 洞外四向图片 + 背景+4 条带
#   nine_patch_mask.scn  MODE=nine_mask   9-patch + reverse 集成：9 块按带裁剪后仍 4 批
#   mask_hit.scn         MODE=hit         嵌套 Mask + clip_children=false 的命中矩阵
#   nine_patch_gallery.scn  MODE=nine_gallery 9-patch 用法对比（尺寸/翻转/region/无 Control/合批）
#   mask_gallery.scn     MODE=mask_gallery Mask 用法对比（normal/reverse/nested/clip_self/button/text）
#
# MODE=all 依次跑全部（聚合回归）。mask 场景的 ui.Image **不带** nine_patch_margin，
# 9-patch 场景**不带** ui.Mask，互不借道；mask 子图统一用本目录 1024×1536 的 girl.png
# （大图不进 UI 图集，顺便覆盖“裁剪 + 非图集大图”这条路径）。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE_DIR="$2"
WORK="$3"
MODE="${4:-all}"

export MAQUE_HEADLESS=1
rm -rf "$WORK"; mkdir -p "$WORK"
fail() { echo "ui_mask_nine_e2e[$MODE]: FAIL $*" >&2; exit 1; }
shot() { local out="$1" scene="$2"; shift 2; env "$@" MAQUE_TRACE_BATCH=1 "$PLAYER" --shot "$out" "$scene" 2>&1; }
stat_of() { printf '%s\n' "$1" | grep -o '\[ui_batch\] items=.*' | tail -1; }

run_nine() {
    local scene="$SCENE_DIR/nine_patch.scn"
    local log; log=$(shot "$WORK/nine.ppm" "$scene")
    [ -s "$WORK/nine.ppm" ] || fail "9-patch 没有产出 PPM（$log）"
    local st; st=$(stat_of "$log")
    [ -n "$st" ] || fail "9-patch 缺少 [ui_batch] 统计：$log"
    "$PY" - "$st" <<'PY' || exit 1
import re, sys
st = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', sys.argv[1])}
bad = []
if st.get('items') != 10: bad.append(f"items={st.get('items')}（9 块 + 背景应为 10）")
if st.get('draws') != 2: bad.append(f"9 块未合成一次 draw：draws={st.get('draws')}")
if st.get('merges', 0) < 8: bad.append(f"merges={st.get('merges')}（9 块应合并 8 次）")
if st.get('atlas_items') != 9: bad.append(f"atlas_items={st.get('atlas_items')}（9 块都应进 UI 图集）")
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
print('ui_mask_nine_e2e: [nine_patch]', st)
PY
    "$PY" - "$WORK/nine.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read(); parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def c(x, y):
    i = (y * w + x) * 3; return tuple(px[i:i+3])
red, green, blue, yellow = (255,0,0), (0,255,0), (0,0,255), (255,255,0)
checks = [
    ('tl', c(12, 12), red), ('tm', c(40, 12), green), ('tr', c(84, 12), red),
    ('ml', c(12, 48), blue), ('center', c(48, 48), yellow), ('mr', c(84, 48), blue),
    ('bl', c(12, 84), red), ('bm', c(48, 84), green), ('br', c(84, 84), red),
]
bad = [f'{n}: {got} != {want}' for n, got, want in checks if got != want]
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
PY
    shot "$WORK/nine_nobatch.ppm" "$scene" MAQUE_UI_BATCH=0 >/dev/null 2>&1
    shot "$WORK/nine_noatlas.ppm" "$scene" MAQUE_UI_ATLAS=0 >/dev/null 2>&1
    cmp -s "$WORK/nine.ppm" "$WORK/nine_nobatch.ppm" || fail "9-patch MAQUE_UI_BATCH=0 像素不一致"
    cmp -s "$WORK/nine.ppm" "$WORK/nine_noatlas.ppm" || fail "9-patch MAQUE_UI_ATLAS=0 像素不一致"
    shot "$WORK/nine_again.ppm" "$scene" >/dev/null 2>&1
    cmp -s "$WORK/nine.ppm" "$WORK/nine_again.ppm" || fail "9-patch 同命令两跑像素不一致"
}

run_normal() {
    local scene="$SCENE_DIR/mask_normal.scn"
    local log; log=$(shot "$WORK/normal.ppm" "$scene")
    [ -s "$WORK/normal.ppm" ] || fail "普通 Mask 没有产出 PPM（$log）"
    local st; st=$(stat_of "$log")
    "$PY" - "$WORK/normal.ppm" "$st" <<'PY' || exit 1
import re, sys
d = open(sys.argv[1], 'rb').read(); parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def c(x, y):
    i = (y * w + x) * 3; return tuple(px[i:i+3])
bg = (13, 15, 20)
bad = []
if c(48, 48) == bg: bad.append("普通 Mask 洞内仍是背景（应该显示图片）")
if c(16, 16) != bg: bad.append(f"普通 Mask 洞外不是背景：{c(16,16)}")
st = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', sys.argv[2])}
if st.get('draws') != 2: bad.append(f"普通 Mask 应只有背景+图片 2 次 draw：{st.get('draws')}")
if st.get('segments'): bad.append(f"普通 Mask 不应切段：{st.get('segments')}")
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
print('ui_mask_nine_e2e: [mask_normal]', st)
PY
}

run_reverse() {
    local scene="$SCENE_DIR/mask_reverse.scn"
    local log; log=$(shot "$WORK/reverse.ppm" "$scene")
    [ -s "$WORK/reverse.ppm" ] || fail "reverse Mask 没有产出 PPM（$log）"
    local st; st=$(stat_of "$log")
    "$PY" - "$WORK/reverse.ppm" "$st" <<'PY' || exit 1
import re, sys
d = open(sys.argv[1], 'rb').read(); parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def c(x, y):
    i = (y * w + x) * 3; return tuple(px[i:i+3])
bg = (13, 15, 20)
bad = []
if c(48, 48) != bg: bad.append(f"reverse 洞中心不是背景：{c(48,48)}")
for name, x, y in [('左上',16,16), ('左中',16,48), ('右中',80,48), ('上中',48,16), ('下中',48,80)]:
    if c(x, y) == bg: bad.append(f"reverse {name} 仍是背景（对应条带丢了）")
st = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', sys.argv[2])}
if st.get('draws') != 5: bad.append(f"reverse 应为背景+4 条带（5 draws）：{st.get('draws')}")
if st.get('segments'): bad.append(f"reverse 被无谓切段：{st.get('segments')}")
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
print('ui_mask_nine_e2e: [mask_reverse]', st)
PY
    shot "$WORK/reverse_again.ppm" "$scene" >/dev/null 2>&1
    cmp -s "$WORK/reverse.ppm" "$WORK/reverse_again.ppm" || fail "reverse 同命令两跑像素不一致"
}

run_nine_mask() {
    local scene="$SCENE_DIR/nine_patch_mask.scn"
    local log; log=$(shot "$WORK/nine_mask.ppm" "$scene")
    [ -s "$WORK/nine_mask.ppm" ] || fail "9-patch + reverse 没有产出 PPM（$log）"
    local st; st=$(stat_of "$log")
    "$PY" - "$WORK/nine_mask.ppm" "$st" <<'PY' || exit 1
import re, sys
d = open(sys.argv[1], 'rb').read(); parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def c(x, y):
    i = (y * w + x) * 3; return tuple(px[i:i+3])
bg = (13, 15, 20)
bad = []
if c(48, 48) != bg: bad.append(f"9-patch reverse 洞中心不是背景：{c(48,48)}")
if c(16, 16) == bg: bad.append("9-patch reverse 洞外没有图片")
st = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', sys.argv[2])}
if st.get('draws') != 5: bad.append(f"9-patch + reverse 应按带合批为 5 draws：{st.get('draws')}")
if st.get('segments'): bad.append(f"9-patch + reverse 被无谓切段：{st.get('segments')}")
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
print('ui_mask_nine_e2e: [nine_patch_mask]', st)
PY
}


run_nine_gallery() {
    local scene="$SCENE_DIR/nine_patch_gallery.scn"
    local log; log=$(shot "$WORK/nine_gallery.ppm" "$scene")
    [ -s "$WORK/nine_gallery.ppm" ] || fail "9-patch gallery 没有产出 PPM（$log）"
    local st; st=$(stat_of "$log")
    "$PY" - "$st" <<'PY' || exit 1
import re, sys
st = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', sys.argv[1])}
bad = []
if st.get('atlas_items') != 63: bad.append(f"7 个 9-patch × 9 块应为 63 个图集图元：{st.get('atlas_items')}")
if st.get('draws', 99) > 4: bad.append(f"9-patch gallery 应约 3-4 draws（背景/图集/文字）：{st.get('draws')}")
if st.get('segments'): bad.append(f"9-patch gallery 不应切段：{st.get('segments')}")
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
print('ui_mask_nine_e2e: [nine_patch_gallery]', st)
PY
    "$PY" - "$WORK/nine_gallery.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read(); parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def c(x, y):
    i = (y * w + x) * 3; return tuple(px[i:i+3])
R,O,Y=(255,0,0),(255,128,0),(255,255,0)
G,B,C=(0,255,0),(0,0,255),(0,255,255)
M,P,W=(255,0,255),(128,0,255),(255,255,255)
bad=[]
def expect(name, pts, colors):
    for (x,y), want in zip(pts, colors):
        got=c(x,y)
        if got != want: bad.append(f'{name} ({x},{y}) {got} != {want}')
expect('normal', [(24,24),(60,24),(96,24),(24,60),(60,60),(96,60),(24,96),(60,96),(96,96)],
       [R,O,Y,G,B,C,M,P,W])
expect('small', [(144,24),(150,24),(156,24),(144,30),(150,30),(156,30),(144,36),(150,36),(156,36)],
       [R,O,Y,G,B,C,M,P,W])
expect('wide', [(204,24),(260,24),(316,24),(204,40),(260,40),(316,40),(204,56),(260,56),(316,56)],
       [R,O,Y,G,B,C,M,P,W])
expect('flip_h', [(364,24),(400,24),(436,24),(364,60),(400,60),(436,60),(364,96),(400,96),(436,96)],
       [Y,O,R,C,B,G,W,P,M])
expect('flip_v', [(484,24),(520,24),(556,24),(484,60),(520,60),(556,60),(484,96),(520,96),(556,96)],
       [M,P,W,G,B,C,R,O,Y])
expect('region', [(24,164),(60,164),(96,164),(24,200),(60,200),(96,200),(24,236),(60,236),(96,236)],
       [R,O,Y,G,B,C,M,P,W])
expect('noctl', [(144,164),(152,164),(160,164),(144,172),(152,172),(160,172),(144,180),(152,180),(160,180)],
       [R,O,Y,G,B,C,M,P,W])
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
PY
    shot "$WORK/nine_gallery_nobatch.ppm" "$scene" MAQUE_UI_BATCH=0 >/dev/null 2>&1
    shot "$WORK/nine_gallery_noatlas.ppm" "$scene" MAQUE_UI_ATLAS=0 >/dev/null 2>&1
    cmp -s "$WORK/nine_gallery.ppm" "$WORK/nine_gallery_nobatch.ppm" || fail "9-patch gallery MAQUE_UI_BATCH=0 像素不一致"
    cmp -s "$WORK/nine_gallery.ppm" "$WORK/nine_gallery_noatlas.ppm" || fail "9-patch gallery MAQUE_UI_ATLAS=0 像素不一致"
    shot "$WORK/nine_gallery_again.ppm" "$scene" >/dev/null 2>&1
    cmp -s "$WORK/nine_gallery.ppm" "$WORK/nine_gallery_again.ppm" || fail "9-patch gallery 同命令两跑像素不一致"
}

run_mask_gallery() {
    local scene="$SCENE_DIR/mask_gallery.scn"
    local log; log=$(shot "$WORK/mask_gallery.ppm" "$scene")
    [ -s "$WORK/mask_gallery.ppm" ] || fail "Mask gallery 没有产出 PPM（$log）"
    local st; st=$(stat_of "$log")
    "$PY" - "$WORK/mask_gallery.ppm" "$st" <<'PY' || exit 1
import re, sys
d = open(sys.argv[1], 'rb').read(); parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def c(x, y):
    i = (y * w + x) * 3; return tuple(px[i:i+3])
bg = (13, 15, 20)
bad=[]
def nonbg(p): return c(*p) != bg
if c(70,90) == bg: bad.append("normal 洞内没有图片")
if c(140,90) != bg: bad.append(f"normal 洞外不是背景：{c(140,90)}")
if c(30,50) == bg: bad.append("normal 左上角没有图片")
if c(250,90) != bg: bad.append(f"reverse 洞中心不是背景：{c(250,90)}")
if not nonbg((180,90)): bad.append("reverse 左侧条带丢失")
if not nonbg((250,20)): bad.append("reverse 上方条带丢失")
if not nonbg((400,60)): bad.append("nested 外环没有图片")
if c(440,100) != bg: bad.append(f"nested 内洞不是背景：{c(440,100)}")
if c(510,100) != bg: bad.append(f"nested 外框之外不是背景：{c(510,100)}")
if not nonbg((60,260)): bad.append("clip_self 自身可见区没有按钮")
if c(10,260) != bg: bad.append(f"clip_self 自身越界仍被画：{c(10,260)}")
if not nonbg((120,260)): bad.append("clip_self 子节点被误裁")
if not nonbg((220,250)): bad.append("button 裁剪后可见区为空")
if c(280,250) != bg: bad.append(f"button 越界仍被画：{c(280,250)}")
if c(220,230) != bg: bad.append(f"button 上方越界仍被画：{c(220,230)}")
inside = sum(1 for y in range(220,244) for x in range(380,480) if c(x,y) != bg)
right  = sum(1 for y in range(220,244) for x in range(480,560) if c(x,y) != bg)
below  = sum(1 for y in range(244,270) for x in range(380,480) if c(x,y) != bg)
if inside < 100: bad.append(f"text 裁剪后 mask 内几乎没有字：{inside}")
if right != 0: bad.append(f"text 裁剪右边界泄漏：{right}")
if below != 0: bad.append(f"text 裁剪下边界泄漏：{below}")
st = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', sys.argv[2])}
if st.get('segments'): bad.append(f"Mask gallery 被无谓切段：{st.get('segments')}")
if st.get('draws', 99) > 20: bad.append(f"Mask gallery draw 异常多：{st.get('draws')}")
if bad:
    for b in bad: print('ui_mask_nine_e2e:', b)
    sys.exit(1)
print('ui_mask_nine_e2e: [mask_gallery]', st)
PY
    shot "$WORK/mask_gallery_again.ppm" "$scene" >/dev/null 2>&1
    cmp -s "$WORK/mask_gallery.ppm" "$WORK/mask_gallery_again.ppm" || fail "Mask gallery 同命令两跑像素不一致"
}

run_hit() {
    local scene="$SCENE_DIR/mask_hit.scn"
    local log; log=$(env MAQUE_HEADLESS=1 "$PLAYER" --frames 3 "$scene" 2>&1)
    for probe in nested_ring nested_hole nested_out self_in self_out child_hit; do
        printf '%s\n' "$log" | grep -q "hit_$probe=.* OK" || fail "命中矩阵 $probe 未通过：$log"
    done
    printf '%s\n' "$log" | grep -q ' FAIL' && fail "命中矩阵存在 FAIL：$log"
    echo "ui_mask_nine_e2e: [mask_hit] nested + clip_self ok"
}

case "$MODE" in
    all) run_nine; run_normal; run_reverse; run_nine_mask; run_nine_gallery; run_mask_gallery; run_hit ;;
    nine|nine_patch) run_nine ;;
    normal|mask_normal) run_normal ;;
    reverse|mask_reverse) run_reverse ;;
    nine_mask|nine_patch_mask) run_nine_mask ;;
    nine_gallery|nine_patch_gallery) run_nine_gallery ;;
    mask_gallery|mask) run_mask_gallery ;;
    hit|mask_hit) run_hit ;;
    *) fail "未知 MODE '$MODE'（all/nine/normal/reverse/nine_mask/nine_gallery/mask_gallery/hit）" ;;
esac

echo "ui_mask_nine_e2e: ok（MODE=$MODE）"
