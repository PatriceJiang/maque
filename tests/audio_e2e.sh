#!/usr/bin/env bash
# 音频端到端（docs/30）：脚本 audio_* → VFS 解码 → 混音器逻辑时钟 → 状态机。
#   ① MAQUE_AUDIO=null：不出声，但 playhead/state 必须按模拟时间确定性推进（CI 主路径）；
#   ② SDL + dummy 驱动：真实设备后端/回调线程也能跑（无声卡/无显示器环境）。
set -u
. "$(dirname "$0")/maque_python.sh"

PLAYER="$1"
SCN="$2"
WORK="$3"

fail() { echo "audio_e2e: FAIL $*" >&2; exit 1; }
[ -f "$SCN" ] || { echo "audio_e2e: skip（缺 $SCN）"; exit 77; }
[ -f "$(dirname "$SCN")/beep.wav" ] || { echo "audio_e2e: skip（缺 beep.wav）"; exit 77; }
[ -f "$(dirname "$SCN")/beep.ogg" ] || { echo "audio_e2e: skip（缺 beep.ogg）"; exit 77; }

rm -rf "$WORK"; mkdir -p "$WORK"

# ---- ① null 后端：逻辑时钟/状态机（确定性，不依赖声卡）----
MAQUE_AUDIO=null MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=180 \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/log-null.txt" 2>&1 \
    || { cat "$WORK/log-null.txt"; fail "null 后端运行失败"; }

grep -q 'audio_e2e: enabled=1' "$WORK/log-null.txt" || fail "音频实例未创建"
grep -q 'audio_e2e: voice=1' "$WORK/log-null.txt" || { tail -5 "$WORK/log-null.txt"; fail "audio_play 未返回 voice"; }
grep -q 'audio_e2e: state0=1' "$WORK/log-null.txt" || fail "初始状态应为 Playing"
grep -q 'audio_e2e: head3=0\.0' "$WORK/log-null.txt" || fail "playhead 未按模拟时间推进"
grep -q 'audio_e2e: state30=0' "$WORK/log-null.txt" || fail "0.1s 的 clip 在 0.5s 时应已 Stopped"
grep -q 'audio_e2e: head30=0\.1' "$WORK/log-null.txt" || fail "非 loop 播完 playhead 应夹在 clip 长度"
grep -q 'audio_e2e: state_after_stop=0' "$WORK/log-null.txt" || fail "stop 后状态应为 0"
grep -q 'audio_e2e: comp_state3=1' "$WORK/log-null.txt" || fail "AudioSource autoplay 未启动"
grep -q 'audio_e2e: comp_head3=0\.0' "$WORK/log-null.txt" || fail "组件 playhead 未推进"
grep -q 'audio_e2e: comp_state30=0' "$WORK/log-null.txt" || fail "组件在 clip 结束后应为 Stopped"
grep -q 'audio_e2e: comp_head30=0\.1' "$WORK/log-null.txt" || fail "组件 playhead 应夹在 clip 长度"
grep -q 'audio_e2e: ogg_state3=1' "$WORK/log-null.txt" || fail "OGG AudioSource autoplay 未启动"
grep -q 'audio_e2e: ogg_head3=0\.0' "$WORK/log-null.txt" || fail "OGG playhead 未推进"
grep -q 'audio_e2e: ogg_state30=0' "$WORK/log-null.txt" || fail "OGG 在 clip 结束后应为 Stopped"
grep -q 'audio_e2e: ogg_head30=0\.2' "$WORK/log-null.txt" || fail "OGG playhead 应夹在 clip 长度"
grep -q 'audio_e2e: stopped' "$WORK/log-null.txt" || fail "脚本未走到 quit"
echo "audio_e2e: ① null 后端 + 逻辑时钟/状态机 OK"

# ---- ② SDL 设备后端（dummy 驱动；没有 SDL 音频时 ABI 会回退 null，仍应通过）----
SDL_AUDIODRIVER=dummy MAQUE_AUDIO=sdl MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=180 \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/log-sdl.txt" 2>&1 \
    || { cat "$WORK/log-sdl.txt"; fail "SDL(dummy) 运行失败"; }
grep -q 'runtime: audio device requested' "$WORK/log-sdl.txt" || fail "未走设备请求路径"
grep -q 'audio_e2e: stopped' "$WORK/log-sdl.txt" || fail "SDL 路径脚本未跑完"
echo "audio_e2e: ② SDL dummy 设备后端 OK"

# ---- ③ 离线抓音：null pump 每模拟步 mix → WAV；断言非静音（ogg/wav 两条解码路径都进混音）----
CAP="$WORK/capture.wav"
MAQUE_AUDIO_CAPTURE="$CAP" MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=180 \
    timeout 60 "$PLAYER" "$SCN" >"$WORK/log-cap.txt" 2>&1 \
    || { cat "$WORK/log-cap.txt"; fail "抓音运行失败"; }
[ -s "$CAP" ] || fail "未生成抓音文件"
grep -q 'runtime: audio capture done' "$WORK/log-cap.txt" || fail "抓音未正常收尾"
if [ -z "${PY:-}" ]; then
    echo "audio_e2e: skip ③（无 Python 3，无法校验 WAV）"
else
    "$PY" - "$CAP" <<'PYWAV' || exit 1
import sys, wave, struct, math
with wave.open(sys.argv[1], 'rb') as w:
    assert w.getnchannels() == 2, "capture 应为 stereo"
    assert w.getsampwidth() == 2, "capture 应为 PCM16"
    n = w.getnframes()
    data = w.readframes(n)
assert n > 1000, f"capture 帧数过少: {n}"
vals = struct.unpack('<%dh' % (len(data) // 2), data)
rms = math.sqrt(sum(v * v for v in vals) / len(vals)) / 32768.0
assert rms > 0.01, f"capture 基本静音: rms={rms}"
print('audio_e2e: ③ capture frames=%d rms=%.4f OK' % (n, rms))
PYWAV
fi

echo "audio_e2e: PASS"
