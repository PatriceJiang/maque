#!/usr/bin/env bash
# Tracy 内存域端到端（docs/29 §10）：不需要 profiler GUI / tracy-capture。
#
#  ① 文本场景（脚本文本 + 字形图集）headless 跑 90 帧：
#     MAQUE_TRACY_MEM_REPORT=1 每 0.2s 打一行 [mem] 明细 / 分区；
#     script（QuickJS 堆）、node（场景节点）、texture（GPU 图集纹理）、font（CPU 字形图集）
#     必须**非零** —— 这是"五个域真的接上了"的硬证据。
#  ② MAQUE_TRACY_MEM=0：运行时总开关关闭 → 不打印任何 [mem]（也不做采样）。
#  ③ 退出时（shutdown）仍打印一次最终快照（CI 无 GUI 也能看结论）。
#  ④ MAQUE_TRACY=1（启动 client）时报告照常输出（曲线路径 + 文本路径互不阻塞）。
#
# 构建为 MAQUE_TRACY=OFF / MAQUE_TRACY_MEMORY=OFF 时 SKIP 77。
set -u
PLAYER="$1"
SAMPLES="$2"
WORK="$3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() { echo "tracy_mem_e2e: FAIL $*" >&2; exit 1; }
skip() { echo "tracy_mem_e2e: skip（$*）"; exit 77; }

SCN="$SAMPLES/text/text.scn"
[ -f "$SCN" ] || skip "缺 $SCN"

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_TRACY MAQUE_TRACY_MEM MAQUE_TRACY_MEM_REPORT MAQUE_TRACY_MEM_INTERVAL \
      MAQUE_MAX_FRAMES MAQUE_WINDOW MAQUE_PRESENT MAQUE_TRACY_PROFILER 2>/dev/null || true

count_mem() { grep -c '^\[mem\]' "$1" 2>/dev/null || true; }

# ---- ① 明细/分区非零 ----
env MAQUE_TRACY_MEM_REPORT=1 MAQUE_TRACY_MEM_INTERVAL=0.2 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=90 \
    timeout 120 "$PLAYER" "$SCN" >"$WORK/report.log" 2>&1 || fail "player 退出码非 0"

LINES=$(count_mem "$WORK/report.log")
[ "${LINES:-0}" -gt 0 ] || skip "该构建未编入内存记账（MAQUE_TRACY=OFF / MAQUE_TRACY_MEMORY=OFF）"
[ "${LINES:-0}" -ge 2 ] || fail "[mem] 行数太少（$LINES）—— tick 报告未生效"

DETAIL=$(grep -m1 '^\[mem\] tick: script ' "$WORK/report.log" || true)
[ -n "$DETAIL" ] || fail "缺 [mem] tick 明细行"
echo "tracy_mem_e2e: $DETAIL"

for dom in script node texture font; do
  case "$DETAIL" in
    *"$dom 0 B"*) fail "内存域 $dom 为 0 —— 该域没有接上（$DETAIL）" ;;
  esac
done
case "$DETAIL" in
  *"script "*"B"*) : ;;
  *) fail "明细行缺 script 域（$DETAIL）" ;;
esac
echo "$DETAIL" | grep -qE 'script [0-9.]+ (B|KiB|MiB|GiB)' || fail "script 域格式异常（$DETAIL）"
echo "$DETAIL" | grep -qE 'heap [0-9.]+ (B|KiB|MiB|GiB)' || fail "缺 heap 域（$DETAIL）"

AREA=$(grep -m1 '^\[mem\] tick: area ' "$WORK/report.log" || true)
[ -n "$AREA" ] || fail "缺 [mem] tick 分区行"
echo "tracy_mem_e2e: $AREA"
case "$AREA" in *"area ui 0 B"*) fail "UI 分区为 0（字体域应计入 UI）" ;; esac
case "$AREA" in *"area 3d 0 B"*) fail "3D 分区为 0（纹理域应计入 3D）" ;; esac
case "$AREA" in *"tracked 0 B"*) fail "已分类总量为 0" ;; esac

# ---- ③ 退出快照（shutdown 报告；current 可能已被 teardown 清零，峰值必须非零）----
grep -q '^\[mem\] shutdown:' "$WORK/report.log" || fail "退出时没有 [mem] shutdown 快照"
PEAK=$(grep -m1 '^\[mem\] shutdown: peak script ' "$WORK/report.log" || true)
[ -n "$PEAK" ] || fail "退出快照缺峰值行"
echo "tracy_mem_e2e: $PEAK"
for dom in script node texture font; do
  case "$PEAK" in
    *"$dom 0 B"*) fail "峰值 $dom 为 0 —— 该域从未记账（$PEAK）" ;;
  esac
done

# ---- ② 运行时关闭：零输出、零采样 ----
env MAQUE_TRACY_MEM=0 MAQUE_TRACY_MEM_REPORT=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=30 \
    timeout 120 "$PLAYER" "$SCN" >"$WORK/off.log" 2>&1 || fail "关闭内存记账后 player 退出码非 0"
OFF_LINES=$(count_mem "$WORK/off.log")
[ "${OFF_LINES:-0}" -eq 0 ] || fail "MAQUE_TRACY_MEM=0 仍打印了 [mem]（$OFF_LINES 行）"

# ---- ④ 启动 Tracy client 时报告照常（不需要 profiler 连接）----
env MAQUE_TRACY=1 MAQUE_TRACY_MEM_REPORT=1 MAQUE_TRACY_MEM_INTERVAL=0.2 \
    MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=60 \
    timeout 120 "$PLAYER" "$SCN" >"$WORK/client.log" 2>&1 || fail "MAQUE_TRACY=1 时 player 退出码非 0"
grep -q '^\[mem\] tick: script ' "$WORK/client.log" || fail "启动 Tracy 后没有 [mem] 报告"
echo "tracy_mem_e2e: PASS"
