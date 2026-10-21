#!/usr/bin/env bash
# scn → 图片（截图/预览）端到端：`maque_player --shot ...`。
#   ① PNG 有效（签名/CRC/IHDR/IDAT 可解压）且内容非空白
#   ② 确定性：同帧两次 → 逐字节一致；不同帧 → 不同
#   ③ .ppm 走原样写出（P6 头 + 精确字节数）
#   ④ 截图后即退出（quit=1）—— 一条命令得到图
#   ⑤ 目标不可写 → 明确报错
#   ⑥ **批量截图**（--shot-seq + --shot-frames）：一条命令拿多帧，帧号不同画面不同
#   ⑦ **定时批量**（--shot-every + --shot-count）：拍够张数即退出，退出码 0
#   ⑧ `--frames N` 到上限属于**正常结束**（退出码 0，不是失败）
set -u
PLAYER="$1"; SCENES="$2"; WORK="$3"

export MAQUE_PRESENT=off        # 纯离屏（无需窗口/显示）

fail() { echo "shot_e2e: FAIL $*" >&2; exit 1; }

# PNG 校验（内联 python：不做额外依赖）
PY=python
command -v python >/dev/null 2>&1 || { echo "shot_e2e: skip（无 python，无法校验 PNG）"; exit 77; }

rm -rf "$WORK"; mkdir -p "$WORK"

shot() {   # shot <out> <frame> <scene> → 日志
    timeout 60 "$PLAYER" --shot "$1" --shot-frame "$2" "$3" 2>&1
}

# ---- ① PNG：有效 + 非空白 ----
LOG=$(shot "$WORK/ball_60.png" 60 "$SCENES/ball.scn")
printf '%s\n' "$LOG" | grep -q "runtime: shot '.*ball_60.png' 640x240 frame=60" || fail "未报告截图（$LOG）"
[ -s "$WORK/ball_60.png" ] || fail "PNG 未生成"
"$PY" - "$WORK/ball_60.png" <<'PY' || exit 1
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n', '签名错误'
pos, idat, seen = 8, b'', []
while pos < len(d):
    ln = struct.unpack('>I', d[pos:pos+4])[0]; typ = d[pos+4:pos+8]
    data = d[pos+8:pos+8+ln]; crc = struct.unpack('>I', d[pos+8+ln:pos+12+ln])[0]
    assert crc == zlib.crc32(typ + data) & 0xffffffff, f'{typ} CRC 错误'
    if typ == b'IHDR':
        w, h, bd, ct = struct.unpack('>IIBB', data[:10])
        assert (w, h, bd, ct) == (640, 240, 8, 6), f'IHDR 异常 {(w,h,bd,ct)}'
    if typ == b'IDAT': idat += data
    seen.append(typ); pos += 12 + ln
raw = zlib.decompress(idat)
assert len(raw) == 240 * (640 * 4 + 1), 'IDAT 解压尺寸不符'
assert len({raw[i:i+4] for i in range(0, len(raw), 4)}) > 50, '画面近似空白'
assert seen[0] == b'IHDR' and seen[-1] == b'IEND'
PY

# ---- ② 确定性 + 帧号有影响 ----
shot "$WORK/ball_60_again.png" 60 "$SCENES/ball.scn" >/dev/null
cmp -s "$WORK/ball_60.png" "$WORK/ball_60_again.png" || fail "同帧两次截图不一致（非确定性）"
shot "$WORK/ball_5.png" 5 "$SCENES/ball.scn" >/dev/null
cmp -s "$WORK/ball_5.png" "$WORK/ball_60.png" && fail "不同帧截图却相同（shot-frame 未生效？）"

# ---- ③ PPM 原样写出 ----
shot "$WORK/ball.ppm" 5 "$SCENES/ball.scn" >/dev/null
head -c 15 "$WORK/ball.ppm" | grep -q 'P6' || fail "PPM 头错误"
SIZE=$(stat -c%s "$WORK/ball.ppm")
[ "$SIZE" = "$((15 + 640 * 240 * 3))" ] || fail "PPM 字节数不符（$SIZE）"

# ---- ④ 截图后即退出 ----
printf '%s\n' "$LOG" | grep -q "exited after 61 frames (quit=1)" || fail "截图后未退出（$LOG）"

# ---- ⑤ 目标不可写 → 报错 ----
LOG=$(shot "$WORK/no-such-dir/x.png" 5 "$SCENES/ball.scn")
printf '%s\n' "$LOG" | grep -q "shot 写入失败" || fail "不可写路径未报错"

# ---- ⑥ 批量截图（--shot-seq + --shot-frames）：一条命令多帧 ----
# 价值：动画/物理这类"随时间变化"的东西，只拍第 1 帧看不出对错。
SEQ="$WORK/multi"
mkdir -p "$SEQ"
LOG=$(timeout 60 "$PLAYER" --shot-seq "$SEQ/f{}.png" --shot-frames 1,30,60 "$SCENES/ball.scn" 2>&1)
for N in 1 30 60; do
    [ -s "$SEQ/f$N.png" ] || fail "批量截图缺帧 $N（$LOG）"
    printf '%s\n' "$LOG" | grep -q "frame=$N" || fail "批量截图未报告帧 $N（$LOG）"
done
cmp -s "$SEQ/f1.png" "$SEQ/f30.png" && fail "批量截图 f1 == f30（帧号没生效）"
cmp -s "$SEQ/f30.png" "$SEQ/f60.png" && fail "批量截图 f30 == f60（帧号没生效）"
# 拍完最后一张即退出（不是靠 timeout 拖到 3600 帧上限）
printf '%s\n' "$LOG" | grep -q "exited after 61 frames" || fail "批量截图未在最后一帧退出（$LOG）"

# ---- ⑦ 定时批量（--shot-every + --shot-count）：拍够即退、退出码 0 ----
rm -rf "$SEQ/every"; mkdir -p "$SEQ/every"
timeout 60 "$PLAYER" --shot-seq "$SEQ/every/e.png" --shot-every 20 --shot-count 3 \
    "$SCENES/ball.scn" >"$SEQ/every.log" 2>&1
RC=$?
[ "$RC" = 0 ] || fail "定时批量截图退出码非 0（$RC）"
CNT=$(find "$SEQ/every" -name 'e_*.png' | wc -l)
[ "$CNT" = 3 ] || fail "定时批量截图数量应 3，实际 $CNT"
grep -q "shot\[3\]" "$SEQ/every.log" || fail "定时批量截图未报告第 3 张"

# ---- ⑧ --frames N 到上限 = 正常结束（退出码 0）----
timeout 60 "$PLAYER" --frames 5 "$SCENES/ball.scn" >/dev/null 2>&1
RC=$?
[ "$RC" = 0 ] || fail "--frames 5 退出码应 0（实际 $RC）—— 帧数上限是正常结束，不是失败"

echo "shot_e2e: ok（PNG 有效/确定性/PPM/退出/错误路径/批量截图/定时批量/--frames）"
