#!/usr/bin/env bash
# 主相机 Orbit 开关端到端（plans/36）：
#   ① render.Camera3D.orbit_controls=false：MAQUE_ORBIT 也不生效（画面与不设完全一致）
#   ② orbit_controls=true：MAQUE_ORBIT 改变机位 → 画面必须不同
# 两个场景几何/灯光完全相同，只差该字段。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCN_OFF="$2"
SCN_ON="$3"
WORK="$4"

fail() { echo "orbit_toggle_e2e: FAIL $*" >&2; exit 1; }
[ -f "$SCN_OFF" ] && [ -f "$SCN_ON" ] || { echo "orbit_toggle_e2e: skip（缺场景）"; exit 77; }
rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_CAPTURE MAQUE_CAPTURE_WGPU \
      MAQUE_WINDOW MAQUE_PRESENT MAQUE_IMGUI 2>/dev/null || true
export MAQUE_WINDOW=0x0

shot() {  # shot <out.ppm> <scene> [env...]
    local out="$1" scn="$2"; shift 2
    timeout 180 env "$@" "$PLAYER" --shot "$out" --shot-frame 2 --frames 3 "$scn" \
        >"$out.log" 2>&1 || { cat "$out.log"; fail "渲染失败 $scn"; }
    grep -q "runtime: shot" "$out.log" || { cat "$out.log"; fail "截图未写入"; }
}

shot "$WORK/off_base.ppm" "$SCN_OFF"
shot "$WORK/off_orbit.ppm" "$SCN_OFF" MAQUE_ORBIT=0.5,0.35,6
shot "$WORK/on_base.ppm" "$SCN_ON"
shot "$WORK/on_orbit.ppm" "$SCN_ON" MAQUE_ORBIT=0.5,0.35,6

cmp -s "$WORK/off_base.ppm" "$WORK/off_orbit.ppm" \
    || fail "orbit_controls=false 时 MAQUE_ORBIT 仍改写了相机"
cmp -s "$WORK/on_base.ppm" "$WORK/on_orbit.ppm" \
    && fail "orbit_controls=true 时 MAQUE_ORBIT 没有生效"

echo "orbit_toggle_e2e: PASS（关闭时忽略 Orbit，开启时 Orbit 生效）"
