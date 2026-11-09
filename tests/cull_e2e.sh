#!/usr/bin/env bash
# 3D 视锥剔除端到端（plans/36）：41 个可见性候选里 40 个在相机左侧视锥外。
#   ① MAQUE_CULL_INFO=1：gfx 打印 culled draws ≥ 40（且只画 1 个可见 mesh）
#   ② MAQUE_CULL=0：不再剔除（无 culled 日志），画面与剔除开启逐字节一致
#   ③ 两次渲染各自确定性（同参数两跑一致）
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

fail() { echo "cull_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_CAPTURE MAQUE_CAPTURE_WGPU \
      MAQUE_WINDOW MAQUE_PRESENT MAQUE_IMGUI 2>/dev/null || true
export MAQUE_WINDOW=0x0

run_shot() {  # run_shot <ppm> [env...]
    local ppm="$1"; shift
    timeout 180 env "$@" "$PLAYER" --shot "$ppm" --shot-frame 2 --frames 3 "$SCENE" \
        >"$WORK/$(basename "$ppm").log" 2>&1 \
        || { cat "$WORK/$(basename "$ppm").log"; fail "$(basename "$ppm") 渲染失败"; }
    grep -q "runtime: shot" "$WORK/$(basename "$ppm").log" \
        || { cat "$WORK/$(basename "$ppm").log"; fail "截图未写入"; }
}

run_shot "$WORK/on.ppm" MAQUE_CULL_INFO=1 MAQUE_RENDER3D_INFO=1
run_shot "$WORK/on_b.ppm" MAQUE_CULL_INFO=1
run_shot "$WORK/off.ppm" MAQUE_CULL=0 MAQUE_CULL_INFO=1

grep -q "runtime: 3d scene cams=1 meshes=41" "$WORK/on.ppm.log" \
    || { cat "$WORK/on.ppm.log"; fail "场景应有 41 个 MeshRenderer"; }

CULLED=$("$PY" - "$WORK/on.ppm.log" <<'PY'
import re, sys
n = 0
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'gfx: culled draws (\d+)/(\d+)', line)
    if m:
        n = max(n, int(m.group(1)))
print(n)
PY
)
[ -n "$CULLED" ] && [ "$CULLED" -ge 40 ] || { cat "$WORK/on.ppm.log"; fail "剔除数量不足（$CULLED < 40）"; }
if grep -q "gfx: culled draws" "$WORK/off.ppm.log"; then
    cat "$WORK/off.ppm.log"; fail "MAQUE_CULL=0 不应有剔除日志"
fi
cmp -s "$WORK/on.ppm" "$WORK/on_b.ppm" || fail "剔除开启两次渲染不一致（非确定性）"
cmp -s "$WORK/on.ppm" "$WORK/off.ppm" || fail "剔除开/关画面不一致（误剔）"

echo "cull_e2e: PASS（culled=$CULLED/41，开/关像素一致，确定性）"
