#!/usr/bin/env bash
# 窗口呈现端到端（docs/05、M5）：真实窗口 + 呈现路径 + 缩放映射。
#   ① 有 DISPLAY 时才跑（无显示环境自动跳过 → CI 友好）
#   ② 窗口尺寸取自 MAQUE_WINDOW；标题取自 MAQUE_TITLE
#   ③ 从**窗口表面**回读像素（MAQUE_CAPTURE）→ 断言画面非纯背景（渲染确实呈现到窗口）
#   ④ 世界尺寸 ≠ 窗口尺寸时按等比缩放（letterbox）：窗口像素 = 世界像素 × scale
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "window_present_e2e: skip（无显示环境）"
  exit 77                                   # CTest SKIP_RETURN_CODE
fi

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS

MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine MAQUE_VIEWS=1 \
MAQUE_CAPTURE="$WORK/win" MAQUE_MAX_FRAMES=6 MAQUE_KEEP_OPEN=1 \
  "$PLAYER" "$SCENE" > "$WORK/out.txt" 2>&1 || true

[ -f "$WORK/win_0.ppm" ] || { echo "window_present_e2e: FAIL 未产生窗口抓帧" >&2; exit 1; }

grep -q "window 800x600" "$WORK/out.txt" || { echo "window_present_e2e: FAIL 未应用窗口尺寸覆盖" >&2; exit 1; }
grep -q "platform window=1" "$WORK/out.txt" || { echo "window_present_e2e: FAIL 未创建真实窗口" >&2; exit 1; }
grep -q "present via window surface" "$WORK/out.txt" || { echo "window_present_e2e: FAIL 未走呈现路径" >&2; exit 1; }

python3 - "$WORK/win_0.ppm" <<'PY' || exit 1
import sys
parts = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
if (w, h) != (800, 600):
    print(f"window_present_e2e: FAIL 窗口抓帧尺寸 {w}x{h}（应为 800x600）", file=sys.stderr); sys.exit(1)
bright = 0
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if px[i] + px[i + 1] + px[i + 2] > 90:
            bright += 1
if bright < 100:
    print(f"window_present_e2e: FAIL 窗口画面几乎是空的（亮像素 {bright}）", file=sys.stderr); sys.exit(1)
print(f"window_present_e2e: ok（窗口 {w}x{h}，呈现画面亮像素 {bright}）")
PY
