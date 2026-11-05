#!/usr/bin/env bash
# UI 动态合图端到端（docs/13 §2D/UI；实现 modules/render/gfx/src/ui_atlas.{h,cpp}）。
# 场景 scenes/ui_batch/ui_atlas.scn：7 个 ui.Image（4 张不同小图：原生尺寸 / 2× 放大 /
# UV 子矩形+翻转 / 无 ui.Control 走贴图缺省尺寸）+ 1 个世界层 scn.Sprite（同一张小图）
# + 文字（字体图集）+ 背景。精灵与 UI 图片共用同一张贴图缓存 → 同一图集区域。
#   ① 图集开启：4 张不同小图并进同一张 1024² → drawcall 明显下降
#   ② 对照组 MAQUE_UI_ATLAS=0：每张贴图各一次 draw
#   ③ **字体图集不进 UI 图集**：atlas == 4（只有 4 张 UI 图片；若把字体图集也吸进去会是 5）
#   ④ A/B 像素逐字节一致（图集只改纹理来源/UV，不改画面）
#   ⑤ 确定性：同命令两跑逐字节一致
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

export MAQUE_HEADLESS=1
rm -rf "$WORK"; mkdir -p "$WORK"

fail() { echo "ui_atlas_e2e: FAIL $*" >&2; exit 1; }

shot() {   # shot <out.ppm> [env...]
    local out="$1"; shift
    env "$@" MAQUE_TRACE_BATCH=1 "$PLAYER" --shot "$out" "$SCENE" 2>&1
}

ON_LOG=$(shot "$WORK/on.ppm")
OFF_LOG=$(shot "$WORK/off.ppm" MAQUE_UI_ATLAS=0)
AGAIN_LOG=$(shot "$WORK/on_again.ppm")

[ -s "$WORK/on.ppm" ] || fail "图集模式没有产出 PPM（$ON_LOG）"
[ -s "$WORK/off.ppm" ] || fail "对照组没有产出 PPM（$OFF_LOG）"

pick() { printf '%s\n' "$1" | grep -o '\[ui_batch\] items=.*' | tail -1; }
ON=$(pick "$ON_LOG")
OFF=$(pick "$OFF_LOG")
AGAIN=$(pick "$AGAIN_LOG")
[ -n "$ON" ] || fail "缺少 [ui_batch] 统计（图集）：$ON_LOG"
[ -n "$OFF" ] || fail "缺少 [ui_batch] 统计（对照）：$OFF_LOG"
[ "$ON" = "$AGAIN" ] || fail "同命令两次的统计不一致：'$ON' vs '$AGAIN'"

"$PY" - "$ON" "$OFF" <<'PY' || exit 1
import re, sys

def parse(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}

on, off = parse(sys.argv[1]), parse(sys.argv[2])
bad = []
if on.get('items', 0) != off.get('items', -1):
    bad.append(f"items 不一致 {on.get('items')} vs {off.get('items')}")
if on.get('items', 0) < 20:
    bad.append(f"场景图元太少（{on.get('items')}）")
if off.get('atlas', -1) != 0:
    bad.append(f"MAQUE_UI_ATLAS=0 仍有图集打包（atlas={off.get('atlas')}）")
# ① 图集里恰好 4 张 UI 图片；字体图集若被吸进来会变成 5（③）
if on.get('atlas', 0) != 4:
    bad.append(f"图集张数应为 4（仅 UI 图片，字体图集必须分开）：{on.get('atlas')}")
if on.get('atlas_items', 0) != 9:
    bad.append(f"重映射到图集的图元数应为 9（8 个 UI 图片 + 1 个世界精灵）：{on.get('atlas_items')}")
# ② 4 张图 → 1 次；文字 1 次；实心背景 1 次
if on.get('draws', 99) > 5:
    bad.append(f"图集后 draw 仍太多：{on.get('draws')}")
if off.get('draws', 0) < 6:
    bad.append(f"对照组 draw 太少（{off.get('draws')}），看不出收益")
if on.get('draws', 99) >= off.get('draws', 0):
    bad.append(f"draw 没有下降：{off.get('draws')} → {on.get('draws')}")
if bad:
    for b in bad:
        print('ui_atlas_e2e:', b)
    sys.exit(1)
print(f"ui_atlas_e2e: items={on['items']} draws {off['draws']} → {on['draws']} "
      f"(atlas={on['atlas']} 张, atlas_items={on['atlas_items']})")
PY

# ④ 图集只改纹理来源/UV，不改画面
cmp -s "$WORK/on.ppm" "$WORK/off.ppm" || fail "开/关 UI 图集的像素不一致（UV 重映射或外扩边有误）"
# ⑤ 确定性
cmp -s "$WORK/on.ppm" "$WORK/on_again.ppm" || fail "同命令两跑像素不一致（确定性）"

"$PY" - "$WORK/on.ppm" <<'PY' || exit 1
import sys
raw = open(sys.argv[1], 'rb').read()
parts = raw.split(b'\n', 3)
assert parts[0] == b'P6', f'不是 P6：{parts[0]!r}'
w, h = map(int, parts[1].split())
px = parts[3]
assert len(px) == w * h * 3, f'像素字节数不符 {len(px)} != {w*h*3}'
colors = {px[i:i + 3] for i in range(0, len(px), 3)}
assert len(colors) > 64, f'画面近似空白（{len(colors)} 种颜色）'
PY

echo "ui_atlas_e2e: ok"
