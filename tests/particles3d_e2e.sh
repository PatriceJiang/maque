#!/usr/bin/env bash
# 3D 粒子端到端（plans/27 M1-4）：CPU 确定性模拟 + billboard 动态网格（一个发射器一次 draw）。
#   ① 第 60 帧有粒子（非背景像素占比）；② MAQUE_PARTICLES=0 时几乎全背景；
#   ③ 第 1 帧 vs 第 60 帧画面变化（动画）；④ 两次运行逐字节一致。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"; SCN="$2"; WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "particles3d_e2e: skip（无显示环境）"; exit 77
fi
[ -f "$SCN" ] || { echo "particles3d_e2e: skip（缺 $SCN）"; exit 77; }
fail() { echo "particles3d_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_ENV_IBL MAQUE_RENDER3D_INFO \
      MAQUE_PARTICLES 2>/dev/null || true
export MAQUE_WINDOW=0x0

run_shot() {  # run_shot <ppm> <frame> [env...]
    local ppm="$1" frame="$2"; shift 2
    timeout 120 env "$@" "$PLAYER" --shot "$ppm" --shot-frame "$frame" --frames "$((frame + 1))" "$SCN" \
        >"$WORK/$(basename "$ppm").log" 2>&1 \
        || { cat "$WORK/$(basename "$ppm").log"; fail "$(basename "$ppm") 渲染失败"; }
    grep -q "runtime: shot" "$WORK/$(basename "$ppm").log" \
        || { cat "$WORK/$(basename "$ppm").log"; fail "截图未写入"; }
    if grep -qiE 'validation|WGSL|pipeline.*error|shader.*error' "$WORK/$(basename "$ppm").log"; then
        cat "$WORK/$(basename "$ppm").log"; fail "shader/管线错误"
    fi
}

run_shot "$WORK/on.ppm" 60
run_shot "$WORK/on_b.ppm" 60
run_shot "$WORK/off.ppm" 60 MAQUE_PARTICLES=0
run_shot "$WORK/f1.ppm" 1

# 两个发射器由 gfx 粒子批次绘制（不再各自建 mesh）；场景本身没有 MeshRenderer。
grep -q "runtime: 3d scene cams=1 meshes=0" "$WORK/on.ppm.log" \
    || { cat "$WORK/on.ppm.log"; fail "粒子场景不应有 MeshRenderer"; }

cmp -s "$WORK/on.ppm" "$WORK/on_b.ppm" || fail "两次渲染不一致（非确定性）"

"$PY" - "$WORK" <<'PY' || exit 1
import sys
from pathlib import Path

work = Path(sys.argv[1])


def load(name):
    d = (work / name).read_bytes()
    magic, dims, _max, px = d.split(b'\n', 3)
    assert magic == b'P6', magic
    w, h = map(int, dims.split())
    return w, h, px


def nonbg(name, bg=(2, 2), thr=30):
    w, h, px = load(name)
    i0 = (bg[1] * w + bg[0]) * 3
    base = px[i0:i0 + 3]
    n = 0
    for i in range(0, len(px), 3):
        if abs(px[i] - base[0]) + abs(px[i + 1] - base[1]) + abs(px[i + 2] - base[2]) > thr:
            n += 1
    return w, h, n / (w * h)


def diff_px(a, b, thr=30):
    wa, ha, pa = load(a)
    wb, hb, pb = load(b)
    assert (wa, ha) == (wb, hb)
    n = 0
    for i in range(0, len(pa), 3):
        if abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1]) + abs(pa[i + 2] - pb[i + 2]) > thr:
            n += 1
    return n

w, h, on = nonbg('on.ppm')
_, _, off = nonbg('off.ppm')
anim = diff_px('on.ppm', 'f1.ppm')
print(f'particles3d_e2e: {w}x{h} on={on:.3f} off={off:.3f} anim_px={anim}')
ok = True
ok &= on > 0.02
ok &= off < 0.005
ok &= anim > 2000
sys.exit(0 if ok else 1)
PY
echo "particles3d_e2e: PASS（2 个发射器、粒子可见/可关、随时间变化、确定性）"
