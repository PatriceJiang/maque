#!/usr/bin/env bash
# Boids 端到端（plans/27 3D 粒子 / plans/28）：
#   ① boids.scn（JS 参考）：脚本用引擎 rng 初始化 + 空间网格模拟，每帧 particles3d_upload →
#      render.ParticleField3D（一个发射器/一个 draw）；断言 N/count/可见/确定性/可关。
#   ② boids-webgpu.scn（全 shader）：ShaderGraph 两个 O(N²) 状态 pass + 320×180 splat + 上采样；
#      断言 shader graph 4 pass、画面可见、确定性、render 时间预算。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"; SCN_JS="$2"; SCN_GPU="$3"; WORK="$4"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "boids_e2e: skip（无显示环境）"; exit 77
fi
[ -f "$SCN_JS" ] || { echo "boids_e2e: skip（缺 $SCN_JS）"; exit 77; }
[ -f "$SCN_GPU" ] || { echo "boids_e2e: skip（缺 $SCN_GPU）"; exit 77; }
fail() { echo "boids_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_ENV_IBL MAQUE_RENDER3D_INFO \
      MAQUE_PARTICLES MAQUE_TRACE_STATS 2>/dev/null || true
export MAQUE_WINDOW=0x0

shot() {  # shot <scene> <ppm> <frame> [VAR=value ...]
    local scene="$1" ppm="$2" frame="$3"; shift 3
    timeout 180 env "$@" "$PLAYER" --shot "$ppm" --shot-frame "$frame" --frames "$((frame + 1))" \
        "$scene" >"$WORK/$(basename "$ppm").log" 2>&1 \
        || { cat "$WORK/$(basename "$ppm").log"; fail "$(basename "$ppm") 渲染失败"; }
    grep -q "runtime: shot" "$WORK/$(basename "$ppm").log" \
        || { cat "$WORK/$(basename "$ppm").log"; fail "$(basename "$ppm") 截图未写入"; }
}

# ① JS 参考：N=300（Debug/ASan 下 QuickJS 的邻居循环是主要成本）
shot "$SCN_JS" "$WORK/js.ppm" 90 BOIDS_N=300
grep -q "\[boids\] N=300 field=" "$WORK/js.ppm.log" || { cat "$WORK/js.ppm.log"; fail "JS boids 未初始化 N=300"; }
grep -q "runtime: 3d scene cams=1 meshes=0" "$WORK/js.ppm.log" \
    || { cat "$WORK/js.ppm.log"; fail "Boids 场景不应有 MeshRenderer（粒子场独立绘制）"; }

shot "$SCN_JS" "$WORK/js_off.ppm" 60 BOIDS_N=300 MAQUE_PARTICLES=0
shot "$SCN_JS" "$WORK/js_a.ppm" 60 BOIDS_N=300
shot "$SCN_JS" "$WORK/js_b.ppm" 60 BOIDS_N=300
cmp -s "$WORK/js_a.ppm" "$WORK/js_b.ppm" || fail "JS boids 两次渲染不一致"

# ② 全 shader：ShaderGraph（无 JS / 无 CPU 回读）
shot "$SCN_GPU" "$WORK/gpu.ppm" 120
grep -q "shader graph 'webgpu/program.json' loaded (entity=0, passes=4, buffers=3)" "$WORK/gpu.ppm.log" \
    || { cat "$WORK/gpu.ppm.log"; fail "shader graph 未加载 4 pass"; }
shot "$SCN_GPU" "$WORK/gpu_a.ppm" 60
shot "$SCN_GPU" "$WORK/gpu_b.ppm" 60
cmp -s "$WORK/gpu_a.ppm" "$WORK/gpu_b.ppm" || fail "GPU boids 两次渲染不一致"

# ③ 性能：GPU 场景 240 帧的统计（Debug+ASan 下也只应远快于实时）
timeout 180 env MAQUE_TRACE_STATS=1 "$PLAYER" --frames 241 "$SCN_GPU" >"$WORK/gpu_perf.txt" 2>&1 \
    || { cat "$WORK/gpu_perf.txt"; fail "GPU boids 性能运行失败"; }
grep -q "\[stats\] frame=240" "$WORK/gpu_perf.txt" || { cat "$WORK/gpu_perf.txt"; fail "缺少 240 帧统计"; }
timeout 180 env MAQUE_TRACE_STATS=1 BOIDS_N=300 "$PLAYER" --frames 91 "$SCN_JS" >"$WORK/js_perf.txt" 2>&1 \
    || { cat "$WORK/js_perf.txt"; fail "JS boids 性能运行失败"; }

"$PY" - "$WORK" <<'PY' || exit 1
import re, sys
from pathlib import Path

work = Path(sys.argv[1])


def load(name):
    d = (work / name).read_bytes()
    magic, dims, _max, px = d.split(b'\n', 3)
    assert magic == b'P6', magic
    w, h = map(int, dims.split())
    return w, h, px


def bright_ratio(name, thr):
    w, h, px = load(name)
    n = sum(1 for i in range(0, len(px), 3) if max(px[i], px[i + 1], px[i + 2]) > thr)
    return n / (w * h)


def stats(name, frame):
    text = (work / name).read_text(encoding='utf-8', errors='replace')
    m = None
    for line in text.splitlines():
        if line.startswith(f'[stats] frame={frame} '):
            m = line
    assert m, f'no stats frame={frame} in {name}'
    d = dict(re.findall(r'(\w+)=([0-9.]+)', m))
    return float(d['fps']), float(d['logic']), float(d['render']), float(d['total'])

js_on = bright_ratio('js.ppm', 40)
js_off = bright_ratio('js_off.ppm', 40)
gpu = bright_ratio('gpu.ppm', 80)
fps, logic, render, total = stats('gpu_perf.txt', 240)
jsfps, jslogic, jsrender, jstotal = stats('js_perf.txt', 90)
print(f'boids_e2e: js on={js_on:.4f} off={js_off:.4f} | gpu on={gpu:.4f}')
print(f'boids_e2e: GPU N=256  fps={fps:.1f} logic={logic:.1f}ms render={render:.1f}ms total={total:.1f}ms')
print(f'boids_e2e: JS  N=300  fps={jsfps:.1f} logic={jslogic:.1f}ms render={jsrender:.1f}ms total={jstotal:.1f}ms')

ok = True
ok &= js_on > 0.003                  # 300 个软点可见
ok &= js_off < 0.0015                # MAQUE_PARTICLES=0 时粒子场不画
ok &= js_on > js_off * 3.0
ok &= gpu > 0.01                     # 256 个发光点覆盖约 2~5%
ok &= render < 60.0                  # Debug+ASan 的预算（Release 实测 ~7ms）
ok &= fps > 10.0
sys.exit(0 if ok else 1)
PY
echo "boids_e2e: PASS（JS 参考 + 全 shader 版本、可见性/确定性、性能预算）"
