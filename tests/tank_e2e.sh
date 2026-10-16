#!/usr/bin/env bash
# tank 端到端测试（samples/tank）
#
#   smoke <maque_tank> <scene.scn>   场景/脚本/素材能加载，跑 60 帧不崩
#   play  <maque_tank> <scene.scn>   玩法：autoplay 900 帧（开火→命中→扣血→击毁→计分）
#                                    + 同一输入两次运行 world_trail 一致（确定性）
#                                    + MAQUE_TANK_KILL_AT 钩子确实扣血/击毁（判定没失效）
#   kill  <maque_tank> <scene.scn>   判定鉴别力单测：KILL_AT 强制瞄准开火 → 必须扣血→击毁→加分，
#                                    且同一输入两次运行的击杀帧逐位一致
#
# 约定（与其它 e2e 一致）：exit 77 = 跳过（环境不满足），由 CTest 的 SKIP_RETURN_CODE 识别。
set -u

MODE="${1:-}"; BIN="${2:-}"; SCN="${3:-}"
if [ -z "$MODE" ] || [ -z "$BIN" ] || [ -z "$SCN" ]; then
    echo "usage: tank_e2e.sh <smoke|play|kill> <maque_tank> <scene.scn>" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
# 需要 Python 解析日志（Windows 上 python3 可能是 Store 占位程序）
if [ -f "$DIR/maque_python.sh" ]; then . "$DIR/maque_python.sh"; fi
PY="${PY:-}"
if [ -z "$PY" ]; then
    for c in python3 python; do command -v "$c" >/dev/null 2>&1 && "$c" -c 'pass' >/dev/null 2>&1 && { PY="$c"; break; }; done
fi

WORK="$(mktemp -d 2>/dev/null || echo "./tank-e2e-$$")"
mkdir -p "$WORK" || exit 77
trap 'rm -rf "$WORK"' EXIT

export MAQUE_HEADLESS=1
export MAQUE_WINDOW=0x0            # 不要真窗口（CI/无显示环境）
unset MAQUE_PRESENT MAQUE_CAPTURE MAQUE_SIM_CLICK 2>/dev/null || true

fail() { echo "tank_e2e[$MODE]: FAIL $*" >&2; exit 1; }
skip() { echo "tank_e2e[$MODE]: SKIP $*" >&2; exit 77; }

# 跑一次：$1=输出文件 $2=帧数 $3=AUTOPLAY(0/1) $4=KILL_AT(帧号或空)
run() {
    local out="$1" frames="$2" auto="${3:-0}" killat="${4:-}"
    MAQUE_MAX_FRAMES="$frames" MAQUE_TANK_AUTOPLAY="$auto" MAQUE_TANK_KILL_AT="$killat" \
        "$BIN" "$SCN" >"$out" 2>&1
    return 0
}

# 日志里出现过就报错的东西（崩溃/Sanitizer/断言）
check_clean() {
    if grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|SUMMARY: AddressSanitizer|stack-overflow|runtime error:' "$1"; then
        cat "$1"; fail "运行期报错"
    fi
}

# 取最后一个匹配值（日志按 "key=值" 形式）
last_of() { grep -oE "$2" "$1" | tail -1; }
count_of() { grep -cE "$2" "$1"; }

# 一条完整的击毁日志（用于"两次运行是否逐字一致"）
KILL_RE='tank: kill idx=[0-9]+ total=[0-9]+ score=[0-9]+ total_score=[0-9]+ alive=[0-9]+ frame=[0-9]+'

trail_of() { sed -n 's/.*world_trail=\(0x[0-9a-f]*\).*/\1/p' "$1" | tail -1; }

case "$MODE" in
smoke)
    run "$WORK/smoke.log" 60
    grep -q 'tank: ready' "$WORK/smoke.log" || { echo "--- 日志 ---"; cat "$WORK/smoke.log"; fail "脚本未进入 ready（场景/脚本没加载？）"; }
    grep -q 'renderer plugin' "$WORK/smoke.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    grep -q 'tank: wave 1 start enemies=5' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "波次没有生成（敌坦部件缺失？）"; }
    check_clean "$WORK/smoke.log"
    echo "tank_smoke: ok（ready + 渲染插件 + 5 敌坦 + 无报错，60 帧）"
    ;;

play)
    # ① autoplay 900 帧：玩家自驱动（转向最近敌人 + 预判瞄准 + 开火），必须打出"命中→扣血→击毁→计分"
    run "$WORK/a.log" 900 1
    grep -q 'tank: ready' "$WORK/a.log" || { cat "$WORK/a.log"; fail "未进入 ready"; }
    grep -q 'tank: ready 640x480 enemies=5 autoplay=1' "$WORK/a.log" || { cat "$WORK/a.log"; fail "autoplay 钩子没生效"; }
    HITS=$(count_of "$WORK/a.log" 'tank: hit enemy=[0-9]+ hp=[0-9]+')
    KILLS=$(count_of "$WORK/a.log" 'tank: kill idx=[0-9]+')
    [ "$HITS" -ge 1 ] || { cat "$WORK/a.log"; fail "autoplay 900 帧一次都没命中（开火/判定链路断了）"; }
    [ "$KILLS" -ge 1 ] || { cat "$WORK/a.log"; fail "autoplay 900 帧一个敌人都没击毁"; }
    # 击毁行里的本局得分必须真的加上去（100/辆）
    MAXSCORE=$(last_of "$WORK/a.log" "$KILL_RE" | sed 's/.*score=\([0-9]*\).*/\1/')
    [ -n "$MAXSCORE" ] && [ "$MAXSCORE" -ge 100 ] || { cat "$WORK/a.log"; fail "击毁没有计分（score=$MAXSCORE）"; }
    PLAYERHITS=$(count_of "$WORK/a.log" 'tank: player hit hp=[0-9]+')
    check_clean "$WORK/a.log"
    echo "tank_e2e: autoplay ok（hits=$HITS kills=$KILLS score=$MAXSCORE player_hits=$PLAYERHITS）"

    # ② 确定性：同一输入（autoplay）两次运行 → world_trail 必须逐位一致
    run "$WORK/b.log" 900 1
    TA=$(trail_of "$WORK/a.log"); TB=$(trail_of "$WORK/b.log")
    [ -n "$TA" ] || skip "日志里没有 world_trail（引擎版本差异）"
    [ "$TA" = "$TB" ] || fail "两次运行 world_trail 不一致：$TA vs $TB"
    KA=$(last_of "$WORK/a.log" "$KILL_RE")
    KB=$(last_of "$WORK/b.log" "$KILL_RE")
    [ -n "$KA" ] || { cat "$WORK/a.log"; fail "日志里没有完整的击毁行（断言会失效）"; }
    [ "$KA" = "$KB" ] || fail "两次运行的击杀序列不一致：'$KA' vs '$KB'"
    echo "tank_e2e: 确定性 ok（world_trail=$TA，末次击杀一致）"

    # ③ 判定不许失效：KILL_AT=60 强制把炮口对准最近敌人并开火一次
    #    （只改朝向/开火时机，命中与计分走真实代码路径）
    run "$WORK/c.log" 260 0 60
    grep -q 'tank: kill hook armed at frame 60' "$WORK/c.log" || { cat "$WORK/c.log"; fail "KILL_AT 钩子未装载"; }
    grep -q 'tank: kill hook fired frame=60 fired=1' "$WORK/c.log" || { cat "$WORK/c.log"; fail "KILL_AT 钩子未开火"; }
    grep -qE 'tank: hit enemy=4 hp=20' "$WORK/c.log" || { cat "$WORK/c.log"; fail "强制开火没命中/没扣血（判定失效？）"; }
    grep -qE 'tank: hit enemy=4 hp=0' "$WORK/c.log" || { cat "$WORK/c.log"; fail "第二发没打死（扣血/击毁链路断了）"; }
    grep -qE 'tank: kill idx=4 .*score=100' "$WORK/c.log" || { cat "$WORK/c.log"; fail "击毁没有计分"; }
    check_clean "$WORK/c.log"
    echo "tank_e2e: KILL_AT ok（$(last_of "$WORK/c.log" "$KILL_RE")）"
    ;;

kill)
    # 判定鉴别力单测：KILL_AT 的整条链路（瞄准→开火→命中→扣血→击毁→计分）必须成立，且可复现
    run "$WORK/k1.log" 260 0 60
    run "$WORK/k2.log" 260 0 60
    grep -q 'tank: ready' "$WORK/k1.log" || { cat "$WORK/k1.log"; fail "未进入 ready"; }
    grep -q 'tank: kill hook fired frame=60 fired=1' "$WORK/k1.log" || { cat "$WORK/k1.log"; fail "钩子未生效"; }
    H20=$(count_of "$WORK/k1.log" 'tank: hit enemy=4 hp=20')
    H0=$(count_of "$WORK/k1.log" 'tank: hit enemy=4 hp=0')
    [ "$H20" -ge 1 ] || { cat "$WORK/k1.log"; fail "第一发没扣血（40→20）"; }
    [ "$H0" -ge 1 ] || { cat "$WORK/k1.log"; fail "第二发没打到 0 血"; }
    grep -qE 'tank: kill idx=4 .*total=1 score=100 total_score=100 alive=4' "$WORK/k1.log" || { cat "$WORK/k1.log"; fail "击毁/计分/剩余敌人计数不对"; }
    grep -q 'tank: player dead' "$WORK/k1.log" && fail "静止玩家不应在 260 帧内阵亡（这会让 KILL_AT 断言不稳定）"
    T1=$(trail_of "$WORK/k1.log"); T2=$(trail_of "$WORK/k2.log")
    [ -n "$T1" ] || skip "日志里没有 world_trail（引擎版本差异）"
    [ "$T1" = "$T2" ] || fail "两次 KILL_AT 运行 world_trail 不一致：$T1 vs $T2"
    K1=$(last_of "$WORK/k1.log" "$KILL_RE")
    K2=$(last_of "$WORK/k2.log" "$KILL_RE")
    [ -n "$K1" ] || { cat "$WORK/k1.log"; fail "日志里没有完整的击毁行（断言会失效）"; }
    [ "$K1" = "$K2" ] || fail "两次运行的击杀帧不一致：'$K1' vs '$K2'"
    check_clean "$WORK/k1.log"
    echo "tank_kill: ok（40→20→0 血 + 击毁计分 + 两次一致：$K1 / world_trail=$T1）"
    ;;

*)
    echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

exit 0
