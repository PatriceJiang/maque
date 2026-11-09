#!/usr/bin/env bash
# 3D 粒子确定性 / 回滚端到端（plans/28 §4.7/§4.10：G6 + G7 的确定性契约）：
#   ① 逐帧 trail 随帧变化（CPU 模拟状态进 checksum）；
#   ② 同一输入两跑逐帧 trail 逐字节一致；
#   ③ 快照@60 → 前进 20 → restore → 前进 40 == 直接前进 100（3D 粒子状态 + 发射累加器 + RNG 入快照）；
#   ④ MAQUE_PARTICLES=0 只关渲染：checksum 与开着时**完全相同**（模拟不受开关影响）；
#   ⑤ 渲染端确认：MAQUE_PARTICLES=0 时画面几乎纯背景，开着时粒子可见（实例化路径仍工作）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"; SCN="$2"; WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "particles3d_determinism_e2e: skip（无显示环境）"; exit 77
fi
[ -n "${PY:-}" ] || { echo "particles3d_determinism_e2e: skip（无 python3）"; exit 77; }
[ -f "$PLAYER" ] || { echo "particles3d_determinism_e2e: skip（缺 player）"; exit 77; }
[ -f "$SCN" ] || { echo "particles3d_determinism_e2e: skip（缺 $SCN）"; exit 77; }
fail() { echo "particles3d_determinism_e2e: FAIL $*" >&2; exit 1; }
RESP() { grep -F "\"id\":$2,\"result\":" "$1" | head -1; }
CHK() { RESP "$1" "$2" | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*'; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_RENDER3D_INFO \
      MAQUE_PARTICLES 2>/dev/null || true
export MAQUE_WINDOW=0x0 MAQUE_PRESENT=off
unset MAQUE_CONTROL 2>/dev/null || true

run_ctrl() {  # run_ctrl <req> <out> <trail|"">
    local trail="${3-}"
    if [ -n "$trail" ]; then
        MAQUE_CONTROL=- MAQUE_TRAIL_OUT="$trail" "$PLAYER" "$SCN" < "$1" > "$2" 2>&1
    else
        MAQUE_CONTROL=- MAQUE_TRAIL_OUT="" "$PLAYER" "$SCN" < "$1" > "$2" 2>&1
    fi
}

cat > "$WORK/req1.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":2,"method":"World.checksum"}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":30}}
{"jsonrpc":"2.0","id":4,"method":"World.checksum"}
{"jsonrpc":"2.0","id":5,"method":"World.trail"}
EOF
cat > "$WORK/req_plain.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":100}}
{"jsonrpc":"2.0","id":2,"method":"World.checksum"}
EOF
cat > "$WORK/req_rb.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":60}}
{"jsonrpc":"2.0","id":2,"method":"World.snapshot"}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":20}}
{"jsonrpc":"2.0","id":4,"method":"World.restore"}
{"jsonrpc":"2.0","id":5,"method":"Runtime.step","params":{"frames":40}}
{"jsonrpc":"2.0","id":6,"method":"World.checksum"}
EOF

run_ctrl "$WORK/req1.txt" "$WORK/run1.txt" "$WORK/trail1.txt" || { cat "$WORK/run1.txt"; fail "控制面运行失败"; }
[ -s "$WORK/trail1.txt" ] || fail "没有逐帧 trail（checksum 未覆盖 3D 粒子？）"
C1=$(CHK "$WORK/run1.txt" 2)
C2=$(CHK "$WORK/run1.txt" 4)
[ -n "$C1" ] && [ -n "$C2" ] || { cat "$WORK/run1.txt"; fail "World.checksum 无返回"; }
[ "$C1" != "$C2" ] || fail "帧 30 与帧 60 checksum 相同（3D 粒子未进校验和）"
TRAIL_ROWS=$(wc -l < "$WORK/trail1.txt")
[ "$TRAIL_ROWS" -ge 60 ] || fail "逐帧 trail 行数不足（$TRAIL_ROWS < 60）"
UNIQ=$(cut -d' ' -f2 "$WORK/trail1.txt" | sort -u | wc -l)
[ "$UNIQ" -ge 30 ] || fail "逐帧 trail 几乎不变（uniq=$UNIQ）"
echo "particles3d_determinism_e2e: checksum/trail 覆盖 3D 粒子 OK（帧变化 $C1 → $C2, rows=$TRAIL_ROWS）"

# ---- ② 两跑确定性 ----
run_ctrl "$WORK/req1.txt" "$WORK/run2.txt" "$WORK/trail2.txt" || { cat "$WORK/run2.txt"; fail "第二次运行失败"; }
diff -q "$WORK/trail1.txt" "$WORK/trail2.txt" >/dev/null || fail "两跑逐帧 trail 不一致（非确定性）"
echo "particles3d_determinism_e2e: 两跑确定性 OK"

# ---- ③ 快照回滚等价 ----
run_ctrl "$WORK/req_plain.txt" "$WORK/plain.txt" "" || { cat "$WORK/plain.txt"; fail "plain 运行失败"; }
run_ctrl "$WORK/req_rb.txt" "$WORK/rb.txt" "" || { cat "$WORK/rb.txt"; fail "回滚运行失败"; }
PLAIN=$(CHK "$WORK/plain.txt" 2)
ROLL=$(CHK "$WORK/rb.txt" 6)
[ -n "$PLAIN" ] && [ "$PLAIN" = "$ROLL" ] \
    || fail "快照回滚不等价（plain=$PLAIN rollback=$ROLL；3D 粒子/累加器未入快照？）"
echo "particles3d_determinism_e2e: 快照回滚等价 OK（$PLAIN）"

# ---- ④ 渲染开关不影响模拟状态 ----
export MAQUE_PARTICLES=0
run_ctrl "$WORK/req1.txt" "$WORK/off.txt" "" || { cat "$WORK/off.txt"; fail "MAQUE_PARTICLES=0 运行失败"; }
unset MAQUE_PARTICLES
C_OFF=$(CHK "$WORK/off.txt" 4)
[ "$C_OFF" = "$C2" ] || fail "MAQUE_PARTICLES=0 改变了 checksum（$C_OFF != $C2；渲染开关不应影响模拟）"
echo "particles3d_determinism_e2e: MAQUE_PARTICLES 只关渲染 OK"

# ---- ⑤ 画面：占位实例化路径仍可见 ----
run_shot() {  # run_shot <ppm> <frame> [env...]
    local ppm="$1" frame="$2"; shift 2
    local log="$WORK/$(basename "$ppm").log"
    timeout 180 env "$@" "$PLAYER" --shot "$ppm" --shot-frame "$frame" --frames "$((frame + 1))" "$SCN" \
        >"$log" 2>&1 || { cat "$log"; fail "$(basename "$ppm") 渲染失败"; }
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "$(basename "$ppm") 截图未写入"; }
    if grep -qiE 'validation|WGSL|pipeline.*error|shader.*error' "$log"; then
        cat "$log"; fail "$(basename "$ppm") shader/管线错误"
    fi
}
run_shot "$WORK/on.ppm" 60
run_shot "$WORK/off.ppm" 60 MAQUE_PARTICLES=0
NONBG=$("$PY" - "$WORK/on.ppm" "$WORK/off.ppm" <<'PY'
import sys
from pathlib import Path

def load(name):
    d = Path(name).read_bytes()
    magic, dims, _max, px = d.split(b'\n', 3)
    assert magic == b'P6', magic
    w, h = map(int, dims.split())
    return w, h, px

def nonbg(name):
    w, h, px = load(name)
    i0 = (2 * w + 2) * 3
    base = px[i0:i0 + 3]
    n = 0
    for i in range(0, len(px), 3):
        if abs(px[i] - base[0]) + abs(px[i + 1] - base[1]) + abs(px[i + 2] - base[2]) > 30:
            n += 1
    return n / (w * h)

on, off = nonbg(sys.argv[1]), nonbg(sys.argv[2])
print(f'{on:.4f} {off:.4f}')
PY
)
ON=$(echo "$NONBG" | cut -d' ' -f1); OFF=$(echo "$NONBG" | cut -d' ' -f2)
awk -v a="$ON" -v b="$OFF" 'BEGIN { exit !(a > b + 0.02 && a > 0.02) }' \
    || fail "粒子可见性断言失败（on=$ON off=$OFF）"
echo "particles3d_determinism_e2e: 实例化画面可见/可关 OK（on=$ON off=$OFF）"

echo "particles3d_determinism_e2e: PASS（trail 覆盖、两跑一致、快照回滚、渲染开关、画面）"
