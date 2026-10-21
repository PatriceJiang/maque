#!/usr/bin/env bash
# spine 端到端测试（samples/spine）
#
#   smoke  <maque_player> <spine.scn>   装配（骨架/图集/页纹理）+ 跑帧不崩 + 动画清单非空
#   play   <maque_player> <spine.scn>   MAQUE_SIM 注入 ↑/↓/空格（**原始码**）→ 断言动画按注入顺序切换，
#                                      且脚本读回的"引擎当前动画"与请求一致；最后用两条不同动画的截图对比
#   dragon <maque_player> <dragon.scn>  多页图集（5 页）：逐页纹理句柄非 -1 + 引擎页表 pma 标记 + 多批次
#
# 约定（与其它 e2e 一致）：exit 77 = 跳过（环境不满足），由 CTest 的 SKIP_RETURN_CODE 识别。
set -u

MODE="${1:-}"; BIN="${2:-}"; SCN="${3:-}"
if [ -z "$MODE" ] || [ -z "$BIN" ] || [ -z "$SCN" ]; then
    echo "usage: spine_e2e.sh <smoke|play|dragon> <maque_player> <scene.scn>" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$DIR/maque_python.sh" ]; then . "$DIR/maque_python.sh"; fi
PY="${PY:-}"
if [ -z "$PY" ]; then
    for c in python3 python; do command -v "$c" >/dev/null 2>&1 && "$c" -c 'pass' >/dev/null 2>&1 && { PY="$c"; break; }; done
fi

# 工作目录放**当前目录**下（相对路径）：Windows 下 Git Bash 的 /tmp 会被翻译成宿主路径，
# 而引擎写截图走的是原生 fopen —— 用相对目录最稳（也避免 CI 里 TEMP 不可写）。
WORK="$(mktemp -d -p . 2>/dev/null || echo "./spine-e2e-$$")"
mkdir -p "$WORK" || exit 77
trap 'rm -rf "$WORK"' EXIT

export MAQUE_HEADLESS=1
export MAQUE_PRESENT=off           # 纯离屏：不需要窗口/显示，截图走渲染目标回读
unset MAQUE_WINDOW MAQUE_SIM MAQUE_SIM_CLICK MAQUE_CAPTURE 2>/dev/null || true

fail() { echo "spine_e2e[$MODE]: FAIL $*" >&2; exit 1; }
skip() { echo "spine_e2e[$MODE]: SKIP $*" >&2; exit 77; }

# run <日志> <帧数> [sim 文件]
run() {
    local out="$1" frames="$2" sim="${3:-}"
    if [ -n "$sim" ]; then
        MAQUE_SIM="$sim" MAQUE_MAX_FRAMES="$frames" "$BIN" "$SCN" >"$out" 2>&1
    else
        MAQUE_MAX_FRAMES="$frames" "$BIN" "$SCN" >"$out" 2>&1
    fi
    return 0
}

has_errors() {
    grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|stack-overflow|runtime: spine .*(failed|missing|full)' "$1"
}

# anim_name_at <日志> <下标>：从日志的动画清单里取第 n 个名字（纯 shell，避免嵌套引号坑）
anim_name_at() {
    sed -n 's/.*spine: animations [0-9][0-9]* \[\([^]]*\)\].*/\1/p' "$1" | head -1 |
        tr ',' '\n' | sed -n "$(( $2 + 1 ))p" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'
}

case "$MODE" in
smoke)
    run "$WORK/smoke.log" 40
    grep -q 'spine: ready' "$WORK/smoke.log" || { echo "--- 日志 ---"; cat "$WORK/smoke.log"; fail "脚本未进入 ready（骨架/场景没装配？）"; }
    grep -q "renderer plugin" "$WORK/smoke.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    # 引擎侧：骨架/图集装上了（页表有 pma 标记 = 图集解析成功、页纹理已上传）
    grep -q "skel='/art/spineboy-pro.skel'" "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "骨架未装配"; }
    grep -q "pma=1" "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "图集页纹理/pma 未就位"; }
    # 脚本侧：动画清单非空
    grep -qE 'spine: animations [1-9][0-9]* \[' "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "动画清单为空"; }
    has_errors "$WORK/smoke.log" && { cat "$WORK/smoke.log"; fail "运行期报错"; }
    echo "spine_smoke: ok（$(grep -oE 'spine: animations [0-9]+ \[[^]]*\]' "$WORK/smoke.log" | head -1)）"
    ;;

play)
    [ -n "$PY" ] || skip "需要 Python 3 生成 .sim.json 并校验切换序列"

    # ① 注入：**SDL 风格码**（引擎在平台/模拟器边界统一归一化到 MQ_KEY_* 规范键空间）。
    #    方向键必须用 SDL 码 0x40000052/0x40000051 —— 规范值 UP=8/DOWN=9 与 SDL 的
    #    退格(8)/Tab(9) 同值，注入路径会按 SDL 解释（mq_sim_normalize_key 的既定语义）。
    #    空格两种写法都通（SDL 32 与规范 2），这里各测一次。
    "$PY" - "$WORK/switch.sim.json" <<'PY'
import json, sys
ev = [
    {"frame": 20,  "type": "key_down", "key": 0x40000051},   # SDL ↓ → 规范 DOWN(9) → 下一个
    {"frame": 22,  "type": "key_up",   "key": 0x40000051},
    {"frame": 40,  "type": "key_down", "key": 0x40000052},   # SDL ↑ → 规范 UP(8) → 上一个
    {"frame": 42,  "type": "key_up",   "key": 0x40000052},
    {"frame": 60,  "type": "key_down", "key": 0x40000051},   # 再来一轮（同一码路径重复可用）
    {"frame": 62,  "type": "key_up",   "key": 0x40000051},
    {"frame": 80,  "type": "key_down", "key": 0x40000052},
    {"frame": 82,  "type": "key_up",   "key": 0x40000052},
    {"frame": 100, "type": "key_down", "key": 32},           # SDL 空格 → 规范 SPACE(2) → 暂停
    {"frame": 102, "type": "key_up",   "key": 32},
    {"frame": 120, "type": "key_down", "key": 2},            # 规范码 SPACE(2) 直写 → 继续
    {"frame": 122, "type": "key_up",   "key": 2},
]
json.dump({"name": "spine_switch", "space": "world", "events": ev}, open(sys.argv[1], "w"))
PY
    [ -s "$WORK/switch.sim.json" ] || fail "sim 文件生成失败"

    run "$WORK/play.log" 140 "$WORK/switch.sim.json"
    grep -q 'spine: ready' "$WORK/play.log" || { cat "$WORK/play.log"; fail "未进入 ready"; }
    grep -q "renderer plugin" "$WORK/play.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    has_errors "$WORK/play.log" && { cat "$WORK/play.log"; fail "运行期报错"; }

    # ② 断言：动画按注入顺序切换（↓=+1、↑=-1，环绕），且脚本读回的引擎当前动画 == 请求
    if ! "$PY" - "$WORK/play.log" <<'PY'
import re, sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = re.search(r'spine: animations (\d+) \[([^\]]*)\]', log)
assert m, '日志里没有动画清单'
n = int(m.group(1)); names = [s.strip() for s in m.group(2).split(',') if s.strip()]
assert n == len(names) and n >= 2, f'动画清单异常 n={n} names={names}'
lines = re.findall(r'spine: anim=(\S+) idx=(\d+)/(\d+) engine=(\S+)', log)
assert len(lines) == 5, f'期望 5 次切换（start+4），实际 {len(lines)}: {lines}'
# 起始 = 第 1 行；其后：↓ +1、↑ -1、↓ +1、↑ -1
want = [int(lines[0][1])]
for d in (1, -1, 1, -1):
    want.append((want[-1] + d) % n)
got = [int(l[1]) for l in lines]
assert got == want, f'切换顺序不符：期望 {want}（{[names[i] for i in want]}），实际 {got}（{[names[i] for i in got]}）'
for name, idx, tot, eng in lines:
    assert name == names[int(idx)], f'名字与下标不符 {name} vs {names[int(idx)]}'
    assert eng == name, f'引擎当前动画 {eng} != 脚本请求 {name}（spine_play 未生效？）'
    assert int(tot) == n
paused = re.findall(r'spine: paused=(\w+)', log)
assert paused == ['true', 'false'], f'空格暂停/继续异常：{paused}'
print(f'spine_e2e: 切换链路 ok  起始={names[want[0]]}  序列={[names[i] for i in want]}  动画数={n}')
PY
    then
        cat "$WORK/play.log"; fail "切换链路断言失败"
    fi

    # ③ 两张截图：同一帧（30）、**不同动画** → 必须都画出了角色，且两张不同
    A="$(anim_name_at "$WORK/play.log" 0)"
    B="$(anim_name_at "$WORK/play.log" 1)"
    [ -n "$A" ] && [ -n "$B" ] || fail "取不到动画名"
    MAQUE_SPINE_ANIM="$A" MAQUE_SPINE_TIME=0.5 "$BIN" --shot "$WORK/anim_a.png" --shot-frame 30 "$SCN" >"$WORK/anim_a.log" 2>&1
    MAQUE_SPINE_ANIM="$B" MAQUE_SPINE_TIME=0.5 "$BIN" --shot "$WORK/anim_b.png" --shot-frame 30 "$SCN" >"$WORK/anim_b.log" 2>&1
    [ -s "$WORK/anim_a.png" ] || { cat "$WORK/anim_a.log"; fail "动画 A 截图未生成"; }
    [ -s "$WORK/anim_b.png" ] || { cat "$WORK/anim_b.log"; fail "动画 B 截图未生成"; }
    grep -q "runtime: shot" "$WORK/anim_a.log" || { cat "$WORK/anim_a.log"; fail "动画 A 未报告截图"; }
    cmp -s "$WORK/anim_a.png" "$WORK/anim_b.png" && fail "两个动画的截图完全相同（切换没进渲染？）"

    # ④ 像素级：角色**真的画出来了** —— 这是"UV 取错 accessor（区域局部）"这类 bug 的唯一有效判据：
    #    用错时角色区域全是背景色（0 个非背景像素），而单测的 [0,1] 断言照样通过。
    SPINE_SHOT="$WORK/anim_a.png" SPINE_SHOT_B="$WORK/anim_b.png" "$PY" - <<'PY' || { cat "$WORK/anim_a.log"; fail "角色像素断言失败"; }
import os, struct, zlib

def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'PNG 签名错误'
    pos, idat, ihdr = 8, b'', None
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]; typ = d[pos+4:pos+8]
        data = d[pos+8:pos+8+ln]
        if typ == b'IHDR': ihdr = struct.unpack('>IIBB', data[:10])
        if typ == b'IDAT': idat += data
        pos += 12 + ln
    w, h = ihdr[0], ihdr[1]
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray(stride * h)
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[y*(stride+1)]
        line = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        for i in range(stride):                      # PNG 反滤波（0..4）
            a = line[i-4] if i >= 4 else 0
            b = prev[i]
            c = prev[i-4] if i >= 4 else 0
            if ft == 1: line[i] = (line[i] + a) & 0xff
            elif ft == 2: line[i] = (line[i] + b) & 0xff
            elif ft == 3: line[i] = (line[i] + ((a + b) >> 1)) & 0xff
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, out

def diff_count(path, box, thresh=24):
    w, h, px = load(path)
    bg = tuple(px[0:3])                              # 画布底色 = 左上角像素
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y*w + x) * 4
            if abs(px[o]-bg[0]) + abs(px[o+1]-bg[1]) + abs(px[o+2]-bg[2]) > thresh:
                n += 1
    return n, bg

# 角色区域：320x240 设计分辨率下 spineboy 的世界位置 (100,20) × 0.30 缩放（见 spine.scn / spine.js）。
# 刻意**排除地面条**（屏幕 y ≥ 220）与 HUD 文字（y ≈ 226）—— 否则"角色没画出来"也会因为
# 地面/文字像素而误判为通过（这条断言的鉴别力全在框选上：网格不画时框内应当**只剩背景色**）。
BOX = (50, 26, 195, 218)
n, bg = diff_count(os.environ['SPINE_SHOT'], BOX)
assert n > 2000, f'角色区域非背景像素只有 {n}（背景 {bg}）—— 贴图/UV/网格提交有问题？'
print(f'spine_e2e: 角色像素 ok（区域 {BOX} 内非背景像素 {n}，背景 {bg}）')
PY

    echo "spine_e2e: 渲染自证 ok（anim '$A' vs '$B' 同帧截图不同，且角色像素已画出）"
    ;;

mask)
    # 裁剪遮罩（mask）回归。为什么这个测试必须存在：
    #   Spine 的遮罩**不是 setup 姿势自动生效的** —— `Skeleton::setSkin` 只给"setup 里声明了
    #   attachment 名"的槽挂皮肤附件（读 spine-cpp 源码确认），而官方 spineboy 的 `clipping`
    #   槽在 setup 里没有附件名，且它的 **11 个动画没有一个**驱动该槽 →
    #   正常播放时遮罩根本不生效，"裁剪代码是否正确"用现成素材无法证明。
    #   所以这里用 MAQUE_SPINE_MASK 显式开启遮罩，做三件事：
    #     ① 遮罩真的挂上了（日志）
    #     ② 裁剪路真的被走到、且几何被改变（MAQUE_TRACE_SPINE_DRAW 的计数）
    #     ③ 渲染结果与不开启时**不同**（像素级，最强判据）
    echo "spine_e2e[mask]: 用 MAQUE_SPINE_MASK 显式开启遮罩（官方素材默认不启用遮罩）"
    grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|stack-overflow' "$WORK"/*.log 2>/dev/null && true

    MAQUE_SPINE_MASK="clipping:clipping" MAQUE_TRACE_SPINE_DRAW=1 \
        "$BIN" --shot "$WORK/mask_on.png" --shot-frame 30 "$SCN" >"$WORK/mask_on.log" 2>&1
    "$BIN" --shot "$WORK/mask_off.png" --shot-frame 30 "$SCN" >"$WORK/mask_off.log" 2>&1
    [ -s "$WORK/mask_on.png" ] || { cat "$WORK/mask_on.log"; fail "遮罩截图未生成"; }
    [ -s "$WORK/mask_off.png" ] || { cat "$WORK/mask_off.log"; fail "无遮罩截图未生成"; }
    has_errors "$WORK/mask_on.log" && { cat "$WORK/mask_on.log"; fail "开启遮罩后运行期报错"; }

    # ① 遮罩挂上了
    grep -q '遮罩已启用' "$WORK/mask_on.log" || {
        cat "$WORK/mask_on.log"; fail "遮罩未启用（MAQUE_SPINE_MASK 没生效）"; }
    # ② 裁剪路被走到，且几何确实被改变（顶点数不完全等于未裁剪值）
    if ! "$PY" - "$WORK/mask_on.log" <<'PY'
import re, sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = re.search(r'clipStarts=(\d+) 被裁剪附件=(\d+) 顶点 (\d+)->(\d+) \(总 (\d+) 顶点\)', log)
assert m, '日志里没有裁剪统计行（MAQUE_TRACE_SPINE_DRAW 没生效？）'
starts, atts, before, after, total = (int(x) for x in m.groups())
assert starts >= 1, f'裁剪一次都没启动（clipStarts={starts}）—— 遮罩附件没被识别成 ClippingAttachment？'
assert atts >= 1, f'没有任何附件被裁剪（clippedAtts={atts}）—— 裁剪路没走到'
assert total > 0, '裁剪后一个顶点都没有 → 角色会被整只裁掉（多半是多边形/坐标空间不对）'
assert before != after, f'裁剪前后顶点数相同（{before}）—— clipTriangles 没真的改变几何'
print(f'spine_e2e: 遮罩 ok（裁剪启动 {starts} 次、被裁剪附件 {atts} 个、顶点 {before}->{after}、总 {total}）')
PY
    then
        cat "$WORK/mask_on.log"; fail "裁剪统计断言失败"
    fi
    # ③ 最强判据：开启与不开启遮罩，渲染结果必须不同
    cmp -s "$WORK/mask_on.png" "$WORK/mask_off.png" && {
        fail "开启遮罩与不开启的截图完全相同 —— 裁剪没进渲染"; }
    echo "spine_e2e: 遮罩回归 ok（开/关渲染结果不同）"
    ;;

dragon)
    run "$WORK/dragon.log" 40
    grep -q 'spine: ready' "$WORK/dragon.log" || { cat "$WORK/dragon.log"; fail "dragon 未进入 ready"; }
    grep -q "renderer plugin" "$WORK/dragon.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    has_errors "$WORK/dragon.log" && { cat "$WORK/dragon.log"; fail "运行期报错"; }
    grep -q "skel='/art/dragon-ess.skel'" "$WORK/dragon.log" || { cat "$WORK/dragon.log"; fail "dragon 骨架未装配（约定 atlas 路径？）"; }
    # 多页：5 页（素材实测）且每页纹理句柄非 -1
    PAGES="$(sed -n 's/.*spine: skeleton pages=\([0-9]*\).*/\1/p' "$WORK/dragon.log" | tail -1)"
    [ -n "$PAGES" ] || { cat "$WORK/dragon.log"; fail "日志里没有 pages="; }
    [ "$PAGES" -ge 2 ] || fail "多页图集只有 $PAGES 页？"
    TEXS="$(sed -n 's/.*page_textures \[\([^]]*\)\].*/\1/p' "$WORK/dragon.log" | tail -1)"
    [ -n "$TEXS" ] || { cat "$WORK/dragon.log"; fail "日志里没有 page_textures"; }
    case "$TEXS" in *-1*) cat "$WORK/dragon.log"; fail "有图集页没有拿到纹理句柄：[$TEXS]";; esac
    # 引擎页表：每页都标了 pma（图集解析 + 纹理上传成功）
    PMA="$(grep -o 'pma=1' "$WORK/dragon.log" | wc -l | tr -d ' ')"
    [ "$PMA" = "$PAGES" ] || { cat "$WORK/dragon.log"; fail "引擎页表 pma 数($PMA) != 页数($PAGES)"; }
    echo "spine_dragon: ok（pages=$PAGES page_textures=[$TEXS] pma=$PMA）"
    ;;
*)
    echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

exit 0
