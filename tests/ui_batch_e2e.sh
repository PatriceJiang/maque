#!/usr/bin/env bash
# UI drawcall 合批端到端（docs/13 §2D/UI；实现 modules/render/gfx/src/ui_batch.{h,cpp}）。
# 场景 scenes/ui_batch/ui_batch.scn：4×4 按钮 + 每个按钮上的文字 + 标题（另一字号图集）
#   + 一组"面板压在文字上"的顺序反转用例。
#   ① 分组开启：items 很多、draws 很少（按渲染状态分组）
#   ② 对照组 MAQUE_UI_BATCH=0（画家序）：draws 明显更多
#   ③ **A/B 逐字节一致**：分组只改提交顺序，不改像素（顺序安全保证的最终判据）
#   ④ 发生至少一次"顺序安全切段"（面板压文字用例；否则顺序会被翻转）
#   ⑤ 确定性：同命令两跑 → 逐字节一致
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

export MAQUE_HEADLESS=1                   # 纯离屏：不需要显示/窗口
rm -rf "$WORK"; mkdir -p "$WORK"

fail() { echo "ui_batch_e2e: FAIL $*" >&2; exit 1; }

# shot <out.ppm> [env...]：渲染一帧 → PPM + [ui_batch] 统计
shot() {
    local out="$1"; shift
    env "$@" MAQUE_TRACE_BATCH=1 "$PLAYER" --shot "$out" "$SCENE" 2>&1
}

ON_LOG=$(shot "$WORK/on.ppm")
OFF_LOG=$(shot "$WORK/off.ppm" MAQUE_UI_BATCH=0)
AGAIN_LOG=$(shot "$WORK/on_again.ppm")

[ -s "$WORK/on.ppm" ] || fail "分组模式没有产出 PPM（$ON_LOG）"
[ -s "$WORK/off.ppm" ] || fail "画家序模式没有产出 PPM（$OFF_LOG）"

pick() { printf '%s\n' "$1" | grep -o '\[ui_batch\] items=.*' | tail -1; }
ON=$(pick "$ON_LOG")
OFF=$(pick "$OFF_LOG")
AGAIN=$(pick "$AGAIN_LOG")
[ -n "$ON" ] || fail "缺少 [ui_batch] 统计（分组）：$ON_LOG"
[ -n "$OFF" ] || fail "缺少 [ui_batch] 统计（画家序）：$OFF_LOG"
[ "$ON" = "$AGAIN" ] || fail "同命令两次的批次统计不一致：'$ON' vs '$AGAIN'"

# ① ② ④：draw 数下降 + 切段发生
"$PY" - "$ON" "$OFF" <<'PY' || exit 1
import re, sys

def parse(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}

on, off = parse(sys.argv[1]), parse(sys.argv[2])
bad = []
if on.get('items', 0) != off.get('items', -1):
    bad.append(f"items 不一致 {on.get('items')} vs {off.get('items')}")
if on.get('items', 0) < 80:
    bad.append(f"场景图元太少（{on.get('items')}），压力不足")
if on.get('draws', 0) > 8:
    bad.append(f"分组后 draw 仍太多：{on.get('draws')}")
if off.get('draws', 0) < 10:
    bad.append(f"对照组 draw 太少（{off.get('draws')}），看不出收益")
if on.get('draws', 0) * 2 > off.get('draws', 1):
    bad.append(f"draw 下降不足：{off.get('draws')} → {on.get('draws')}（期望 ≤ 一半）")
if on.get('segments', 0) < 1:
    bad.append("没有触发顺序安全切段（面板压文字用例未生效？）")
if bad:
    for b in bad:
        print('ui_batch_e2e:', b)
    sys.exit(1)
print(f"ui_batch_e2e: items={on['items']} draws {off['draws']} → {on['draws']} "
      f"(segments={on.get('segments', 0)})")
PY

# ③ 分组只是改顺序，不改像素
cmp -s "$WORK/on.ppm" "$WORK/off.ppm" || fail "分组开启/关闭的像素不一致（顺序安全性被破坏）"
# ⑤ 确定性
cmp -s "$WORK/on.ppm" "$WORK/on_again.ppm" || fail "同命令两跑像素不一致（确定性）"

# 画面非空白（PPM P6：解析头后数不同颜色数）
"$PY" - "$WORK/on.ppm" <<'PY' || exit 1
import sys
raw = open(sys.argv[1], 'rb').read()
parts = raw.split(b'\n', 3)
assert parts[0] == b'P6', f'不是 P6：{parts[0]!r}'
w, h = map(int, parts[1].split())
px = parts[3]
assert len(px) == w * h * 3, f'像素字节数不符 {len(px)} != {w*h*3}'
colors = {px[i:i + 3] for i in range(0, len(px), 3)}
assert len(colors) > 16, f'画面近似空白（{len(colors)} 种颜色）'
PY

echo "ui_batch_e2e: ok"
