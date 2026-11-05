#!/usr/bin/env bash
# samples/tank 端到端测试（FC《坦克大战》复刻）。
#
#   smoke  <maque_player> <tank.scn>   标题点击 → 新游戏 → 动态 stage 场景加载 → 关卡 intro
#   play   <maque_player> <tank.scn>   autoplay 2200 帧：开火/打砖/击杀/计分 + 两次 world_trail 一致
#   rules  <maque_player> <tank.scn>   真实代码路径的规则探针：
#                                        砖块弹道、基地摧毁、6 种道具、stage3 场景加载
#   zorder <maque_player>               2D z_index 回归（docs/bugs/013，PPM 中心像素）
#
# 约定：exit 77 = 环境不满足（无渲染插件 / 无 Python），CTest SKIP_RETURN_CODE 识别。
set -u

MODE="${1:-}"; BIN="${2:-}"; SCN="${3:-}"
if [ -z "$MODE" ] || [ -z "$BIN" ]; then
    echo "usage: tank_e2e.sh <smoke|play|rules|zorder> <maque_player> [scene.scn]" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
if [ -f "$DIR/maque_python.sh" ]; then . "$DIR/maque_python.sh"; fi
PY="${PY:-}"
if [ -z "$PY" ]; then
    for c in python3 python; do
        command -v "$c" >/dev/null 2>&1 && "$c" -c 'pass' >/dev/null 2>&1 && { PY="$c"; break; }
    done
fi

WORK="$(mktemp -d 2>/dev/null || echo "./tank-e2e-$$")"
mkdir -p "$WORK" || exit 77
ZSHOT="./tank-zorder-$$.ppm"
trap 'rm -rf "$WORK"; rm -f "$ZSHOT"' EXIT

export MAQUE_HEADLESS=1
export MAQUE_WINDOW=0x0
export MAQUE_AUDIO=off
export MAQUE_TANK_BGM=0
unset MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_SIM MAQUE_SIM_CLICK 2>/dev/null || true
unset MAQUE_TANK_AUTOPLAY MAQUE_TANK_GOD MAQUE_TANK_TRACE MAQUE_TANK_SKIP_TITLE \
      MAQUE_TANK_STAGE MAQUE_TANK_BRICK_AT MAQUE_TANK_BASE_AT MAQUE_TANK_POWER_AT \
      MAQUE_TANK_POWER_KIND 2>/dev/null || true

fail() { echo "tank_e2e[$MODE]: FAIL $*" >&2; exit 1; }
skip() { echo "tank_e2e[$MODE]: SKIP $*" >&2; exit 77; }

run() {  # run <log> <frames> [VAR=value ...]
    local log="$1" frames="$2"; shift 2
    env "$@" MAQUE_MAX_FRAMES="$frames" "$BIN" "$SCN" >"$log" 2>&1
    return 0
}

check_clean() {
    if grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|SUMMARY: AddressSanitizer|stack-overflow|runtime error:|script:lua|attempt to ' "$1"; then
        cat "$1"
        fail "运行期报错"
    fi
}

need_renderer() {
    grep -q "runtime: renderer '" "$1" || skip "渲染插件不可用（无 wgpu/GPU）"
}

trail_of() { sed -n 's/.*world_trail=\(0x[0-9a-f]*\).*/\1/p' "$1" | tail -1; }
count_of() { grep -cE "$2" "$1"; }
last_of()  { grep -oE "$2" "$1" | tail -1; }

KILL_RE='tank: kill idx=[0-9]+ kind=[a-z]+ score=[0-9]+ total_score=[0-9]+ remaining=[0-9]+ frame=[0-9]+'

case "$MODE" in
smoke)
    run "$WORK/smoke.log" 360 MAQUE_SIM_CLICK="20:320:240"
    check_clean "$WORK/smoke.log"
    need_renderer "$WORK/smoke.log"
    grep -q 'tank: ready 640x480' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "未进入 ready"; }
    grep -q 'tank: new game stage=1' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "标题点击没有开始新游戏"; }
    grep -q 'tank: stage scene ready stage=1' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "动态 stage 场景没有加载"; }
    grep -q 'tank: stage 1 start enemies=20' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "关卡没有开始"; }
    grep -q 'tank: stage 1 play' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "intro 没有进入 play"; }
    echo "tank_smoke: ok（标题→新游戏→stage 场景→play，360 帧）"
    ;;

play)
    run "$WORK/a.log" 2200 MAQUE_TANK_AUTOPLAY=1 MAQUE_TANK_GOD=1 MAQUE_TANK_SKIP_TITLE=1
    check_clean "$WORK/a.log"
    need_renderer "$WORK/a.log"
    grep -q 'tank: stage 1 play' "$WORK/a.log" || { cat "$WORK/a.log"; fail "未进入 play"; }
    FIRES=$(count_of "$WORK/a.log" 'tank: player fire')
    HITS=$(count_of "$WORK/a.log" 'tank: hit enemy=')
    KILLS=$(count_of "$WORK/a.log" 'tank: kill idx=')
    BRICKS=$(count_of "$WORK/a.log" 'tank: brick broke')
    [ "$FIRES" -ge 1 ] || { cat "$WORK/a.log"; fail "autoplay 没有开火"; }
    [ "$BRICKS" -ge 1 ] || { cat "$WORK/a.log"; fail "子弹没有打碎砖块"; }
    [ "$HITS" -ge 1 ] || { cat "$WORK/a.log"; fail "autoplay 没有命中敌人"; }
    [ "$KILLS" -ge 1 ] || { cat "$WORK/a.log"; fail "autoplay 没有击杀"; }
    LASTKILL=$(last_of "$WORK/a.log" "$KILL_RE")
    SCORE=$(echo "$LASTKILL" | sed -n 's/.*total_score=\([0-9]*\).*/\1/p')
    [ -n "$SCORE" ] && [ "$SCORE" -ge 100 ] || { cat "$WORK/a.log"; fail "击杀没有计分（score=$SCORE）"; }

    run "$WORK/b.log" 2200 MAQUE_TANK_AUTOPLAY=1 MAQUE_TANK_GOD=1 MAQUE_TANK_SKIP_TITLE=1
    TA=$(trail_of "$WORK/a.log"); TB=$(trail_of "$WORK/b.log")
    [ -n "$TA" ] || skip "日志里没有 world_trail（引擎版本差异）"
    [ "$TA" = "$TB" ] || fail "两次运行 world_trail 不一致：$TA vs $TB"
    KA=$(last_of "$WORK/a.log" "$KILL_RE"); KB=$(last_of "$WORK/b.log" "$KILL_RE")
    [ -n "$KA" ] || { cat "$WORK/a.log"; fail "日志里没有完整击杀行"; }
    [ "$KA" = "$KB" ] || fail "两次运行的击杀序列不一致：'$KA' vs '$KB'"
    echo "tank_play: ok（fire=$FIRES brick=$BRICKS hit=$HITS kill=$KILLS score=$SCORE，world_trail=$TA）"
    ;;

rules)
    need_log="$WORK/rules-ready.log"
    run "$need_log" 220 MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_GOD=1
    check_clean "$need_log"
    need_renderer "$need_log"

    # ① 真实弹道打砖块
    run "$WORK/brick.log" 220 MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_GOD=1 MAQUE_TANK_BRICK_AT=130
    check_clean "$WORK/brick.log"
    grep -q 'tank: drill brick armed' "$WORK/brick.log" || { cat "$WORK/brick.log"; fail "brick 探针没有布场"; }
    grep -q 'tank: brick broke' "$WORK/brick.log" || { cat "$WORK/brick.log"; fail "真实子弹没有打碎砖块"; }
    grep -q 'tank: brick broke.*cells=4' "$WORK/brick.log" || { cat "$WORK/brick.log"; fail "子弹只打掉半块墙（应整块 2x2 消掉）"; }

    # ② 真实弹道摧毁基地 → game over
    run "$WORK/base.log" 220 MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_GOD=1 MAQUE_TANK_BASE_AT=130
    check_clean "$WORK/base.log"
    grep -q 'tank: drill base armed' "$WORK/base.log" || { cat "$WORK/base.log"; fail "base 探针没有布场"; }
    grep -q 'tank: base destroyed' "$WORK/base.log" || { cat "$WORK/base.log"; fail "子弹没有摧毁基地"; }
    grep -q 'tank: game over reason=base' "$WORK/base.log" || { cat "$WORK/base.log"; fail "基地摧毁后没有 game over"; }

    # ③ 六种道具（真实 spawn → 玩家碰撞拾取 → 效果）
    power_case() {  # <kind> <expected regex> <desc>
        local kind="$1" pat="$2" desc="$3"
        local log="$WORK/power-$kind.log"
        run "$log" 220 MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_GOD=1 \
            MAQUE_TANK_POWER_AT=130 MAQUE_TANK_POWER_KIND="$kind"
        check_clean "$log"
        grep -q "tank: drill power kind=$kind" "$log" || { cat "$log"; fail "$desc 探针没有布场"; }
        grep -qE "$pat" "$log" || { cat "$log"; fail "$desc 未生效（$pat）"; }
        echo "  powerup $kind ok"
    }
    power_case star   'tank: powerup star level=1'            'star'
    power_case helmet 'tank: powerup helmet shield=10'        'helmet'
    power_case grenade 'tank: powerup grenade kills=[1-9][0-9]*' 'grenade'
    power_case shovel 'tank: powerup shovel base walls steel' 'shovel'
    power_case tank   'tank: powerup tank lives=3'            'tank'
    power_case timer  'tank: powerup timer freeze=8'          'timer'

    # ④ 清关 → 卸载 stage1 → 动态实例化 stage2 → intro → play（场景生命周期）
    run "$WORK/clear.log" 420 MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_GOD=1 MAQUE_TANK_CLEAR_AT=140
    check_clean "$WORK/clear.log"
    grep -q 'tank: stage 1 clear' "$WORK/clear.log" || { cat "$WORK/clear.log"; fail "没有触发 stage 1 clear"; }
    grep -q 'tank: stage 2 loading scene=stages/stage2.scn' "$WORK/clear.log" || { cat "$WORK/clear.log"; fail "清关后没有请求 stage2 场景"; }
    grep -q 'tank: stage scene ready stage=2' "$WORK/clear.log" || { cat "$WORK/clear.log"; fail "stage2 动态实例没有构建"; }
    grep -q 'tank: stage 2 play' "$WORK/clear.log" || { cat "$WORK/clear.log"; fail "stage2 没有进入 play"; }
    echo "  stage clear -> stage2 ok"

    # ④ 第 3 关动态场景（树/冰/水）能被加载并建出碰撞网格
    run "$WORK/stage3.log" 12 MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_STAGE=3
    check_clean "$WORK/stage3.log"
    grep -q 'tank: stage scene ready stage=3' "$WORK/stage3.log" || { cat "$WORK/stage3.log"; fail "stage3 场景没有加载"; }
    grep -q 'tiles=[1-9][0-9]*' "$WORK/stage3.log" || { cat "$WORK/stage3.log"; fail "stage3 没有 tile 节点"; }
    # ⑤ 音频路径：抓音必须非静音（证明 audio_on / WAV 资源 / 开火音效真的在播）
    CAP="$WORK/tank_audio.wav"
    run "$WORK/audio.log" 160 MAQUE_AUDIO=capture MAQUE_AUDIO_CAPTURE="$CAP" \
        MAQUE_TANK_SKIP_TITLE=1 MAQUE_TANK_AUTOPLAY=1 MAQUE_TANK_STAGE=1 MAQUE_TANK_GOD=1
    check_clean "$WORK/audio.log"
    [ -s "$CAP" ] || { cat "$WORK/audio.log"; fail "音频抓取没有生成 WAV"; }
    "$PY" - "$CAP" <<'PY' || { cat "$WORK/audio.log"; fail "坦克音频是静音"; }
import struct, sys, wave
w = wave.open(sys.argv[1], "rb")
data = w.readframes(w.getnframes())
assert w.getsampwidth() == 2 and len(data) > 0, (w.getsampwidth(), len(data))
vals = struct.unpack("<%dh" % (len(data) // 2), data)
rms = (sum(v * v for v in vals) / len(vals)) ** 0.5
print(f"tank audio rms={rms:.0f}")
raise SystemExit(0 if rms > 100 else 1)
PY
    echo "  audio capture ok"

    echo "tank_rules: ok（砖/基地/6 道具/清关→stage2/stage3/音频）"
    ;;

zorder)
    ZSCN="$ROOT/tests/data/zorder/zorder.scn"
    [ -f "$ZSCN" ] || fail "缺少 $ZSCN"
    [ -n "$PY" ] || skip "没有可用 Python（解析 PPM 像素）"
    "$BIN" --shot "$ZSHOT" --shot-frame 2 "$ZSCN" >"$WORK/z.log" 2>&1
    check_clean "$WORK/z.log"
    need_renderer "$WORK/z.log"
    grep -q 'zorder: id=' "$WORK/z.log" || { cat "$WORK/z.log"; fail "zorder 脚本没有运行"; }
    [ -f "$ZSHOT" ] || { cat "$WORK/z.log"; fail "截图没有落盘：$ZSHOT"; }
    $PY - "$ZSHOT" <<'PY' || fail "2D z_index 没有生效（中心像素不是绿色）"
import sys
from pathlib import Path
b = Path(sys.argv[1]).read_bytes()
parts = b.split(b"\n", 3)
assert parts[0] == b"P6", parts[0]
w, h = map(int, parts[1].split())
data = parts[3]
x, y = w // 2, h // 2
i = (y * w + x) * 3
r, g, bl = data[i], data[i + 1], data[i + 2]
sys.stderr.write(f"zorder center={r},{g},{bl}\n")
raise SystemExit(0 if (g > 200 and r < 50 and bl < 50) else 1)
PY
    echo "tank_zorder: ok（z=9000 的绿色块盖住 z=0 的红色块）"
    ;;

*)
    echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

exit 0
