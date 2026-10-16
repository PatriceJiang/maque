#!/usr/bin/env bash
# 官方 spine demo 移植的端到端测试（samples/spine）
#
#   demos <maque_spine> <scene_dir>   逐个 demo 场景跑 N 帧：进入 ready + 动画清单非空 + 无报错
#   all   <maque_spine> <scene_dir>   all.scn 总览：出图 + **逐格像素断言** + 骨架数 == 期望
#
# 约定（与其它 e2e 一致）：exit 77 = 跳过（环境不满足），由 CTest 的 SKIP_RETURN_CODE 识别。
#
# 为什么 all 模式要做**逐格**像素断言：总览里有 12 个骨架，任何一格"没画出来"（比如
# mix-and-match 的 default 皮肤没有附件 → 0 顶点，实测过）在全图统计里都会被别的格子掩盖掉；
# 只有每格各自计数才抓得住。（这条断言实测抓到过 Goblins 漏设 skin 只剩长矛+盾的问题。）
set -u

MODE="${1:-}"; BIN="${2:-}"; DIR="${3:-}"
if [ -z "$MODE" ] || [ -z "$BIN" ] || [ -z "$DIR" ]; then
    echo "usage: spine_demos_e2e.sh <demos|all> <maque_spine> <scene_dir>" >&2
    exit 2
fi

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$TESTS_DIR/maque_python.sh" ]; then . "$TESTS_DIR/maque_python.sh"; fi
PY="${PY:-}"
if [ -z "$PY" ]; then
    for c in python3 python; do command -v "$c" >/dev/null 2>&1 && "$c" -c 'pass' >/dev/null 2>&1 && { PY="$c"; break; }; done
fi

WORK="$(mktemp -d -p . 2>/dev/null || echo "./spine-demos-$$")"
mkdir -p "$WORK" || exit 77
trap 'rm -rf "$WORK"' EXIT

export MAQUE_HEADLESS=1
export MAQUE_PRESENT=off
unset MAQUE_WINDOW MAQUE_SIM MAQUE_SIM_CLICK MAQUE_CAPTURE 2>/dev/null || true

fail() { echo "spine_demos[$MODE]: FAIL $*" >&2; exit 1; }
skip() { echo "spine_demos[$MODE]: SKIP $*" >&2; exit 77; }

has_errors() {
    grep -qiE 'panic|Assertion failed|ERROR: AddressSanitizer|stack-overflow|runtime: spine .*(failed|missing|full)|script error' "$1"
}

# 官方 demo 清单（cocos2d-x 11 个 + 官方 C++ 两个入口）
DEMOS="spineboy separator goblins raptor tank coin mixandmatch batching sequence ik physics glfw sdl"

case "$MODE" in
demos)
    for d in $DEMOS; do
        scn="$DIR/demo-$d.scn"
        [ -f "$scn" ] || fail "缺少场景 $scn"
        log="$WORK/$d.log"
        MAQUE_MAX_FRAMES=30 "$BIN" "$scn" >"$log" 2>&1
        grep -q "renderer plugin" "$log" || skip "渲染插件不可用（无 wgpu/GPU）"
        grep -q "spine\[" "$log" || { cat "$log"; fail "$d: 脚本没跑起来（没有 spine[...] 日志）"; }
        grep -q "ready " "$log" || { cat "$log"; fail "$d: 未进入 ready"; }
        # 动画清单非空（batching 打印一次、all 逐格打印；单个 demo 每个至少 1 个动画）
        grep -qE 'animations [1-9][0-9]* \[' "$log" || { cat "$log"; fail "$d: 动画清单为空"; }
        has_errors "$log" && { cat "$log"; fail "$d: 运行期报错"; }
        anims="$(sed -n 's/.*animations \([0-9]*\) \[\([^]]*\)\].*/\1 [\2]/p' "$log" | head -1)"
        echo "  $d: ok  animations $anims"
    done
    echo "spine_demos: ok（$(echo $DEMOS | wc -w) 个官方 demo 场景各跑 30 帧）"
    ;;

all)
    [ -n "$PY" ] || skip "需要 Python 3 解码 PNG 做像素断言"
    scn="$DIR/all.scn"
    [ -f "$scn" ] || fail "缺少 $scn"
    log="$WORK/all.log"
    "$BIN" --shot "$WORK/all.png" --shot-frame 30 "$scn" >"$log" 2>&1
    grep -q "renderer plugin" "$log" || skip "渲染插件不可用（无 wgpu/GPU）"
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "未出图"; }
    [ -s "$WORK/all.png" ] || fail "PNG 未生成"
    has_errors "$log" && { cat "$log"; fail "运行期报错"; }

    # 骨架数量：引擎日志的 spine instances=N 必须等于 all.scn 的格子数
    INST="$(sed -n 's/.*spine instances=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    EXPECT="$("$PY" -c "
import json,sys
d=json.load(open(r'$scn',encoding='utf-8'))
print(sum(1 for e in d['entities'] if e['name'].startswith('cell') and not e['name'].startswith('cellname')))
")"
    [ -n "$INST" ] || { cat "$log"; fail "日志里没有 spine instances="; }
    [ "$INST" = "$EXPECT" ] || { cat "$log"; fail "骨架数 $INST != 期望 $EXPECT"; }

    # 逐格像素断言 + 全图统计
    "$PY" - "$scn" "$WORK/all.png" <<'PY' || { cat "$log"; fail "逐格像素断言失败"; }
import json, struct, sys, zlib
scn_path, png_path = sys.argv[1], sys.argv[2]
d = json.load(open(scn_path, encoding='utf-8'))
cells = {}
for e in d['entities']:
    if e['name'].startswith('cell') and not e['name'].startswith('cellname'):
        tf = [c for c in e['components'] if c['type'] == 'scn.Transform2D'][0]
        cells[e['name']] = (tf['x'], tf['y'])
W, H = d['width'], d['height']
assert len(cells) >= 12, f'all.scn 只有 {len(cells)} 个格子'

raw_png = open(png_path, 'rb').read()
pos, idat, ihdr = 8, b'', None
while pos < len(raw_png):
    ln = struct.unpack('>I', raw_png[pos:pos+4])[0]; typ = raw_png[pos+4:pos+8]
    data = raw_png[pos+8:pos+8+ln]
    if typ == b'IHDR': ihdr = struct.unpack('>IIBB', data[:10])
    if typ == b'IDAT': idat += data
    pos += 12 + ln
w, h = ihdr[0], ihdr[1]
assert (w, h) == (W, H), f'截图尺寸 {(w,h)} != 场景 {(W,H)}'
raw = zlib.decompress(idat); stride = w * 4
out = bytearray(stride * h); prev = bytearray(stride)
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
    out[y*stride:(y+1)*stride] = line; prev = line

bg = tuple(out[0:3])
def diff(box):
    x0, y0, x1, y1 = box
    n = 0
    for y in range(max(0, y0), min(h, y1)):
        for x in range(max(0, x0), min(w, x1)):
            o = (y*w + x) * 4
            if abs(out[o]-bg[0]) + abs(out[o+1]-bg[1]) + abs(out[o+2]-bg[2]) > 30:
                n += 1
    return n

bad = []
total = 0
for name, (cx, cy) in sorted(cells.items()):
    # 场景 y 向上 → 图像 y 向下；每格取 ±170 的方框（骨架站在格子中心附近）
    iy = h - cy
    n = diff((int(cx-170), int(iy-170), int(cx+170), int(iy+170)))
    total += n
    if n < 300:
        bad.append(f'{name} 区域非背景像素只有 {n}（该骨架没画出来？）')
assert not bad, '; '.join(bad)
print(f'spine_all: 逐格像素 ok（{len(cells)} 格各自 >300 非背景像素，全图 {total}，背景 {bg}）')
PY
    echo "spine_all: ok（instances=$INST 格数=$EXPECT，逐格像素断言通过）"
    ;;
*)
    echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

exit 0
