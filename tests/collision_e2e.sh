#!/usr/bin/env bash
# 3D 碰撞查询端到端（plans/36）：球重叠 / 连续扫掠球 / 定向盒 SAT / 命中距离与点。
#   · 场景两个 scale=2 的 cube（世界半边长 1.2）；查询只读、不改场景。
#   · 与 scene_raycast 共用同一套 OBB（glTF / .mesh.json / 内建 cube bounds）。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "collision_e2e: FAIL $*" >&2; exit 1; }

timeout 120 "$PLAYER" --frames 4 "$SCENE" > "$WORK/run.log" 2>&1
RC=$?
[ "$RC" -eq 0 ] || { cat "$WORK/run.log"; fail "player exit=$RC"; }
grep -q "\[game\] collision: PASS" "$WORK/run.log" || { cat "$WORK/run.log"; fail "脚本自检未通过"; }

echo "collision_e2e: PASS（sphere overlap/cast、OBB SAT、命中距离/点、只读）"
