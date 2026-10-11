#!/usr/bin/env bash
# 真实输入通路端到端（docs/19）：OS/SDL 事件 → 平台翻译 → 输入模块 → 命中测试 → 脚本回调。
#   ① 无显示环境自动跳过（输入帧只在有窗口时消费：`if (window) mq_platform_poll(...)`）
#   ② MAQUE_SIM_CLICK="帧:x:y[;...]" 把指针事件压入 **SDL 事件队列** → 走与真实鼠标相同的翻译路径
#      （本机 Wayland 会话下 XTest 注入无效，故用 SDL 队列；见 tools/xclick.py 说明）
#   ③ 断言：脚本收到 on_pointer_down 并在点击处生成球；窗口像素亮区随球数增加
#   ④ 断言：窗口坐标 → 世界坐标映射正确（窗口 800x600 显示世界 320x240 → 2.5x）
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "input_click_e2e: skip（无显示环境）"
  exit 77                                   # CTest SKIP_RETURN_CODE
fi

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS

count_bright() {   # $1 = ppm
  python3 - "$1" <<'PY'
import sys
parts = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
n = 0
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if px[i] + px[i + 1] + px[i + 2] > 90: n += 1
print(n)
PY
}

# ---- 基线：无点击（单球）----
MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine MAQUE_VIEWS=1 MAQUE_KEEP_OPEN=1 \
MAQUE_CAPTURE="$WORK/base" MAQUE_CAPTURE_FRAMES=3 MAQUE_CAPTURE_EVERY=20 MAQUE_MAX_FRAMES=70 \
  "$PLAYER" "$SCENE" > "$WORK/base.log" 2>&1 || true
[ -f "$WORK/base_0.ppm" ] || { echo "input_click_e2e: FAIL 基线未产生窗口抓帧" >&2; exit 1; }
grep -q 'ball: click' "$WORK/base.log" && { echo "input_click_e2e: FAIL 基线不应有点击" >&2; exit 1; }
BASE=$(count_bright "$WORK/base_2.ppm")

# ---- 注入两次点击（窗口坐标 (100,150) 与 (600,450)）----
MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine MAQUE_VIEWS=1 MAQUE_KEEP_OPEN=1 \
MAQUE_SIM_CLICK="10:100:150;40:600:450" \
MAQUE_CAPTURE="$WORK/click" MAQUE_CAPTURE_FRAMES=6 MAQUE_CAPTURE_EVERY=20 MAQUE_MAX_FRAMES=130 \
  "$PLAYER" "$SCENE" > "$WORK/click.log" 2>&1 || true

grep -q 'ball: click #1 spawn' "$WORK/click.log" || { echo "input_click_e2e: FAIL 第 1 次点击未到达脚本" >&2; exit 1; }
grep -q 'ball: click #2 spawn' "$WORK/click.log" || { echo "input_click_e2e: FAIL 第 2 次点击未到达脚本" >&2; exit 1; }
# 窗口坐标 → 世界坐标（800x600 显示 320x240 世界 → 2.5x，且 y 轴翻转：150/2.5=60 → 240-60=180）
grep -q 'at (40,180)' "$WORK/click.log" || { echo "input_click_e2e: FAIL 坐标映射(40,180) 不符" >&2; exit 1; }
grep -q 'at (240,60)' "$WORK/click.log" || { echo "input_click_e2e: FAIL 坐标映射(240,60) 不符" >&2; exit 1; }
grep -q 'balls=3' "$WORK/click.log" || { echo "input_click_e2e: FAIL 球数应为 3" >&2; exit 1; }

AFTER=$(count_bright "$WORK/click_5.ppm")
[ "$AFTER" -gt "$BASE" ] || { echo "input_click_e2e: FAIL 点击后画面未增加球（$AFTER <= $BASE）" >&2; exit 1; }

echo "input_click_e2e: ok（点击 2 次全部到达脚本；亮像素 $BASE → $AFTER）"
exit 0
