#!/usr/bin/env bash
# DragonBones 端到端测试（samples/dragonbones，docs/36）
#
#   smoke        <maque_player> <mecha.scn>   装配（骨架/图集/页纹理）+ 绘制数据非空 + 跑帧不崩
#   switch       <maque_player> <mecha.scn>   MAQUE_SIM 注入 ↑/↓/空格 → 断言动画按注入顺序切换，
#                                             且脚本读回的"引擎当前动画"与请求一致；再像素自证
#   determinism  <maque_player> <mecha.scn>   同场景跑两次（定格时间）→ 截图与 world_trail 逐字节一致
#
# 约定（与其它 e2e 一致）：exit 77 = 跳过（环境不满足），由 CTest 的 SKIP_RETURN_CODE 识别。
set -u

MODE="${1:-}"; BIN="${2:-}"; SCN="${3:-}"
if [ -z "$MODE" ] || [ -z "$BIN" ] || [ -z "$SCN" ]; then
    echo "usage: dragonbones_e2e.sh <smoke|switch|determinism> <maque_player> <mecha.scn>" >&2
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
WORK="$(mktemp -d -p . 2>/dev/null || echo "./dragonbones-e2e-$$")"
mkdir -p "$WORK" || exit 77
trap 'rm -rf "$WORK"' EXIT

export MAQUE_HEADLESS=1
export MAQUE_PRESENT=off           # 纯离屏：不需要窗口/显示，截图走渲染目标回读
unset MAQUE_WINDOW MAQUE_SIM MAQUE_SIM_CLICK MAQUE_CAPTURE MAQUE_TRACE_DRAGONBONES 2>/dev/null || true

fail() { echo "dragonbones_e2e[$MODE]: FAIL $*" >&2; exit 1; }
skip() { echo "dragonbones_e2e[$MODE]: SKIP $*" >&2; exit 77; }

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
    # 注意别把装配诊断 `missing_tex=0` 当成错误（它含 "missing"）：只匹配真正的失败行。
    grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|stack-overflow|runtime: dragonbones .*(read failed|parse failed|buffers full)' "$1"
}

# 场景里 mecha 的世界位置 (240,30) + 自适应缩放 ~0.71（节点 scale）：
# 屏幕包围盒约 (125,88)-(292,312)（见 dragonbones.js 的 fitOnce 与 MAQUE_TRACE_DRAGONBONES）；
# 框选时**排除 HUD**（y≈306 的文字）与顶部提示（y≈12），否则"角色没画出来"也会因文字像素误判通过。
BOX="(118, 80, 300, 300)"

case "$MODE" in
smoke)
    MAQUE_TRACE_DRAGONBONES=1 run "$WORK/smoke.log" 40
    grep -q 'dragonbones: ready' "$WORK/smoke.log" || { echo "--- 日志 ---"; cat "$WORK/smoke.log"; fail "脚本未进入 ready（骨架/场景没装配？）"; }
    grep -q "runtime: renderer '" "$WORK/smoke.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    grep -q "runtime: dragonbones entity" "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "DragonBones 实体未装配"; }
    grep -q "anim=idle" "$WORK/smoke.log" || { cat "$WORK/smoke.log"; fail "idle 未播放"; }
    # 引擎侧：页纹理真的上传了（句柄非 -1）+ 尺寸来自图片/图集
    grep -q "pages=1 page_textures \[0\] sizes \[1024x1024\]" "$WORK/smoke.log" || {
        cat "$WORK/smoke.log"; fail "图集页纹理未就位（或尺寸不对）"; }
    # 绘制数据面：14 个可见 image 槽 × 4 顶点（2 个隐藏 + 2 个嵌套子骨架未支持）
    grep -q 'verts=56 idx=84' "$WORK/smoke.log" || {
        grep -o '\[dragonbones\].*' "$WORK/smoke.log" | head -3
        cat "$WORK/smoke.log"; fail "绘制数据规模不符（期望 56 顶点 / 84 索引）"; }
    has_errors "$WORK/smoke.log" && { cat "$WORK/smoke.log"; fail "运行期报错"; }
    echo "dragonbones_smoke: ok（$(grep -oE 'dragonbones: animations [0-9]+ \[[^]]*\]' "$WORK/smoke.log" | head -1)）"
    ;;

switch)
    [ -n "$PY" ] || skip "需要 Python 3 生成 .sim.json 并校验切换序列"
    "$PY" - "$WORK/switch.sim.json" <<'PY'
import json, sys
ev = [
    {"frame": 20,  "type": "key_down", "key": 0x40000051},   # SDL ↓ → 规范 DOWN(9) → 下一个
    {"frame": 22,  "type": "key_up",   "key": 0x40000051},
    {"frame": 40,  "type": "key_down", "key": 0x40000052},   # SDL ↑ → 规范 UP(8) → 上一个
    {"frame": 42,  "type": "key_up",   "key": 0x40000052},
    {"frame": 60,  "type": "key_down", "key": 0x40000051},
    {"frame": 62,  "type": "key_up",   "key": 0x40000051},
    {"frame": 80,  "type": "key_down", "key": 0x40000052},
    {"frame": 82,  "type": "key_up",   "key": 0x40000052},
    {"frame": 100, "type": "key_down", "key": 32},           # SDL 空格 → 规范 SPACE(2) → 暂停
    {"frame": 102, "type": "key_up",   "key": 32},
    {"frame": 120, "type": "key_down", "key": 2},            # 规范码 SPACE(2) 直写 → 继续
    {"frame": 122, "type": "key_up",   "key": 2},
]
json.dump({"name": "dragonbones_switch", "space": "world", "events": ev}, open(sys.argv[1], "w"))
PY
    [ -s "$WORK/switch.sim.json" ] || fail "sim 文件生成失败"

    run "$WORK/play.log" 140 "$WORK/switch.sim.json"
    grep -q 'dragonbones: ready' "$WORK/play.log" || { cat "$WORK/play.log"; fail "未进入 ready"; }
    grep -q "runtime: renderer '" "$WORK/play.log" || skip "渲染插件不可用（无 wgpu/GPU）"
    has_errors "$WORK/play.log" && { cat "$WORK/play.log"; fail "运行期报错"; }

    # ① 切换序列：↓=+1、↑=-1（环绕），且脚本读回的引擎当前动画 == 请求
    if ! "$PY" - "$WORK/play.log" <<'PY'
import re, sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = re.search(r'dragonbones: animations (\d+) \[([^\]]*)\]', log)
assert m, '日志里没有动画清单'
n = int(m.group(1)); names = [s.strip() for s in m.group(2).split(',') if s.strip()]
assert n == len(names) and n >= 2, f'动画清单异常 n={n} names={names}'
lines = re.findall(r'dragonbones: anim=(\S+) idx=(\d+)/(\d+) engine=(\S+)', log)
assert len(lines) == 5, f'期望 5 次切换（start+4），实际 {len(lines)}: {lines}'
want = [int(lines[0][1])]
for d in (1, -1, 1, -1):
    want.append((want[-1] + d) % n)
got = [int(l[1]) for l in lines]
assert got == want, f'切换顺序不符：期望 {want}（{[names[i] for i in want]}），实际 {got}（{[names[i] for i in got]}）'
for name, idx, tot, eng in lines:
    assert name == names[int(idx)], f'名字与下标不符 {name} vs {names[int(idx)]}'
    assert eng == name, f'引擎当前动画 {eng} != 脚本请求 {name}（dragonbones_play 未生效？）'
    assert int(tot) == n
paused = re.findall(r'dragonbones: paused=(\w+)', log)
assert paused == ['true', 'false'], f'空格暂停/继续异常：{paused}'
print(f'dragonbones_e2e: 切换链路 ok  起始={names[want[0]]}  序列={[names[i] for i in want]}  动画数={n}')
PY
    then
        cat "$WORK/play.log"; fail "切换链路断言失败"
    fi

    # ② 两张截图：同一帧（30）、**不同动画** → 都画出了角色，且两张不同
    A="$("$PY" -c "import re,sys;log=open(sys.argv[1],encoding='utf-8',errors='replace').read();m=re.search(r'dragonbones: animations \\d+ \\[([^\\]]*)\\]',log);print([s.strip() for s in m.group(1).split(',')][0])" "$WORK/play.log")"
    B="$("$PY" -c "import re,sys;log=open(sys.argv[1],encoding='utf-8',errors='replace').read();m=re.search(r'dragonbones: animations \\d+ \\[([^\\]]*)\\]',log);print([s.strip() for s in m.group(1).split(',')][1])" "$WORK/play.log")"
    [ -n "$A" ] && [ -n "$B" ] || fail "取不到动画名"
    MAQUE_DRAGONBONES_ANIM="$A" MAQUE_DRAGONBONES_TIME=0.5 "$BIN" --shot "$WORK/anim_a.png" --shot-frame 30 "$SCN" >"$WORK/anim_a.log" 2>&1
    MAQUE_DRAGONBONES_ANIM="$B" MAQUE_DRAGONBONES_TIME=0.5 "$BIN" --shot "$WORK/anim_b.png" --shot-frame 30 "$SCN" >"$WORK/anim_b.log" 2>&1
    [ -s "$WORK/anim_a.png" ] || { cat "$WORK/anim_a.log"; fail "动画 A 截图未生成"; }
    [ -s "$WORK/anim_b.png" ] || { cat "$WORK/anim_b.log"; fail "动画 B 截图未生成"; }
    cmp -s "$WORK/anim_a.png" "$WORK/anim_b.png" && fail "两个动画的截图完全相同（切换没进渲染？）"

    # ③ 像素级：角色**真的画出来了**（网格不提交/UV 取错时框内只剩背景色）
    DB_SHOT="$WORK/anim_a.png" DB_BOX="$BOX" "$PY" - <<'PY' || { cat "$WORK/anim_a.log"; fail "角色像素断言失败"; }
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
        for i in range(stride):
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

box = tuple(int(v) for v in os.environ['DB_BOX'].strip('()').split(','))
w, h, px = load(os.environ['DB_SHOT'])
bg = tuple(px[0:3])                              # 画布底色 = 左上角像素
x0, y0, x1, y1 = box
n = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        o = (y*w + x) * 4
        if abs(px[o]-bg[0]) + abs(px[o+1]-bg[1]) + abs(px[o+2]-bg[2]) > 24:
            n += 1
assert n > 2000, f'角色区域非背景像素只有 {n}（背景 {bg}）—— 纹理/UV/网格提交有问题？'
print(f'dragonbones_e2e: 角色像素 ok（区域 {box} 内非背景像素 {n}，背景 {bg}）')
PY
    echo "dragonbones_e2e: 渲染自证 ok（anim '$A' vs '$B' 同帧截图不同，且角色像素已画出）"
    ;;

determinism)
    export MAQUE_DRAGONBONES_TIME=0.8
    run "$WORK/d1.log" 30
    "$BIN" --shot "$WORK/d1.png" --shot-frame 30 "$SCN" >"$WORK/d1s.log" 2>&1
    run "$WORK/d2.log" 30
    "$BIN" --shot "$WORK/d2.png" --shot-frame 30 "$SCN" >"$WORK/d2s.log" 2>&1
    [ -s "$WORK/d1.png" ] && [ -s "$WORK/d2.png" ] || fail "截图未生成"
    cmp -s "$WORK/d1.png" "$WORK/d2.png" || fail "两次运行的截图不一致（非确定性）"
    T1="$(sed -n 's/.*world_trail=\(0x[0-9a-f]*\).*/\1/p' "$WORK/d1.log" | tail -1)"
    T2="$(sed -n 's/.*world_trail=\(0x[0-9a-f]*\).*/\1/p' "$WORK/d2.log" | tail -1)"
    [ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "world_trail 不一致（$T1 vs $T2）"
    echo "dragonbones_determinism: ok（world_trail=$T1，截图 sha 一致）"
    ;;
*)
    echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

exit 0
