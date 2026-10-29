#!/usr/bin/env bash
# F4「引擎选项」面板端到端（docs/28）：
#   ① F4 沿检测：单次按下 → `runtime: engine options panel ON` 恰好一次，
#      且首次按 F4 会顺手打开整个 ImGui（日志同时出现 `runtime: imgui panel ON`）；
#      两次按下 → ON/OFF 各一次；按住多帧（连续 keydown）→ ON 仍只一次。
#   ② 面板确实进像素：同场景 `--shot` 不按 F4 vs 第 5 帧按 F4 → 抓图差异像素 > 500。
#   ③ MSAA：`MAQUE_ENGINE_OPTIONS="msaa=4"` → gfx 打 `gfx: 3D MSAA 4x` 且环境变量解析日志
#      含 `msaa=4x`；默认（不设）不应出现该 gfx 行；`MAQUE_MSAA=0` + 选项 msaa=4 仍 4x
#      （面板选项优先级最高）。注：gfx 的 MSAA 诊断行只在 `MAQUE_RENDER3D_INFO=1` 下打印，
#      故本组三次运行都带该诊断开关（否则 4x 生效但日志不可见）。
#   ④ 默认太阳光：场景无 render.Light 时引擎补一盏默认方向光 → 关掉它
#      （`MAQUE_ENGINE_OPTIONS="sun=0"`）后物体像素平均亮度显著变暗（Δ > 5.0），
#      且两图不同像素 > 500；`sun_intensity=3` 比默认更亮（Δ > 2.0）。
#   ⑤ 主相机 near/far 覆盖：`far=0.5` / `near=20` 把物体裁掉（非背景占比 < 0.005），
#      默认非背景占比 > 0.03；日志含 `cam=override` 及 `far=0.500` / `near=20.000`。
#   ⑥ F2/F4 互不干扰：
#      · F4 自己打开的 ImGui 在收起面板时恢复关闭（不会把 stats 留在屏上）；
#      · ImGui 已开（F2）时 F4 只开关选项面板、不关 stats；
#      · F2 关掉整个 ImGui 会清空选项面板状态 → F2 再开不会"复活"F4 面板（右侧区域像素与
#        只按 F2 的参考图逐像素一致；对照组 F4 常开则差异 >5000 像素，证明断言有鉴别力）。
#   ⑦ 引擎热键不双触发：F2/F4 在面板开关后**不再下发给脚本**（普通按键仍能到达，作对照）。
#   ⑧ 每次运行后的日志都不含 `AddressSanitizer`（构建默认 Debug + ASan）。
#
# 无显示环境可跑：全程 `MAQUE_WINDOW=0x0 MAQUE_PRESENT=off` + `--shot`/`--frames`。
# headless 没有真实键盘 → `MAQUE_SIM_KEY_AT="<帧>:<键码>"` 往**本帧设备事件队列**注入按下事件
# （与真实键盘完全同一条路径：iframe → imgui_input_feed → 热键判断）；
# 键码是引擎规范键空间：F4 = MQ_KEY_F4 = 1004。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player 可执行文件；$2 = 仓库根（CMAKE_SOURCE_DIR）；$3 = 工作目录
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="${1:-}"
ROOT="${2:-}"
WORK="${3:-}"

[ -n "${PY:-}" ] || { echo "imgui_engine_options_e2e: skip（无可用 Python 3，无法解析 PPM）"; exit 77; }
[ -n "$PLAYER" ] && [ -f "$PLAYER" ] \
    || { echo "imgui_engine_options_e2e: skip（缺 player 可执行文件: ${PLAYER:-<未给出>}）"; exit 77; }
[ -n "$ROOT" ] && [ -d "$ROOT" ] \
    || { echo "imgui_engine_options_e2e: skip（缺仓库根目录: ${ROOT:-<未给出>}）"; exit 77; }
[ -n "$WORK" ] || { echo "imgui_engine_options_e2e: skip（未给出工作目录）"; exit 77; }

fail() { echo "imgui_engine_options_e2e: FAIL $*" >&2; exit 1; }

# 每个 player 运行后都必须确认没有 ASan 报告。
check_clean() {
    if grep -q "AddressSanitizer" "$1" 2>/dev/null; then
        cat "$1" >&2
        fail "日志出现 AddressSanitizer 报告（$1）"
    fi
}

# 运行一次 player：$1=日志，$2=失败描述，其余为 player 参数（全部加 timeout 90）。
# 需要环境变量时用调用处的前缀赋值给出：`MAQUE_X=... run <日志> <描述> <参数…>`。
run() {
    local log="$1" desc="$2"; shift 2
    timeout 90 "$PLAYER" "$@" >"$log" 2>&1 || { cat "$log" >&2; fail "$desc"; }
    check_clean "$log"
}

# 日志里某个固定串出现的次数（0 = 没有）。
count_lines() {
    local n
    n=$(grep -cF -- "$1" "$2" 2>/dev/null || true)
    echo "${n:-0}"
}

rm -rf "$WORK"; mkdir -p "$WORK" || fail "无法创建工作目录 $WORK"

# 干扰项全清掉，再强制「无窗口 + 纯离屏」（CI / 无显示环境可用）。
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_CAPTURE_EVERY MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_IMGUI_SCALE MAQUE_IMGUI_FONT_SIZE \
      MAQUE_SHOT MAQUE_SHOT_FRAME MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_SHOT_EVERY MAQUE_SHOT_COUNT \
      MAQUE_ENV_IBL MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO \
      MAQUE_WINDOW MAQUE_WINDOW_EVENT_AT MAQUE_WINDOW_MAXIMIZED MAQUE_ENGINE_OPTIONS MAQUE_KEEP_OPEN \
      MAQUE_TITLE MAQUE_VIEWS MAQUE_SIM MAQUE_SIM_KEY_AT MAQUE_SIM_CLICK MAQUE_DPI_SCALE \
      MAQUE_TRACE_INPUT MAQUE_TRACE_STATS 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# 现场生成 640x360 测试场景：一个透视相机 + 一个内建 cube，**故意没有 render.Light**
# ——「默认太阳光」（场景无光源时的回退方向光）才会生效。
SCN="$WORK/imgui_engine_options_e2e.scn"
cat >"$SCN" <<'JSON'
{
  "title": "imgui_engine_options_e2e",
  "script": "keylog.js",
  "width": 640,
  "height": 360,
  "views": 1,
  "entities": [
    {
      "name": "camera",
      "components": [
        { "type": "scene.Transform3D", "local": {
            "position": { "x": 0.0, "y": 1.0, "z": 4.4 },
            "rotation": { "x": -0.22, "y": 0.0, "z": 0.0 } } },
        { "type": "render.Camera3D", "fov_y": 55.0, "near": 0.1, "far": 100.0,
          "clear_color": { "r": 0.07, "g": 0.08, "b": 0.11, "a": 1.0 } }
      ]
    },
    {
      "name": "cube",
      "components": [
        { "type": "scene.Transform3D", "local": {
            "position": { "x": 0.0, "y": 0.0, "z": 0.0 },
            "rotation": { "x": 0.0, "y": 0.0, "z": 0.0 },
            "scale": { "x": 1.0, "y": 1.0, "z": 1.0 } } },
        { "type": "render.MeshRenderer" }
      ]
    }
  ]
}
JSON
[ -s "$SCN" ] || fail "测试场景写入失败: $SCN"

# 按键探针脚本：记录脚本实际收到的按键（用于验证 F2/F4 引擎热键不再下发给玩法）。
cat >"$WORK/keylog.js" <<'JS'
function on_key_down(target, key) {
    log('e2e_key ' + key);
}
JS
[ -s "$WORK/keylog.js" ] || fail "按键探针脚本写入失败: $WORK/keylog.js"

# ---- ① F4 沿检测 -------------------------------------------------------------
echo "imgui_engine_options_e2e: ① F4 沿检测（单次/两次/按住）"

MAQUE_SIM_KEY_AT="10:1004" run "$WORK/f4_one.log" "F4 单次按下运行失败" --frames 40 "$SCN"
on=$(count_lines "runtime: engine options panel ON" "$WORK/f4_one.log")
off=$(count_lines "runtime: engine options panel OFF" "$WORK/f4_one.log")
[ "$on" = 1 ] || fail "F4 单次按下 ON 次数应为 1，实际 $on（$(grep -F 'engine options panel' "$WORK/f4_one.log" | head -5)）"
[ "$off" = 0 ] || fail "F4 单次按下不应出现 OFF，实际 $off"
grep -qF "runtime: imgui panel ON" "$WORK/f4_one.log" \
    || fail "首次按 F4 未自动打开 ImGui（缺 'runtime: imgui panel ON'）"

MAQUE_SIM_KEY_AT="10:1004,20:1004" run "$WORK/f4_two.log" "F4 两次按下运行失败" --frames 40 "$SCN"
on=$(count_lines "runtime: engine options panel ON" "$WORK/f4_two.log")
off=$(count_lines "runtime: engine options panel OFF" "$WORK/f4_two.log")
[ "$on" = 1 ] || fail "F4 两次按下 ON 次数应为 1，实际 $on（$(grep -F 'engine options panel' "$WORK/f4_two.log" | head -5)）"
[ "$off" = 1 ] || fail "F4 两次按下 OFF 次数应为 1，实际 $off"
# 本用例开始时 ImGui 是关的（F4 顺手打开）→ 收起面板时应把 ImGui 一并恢复关闭，不残留 stats。
imgui_on=$(count_lines "runtime: imgui panel ON" "$WORK/f4_two.log")
imgui_off=$(count_lines "runtime: imgui panel OFF" "$WORK/f4_two.log")
[ "$imgui_on" = 1 ] || fail "F4 两次按下：ImGui 应被 F4 打开恰好 1 次，实际 $imgui_on"
[ "$imgui_off" = 1 ] || fail "F4 自己打开的 ImGui 在收起面板时应恢复关闭（期望 imgui OFF=1，实际 $imgui_off）"

# 按住不放（第 10..13 帧连续注入按下）→ 只算一次上升沿。
HOLD=""
for f in 10 11 12 13; do HOLD="${HOLD}${HOLD:+,}$f:1004"; done
MAQUE_SIM_KEY_AT="$HOLD" run "$WORK/f4_hold.log" "F4 按住运行失败" --frames 40 "$SCN"
on=$(count_lines "runtime: engine options panel ON" "$WORK/f4_hold.log")
off=$(count_lines "runtime: engine options panel OFF" "$WORK/f4_hold.log")
[ "$on" = 1 ] || fail "F4 按住（连续 keydown）ON 次数应仍为 1，实际 $on（$(grep -F 'engine options panel' "$WORK/f4_hold.log" | head -5)）"
[ "$off" = 0 ] || fail "F4 按住不应出现 OFF，实际 $off"

# ---- ② 面板确实进像素 -------------------------------------------------------
echo "imgui_engine_options_e2e: ② 面板进像素（--shot 前后不同）"

run "$WORK/panel_off.log" "关面板截图运行失败" \
    --shot "$WORK/panel_off.ppm" --shot-frame 25 "$SCN"
MAQUE_SIM_KEY_AT="5:1004" run "$WORK/panel_on.log" "开面板（F4）截图运行失败" \
    --shot "$WORK/panel_on.ppm" --shot-frame 25 "$SCN"
grep -qF "runtime: engine options panel ON" "$WORK/panel_on.log" \
    || fail "截图运行里 F4 未生效（缺 'runtime: engine options panel ON'）"
[ -s "$WORK/panel_off.ppm" ] || fail "关面板截图未生成"
[ -s "$WORK/panel_on.ppm" ] || fail "开面板截图未生成"

"$PY" - "$WORK/panel_off.ppm" "$WORK/panel_on.ppm" <<'PY' \
    || fail "面板未画进像素缓冲（两图差异像素不足）"
import sys


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


w, h, a = load(sys.argv[1])
w2, h2, b = load(sys.argv[2])
assert (w, h) == (640, 360), f'尺寸 {w}x{h} != 设计分辨率 640x360'
assert (w2, h2) == (w, h), f'两图尺寸不同: {w2}x{h2} vs {w}x{h}'
diff = 0
for i in range(0, len(a), 3):
    if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12:
        diff += 1
print(f'imgui_engine_options_e2e: 面板像素 {w}x{h} 差异像素={diff}')
sys.exit(0 if diff > 500 else 1)
PY

# ---- ③ MSAA -----------------------------------------------------------------
echo "imgui_engine_options_e2e: ③ MSAA（选项/环境变量/优先级）"

# 注意：gfx 的 `gfx: 3D MSAA 4x（WxH，pass resolve 到目标）` 只在 MAQUE_RENDER3D_INFO=1
# 时打印（诊断开关），所以三次运行都带它；基线不带 ENGINE_OPTIONS。
MAQUE_RENDER3D_INFO=1 run "$WORK/msaa_base.log" "MSAA 基线运行失败" \
    --frames 3 --shot "$WORK/msaa_base.ppm" "$SCN"
[ -s "$WORK/msaa_base.ppm" ] || fail "MSAA 基线截图未生成"
if grep -qF "gfx: 3D MSAA 4x" "$WORK/msaa_base.log"; then
    fail "默认不应启用 4x MSAA（日志出现 'gfx: 3D MSAA 4x'）"
fi

MAQUE_RENDER3D_INFO=1 MAQUE_ENGINE_OPTIONS="msaa=4" run "$WORK/msaa_env.log" "MAQUE_ENGINE_OPTIONS=msaa=4 运行失败" \
    --frames 3 --shot "$WORK/msaa_env.ppm" "$SCN"
[ -s "$WORK/msaa_env.ppm" ] || fail "msaa=4 截图未生成"
grep -qF "gfx: 3D MSAA 4x" "$WORK/msaa_env.log" \
    || { cat "$WORK/msaa_env.log" >&2; fail "MAQUE_ENGINE_OPTIONS=msaa=4 未启用 4x MSAA（缺 gfx 日志）"; }
grep -qF "engine options (env) msaa=4x" "$WORK/msaa_env.log" \
    || fail "缺环境变量解析日志（应在 'runtime: engine options (env)' 行含 msaa=4x）"

# 优先级：面板/环境选项覆盖 MAQUE_MSAA（MAQUE_MSAA=0 也不能把 4x 压掉）。
MAQUE_RENDER3D_INFO=1 MAQUE_MSAA=0 MAQUE_ENGINE_OPTIONS="msaa=4" run "$WORK/msaa_prec.log" "MSAA 优先级运行失败" \
    --frames 3 --shot "$WORK/msaa_prec.ppm" "$SCN"
grep -qF "gfx: 3D MSAA 4x" "$WORK/msaa_prec.log" \
    || fail "MAQUE_MSAA=0 + 选项 msaa=4 仍应出现 'gfx: 3D MSAA 4x'（选项优先级最高）"

# ---- ④ 默认太阳光 -----------------------------------------------------------
echo "imgui_engine_options_e2e: ④ 默认太阳光（关掉后物体变暗）"

run "$WORK/sun_on.log" "默认太阳光渲染失败" \
    --shot "$WORK/sun_on.ppm" --frames 3 "$SCN"
MAQUE_ENGINE_OPTIONS="sun=0" run "$WORK/sun_off.log" "MAQUE_ENGINE_OPTIONS=sun=0 运行失败" \
    --shot "$WORK/sun_off.ppm" --frames 3 "$SCN"
grep -qF "engine options (env)" "$WORK/sun_off.log" || fail "缺环境变量解析日志（engine options (env)）"
grep -qF "sun=off" "$WORK/sun_off.log" || fail "环境变量解析日志应含 sun=off"
[ -s "$WORK/sun_on.ppm" ] || fail "默认太阳光截图未生成"
[ -s "$WORK/sun_off.ppm" ] || fail "sun=0 截图未生成"

"$PY" - "$WORK/sun_on.ppm" "$WORK/sun_off.ppm" <<'PY' \
    || fail "关闭默认太阳光后物体没有明显变暗（像素断言不满足）"
import sys

BG_TOL = 25
PY_DIFF = 12


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


w, h, a = load(sys.argv[1])
w2, h2, b = load(sys.argv[2])
assert (w, h) == (640, 360), f'尺寸 {w}x{h} != 设计分辨率 640x360'
assert (w2, h2) == (w, h), f'两图尺寸不同: {w2}x{h2} vs {w}x{h}'

bg = (a[(2 * w + 2) * 3], a[(2 * w + 2) * 3 + 1], a[(2 * w + 2) * 3 + 2])


def l1(px, i):
    return abs(px[i] - bg[0]) + abs(px[i + 1] - bg[1]) + abs(px[i + 2] - bg[2])


# 物体像素 = 任一图中与背景 L1 > 25 的并集（太阳光只改物体着色，不挡几何）。
obj = [i for i in range(0, len(a), 3) if l1(a, i) > BG_TOL or l1(b, i) > BG_TOL]
assert len(obj) > 1000, f'物体像素太少（{len(obj)}），场景可能没渲染出 cube'


def mean_sum(px):
    return sum(px[i] + px[i + 1] + px[i + 2] for i in obj) / (3.0 * len(obj))


mean_on = mean_sum(a)
mean_off = mean_sum(b)
ndiff = sum(1 for i in range(0, len(a), 3)
            if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > PY_DIFF)
print(f'imgui_engine_options_e2e: 默认太阳光 物体像素={len(obj)} '
      f'平均亮度 默认={mean_on:.2f} sun=0={mean_off:.2f} Δ={mean_on - mean_off:.2f} 不同像素={ndiff}')
sys.exit(0 if (mean_on > mean_off + 5.0) and ndiff > 500 else 1)
PY

# 强度：sun_intensity=3 比默认 1.10 更亮（契约里的可选用例）。
MAQUE_ENGINE_OPTIONS="sun_intensity=3" run "$WORK/sun_intensity.log" "MAQUE_ENGINE_OPTIONS=sun_intensity=3 运行失败" \
    --shot "$WORK/sun_intensity.ppm" --frames 3 "$SCN"
grep -qF "intensity=3.00" "$WORK/sun_intensity.log" \
    || fail "环境变量解析日志应含 intensity=3.00"
[ -s "$WORK/sun_intensity.ppm" ] || fail "sun_intensity=3 截图未生成"

"$PY" - "$WORK/sun_on.ppm" "$WORK/sun_intensity.ppm" <<'PY' \
    || fail "sun_intensity=3 没有比默认太阳光更亮（Δ <= 2.0）"
import sys

BG_TOL = 25


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


w, h, a = load(sys.argv[1])
w2, h2, b = load(sys.argv[2])
assert (w, h) == (640, 360) and (w2, h2) == (w, h), f'尺寸不符: {w}x{h} vs {w2}x{h2}'

bg = (a[(2 * w + 2) * 3], a[(2 * w + 2) * 3 + 1], a[(2 * w + 2) * 3 + 2])


def l1(px, i):
    return abs(px[i] - bg[0]) + abs(px[i + 1] - bg[1]) + abs(px[i + 2] - bg[2])


obj = [i for i in range(0, len(a), 3) if l1(a, i) > BG_TOL or l1(b, i) > BG_TOL]
assert len(obj) > 1000, f'物体像素太少（{len(obj)}）'
mean_a = sum(a[i] + a[i + 1] + a[i + 2] for i in obj) / (3.0 * len(obj))
mean_b = sum(b[i] + b[i + 1] + b[i + 2] for i in obj) / (3.0 * len(obj))
print(f'imgui_engine_options_e2e: 太阳光强度 默认={mean_a:.2f} '
      f'intensity=3={mean_b:.2f} Δ={mean_b - mean_a:.2f}')
sys.exit(0 if mean_b > mean_a + 2.0 else 1)
PY

# ---- ⑤ 主相机 near/far 覆盖 -------------------------------------------------
echo "imgui_engine_options_e2e: ⑤ 主相机 near/far 覆盖"

run "$WORK/cam_base.log" "相机基线渲染失败" \
    --shot "$WORK/cam_base.ppm" --frames 3 "$SCN"
MAQUE_ENGINE_OPTIONS="far=0.5" run "$WORK/cam_far.log" "MAQUE_ENGINE_OPTIONS=far=0.5 运行失败" \
    --shot "$WORK/cam_far05.ppm" --frames 3 "$SCN"
MAQUE_ENGINE_OPTIONS="near=20" run "$WORK/cam_near.log" "MAQUE_ENGINE_OPTIONS=near=20 运行失败" \
    --shot "$WORK/cam_near20.ppm" --frames 3 "$SCN"
grep -qF "cam=override" "$WORK/cam_far.log" || fail "far=0.5 环境变量解析日志应含 cam=override"
grep -qF "far=0.500" "$WORK/cam_far.log" || fail "far=0.5 环境变量解析日志应含 far=0.500"
grep -qF "cam=override" "$WORK/cam_near.log" || fail "near=20 环境变量解析日志应含 cam=override"
grep -qF "near=20.000" "$WORK/cam_near.log" || fail "near=20 环境变量解析日志应含 near=20.000"
[ -s "$WORK/cam_base.ppm" ] || fail "相机基线截图未生成"
[ -s "$WORK/cam_far05.ppm" ] || fail "far=0.5 截图未生成"
[ -s "$WORK/cam_near20.ppm" ] || fail "near=20 截图未生成"

"$PY" - "$WORK/cam_base.ppm" "$WORK/cam_far05.ppm" "$WORK/cam_near20.ppm" <<'PY' \
    || fail "主相机 near/far 覆盖未生效（物体未被裁掉或基线不可见）"
import sys

BG_TOL = 25


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


def nonbg_ratio(px, w, h):
    bg = (px[(2 * w + 2) * 3], px[(2 * w + 2) * 3 + 1], px[(2 * w + 2) * 3 + 2])
    n = sum(1 for i in range(0, len(px), 3)
            if abs(px[i] - bg[0]) + abs(px[i + 1] - bg[1]) + abs(px[i + 2] - bg[2]) > BG_TOL)
    return n / float(w * h)


w, h, base = load(sys.argv[1])
w2, h2, far = load(sys.argv[2])
w3, h3, near = load(sys.argv[3])
assert (w, h) == (640, 360), f'尺寸 {w}x{h} != 设计分辨率 640x360'
assert (w2, h2) == (w, h) and (w3, h3) == (w, h), '三张截图尺寸不一致'
ratio_base = nonbg_ratio(base, w, h)
ratio_far = nonbg_ratio(far, w, h)
ratio_near = nonbg_ratio(near, w, h)
print(f'imgui_engine_options_e2e: 相机覆盖 非背景占比 默认={ratio_base:.4f} '
      f'far=0.5={ratio_far:.4f} near=20={ratio_near:.4f}')
# 默认物体可见；far=0.5（相机 far < 物体距离 ~4.5）与 near=20（物体在近平面内）都应裁掉。
ok = ratio_base > 0.03 and ratio_far < 0.005 and ratio_near < 0.005
sys.exit(0 if ok else 1)
PY

# ---- ⑥ F2/F4 互不干扰 ------------------------------------------------------
echo "imgui_engine_options_e2e: ⑥ F2/F4 互不干扰（状态不复活 / 不误关 stats）"

# 参考：只按 F2 开 ImGui（无 F4 面板）。
MAQUE_SIM_KEY_AT="5:1002" run "$WORK/f2_only.log" "F2 参考运行失败" \
    --shot "$WORK/f2_only.ppm" --shot-frame 45 "$SCN"

# 混合：F2 开 → F4 开面板 → F2 全关（应同时收起面板）→ F2 再开（不得复活 F4 面板）。
MAQUE_SIM_KEY_AT="5:1002,15:1004,25:1002,35:1002" run "$WORK/f2f4_mix.log" "F2/F4 混合运行失败" \
    --shot "$WORK/f2f4_mix.ppm" --shot-frame 45 "$SCN"
on=$(count_lines "runtime: engine options panel ON" "$WORK/f2f4_mix.log")
off=$(count_lines "runtime: engine options panel OFF" "$WORK/f2f4_mix.log")
[ "$on" = 1 ] || fail "F2/F4 混合：选项面板 ON 应恰好 1 次，实际 $on（$(grep -F 'engine options panel' "$WORK/f2f4_mix.log" | head -5)）"
[ "$off" = 1 ] || fail "F2/F4 混合：F2 关闭 ImGui 时应收起选项面板（期望 OFF=1，实际 $off）"
imgui_on=$(count_lines "runtime: imgui panel ON" "$WORK/f2f4_mix.log")
imgui_off=$(count_lines "runtime: imgui panel OFF" "$WORK/f2f4_mix.log")
[ "$imgui_on" = 2 ] || fail "F2/F4 混合：ImGui 应被 F2 打开 2 次（15/35 帧），实际 $imgui_on"
[ "$imgui_off" = 1 ] || fail "F2/F4 混合：ImGui 应被 F2 关闭 1 次（25 帧），实际 $imgui_off"

# 对照：F4 面板常开（同样的 F2 开 + F4 开，但不再按 F2 关）→ 右侧区域应出现大片差异。
MAQUE_SIM_KEY_AT="5:1002,15:1004" run "$WORK/f2f4_ctrl.log" "F2/F4 对照运行失败" \
    --shot "$WORK/f2f4_ctrl.ppm" --shot-frame 45 "$SCN"

# 只按 F2 时 F4；ImGui 已开时 F4 只开关选项面板、不关 stats。
MAQUE_SIM_KEY_AT="5:1002,15:1004,25:1004" run "$WORK/f4_keep_imgui.log" "F4 不误关 stats 运行失败" \
    --frames 45 "$SCN"
imgui_on=$(count_lines "runtime: imgui panel ON" "$WORK/f4_keep_imgui.log")
imgui_off=$(count_lines "runtime: imgui panel OFF" "$WORK/f4_keep_imgui.log")
[ "$imgui_on" = 1 ] || fail "ImGui 已开时 F4：ImGui ON 应保持 1 次，实际 $imgui_on"
[ "$imgui_off" = 0 ] || fail "ImGui 已开时 F4 收起面板不应关闭 ImGui（期望 OFF=0，实际 $imgui_off）"
on=$(count_lines "runtime: engine options panel ON" "$WORK/f4_keep_imgui.log")
off=$(count_lines "runtime: engine options panel OFF" "$WORK/f4_keep_imgui.log")
[ "$on" = 1 ] && [ "$off" = 1 ] || fail "ImGui 已开时 F4 应只开关选项面板（期望 ON=1/OFF=1，实际 $on/$off）"

[ -s "$WORK/f2_only.ppm" ] || fail "F2 参考截图未生成"
[ -s "$WORK/f2f4_mix.ppm" ] || fail "F2/F4 混合截图未生成"
[ -s "$WORK/f2f4_ctrl.ppm" ] || fail "F2/F4 对照截图未生成"

"$PY" - "$WORK/f2_only.ppm" "$WORK/f2f4_mix.ppm" "$WORK/f2f4_ctrl.ppm" <<'PY' \
    || fail "F2 再开复活了 F4 面板（右侧区域出现面板像素）"
import sys

# stats 面板固定在左上（x<250），选项面板默认贴在它右侧（x>=256）→ 只比较右侧区域，
# 避开 stats 里的 FPS/耗时文字（跨进程有抖动）。
X0 = 250
THR = 12


def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{p}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{p}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


w, h, ref = load(sys.argv[1])
w2, h2, mix = load(sys.argv[2])
w3, h3, ctrl = load(sys.argv[3])
assert (w, h) == (640, 360), f'尺寸 {w}x{h} != 设计分辨率 640x360'
assert (w2, h2) == (w, h) and (w3, h3) == (w, h), '三张截图尺寸不一致'


def region_diff(a, b):
    n = 0
    for y in range(h):
        base = y * w * 3
        for x in range(X0, w):
            i = base + x * 3
            if (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])) > THR:
                n += 1
    return n


diff_mix = region_diff(ref, mix)     # 期望 0（F2 再开不应出现 F4 面板）
diff_ctrl = region_diff(ref, ctrl)   # 期望 > 5000（F4 常开 → 面板像素确实被检测到）
print(f'imgui_engine_options_e2e: F2/F4 互不干扰 右侧区域差异 mix={diff_mix} ctrl={diff_ctrl}')
sys.exit(0 if (diff_mix <= 10 and diff_ctrl > 5000) else 1)
PY

# ---- ⑦ 引擎热键不双触发 -----------------------------------------------------
echo "imgui_engine_options_e2e: ⑦ 引擎热键 F2/F4 不传给脚本（防双触发）"

# 65 = 'A'（普通按键，应能到达脚本 = 证明输入通路正常）；1002/1004 = F2/F4（引擎热键，应被消费）。
# 注：设备事件只在**有真实窗口**时经 input 模块下发给脚本（headless 无窗口路径不投递玩法输入）
# → 本用例单独开一个小窗口（仍 MAQUE_PRESENT=off，不呈现）。
MAQUE_WINDOW=800x650 MAQUE_SIM_KEY_AT="5:65,10:1002,15:1004" \
    run "$WORK/key_router.log" "按键路由运行失败" --frames 40 "$SCN"
grep -qF "e2e_key 65" "$WORK/key_router.log" \
    || fail "普通按键 65 未到达脚本（输入通路异常，无法验证 F2/F4 是否被消费）"
if grep -qF "e2e_key 1002" "$WORK/key_router.log"; then
    fail "F2 仍被下发给脚本（开面板会同时触发游戏自己的 F2 动作）"
fi
if grep -qF "e2e_key 1004" "$WORK/key_router.log"; then
    fail "F4 仍被下发给脚本（开面板会同时触发游戏自己的 F4 动作）"
fi

# ---- ⑧ 全部日志无 ASan ------------------------------------------------------
for log in "$WORK"/*.log; do
    [ -e "$log" ] && check_clean "$log"
done

echo "imgui_engine_options_e2e: ok（F4 沿检测/面板进像素/MSAA/默认太阳光/主相机 near·far/F2 与 F4 互不干扰/热键不双触发/无 ASan）"
