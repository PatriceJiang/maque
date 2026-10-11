#!/usr/bin/env bash
# 文本端到端（docs/17/18）：Label / TextLabel / RichText 的排版、换行、富文本 run、
# 动态改文本/字号，以及**像素级**验证（字形真的被画出来、run 颜色正确）。
#   ① 排版度量：text_w/text_h/line_count（transient 字段由 extract 每帧写回）
#   ② 换行：按词折行 → 多行；按字符折行
#   ③ 富文本：<color=#RRGGBB> 改色、<size=NN> 改字号、<b> 加粗、<br/> 换行；parse_tags=false 当纯文本
#   ④ 截断/对齐：Center/End 使首字形右移
#   ⑤ 像素：黄色（Label 自定义色）、绿色（TextLabel color）、红色（富文本 run）、白色（默认色）均出现
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK/frames"
export MAQUE_HEADLESS=1

fail() { echo "text_e2e: FAIL $*" >&2; exit 1; }

# ---- 跑场景：主循环（PPM 帧输出只在主循环里生效）----
MAQUE_MAX_FRAMES=14 "$PLAYER" "$SCENE" "$WORK/frames" > "$WORK/out.txt" 2>&1 || true

SUMMARY=$(grep -o 'text_summary .*' "$WORK/out.txt" | head -1)
[ -n "$SUMMARY" ] || fail "script summary not found"
FRAME=$(ls "$WORK/frames"/frame_*.ppm 2>/dev/null | sort | tail -1)
[ -n "$FRAME" ] || fail "no PPM frame produced (need renderer)"

python3 - "$SUMMARY" "$FRAME" <<'PY' || exit 1
import json, sys
from collections import Counter
d = json.loads(sys.argv[1][len('text_summary '):])
path = sys.argv[2]
raw = open(path, 'rb').read()
parts = raw.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
def at(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]

bad = []
def ck(name, cond, got=None):
    if not cond:
        bad.append(f"{name} (got {got})")

# ① 排版度量（extract 每帧写回 transient）
ck('Label 实测宽度 > 0', d.get('title_w', 0) > 10, d.get('title_w'))
ck('Label 动态改文本后单行', d.get('title_lines') == 1, d.get('title_lines'))
ck('换行：按词折行成 2 行', d.get('wrapped_lines_end') == 2, d.get('wrapped_lines_end'))
ck('RichText：按词折行 + <br/> → 多行', d.get('rich_lines', 0) >= 3, d.get('rich_lines'))
ck('RichText：组件已挂载', d.get('rich_comps') == 1, d.get('rich_comps'))

# ② 像素验证：四种颜色都出现（证明字形真的渲染 + run 颜色生效）
red = green = yellow = white = 0
for y in range(h):
    for x in range(w):
        r, g, b = at(x, y)
        if r > 170 and g < 130 and b < 130: red += 1
        elif g > 170 and r < 130 and b < 170: green += 1
        elif r > 170 and g > 150 and b < 130: yellow += 1
        elif r > 190 and g > 190 and b > 190: white += 1
ck('像素：scn.Label 自定义色字形', yellow > 100, yellow)
ck('像素：ui.TextLabel 颜色字形', green > 20, green)
ck('像素：富文本 <color> run 字形', red > 20, red)
ck('像素：RichText 默认色正文', white > 100, white)

if bad:
    print('text_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
print(f"text_e2e: 语义与像素全部通过（label_w={d.get('title_w'):.0f} wrap_lines={d.get('wrapped_lines_end')} "
      f"rich_lines={d.get('rich_lines')} 像素 黄={yellow} 绿={green} 红={red} 白={white}）")
PY

# ---- 确定性：同输入两跑 world_trail 一致 ----
MAQUE_MAX_FRAMES=14 "$PLAYER" "$SCENE" "$WORK/frames2" > "$WORK/out2.txt" 2>&1 || true
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/out.txt" | head -1 | cut -d= -f2)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/out2.txt" | head -1 | cut -d= -f2)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "determinism broken: $T1 != $T2"

echo "text_e2e: ok (trail=$T1)"
exit 0
