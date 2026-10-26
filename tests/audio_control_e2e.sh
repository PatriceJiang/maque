#!/usr/bin/env bash
# 音频控制面（docs/30）：Audio.info/play/voices/stop/set_master/set_bus。
# 无脚本场景 → 第一个 voice 句柄确定：gen=2, slot=0 → (2<<32)|1 = 8589934593。
set -u
PLAYER="$1"
SCN="$2"

fail() { echo "audio_control_e2e: FAIL $*" >&2; exit 1; }
[ -f "$SCN" ] || { echo "audio_control_e2e: skip（缺场景）"; exit 77; }
[ -f "$(dirname "$SCN")/beep.wav" ] || { echo "audio_control_e2e: skip（缺 beep.wav）"; exit 77; }

OUT=$(MAQUE_CONTROL=- MAQUE_AUDIO=null MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=2 \
    timeout 60 "$PLAYER" "$SCN" 2>/dev/null <<'JSON'
{"jsonrpc":"2.0","id":1,"method":"Audio.info"}
{"jsonrpc":"2.0","id":2,"method":"Audio.play","params":{"path":"beep.wav","volume":0.5,"loop":false,"bus":"sfx"}}
{"jsonrpc":"2.0","id":3,"method":"Audio.voices"}
{"jsonrpc":"2.0","id":4,"method":"Audio.stop","params":{"voice":"8589934593"}}
{"jsonrpc":"2.0","id":5,"method":"Audio.set_master","params":{"volume":0.8}}
{"jsonrpc":"2.0","id":6,"method":"Audio.set_bus","params":{"bus":"sfx","volume":0.5}}
JSON
) || fail "maque_player 退出非 0"

printf '%s\n' "$OUT"
case "$OUT" in
  *'"id":1,"result":{"ok":true,"sample_rate":48000,"channels":2,"voices":0'*) ;;
  *) fail "Audio.info 不符" ;;
esac
case "$OUT" in
  *'"id":2,"result":{"ok":true,"voice":"8589934593"}'*) ;;
  *) fail "Audio.play 未返回预期 voice" ;;
esac
case "$OUT" in
  *'"id":3,"result":{"ok":true,"voices":[{"voice":"8589934593","state":1'*) ;;
  *) fail "Audio.voices 未列出 voice" ;;
esac
case "$OUT" in
  *'"id":4,"result":{"ok":true}'*) ;;
  *) fail "Audio.stop 失败" ;;
esac
case "$OUT" in
  *'"id":5,"result":{"ok":true}'*) ;;
  *) fail "Audio.set_master 失败" ;;
esac
case "$OUT" in
  *'"id":6,"result":{"ok":true}'*) ;;
  *) fail "Audio.set_bus 失败" ;;
esac
echo "audio_control_e2e: PASS"
