#!/usr/bin/env bash
# 画布/分辨率策略端到端（docs/17）：设计分辨率 → 不同窗口/显示器的排版适配。
#   ① ShowAll 留边：16:9 设计在 4:3 / 竖屏窗口下的 scale 与 offset（居中）
#   ② 尺寸精确匹配：设计 × 整数倍 → scale 为整数、无留边
#   ③ fit 模式切换（ShowAll / FitWidth / NoBorder / Exact）由控制面改组件字段后立即生效
#   ④ **输入映射**：窗口坐标点击 → 设计坐标（含留边偏移）→ 命中按钮
#   ⑤ 安全区（刘海）：可用区扣除内缩，画布矩形随之收缩
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "canvas_e2e: skip（无显示环境：需要真实窗口尺寸与输入）"
  exit 77                                   # CTest SKIP_RETURN_CODE
fi

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS

fail() { echo "canvas_e2e: FAIL $*" >&2; exit 1; }

# run <log> <window WxH> [env...] —— 跑固定帧数并取脚本汇总（frame 90）
run() {
  local log="$1" win="$2"; shift 2
  env MAQUE_WINDOW="$win" MAQUE_TITLE=MaqueEngine MAQUE_VIEWS=1 MAQUE_KEEP_OPEN=1 MAQUE_MAX_FRAMES=95 \
      "$@" "$PLAYER" "$SCENE" > "$log" 2>&1 || true
  grep -o 'canvas_summary {.*}' "$log" | tail -1 | sed 's/^canvas_summary //'
}

check() {   # check <json> <python 表达式，用 m 取字段> <说明>
  python3 - "$1" "$2" <<'PY' || exit 1
import json, sys
m = json.loads(sys.argv[1])
ok = eval(sys.argv[2], {"m": m, "abs": abs, "min": min, "max": max})
if not ok:
    print(f"canvas_e2e: FAIL {sys.argv[2]} ← {m}", file=sys.stderr); sys.exit(1)
PY
}

# ---- ① 16:9 设计（480x270）在 4:3 窗口：ShowAll → 宽度决定缩放，上下留边居中 ----
S=$(run "$WORK/a.log" 800x600)
check "$S" 'abs(m["scale"] - 800/480) < 1e-3'            "4:3 窗口 ShowAll scale"
check "$S" 'abs(m["off_y"] - (600 - 270*800/480)/2) < 1'  "上下留边居中（off_y=75）"
check "$S" 'abs(m["off_x"]) < 1'                          "无左右留边"
check "$S" 'm["design_w"] == 480 and m["design_h"] == 270' "设计分辨率来自 ui.Canvas"

# ---- ② 尺寸精确匹配（960x540 = 2×，540x270+... ）：整数缩放、无留边 ----
S=$(run "$WORK/b.log" 960x540)
check "$S" 'abs(m["scale"] - 2.0) < 1e-3'  "2× 窗口 scale=2"
check "$S" 'abs(m["off_x"]) < 1 and abs(m["off_y"]) < 1' "无留边"

# ---- ③ 竖屏窗口（600x900）：按宽度适配，上下大留边 ----
S=$(run "$WORK/c.log" 600x900)
check "$S" 'abs(m["scale"] - min(600/480, 900/270)) < 1e-3' "竖屏 scale=min(宽,高)"
check "$S" 'm["off_y"] > 200'                               "竖屏上下留边明显"

# ---- ④ 输入映射：窗口点击 → 世界/设计坐标（含留边）→ 命中按钮 ----
# 按钮世界矩形 y ∈ [100,132]（y 向上）；设计空间 y 向下 → 屏幕 y = 270 - y；
# 800x600 窗口下 scale=800/480=1.6667、off_y=75 → 按钮中心 (240,116) 的窗口坐标 ≈ (400, 332)。
# 脚本收到的是**世界坐标**（y 向上）→ 期望 (240,116)。
S=$(run "$WORK/d.log" 800x600 MAQUE_SIM_CLICK="30:400:332")
check "$S" 'm["hits"] >= 1'                                  "点击命中画布内容"
check "$S" 'abs(m["last_x"] - 240) < 3 and abs(m["last_y"] - 116) < 4' "窗口→世界坐标映射（含留边与 y 翻转）"

# ---- ⑤ 安全区：40/20/40/20 内缩后可用区变小、偏移从安全区起算 ----
S=$(run "$WORK/e.log" 800x600 MAQUE_SAFE_AREA="40,20,40,20")
check "$S" 'm["scale"] < 800/480'                            "安全区内缩后缩放变小"
check "$S" 'm["off_x"] >= 40 and m["off_y"] >= 20'           "偏移不小于安全区"

# ---- ⑥ 文本度量与窗口缩放无关（回归：曾把设备像素当设计像素 → 窗口 3x 时文字 3 倍大）----
# 同一条定宽文本在 1x（设计尺寸窗口）与 3x 下的**设计单位**宽度/高度必须一致。
S1=$(run "$WORK/t1.log" 480x270)
S3=$(run "$WORK/t3.log" 1440x810)
python3 - "$S1" "$S3" <<'EOT' || exit 1
import json, sys
a, b = json.loads(sys.argv[1]), json.loads(sys.argv[2])
if abs(a["scale"] - 1.0) > 1e-3 or abs(b["scale"] - 3.0) > 1e-3:
    print(f'canvas_e2e: FAIL 前置缩放不符（{a["scale"]:.3f} / {b["scale"]:.3f}）', file=sys.stderr)
    sys.exit(1)
# 容差 5%：字形按**设备像素**取整（hinting），换算回设计单位会有零点几像素的差；
# 若回归（设备像素当设计像素）则 3x 会是 1x 的约 3 倍，远超容差。
rel_w = abs(a["probe_w"] - b["probe_w"]) / max(1.0, abs(a["probe_w"]))
rel_h = abs(a["probe_h"] - b["probe_h"]) / max(1.0, abs(a["probe_h"]))
if rel_w > 0.05 or rel_h > 0.05:
    print(f'canvas_e2e: FAIL 文本度量随窗口缩放变化（1x={a["probe_w"]}x{a["probe_h"]} '
          f'3x={b["probe_w"]}x{b["probe_h"]}）', file=sys.stderr)
    sys.exit(1)
print(f' 文本度量与缩放无关：1x/3x 均为 {a["probe_w"]:.0f}x{a["probe_h"]:.0f}（设计单位）')
EOT

# ---- ⑦ fit 模式切换（控制面改组件字段 → 立即生效）----
cat > "$WORK/req.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Scene.set","params":{"id":0,"path":"fit_mode","value":1}}
{"jsonrpc":"2.0","id":2,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":3,"method":"Scene.get","params":{"id":0,"path":"scale"}}
{"jsonrpc":"2.0","id":4,"method":"Scene.get","params":{"id":0,"path":"offset_y"}}
{"jsonrpc":"2.0","id":5,"method":"Scene.set","params":{"id":0,"path":"fit_mode","value":4}}
{"jsonrpc":"2.0","id":6,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":7,"method":"Scene.get","params":{"id":0,"path":"scale"}}
EOF
MAQUE_WINDOW=800x600 MAQUE_CONTROL=- MAQUE_VIEWS=1 MAQUE_MAX_FRAMES=40 \
  "$PLAYER" "$SCENE" < "$WORK/req.txt" > "$WORK/fit.log" 2>&1 || true
python3 - "$WORK/fit.log" <<'PY' || exit 1
import json, re, sys
text = open(sys.argv[1]).read()
res = {}
for line in text.splitlines():
    line = line.strip()
    if not line.startswith('{'): continue
    try: d = json.loads(line)
    except Exception: continue
    if 'id' in d and 'result' in d: res[d['id']] = d['result']
def val(i):
    r = res.get(i)
    if isinstance(r, dict): return float(r.get('value', 0))
    return float(r) if r is not None else None
fw, fwy, nb = val(3), val(4), val(7)
# FitWidth：scale 由宽度决定 = 800/480；垂直方向仍按 align=Center 居中留边
#   off_y = (600 - 270*scale)/2 = 75
# NoBorder：铺满取 max(800/480, 600/270) = 600/270 = 2.222（两侧裁剪）
if fw is None or abs(fw - 800/480) > 1e-3:
    print(f"canvas_e2e: FAIL FitWidth scale={fw}（期望 {800/480:.3f}）", file=sys.stderr); sys.exit(1)
expect_fwy = (600 - 270 * (800 / 480)) / 2
if fwy is None or abs(fwy - expect_fwy) > 1.0:
    print(f"canvas_e2e: FAIL FitWidth off_y={fwy}（期望居中留边 {expect_fwy:.1f}）", file=sys.stderr); sys.exit(1)
if nb is None or abs(nb - 600/270) > 1e-3:
    print(f"canvas_e2e: FAIL NoBorder scale={nb}（期望 {600/270:.3f}）", file=sys.stderr); sys.exit(1)
print(f"canvas_e2e: fit 切换 ok（FitWidth scale={fw:.3f} off_y={fwy:.0f} → NoBorder scale={nb:.3f}）")
PY

echo "canvas_e2e: ok（留边/整数缩放/竖屏/输入映射/安全区/fit 切换 全部通过）"
exit 0
