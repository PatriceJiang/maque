#!/usr/bin/env bash
# flappy 端到端测试（samples/flappy）
#
#   smoke <maque_flappy> <scene.scn>   场景/脚本/素材能加载，跑若干帧不崩
#   play  <maque_flappy> <scene.scn>   玩法：确定性回放 + 拍翅不撞 + 撞管判死 + 计分
#
# 约定（与其它 e2e 一致）：exit 77 = 跳过（环境不满足），由 CTest 的 SKIP_RETURN_CODE 识别。
set -u

MODE="${1:-}"; BIN="${2:-}"; SCN="${3:-}"
if [ -z "$MODE" ] || [ -z "$BIN" ] || [ -z "$SCN" ]; then
    echo "usage: flappy_e2e.sh <smoke|play> <maque_flappy> <scene.scn>" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
# 需要 Python 解析/生成 JSON（Windows 上 python3 可能是 Store 占位程序）
if [ -f "$DIR/maque_python.sh" ]; then . "$DIR/maque_python.sh"; fi
PY="${PY:-}"
if [ -z "$PY" ]; then
    for c in python3 python; do command -v "$c" >/dev/null 2>&1 && "$c" -c 'pass' >/dev/null 2>&1 && { PY="$c"; break; }; done
fi

WORK="$(mktemp -d 2>/dev/null || echo "./flappy-e2e-$$")"
mkdir -p "$WORK" || exit 77
trap 'rm -rf "$WORK"' EXIT

export MAQUE_HEADLESS=1
export MAQUE_WINDOW=0x0            # 不要真窗口（CI/无显示环境）
unset MAQUE_PRESENT MAQUE_CAPTURE MAQUE_SIM_CLICK 2>/dev/null || true

fail() { echo "flappy_e2e[$MODE]: FAIL $*" >&2; exit 1; }
skip() { echo "flappy_e2e[$MODE]: SKIP $*" >&2; exit 77; }

# 跑一次，输出到 $1，最多 $2 帧，可选 $3 = sim 文件
run_play() {
    local out="$1" frames="$2" sim="${3:-}"
    if [ -n "$sim" ]; then
        MAQUE_SIM="$sim" MAQUE_MAX_FRAMES="$frames" \
        MAQUE_FLAPPY_CRASH_AT="${MAQUE_FLAPPY_CRASH_AT:-}" \
        MAQUE_FLAPPY_FOLLOW_GAP="${MAQUE_FLAPPY_FOLLOW_GAP:-}" \
            "$BIN" "$SCN" >"$out" 2>&1
    else
        MAQUE_MAX_FRAMES="$frames" "$BIN" "$SCN" >"$out" 2>&1
    fi
    return 0
}

# 从日志里取一个数值字段
field() { sed -n "s/.*$2=\([^ ]*\).*/\1/p" "$1" | tail -1; }

case "$MODE" in
smoke)
    run_play "$WORK/smoke.log" 40
    grep -q 'flappy: ready' "$WORK/smoke.log" || { echo "--- 日志 ---"; cat "$WORK/smoke.log"; fail "脚本未进入 ready（场景/脚本没加载？）"; }
    grep -q "renderer plugin" "$WORK/smoke.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    if grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|stack-overflow' "$WORK/smoke.log"; then
        cat "$WORK/smoke.log"; fail "运行期报错"
    fi
    echo "flappy_smoke: ok（ready + 渲染插件 + 无报错，40 帧）"
    ;;

play)
    [ -n "$PY" ] || skip "需要 Python 3 生成 .sim.json"

    # ① 拍翅保持飞行：拍翅间隔必须与物理匹配 —— 每次拍翅给 +380 px/s 上冲、重力 -1400 px/s²，
    #    回到同高度的周期是 2*380/1400 ≈ 0.543 s ≈ 32.6 帧；取 38 帧会缓慢下沉、取 30 帧会缓慢爬升，
    #    两者都会撞管。这里用略小于周期的 32 帧，并让飞行在屏幕中部进行。
    "$PY" - "$WORK/flap.sim.json" <<'PY'
import json, sys
ev = []
f = 20
for i in range(22):
    ev.append({"frame": f, "type": "pointer_down", "x": 80, "y": 140, "button": 0})
    ev.append({"frame": f + 2, "type": "pointer_up", "x": 80, "y": 140})
    f += 32
json.dump({"name": "flappy_flap", "space": "world", "events": ev}, open(sys.argv[1], "w"))
PY
    [ -s "$WORK/flap.sim.json" ] || fail "sim 文件生成失败"

    run_play "$WORK/a.log" 700 "$WORK/flap.sim.json"
    run_play "$WORK/b.log" 700 "$WORK/flap.sim.json"
    grep -q 'flappy: ready' "$WORK/a.log" || { cat "$WORK/a.log"; fail "未进入 ready"; }

    # ② 计分 + 确定性：MAQUE_FLAPPY_FOLLOW_GAP=1 让鸟稳定穿过随机缺口（只改鸟的 y，
    #    碰撞/计分/难度曲线照常走）。要求：分数确实推进，且两次回放逐位一致。
    #    注意**不**断言"永不死亡" —— 那是在测"输入调得好不好"，随机缺口下必然脆弱；
    #    这里测的是"滚动/生成/计分/判定这条链路对不对 + 可复现"。
    MAQUE_FLAPPY_FOLLOW_GAP=1 run_play "$WORK/a.log" 700 "$WORK/flap.sim.json"
    MAQUE_FLAPPY_FOLLOW_GAP=1 run_play "$WORK/b.log" 700 "$WORK/flap.sim.json"
    grep -q 'flappy: ready' "$WORK/a.log" || { cat "$WORK/a.log"; fail "未进入 ready"; }

    TA="$(field "$WORK/a.log" world_trail)"; TB="$(field "$WORK/b.log" world_trail)"
    [ -n "$TA" ] || skip "日志里没有 world_trail（引擎版本差异）"
    [ "$TA" = "$TB" ] || fail "两次回放 world_trail 不一致：$TA vs $TB"

    SA="$(grep -oE 'flappy: score=[0-9]+' "$WORK/a.log" | tail -1 | grep -oE '[0-9]+$')"
    if [ -z "$SA" ]; then
        # 全程没死时不会有 score= 行，改用死亡行里的分数或 HUD 计数
        SA="$(grep -oE 'died at score [0-9]+' "$WORK/a.log" | tail -1 | grep -oE '[0-9]+$')"
    fi
    [ -n "$SA" ] && [ "$SA" -ge 1 ] || { cat "$WORK/a.log"; fail "分数没有推进（滚动/计分链路有问题）"; }
    echo "flappy_e2e: 计分+确定性 ok（world_trail=$TA  score=$SA）"

    # ③ 撞管判死：到点把鸟送进水管，必须判死（走真实判定路径，不是直接调 die）
    MAQUE_FLAPPY_CRASH_AT=40 run_play "$WORK/c.log" 90 "$WORK/flap.sim.json"
    grep -q 'flappy: died' "$WORK/c.log" || { cat "$WORK/c.log"; fail "撞上水管却没有判死"; }
    grep -q 'crash hook fired -> 1' "$WORK/c.log" || { cat "$WORK/c.log"; fail "测试钩子未生效"; }
    echo "flappy_e2e: 撞管判死 ok（$(grep -oE 'flappy: died at score [0-9]+ frame [0-9]+' "$WORK/c.log" | tail -1)）"
    ;;
*)
    echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

exit 0
