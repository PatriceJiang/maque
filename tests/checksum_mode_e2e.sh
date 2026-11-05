#!/usr/bin/env bash
# 逐帧世界 checksum 默认关闭（docs/10 §5）：
#   · 普通运行：只有终局指纹，MAQUE_CHECKSUM_INFO=1 必须打印 per-frame=off；
#   · MAQUE_CHECKSUM=1：逐帧 trail 打开，打印 per-frame=on，MAQUE_TRAIL_OUT 应有多行。
set -u
PLAYER="$1"
SCN="$2"
WORK="$3"

fail() { echo "checksum_mode_e2e: FAIL $*" >&2; exit 1; }
[ -f "$SCN" ] || { echo "checksum_mode_e2e: skip（缺场景 $SCN）"; exit 77; }
rm -rf "$WORK"; mkdir -p "$WORK"

run_default() {
    env -u MAQUE_CHECKSUM -u MAQUE_REPLAY -u MAQUE_RECORD -u MAQUE_TRAIL_OUT -u MAQUE_CRASH_REPORT \
        MAQUE_CHECKSUM_INFO=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=2 \
        "$PLAYER" "$SCN" >"$1" 2>&1 || fail "默认运行失败（$1）"
}
run_all() {
    env -u MAQUE_REPLAY -u MAQUE_RECORD -u MAQUE_TRAIL_OUT -u MAQUE_CRASH_REPORT \
        MAQUE_CHECKSUM=1 MAQUE_CHECKSUM_INFO=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=2 \
        "$PLAYER" "$SCN" >"$1" 2>&1 || fail "MAQUE_CHECKSUM=1 运行失败（$1）"
}

run_default "$WORK/off.log"
grep -q "runtime: checksum per-frame=off" "$WORK/off.log" \
    || { cat "$WORK/off.log"; fail "默认应关闭逐帧 checksum"; }
grep -qE "world_trail=0x[0-9a-f]+" "$WORK/off.log" || fail "默认运行缺少终局 world_trail"

run_all "$WORK/on.log"
grep -q "runtime: checksum per-frame=on" "$WORK/on.log" \
    || { cat "$WORK/on.log"; fail "MAQUE_CHECKSUM=1 应打开逐帧 checksum"; }

env -u MAQUE_REPLAY -u MAQUE_RECORD -u MAQUE_CRASH_REPORT \
    MAQUE_CHECKSUM=1 MAQUE_TRAIL_OUT="$WORK/trail.txt" MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=3 \
    "$PLAYER" "$SCN" >"$WORK/trail.log" 2>&1 || fail "MAQUE_TRAIL_OUT 运行失败"
[ -s "$WORK/trail.txt" ] || fail "MAQUE_TRAIL_OUT 没有产生逐帧 trail"
[ "$(wc -l <"$WORK/trail.txt")" -ge 1 ] || fail "MAQUE_TRAIL_OUT 行数不足"

echo "checksum_mode_e2e: PASS（默认 off；显式 on 时逐帧 trail 可用）"
