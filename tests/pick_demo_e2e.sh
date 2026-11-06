#!/usr/bin/env bash
# 4x4 拾取 demo 端到端（scenes/pick_demo）：
#   ① 16 对象 / 16 材质装配（8 cube + 8 sphere）；
#   ② 世界空间模拟点击（MAQUE_SIM，space=world；坐标是**设计坐标**，与 input_x/input_y 同一套）
#      → on_pointer_down → scene_pick 命中；
#   ③ 同一对象二次点击换色（node_get_number 读嵌套字段 base_color.r：值必须变）；
#   ④ 对象空隙点击 miss；
#   ⑤ --shot：非背景像素占比 + 颜色多样性（16 个对象确实渲染出来）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE_DIR="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "pick_demo_e2e: FAIL $*" >&2; exit 1; }

# 设计坐标：正交相机 ortho_height=7 / 800x600 → 世界 (x,y) → 设计 (400 + x*85.714, 300 + y*85.714)
#   cube_r0c0 世界 (-2.4, 2.4) → (194.29, 505.71)
#   cube_r3c3 世界 ( 2.4,-2.4) → (605.71,  94.29)
cat > "$WORK/clicks.sim.json" <<'EOF'
{"name":"pick_demo_clicks","space":"world","events":[
  {"frame":6,"type":"click","x":194.29,"y":505.71},
  {"frame":10,"type":"click","x":605.71,"y":94.29},
  {"frame":14,"type":"click","x":194.29,"y":505.71},
  {"frame":18,"type":"click","x":400.0,"y":300.0}
]}
EOF

timeout 90 env MAQUE_MAX_FRAMES=24 MAQUE_SIM="$WORK/clicks.sim.json" \
    "$PLAYER" "$SCENE_DIR/pick_demo.scn" > "$WORK/run.log" 2>&1 || { cat "$WORK/run.log"; fail "demo 运行失败"; }
grep -q "AddressSanitizer\|runtime error:" "$WORK/run.log" && { cat "$WORK/run.log"; fail "sanitizer 报错"; }

grep -q '\[pick_demo\] ready shapes=16' "$WORK/run.log" || fail "demo 未就绪 / 形状数不对"
grep -q '\[pick_demo\] hit .*name=cube_r0c0 .*palette=0 .*clicks=1' "$WORK/run.log" \
    || fail "第 1 次点击未命中 cube_r0c0"
grep -q '\[pick_demo\] hit .*name=cube_r3c3 .*palette=0 .*clicks=1' "$WORK/run.log" \
    || fail "未命中 cube_r3c3"
grep -q '\[pick_demo\] hit .*name=cube_r0c0 .*palette=1 .*clicks=2' "$WORK/run.log" \
    || fail "重复点击未换色（palette 没递增）"
grep -q '\[pick_demo\] miss x=400.00 y=300.00' "$WORK/run.log" || fail "对象空隙点击不应命中"

R1=$(grep 'name=cube_r0c0 .*clicks=1' "$WORK/run.log" | head -1 | sed -n 's/.* r=\([0-9.]*\).*/\1/p')
R2=$(grep 'name=cube_r0c0 .*clicks=2' "$WORK/run.log" | head -1 | sed -n 's/.* r=\([0-9.]*\).*/\1/p')
[ -n "$R1" ] && [ -n "$R2" ] && [ "$R1" != "$R2" ] || fail "换色没有改变 base_color.r（$R1 → $R2）"

# 确定性：同命令两跑，[pick_demo] 日志逐字一致
timeout 90 env MAQUE_MAX_FRAMES=24 MAQUE_SIM="$WORK/clicks.sim.json" \
    "$PLAYER" "$SCENE_DIR/pick_demo.scn" > "$WORK/run2.log" 2>&1 || fail "demo 第二次运行失败"
grep -o '\[pick_demo\].*' "$WORK/run.log" > "$WORK/a.txt"
grep -o '\[pick_demo\].*' "$WORK/run2.log" > "$WORK/b.txt"
cmp -s "$WORK/a.txt" "$WORK/b.txt" || fail "两次运行的点击日志不一致（非确定性）"

# 画面：非背景占比 + 颜色多样性
timeout 90 env "$PLAYER" --shot "$WORK/demo.ppm" --frames 2 "$SCENE_DIR/pick_demo.scn" \
    > "$WORK/shot.log" 2>&1 || { cat "$WORK/shot.log"; fail "截图失败"; }
"$PY" - "$WORK/demo.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
def rgb(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]
bg = rgb(2, 2)
def diff(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
nonbg = 0
colors = set()
total = 0
for y in range(0, h, 3):
    for x in range(0, w, 3):
        total += 1
        c = rgb(x, y)
        if diff(c, bg) > 30:
            nonbg += 1
            colors.add((c[0] // 32, c[1] // 32, c[2] // 32))
ratio = nonbg / max(1, total)
ok = ratio > 0.08 and len(colors) >= 8
print(f'pick_demo_e2e: {w}x{h} bg={bg} nonbg={ratio:.3f} colors={len(colors)}')
sys.exit(0 if ok else 1)
PY

echo "pick_demo_e2e: ok（16 形状 / 命中 / 换色 / 嵌套字段 / 空隙 miss / 画面）"
