#!/usr/bin/env bash
# UI 新功能**独立场景**回归。每个 scene 只测一件事，可按 MODE 单独跑：
#
#   nine_patch.scn       MODE=nine        9-patch 单独：角/边/中心像素 + 9 块合成 1 draw + 图集 A/B
#   mask_normal.scn      MODE=normal      普通 Rect Mask 单独：洞内图片 / 洞外背景 + 2 draws
#   mask_reverse.scn     MODE=reverse     reverse Mask 单独：洞内背景 / 洞外四向图片 + 背景+4 条带
#   nine_patch_mask.scn  MODE=nine_mask   9-patch + reverse 集成：9 块按带裁剪后仍 4 批
#   mask_hit.scn         MODE=hit         reverse 命中：洞内 ui_hit_at=-1 / 洞外命中
#
# MODE=all 依次跑全部（聚合回归）。mask 场景的 ui.Image **不带** nine_patch_margin，
# 9-patch 场景**不带** ui.Mask，互不借道。
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

run_hit() {
    local scene="$SCENE_DIR/mask_hit.scn"
    local log; log=$(env MAQUE_HEADLESS=1 "$PLAYER" --frames 3 "$scene" 2>&1)
    printf '%s\n' "$log" | grep -q 'hit_hole=-1' || fail "reverse 洞内仍命中：$log"
    printf '%s\n' "$log" | grep -Eq 'hit_out=[0-9]+' || fail "reverse 洞外没有命中：$log"
    echo "ui_mask_nine_e2e: [mask_hit] ok"
}

case "$MODE" in
    all) run_nine; run_normal; run_reverse; run_nine_mask; run_hit ;;
    nine|nine_patch) run_nine ;;
    normal|mask_normal) run_normal ;;
    reverse|mask_reverse) run_reverse ;;
    nine_mask|nine_patch_mask) run_nine_mask ;;
    hit|mask_hit) run_hit ;;
    *) fail "未知 MODE '$MODE'（all/nine/normal/reverse/nine_mask/hit）" ;;
esac

echo "ui_mask_nine_e2e: ok（MODE=$MODE）"
