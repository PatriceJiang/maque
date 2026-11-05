#!/usr/bin/env bash
# 纹理/网格装配并行端到端（docs/13 §8、docs/bugs/015）：ABeautifulGame 的 PBR 贴图
# decode + mip、以及 .mesh.json 的 CPU parse 在 setup 期并行（MAQUE_TEX_THREADS），
# GPU 上传仍主线程。
#   ① MAQUE_TEX_INFO=1 / MAQUE_MESH_INFO=1 必须有 preload 统计，jobs>0 且 failed=0；
#   ② 串行（MAQUE_TEX_THREADS=1）与并行（=4）渲染同一帧，PPM **逐字节一致**
#      —— 行分片只改变计算顺序，不改变任何像素（sRGB/线性 mip 与顶点数据保持旧语义）。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCN="$2"
WORK="$3"

fail() { echo "texture_preload_e2e: FAIL $*" >&2; exit 1; }
skip() { echo "texture_preload_e2e: skip（$*）"; exit 77; }

[ -f "$SCN" ] || skip "缺场景 $SCN"
[ "$HAVE_PY" = "1" ] || skip "缺 Python 3（解析 preload 统计）"
unset MAQUE_TEX_PRELOAD MAQUE_TEX_INFO 2>/dev/null || true
rm -rf "$WORK"; mkdir -p "$WORK"

run_shot() {   # run_shot <threads> <ppm> <log>
    env MAQUE_TEX_INFO=1 MAQUE_MESH_INFO=1 MAQUE_TEX_THREADS="$1" MAQUE_WINDOW=0x0 \
        timeout 180 "$PLAYER" --shot "$2" "$SCN" >"$3" 2>&1 \
        || { cat "$3"; fail "threads=$1 渲染失败"; }
}

run_shot 1 "$WORK/serial.ppm" "$WORK/serial.log"
run_shot 4 "$WORK/mt.ppm" "$WORK/mt.log"

[ -s "$WORK/serial.ppm" ] || fail "串行截图缺失"
[ -s "$WORK/mt.ppm" ] || fail "并行截图缺失"

grep -q "runtime: texture preload 'pbr'" "$WORK/serial.log" || fail "串行路径没有 texture preload 统计"
grep -q "runtime: texture preload 'pbr'" "$WORK/mt.log" || fail "并行路径没有 texture preload 统计"
grep -q "runtime: mesh preload:" "$WORK/serial.log" || fail "串行路径没有 mesh preload 统计"
grep -q "runtime: mesh preload:" "$WORK/mt.log" || fail "并行路径没有 mesh preload 统计"

"$PY" - "$WORK/serial.log" "$WORK/mt.log" <<'PY' || fail "preload 统计不符（jobs/uploaded/failed）"
import re, sys
for path in sys.argv[1:]:
    text = open(path, encoding='utf-8', errors='replace').read()
    for label, pat in (
        ("texture", r"texture preload 'pbr': jobs=(\d+) uploaded=(\d+) failed=(\d+) threads=(\d+)"),
        ("mesh", r"mesh preload: jobs=(\d+) uploaded=(\d+) failed=(\d+) threads=(\d+)"),
    ):
        m = re.search(pat, text)
        assert m, f"no {label} preload line in {path}"
        jobs, uploaded, failed, threads = map(int, m.groups())
        assert jobs > 0, (path, label, jobs)
        assert failed == 0, (path, label, failed)
        assert uploaded == jobs, (path, label, uploaded, jobs)
        print(f"{path}: {label} jobs={jobs} threads={threads}")
PY

if cmp -s "$WORK/serial.ppm" "$WORK/mt.ppm"; then
    echo "texture_preload_e2e: PASS（串行/并行纹理+网格输出逐字节一致）"
else
    fail "串行与并行渲染像素不一致（mip 行分片改变了结果）"
fi
