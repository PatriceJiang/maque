#!/usr/bin/env bash
# GPU morph target 动画端到端（plans/28 §3.9）：
#   ① 动画：帧 1 / 25 / 50 画面不同；同帧两次逐字节一致（确定性）。
#   ② 单实例 GPU vs CPU（MAQUE_MORPH_GPU=0）：同帧像素等价（mean abs diff ≤ 1/255）
#      —— 顶点着色器累加与 CPU update_mesh 同语义（含 weights 动画帧）。
#   ③ 双实例共享同一网格、不同 clip 相位：GPU 两处形变不同（修 G2「首实体驱动」）；
#      CPU 回退路径（共享 mesh 只取首实体）与 GPU 必须不同 —— 反向证明权重是 per-draw。
#   ④ MAQUE_MORPH=0：基础网格，与默认权重画面不同；GPU/CPU 在关闭 morph 时仍等价。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"; SCN="$2"; WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "morph_gpu_e2e: skip（无显示环境）"; exit 77
fi
[ -n "${PY:-}" ] || { echo "morph_gpu_e2e: skip（无 python3）"; exit 77; }
[ -f "$PLAYER" ] || { echo "morph_gpu_e2e: skip（缺 player）"; exit 77; }
[ -f "$SCN" ] || { echo "morph_gpu_e2e: skip（缺 $SCN）"; exit 77; }
fail() { echo "morph_gpu_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_RENDER3D_INFO \
      MAQUE_MORPH MAQUE_MORPH_GPU MAQUE_ANIM MAQUE_SKIN 2>/dev/null || true
export MAQUE_WINDOW=0x0 MAQUE_PRESENT=off

run_shot() {  # run_shot <ppm> <frame> <scene> [env...]
    local ppm="$1" frame="$2" scene="$3"; shift 3
    local log="$WORK/$(basename "$ppm").log"
    timeout 180 env "$@" "$PLAYER" --shot "$ppm" --shot-frame "$frame" --frames "$((frame + 1))" "$scene" \
        >"$log" 2>&1 || { cat "$log"; fail "$(basename "$ppm") 渲染失败"; }
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "$(basename "$ppm") 截图未写入"; }
    if grep -qiE 'validation|WGSL|pipeline.*error|shader.*error' "$log"; then
        cat "$log"; fail "$(basename "$ppm") shader/管线错误"
    fi
}

# 平均绝对像素差（0..255）/ 非背景占比
pix() {  # pix mean|nonbg <a> [b]
    "$PY" - "$@" <<'PY'
import sys
from pathlib import Path

def load(name):
    d = Path(name).read_bytes()
    magic, dims, _max, px = d.split(b'\n', 3)
    assert magic == b'P6', magic
    w, h = map(int, dims.split())
    return w, h, px

mode = sys.argv[1]
if mode == 'mean':
    wa, ha, pa = load(sys.argv[2])
    wb, hb, pb = load(sys.argv[3])
    assert (wa, ha) == (wb, hb), ((wa, ha), (wb, hb))
    total = 0
    for i in range(0, len(pa), 3):
        total += abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1]) + abs(pa[i + 2] - pb[i + 2])
    print(f'{total / (len(pa) / 3):.4f}')
else:
    w, h, px = load(sys.argv[2])
    i0 = (2 * w + 2) * 3
    base = px[i0:i0 + 3]
    n = 0
    for i in range(0, len(px), 3):
        if abs(px[i] - base[0]) + abs(px[i + 1] - base[1]) + abs(px[i + 2] - base[2]) > 30:
            n += 1
    print(f'{n / (w * h):.4f}')
PY
}

# ---- ① 动画 + 确定性（双实例样例：两处形变不同相）----
run_shot "$WORK/f1.ppm" 1 "$SCN"
run_shot "$WORK/f1b.ppm" 1 "$SCN"
run_shot "$WORK/f25.ppm" 25 "$SCN"
run_shot "$WORK/f50.ppm" 50 "$SCN"
cmp -s "$WORK/f1.ppm" "$WORK/f1b.ppm" || fail "同帧两次截图不一致（确定性）"
[ "$(pix mean "$WORK/f1.ppm" "$WORK/f25.ppm")" != "0.0000" ] || fail "帧 1 与帧 25 相同（morph 动画未生效）"
[ "$(pix mean "$WORK/f25.ppm" "$WORK/f50.ppm")" != "0.0000" ] || fail "帧 25 与帧 50 相同（morph 动画未生效）"
awk -v a="$(pix mean "$WORK/f1.ppm" "$WORK/f25.ppm")" 'BEGIN { exit !(a > 0.5) }' \
    || fail "morph 动画像素差过小（< 0.5/255）"
echo "morph_gpu_e2e: 动画/确定性 OK"

# ---- ② 单实例 GPU vs CPU 像素等价（删掉双实例样例里的 B 实体与孤儿资源）----
mkdir -p "$WORK/single"
cp -r "$(dirname "$SCN")/assets" "$WORK/single/assets"
"$PY" - "$SCN" "$WORK/single/single.scn" <<'PY' || exit 1
import json, sys
src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src, encoding='utf-8'))
d['title'] = 'morph_gpu_single'
d['entities'] = [e for e in d['entities'] if e.get('name') != 'morph_blob_b']
json.dump(d, open(dst, 'w', encoding='utf-8', newline='\n'), ensure_ascii=False, indent=2)
open(dst, 'a', encoding='utf-8', newline='\n').write('\n')
PY
SINGLE="$WORK/single/single.scn"
run_shot "$WORK/single_gpu25.ppm" 25 "$SINGLE"
run_shot "$WORK/single_cpu25.ppm" 25 "$SINGLE" MAQUE_MORPH_GPU=0
GPU_CPU_DIFF="$(pix mean "$WORK/single_gpu25.ppm" "$WORK/single_cpu25.ppm")"
awk -v d="$GPU_CPU_DIFF" 'BEGIN { exit !(d <= 1.0) }' \
    || fail "单实例 GPU/CPU 同帧不等价（mean_abs=$GPU_CPU_DIFF > 1/255）"
echo "morph_gpu_e2e: 单实例 GPU/CPU 像素等价 OK（mean_abs=$GPU_CPU_DIFF）"

# ---- ③ 双实例共享网格：GPU 每 draw 独立权重（CPU 回退只能首实体驱动）----
run_shot "$WORK/two_gpu25.ppm" 25 "$SCN"
TWO_DIFF="$(pix mean "$WORK/two_gpu25.ppm" "$WORK/single_gpu25.ppm")"
awk -v d="$TWO_DIFF" 'BEGIN { exit !(d > 0.5) }' \
    || fail "双实例与单实例画面几乎相同（B 实例未独立形变，G2 回归；mean_abs=$TWO_DIFF）"
# f25 与 two_gpu25 是同一条路径 → 必须逐字节一致（防"多实例排序/权重错位"）
cmp -s "$WORK/two_gpu25.ppm" "$WORK/f25.ppm" || fail "同一场景两次 GPU 渲染不一致"
run_shot "$WORK/two_cpu25.ppm" 25 "$SCN" MAQUE_MORPH_GPU=0
TWO_CPU_DIFF="$(pix mean "$WORK/two_gpu25.ppm" "$WORK/two_cpu25.ppm")"
awk -v d="$TWO_CPU_DIFF" 'BEGIN { exit !(d > 0.2) }' \
    || fail "双实例 GPU 与 CPU 回退画面相同（per-draw 权重未生效；mean_abs=$TWO_CPU_DIFF）"
echo "morph_gpu_e2e: 双实例 per-draw 权重 OK（B 实例差异=$TWO_DIFF CPU_diff=$TWO_CPU_DIFF）"

# ---- ④ MAQUE_MORPH=0：基础网格 ----
run_shot "$WORK/nomorph25.ppm" 25 "$SCN" MAQUE_MORPH=0
awk -v d="$(pix mean "$WORK/f25.ppm" "$WORK/nomorph25.ppm")" 'BEGIN { exit !(d > 0.5) }' \
    || fail "MAQUE_MORPH=0 与默认权重画面相同（关闭开关未生效）"
run_shot "$WORK/nomorph_cpu25.ppm" 25 "$SCN" MAQUE_MORPH=0 MAQUE_MORPH_GPU=0
cmp -s "$WORK/nomorph25.ppm" "$WORK/nomorph_cpu25.ppm" \
    || fail "MAQUE_MORPH=0 时 GPU/CPU 不一致（应都画基础网格）"
echo "morph_gpu_e2e: MAQUE_MORPH=0 基础网格 OK"

echo "morph_gpu_e2e: PASS（动画/确定性、GPU=CPU、per-draw 权重、关闭开关）"
