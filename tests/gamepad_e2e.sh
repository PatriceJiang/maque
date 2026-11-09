#!/usr/bin/env bash
# 手柄模拟量端到端（plans/36）：MAQUE_GAMEPAD 覆盖 pad0 →
#   input_axis / input_axis_pad / input_gamepad_button(s) / input_gamepad_connected。
# 不需要真实手柄，也不需要窗口（平台 poll 在 headless 下仍写手柄状态）。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1
export MAQUE_GAMEPAD="0.75,-0.5,0,0,0.25,0,3"

fail() { echo "gamepad_e2e: FAIL $*" >&2; exit 1; }

timeout 120 "$PLAYER" --frames 4 "$SCENE" > "$WORK/run.log" 2>&1
RC=$?
[ "$RC" -eq 0 ] || { cat "$WORK/run.log"; fail "player exit=$RC"; }
grep -q "\[game\] gamepad: PASS" "$WORK/run.log" || { cat "$WORK/run.log"; fail "脚本自检未通过"; }

echo "gamepad_e2e: PASS（MAQUE_GAMEPAD 模拟量 → input_axis/按钮位）"
