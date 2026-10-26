#!/usr/bin/env bash
# ImGui 窗口呈现/UI 度量端到端（docs/28）：
#   ① **imgui 开启后游戏画面仍然上屏**：游戏帧与 ImGui 面板走同一个 SDL_Renderer，
#      先画游戏帧、再让 ImGui 官方后端在**单独的 render pass** 叠画，最后**只 present 一次**；
#   ② **stats 面板可以拖动**：去掉 NoMove|NoResize，拖动/缩放交给官方后端；
#   ③ **面板大小/字号固定（按屏幕分辨率派生）**：同一块屏上，800x650 与 1200x900 两种
#      游戏窗口打印的 `ui scale / title / body / panel` 度量必须**完全一致**
#     （旧实现按 窗口/渲染目标 比缩放 → 窗口不同面板大小就不同：用户反馈"标题太大/有的窗口又偏小"）。
#
# 验证方式：真实窗口 + MAQUE_CAPTURE（抓的是 present 前的 renderer 后台缓冲 = 用户看到的整帧），
# 比较同一帧的三次运行：imgui 关 / imgui 开 / imgui 开 + 合成指针拖动面板。
# 注入指针走 MAQUE_IMGUI_POINTER（frame:x:y:type），与真实鼠标同一条路径：
#   iframe → imgui_input_feed → ImGui（CI 无真鼠标）。
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCENES="$2"
WORK="$3"

# 无显示环境判定（Linux/Wayland 看 DISPLAY / WAYLAND_DISPLAY；Windows 始终有窗口系统，见 window_present_e2e）
_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "imgui_present_e2e: skip（无显示环境）"
  exit 77                                   # CTest SKIP_RETURN_CODE
fi

fail() { echo "imgui_present_e2e: FAIL $*" >&2; exit 1; }

SCN="$SCENES/spine/spine.scn"
[ -f "$SCN" ] || { echo "imgui_present_e2e: skip（缺 $SCN）"; exit 77; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_KEEP_OPEN MAQUE_IMGUI MAQUE_PRESENT MAQUE_CAPTURE \
      MAQUE_CAPTURE_FRAMES MAQUE_CAPTURE_EVERY MAQUE_IMGUI_POINTER MAQUE_MAX_FRAMES \
      MAQUE_IMGUI_SCALE MAQUE_IMGUI_FONT_SIZE 2>/dev/null || true

# 抓第 5 次 present（= 运行期第 4 帧，拖动事件已在第 2..4 帧注入完）之后的整帧。
COMMON=(MAQUE_WINDOW=800x650 MAQUE_CAPTURE_EVERY=5 MAQUE_CAPTURE_FRAMES=2 MAQUE_MAX_FRAMES=12)

# ---- ① 基线：imgui 关 ----
env "${COMMON[@]}" MAQUE_IMGUI=0 MAQUE_CAPTURE="$WORK/off" \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/off.log" 2>&1 || fail "imgui 关运行失败"
[ -f "$WORK/off_0.ppm" ] || fail "imgui 关：未产生窗口抓帧（$WORK/off.log）"

# ---- ② imgui 开 ----
env "${COMMON[@]}" MAQUE_IMGUI=1 MAQUE_CAPTURE="$WORK/on" \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/on.log" 2>&1 || fail "imgui 开运行失败"
[ -f "$WORK/on_0.ppm" ] || fail "imgui 开：未产生窗口抓帧"
grep -q "present via renderer=" "$WORK/on.log" || fail "未走统一 renderer 呈现（日志缺 'present via renderer='）"
grep -q "official SDL_Renderer backend" "$WORK/on.log" || fail "面板未走官方 SDL_Renderer 后端"

# UI 度量（固定值，供像素区域与"不随窗口变化"断言用）
METRICS="$(grep -o 'runtime: imgui ui scale=.*' "$WORK/on.log" | head -1)"
[ -n "$METRICS" ] || fail "日志缺 UI 度量行（runtime: imgui ui scale=...）"
UI_SCALE="$(printf '%s\n' "$METRICS" | sed -E 's/.*ui scale=([0-9.]+).*/\1/')"
PANEL_W="$(printf '%s\n' "$METRICS" | sed -E 's/.*panel=([0-9]+)x([0-9]+).*/\1/')"
PANEL_H="$(printf '%s\n' "$METRICS" | sed -E 's/.*panel=([0-9]+)x([0-9]+).*/\2/')"
TITLE_PX="$(printf '%s\n' "$METRICS" | sed -E 's/.*title=([0-9]+).*/\1/')"
BODY_PX="$(printf '%s\n' "$METRICS" | sed -E 's/.*body=([0-9]+).*/\1/')"
[ -n "$UI_SCALE" ] && [ -n "$PANEL_W" ] && [ -n "$PANEL_H" ] || fail "无法解析 UI 度量：$METRICS"
echo "imgui_present_e2e: metrics $METRICS"

# 拖动坐标**必须按 UI 度量换算**：面板内边距 pad = 8*ui_scale，标题栏在它下面。
# 写死 (60,16) 在 HiDPI（如 4K 屏 ui_scale=2.22 → pad≈18）会落到标题栏**上面**，
# 按不到标题栏 → 拖不动（Windows 1080p 下 ui_scale=1 才恰好命中）。
DRAG_X="$(awk -v u="$UI_SCALE" 'BEGIN { printf "%.0f", 20*u }')"   # pad + 12*ui_scale
DRAG_Y="$(awk -v u="$UI_SCALE" 'BEGIN { printf "%.0f", 18*u }')"   # pad + 10*ui_scale
DRAG_DY=220                                                        # 向下拖，面板始终留在窗口内
DRAG_MID=$((DRAG_Y + DRAG_DY / 2))
DRAG_END=$((DRAG_Y + DRAG_DY))

# ---- ③ imgui 开 + 合成指针拖动 stats（标题栏 → 向下 220px）----
env "${COMMON[@]}" MAQUE_IMGUI=1 MAQUE_CAPTURE="$WORK/drag" \
    MAQUE_IMGUI_POINTER="2:$DRAG_X:$DRAG_Y:1;3:$DRAG_X:$DRAG_MID:0;4:$DRAG_X:$DRAG_END:0;5:$DRAG_X:$DRAG_END:2" \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/drag.log" 2>&1 || fail "拖动运行失败"
[ -f "$WORK/drag_0.ppm" ] || fail "拖动：未产生窗口抓帧"

# ---- ④ 软件 renderer 回退路径也要能显示（游戏帧 + 面板）----
env "${COMMON[@]}" MAQUE_IMGUI=1 MAQUE_PRESENT=software MAQUE_CAPTURE="$WORK/sw" \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/sw.log" 2>&1 || fail "software 呈现运行失败"
[ -f "$WORK/sw_0.ppm" ] || fail "software 呈现：未产生窗口抓帧"

# ---- ⑤ 固定 UI 度量：换个更大的窗口，度量必须逐字一致（旧实现会变）----
MAQUE_WINDOW=1200x900 MAQUE_IMGUI=1 MAQUE_MAX_FRAMES=6 \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/big.log" 2>&1 || fail "大窗口运行失败"
METRICS_BIG="$(grep -o 'runtime: imgui ui scale=.*' "$WORK/big.log" | head -1)"
[ -n "$METRICS_BIG" ] || fail "大窗口日志缺 UI 度量行"
[ "$METRICS" = "$METRICS_BIG" ] || \
    fail "UI 度量随窗口变化（应固定）：800x650「$METRICS」 vs 1200x900「$METRICS_BIG」"

"$PY" - "$WORK" "$UI_SCALE" "$PANEL_W" "$PANEL_H" "$TITLE_PX" "$BODY_PX" "$DRAG_DY" <<'PY' || exit 1
import sys

work = sys.argv[1]
ui_scale = float(sys.argv[2])
panel_w = float(sys.argv[3])
panel_h = float(sys.argv[4])
title_px = float(sys.argv[5])
body_px = float(sys.argv[6])
drag_dy = float(sys.argv[7])


def load(name):
    parts = open(f'{work}/{name}', 'rb').read().split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


w, h, off = load('off_0.ppm')
_, _, on = load('on_0.ppm')
_, _, drag = load('drag_0.ppm')
sw_w, sw_h, sw = load('sw_0.ppm')
sx, sy = w / 800.0, h / 650.0
pad = 8.0 * ui_scale

def diff(a, b, x0, y0, x1, y1, thr=12):
    n = 0
    for y in range(int(y0 * sy), min(h, int(y1 * sy))):
        row = y * w * 3
        for x in range(int(x0 * sx), min(w, int(x1 * sx))):
            i = row + x * 3
            if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > thr:
                n += 1
    return n

def uniq(a, x0, y0, x1, y1):
    s = set()
    for y in range(int(y0 * sy), min(h, int(y1 * sy)), 2):
        row = y * w * 3
        for x in range(int(x0 * sx), min(w, int(x1 * sx)), 2):
            i = row + x * 3
            s.add((a[i], a[i + 1], a[i + 2]))
    return len(s)

# 面板初始矩形（固定度量：屏幕分辨率派生）与拖动后的矩形（合成指针向下 drag_dy）。
# OLD 只取旧位置的**上半条带**：HiDPI 下 panel_h 可达 333px，而窗口只有 650px，
# 不可能把整块面板移到"完全离开旧矩形"；只要标题栏那条带恢复成游戏画面，
# 就证明面板确实跟着指针走了（新区用 NEW 全矩形断言"到达"）。
PANEL = (pad, pad, pad + panel_w, pad + panel_h)
# 游戏画面取**面板下方**：面板高度随 ui_scale 变，HiDPI 下"面板右侧"只剩背景渐变
# （颜色数很少 → 误判"游戏没上屏"）；下方始终有角色/地面条带，色彩足够。
GAME = (10, pad + panel_h + 10, w - 10, h - 30)
OLD = (pad + 2, pad + 2, pad + panel_w - 8, pad + 2 + drag_dy * 0.5)
NEW = (pad + 2, pad + drag_dy + 2, pad + panel_w - 8, pad + drag_dy + panel_h - 8)

panel_area = (PANEL[2] - PANEL[0]) * (PANEL[3] - PANEL[1])
new_area = (NEW[2] - NEW[0]) * (NEW[3] - NEW[1])
panel_diff = diff(on, off, *PANEL)
game_diff = diff(on, off, *GAME)
drag_old = diff(drag, off, *OLD)
drag_new = diff(drag, off, *NEW)
game_colors = uniq(on, *GAME)
drag_game_colors = uniq(drag, *GAME)
sw_panel_diff = 0
sw_game_colors = 0
if (sw_w, sw_h) == (w, h):
    sw_panel_diff = diff(sw, off, *PANEL)
    sw_game_colors = uniq(sw, *GAME)

ok = True
def check(cond, msg):
    global ok
    if not cond:
        print(f'imgui_present_e2e: FAIL {msg}', file=sys.stderr)
        ok = False

# ③ 固定 UI 度量：标题只比正文大一档（旧实现 26px:15px 标题压过内容）
check(title_px >= body_px and title_px <= body_px * 1.5,
      f'标题/正文字号比例失衡 title={title_px} body={body_px}')
check((w, h) == (800, 650), f'窗口抓帧尺寸 {w}x{h}（应 800x650）')
# ① 面板画上去了，且游戏画面照样上屏、逐像素不受面板影响
check(panel_diff > panel_area * 0.4, f'imgui 开：面板区域差异过小 {panel_diff}（面板没画出来？）')
check(game_diff < 50, f'imgui 开：游戏区域被面板改动 {game_diff}（应在面板外逐像素一致）')
check(game_colors > 500, f'imgui 开：游戏区域颜色 {game_colors}（游戏画面没上屏？纯色/黑屏）')
# ② 拖动：面板离开旧位置、出现在新位置，游戏画面一直可见
check(drag_old < 100, f'拖动后旧面板区域仍有差异 {drag_old}（面板没跟着指针移动？）')
check(drag_new > new_area * 0.4, f'拖动后新位置差异过小 {drag_new}（面板没移到新位置？）')
check(drag_game_colors > 500, f'拖动后游戏区域颜色 {drag_game_colors}（游戏画面丢了？）')
# ④ 软件 renderer 回退路径
check((sw_w, sw_h) == (w, h), f'software 呈现抓帧尺寸 {sw_w}x{sw_h}')
check(sw_panel_diff > panel_area * 0.4, f'software 呈现：面板区域差异过小 {sw_panel_diff}')
check(sw_game_colors > 500, f'software 呈现：游戏区域颜色 {sw_game_colors}')

if not ok:
    sys.exit(1)
print(f'imgui_present_e2e: ok（面板差异 {panel_diff}/{panel_area:.0f}、游戏区逐像素一致、'
      f'拖动旧区 {drag_old} 新区 {drag_new}/{new_area:.0f}、游戏颜色 {game_colors}）')
PY

# 反向断言：统一呈现后不再调用窗口表面路径（SDL2 不允许与 renderer 混用）
grep -q "window surface" "$WORK/on.log" && fail "仍在用 SDL_GetWindowSurface（与 renderer 混用）"

echo "imgui_present_e2e: ok（游戏上屏 + stats 可拖动 + 固定 UI 度量 + software 回退）"
