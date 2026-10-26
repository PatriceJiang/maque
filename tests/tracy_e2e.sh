#!/usr/bin/env bash
# Tracy 性能分析端到端（docs/29）：
#   ① **客户端按需启动**：默认运行不监听 8086、日志无 "[tracy] client started"；
#      MAQUE_TRACY=1 才启动（headless 也算 <0 开销路径）。
#   ② **数据真的发得出去**：起一个 MAQUE_TRACY=1 的 headless player，用 tracy-capture
#      （Windows：tools/tracy/get-profiler.ps1 下载官方 exe；Linux/macOS：tools/tracy/build-capture.sh
#      从源码构建原生二进制，都装到 .deps/tracy/）连上去抓 2 秒，
#      断言帧数 >10、zone 数 >100、trace 文件非空 —— 这是"埋点最后真的进了 trace"的唯一硬证据。
#   ③ **F3 启动 profiler GUI**：MAQUE_SIM_KEY_AT 注入 F3（与真实键盘同路径）→
#      客户端启动 + spawn 外部 profiler 进程，日志出现 "client started (F3)" / "launched profiler"。
#      CI 不便弹 GUI：用 tracy-capture 当"假 profiler"充当被 spawn 的程序（只需验证拉起成功）。
#
# 依赖：player 必须是以 MAQUE_TRACY=ON 构建的（否则 ① 会 skip 77）；tool 缺失也 skip 77。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SAMPLES="$2"
WORK="$3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() { echo "tracy_e2e: FAIL $*" >&2; exit 1; }
skip() { echo "tracy_e2e: skip（$*）"; exit 77; }
_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }

SCN="$SAMPLES/spine/spine.scn"
[ -f "$SCN" ] || skip "缺 $SCN"

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_MAX_FRAMES MAQUE_STOP_FRAME MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_WINDOW \
      MAQUE_TRACY MAQUE_TRACY_PROFILER MAQUE_TRACY_PORT MAQUE_IMGUI MAQUE_CAPTURE 2>/dev/null || true

# ---- ① 默认（未启动）：不监听 Tracy ----
# 这一段不依赖 tracy-capture，所以在检查工具之前跑 —— 没装工具的平台也能验证"默认零开销"。
env -u MAQUE_TRACY timeout 60 "$PLAYER" --frames 3 "$SCN" >"$WORK/off.log" 2>&1
grep -q "\[tracy\] client started" "$WORK/off.log" && fail "未设 MAQUE_TRACY 却启动了 Tracy 客户端"
echo "tracy_e2e: ① 默认运行未启动客户端 OK"

# tracy-capture / tracy-profiler 是下载/构建的工具（不入库）：.deps/tracy/ 或环境变量覆盖。
# ⚠️ 平台必须匹配：Windows 的 .exe 在 Linux 上执行会得到 "Invalid or incomplete multibyte..."，
#    所以非 Windows 只认原生二进制（且要求可执行位）。
CAPTURE="${MAQUE_TRACY_CAPTURE:-}"
if [ -z "$CAPTURE" ]; then
  if _mq_is_windows; then
    for c in "$REPO_ROOT/.deps/tracy/tracy-capture.exe" "$REPO_ROOT/.deps/tracy/tracy-capture"; do
      if [ -f "$c" ]; then CAPTURE="$c"; break; fi
    done
  else
    for c in "$REPO_ROOT/.deps/tracy/tracy-capture" "$REPO_ROOT/.deps/tracy/tracy-capture.bin"; do
      if [ -x "$c" ]; then CAPTURE="$c"; break; fi
    done
  fi
fi
if [ -z "$CAPTURE" ] || [ ! -e "$CAPTURE" ]; then
  if _mq_is_windows; then
    skip "未找到 tracy-capture（先跑 tools/tracy/get-profiler.ps1）"
  else
    skip "未找到原生 tracy-capture（先跑 tools/tracy/build-capture.sh）"
  fi
fi
echo "tracy_e2e: capture=$CAPTURE"

# ---- ② 抓 trace：headless 持续跑，capture 连上去 2 秒 ----
env MAQUE_TRACY=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=6000 \
    timeout 90 "$PLAYER" "$SCN" >"$WORK/cap_player.log" 2>&1 &
PPID_=$!
trap 'kill "$PPID_" 2>/dev/null || true; wait "$PPID_" 2>/dev/null || true' EXIT
sleep 2   # 等客户端监听 8086 并跑起来
timeout 60 "$CAPTURE" -o "$WORK/trace.tracy" -a 127.0.0.1 -p 8086 -s 2 >"$WORK/capture.log" 2>&1 \
  || { cat "$WORK/capture.log"; fail "tracy-capture 失败"; }
kill "$PPID_" 2>/dev/null || true; wait "$PPID_" 2>/dev/null || true
trap - EXIT

grep -q "\[tracy\] client started (MAQUE_TRACY)" "$WORK/cap_player.log" \
  || { tail -5 "$WORK/cap_player.log"; skip "该构建未编入 Tracy（MAQUE_TRACY=OFF？）"; }
FRAMES=$(sed -nE 's/^Frames: ([0-9]+).*/\1/p' "$WORK/capture.log" | head -1)
ZONES=$(sed -nE 's/^Zones: ([0-9,]+).*/\1/p' "$WORK/capture.log" | head -1 | tr -d ',')
TRACE_BYTES=$(wc -c <"$WORK/trace.tracy" | tr -d ' ')
echo "tracy_e2e: ② capture frames=${FRAMES:-?} zones=${ZONES:-?} trace=${TRACE_BYTES}B"
[ -n "${FRAMES:-}" ] && [ "$FRAMES" -gt 10 ] || fail "trace 帧数太少（$FRAMES）"
[ -n "${ZONES:-}" ] && [ "$ZONES" -gt 100 ] || fail "trace zone 太少（$ZONES）——埋点未进 trace"
[ "$TRACE_BYTES" -gt 1000 ] || fail "trace 文件过小（$TRACE_BYTES B）"

# ---- ③ F3：启动客户端 + 拉起 profiler（CI 用 capture 二进制当假 profiler）----
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "tracy_e2e: ③ skip（无显示环境，F3 用例需要窗口）"
  exit 0
fi
env MAQUE_TRACY_PROFILER="$CAPTURE" MAQUE_WINDOW=800x650 MAQUE_MAX_FRAMES=90 \
    MAQUE_SIM_KEY_AT="20:1003" \
    timeout 90 "$PLAYER" "$SCN" >"$WORK/f3.log" 2>&1
grep -q "\[tracy\] client started (F3)" "$WORK/f3.log" || { tail -5 "$WORK/f3.log"; fail "F3 未启动 Tracy 客户端"; }
grep -q "\[tracy\] launched profiler" "$WORK/f3.log" || { tail -5 "$WORK/f3.log"; fail "F3 未拉起 profiler 进程"; }
echo "tracy_e2e: ③ F3 启动客户端 + 拉起 profiler OK"
echo "tracy_e2e: PASS"
