#!/usr/bin/env bash
# ImGui 面板开关（F2）端到端：
#   ① 单次按下 → 面板 **ON 一次**（不能每帧反复开关）
#   ② 按住不放（连续按下事件）→ 仍只 ON 一次（沿检测；SDL 的自动重复必须被忽略）
#   ③ 按两次（中间松开）→ ON、OFF 各一次
#   ④ 开→关→开（反复 init/shutdown）不崩溃
#   ⑤ 打开后**确实画进了像素缓冲**（截图非空 + 与关面板时不同）
#
# 场景用 demo-spineboy（走 demos.js，里面才有面板；spine.scn 走 spine.js 没有面板）。
# 说明：headless 没有真实键盘 → 用 `MAQUE_SIM_KEY_AT="<帧>:<键码>"` 往**本帧设备事件队列**
# 注入按下事件，走与真实键盘完全相同的路径（iframe → imgui_input_feed → 热键判断）。
set -u
PLAYER="$1"; SCENES="$2"; WORK="$3"
rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_PRESENT=off
unset MAQUE_IMGUI || true          # 默认关，全靠 F2 打开

fail() { echo "imgui_toggle_e2e: FAIL $*" >&2; exit 1; }
ONC()  { grep -c "imgui panel ON" "$1" 2>/dev/null || true; }
OFFC() { grep -c "imgui panel OFF" "$1" 2>/dev/null || true; }

SCN="$SCENES/spine/demo-spineboy.scn"
[ -f "$SCN" ] || { echo "imgui_toggle_e2e: skip（缺 $SCN）"; exit 77; }

# ---- ① 单次按下 → ON 一次 ----
MAQUE_SIM_KEY_AT="20:1002" timeout 60 "$PLAYER" --frames 50 "$SCN" >"$WORK/one.log" 2>&1 \
    || fail "单次按下运行失败"
[ "$(ONC "$WORK/one.log")" = 1 ] || fail "单次按下应 ON 恰好 1 次，实际 $(ONC "$WORK/one.log")（$(grep 'imgui panel' "$WORK/one.log" | head -5)）"
[ "$(OFFC "$WORK/one.log")" = 0 ] || fail "单次按下不应出现 OFF"

# ---- ② 按住不放（连续按下事件）→ 仍只 ON 一次 ----
HOLD=""
for f in 15 16 17 18 19 20 21 22 23 24; do HOLD="${HOLD}${HOLD:+,}$f:1002"; done
MAQUE_SIM_KEY_AT="$HOLD" timeout 60 "$PLAYER" --frames 50 "$SCN" >"$WORK/hold.log" 2>&1 \
    || fail "按住不放运行失败"
[ "$(ONC "$WORK/hold.log")" = 1 ] || fail "按住不放应只 ON 1 次，实际 $(ONC "$WORK/hold.log")（$(grep 'imgui panel' "$WORK/hold.log" | head -5)）"

# ---- ③ 按两次 → ON、OFF 各一次 ----
MAQUE_SIM_KEY_AT="15:1002,35:1002" timeout 60 "$PLAYER" --frames 60 "$SCN" >"$WORK/two.log" 2>&1 \
    || fail "两次按键运行失败"
[ "$(ONC "$WORK/two.log")" = 1 ] || fail "两次按键 ON 次数应为 1（实际 $(ONC "$WORK/two.log")）"
[ "$(OFFC "$WORK/two.log")" = 1 ] || fail "两次按键 OFF 次数应为 1（实际 $(OFFC "$WORK/two.log")）"

# ---- ④ 反复开关（init/shutdown 循环）不崩溃 ----
MAQUE_SIM_KEY_AT="10:1002,20:1002,30:1002,40:1002,50:1002" timeout 60 "$PLAYER" --frames 70 "$SCN" \
    >"$WORK/flip.log" 2>&1 || fail "反复开关崩溃/失败"
grep -q "AddressSanitizer\|Assertion failed" "$WORK/flip.log" && fail "反复开关出现崩溃"

# ---- ⑤ 打开后确实画进像素缓冲 ----
rm -f "$WORK/on.png" "$WORK/off.png"
MAQUE_SIM_KEY_AT="5:1002" timeout 60 "$PLAYER" --shot "$WORK/on.png" --shot-frame 25 "$SCN" \
    >"$WORK/shot_on.log" 2>&1 || fail "开面板截图失败"
timeout 60 "$PLAYER" --shot "$WORK/off.png" --shot-frame 25 "$SCN" >"$WORK/shot_off.log" 2>&1 \
    || fail "关面板截图失败"
[ -s "$WORK/on.png" ] || fail "开面板截图未生成"
[ -s "$WORK/off.png" ] || fail "关面板截图未生成"
cmp -s "$WORK/on.png" "$WORK/off.png" && fail "开/关面板截图相同（面板没画进像素缓冲）"

echo "imgui_toggle_e2e: ok（单次/按住/两次/反复开关/像素可见）"
