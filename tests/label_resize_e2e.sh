#!/usr/bin/env bash
# label 窗口 resize **布局不变性**端到端（docs/17/18；window_resize_e2e 的文本专项）。
# 用户反馈：窗口 resize 后文本布局/大小相对 canvas 应不变。排版现在在**设计像素**空间，
# ss（渲染倍率）只影响字形栅格化，不应改变 text_w/text_h/line_count/fit_font_size。
#
# 场景 scenes/label/resize.scn（设计分辨率 640x360）：wrap（长文本 max_width=120 Word 黄）/
# shrink（font20 max_height=60 Shrink 青）/ mixed（200 字中英混排 max_width=160 品红）/ single（白）。
# resize.js 在第 3 帧（resize 前）与第 9 帧（resize 后）各 log 一行 `label_resize_summary {json}`。
#
# 覆盖：
#   ① run A：MAQUE_WINDOW=800x600（ss=1.25, target 800x450）+ 第 6 帧真实窗口 resize 到
#      1600x1200（ss=2.5, target 1600x900）→ 日志有 `runtime: window resized ...`；
#      stage3 与 stage9 的**所有 metrics 逐字段完全相等**。
#   ② run B：MAQUE_WINDOW=1600x1200（初始 ss=2.5）→ stage3 metrics 与 run A 的 stage3/stage9 完全相等
#      （不同初始窗口/不同 ss 下布局一致）。
#   ③ 像素（相对 canvas 不变）：run A 的 frames PPM 尺寸随 target 变化（800x450 → 1600x900）；
#      `wrap` 黄色 ink 的**归一化 bbox**（x/w,y/h,w/h）resize 前后差 < 0.01。
# 无显示环境 exit 77（CTest SKIP_RETURN_CODE）；真实窗口创建偶发失败时重试一次。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

# 无显示环境判定（与 window_resize_e2e / window_present_e2e 一致；Windows 始终有窗口系统）
_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "label_resize_e2e: skip（无显示环境）"
  exit 77
fi

fail() { echo "label_resize_e2e: FAIL $*" >&2; exit 1; }
[ -f "$PLAYER" ] || fail "player 可执行不存在: $PLAYER"
[ -f "$SCENE" ] || fail "场景不存在: $SCENE"
[ -n "${PY:-}" ] || fail "找不到可用的 Python 3（MAQUE_PYTHON 可覆盖）"

rm -rf "$WORK"; mkdir -p "$WORK"

# 跑前清掉相关 MAQUE_*：CI/交互 shell 环境串味最容易造成假通过/假失败（尤其 MAQUE_HEADLESS）。
unset MAQUE_HEADLESS MAQUE_VIEWS MAQUE_RENDER_SCALE MAQUE_MAX_FRAMES MAQUE_PRESENT \
      MAQUE_WINDOW MAQUE_WINDOW_MAXIMIZED MAQUE_WINDOW_EVENT_AT MAQUE_WINDOW_POS MAQUE_TITLE \
      MAQUE_CAPTURE MAQUE_CAPTURE_FRAMES MAQUE_CAPTURE_EVERY MAQUE_CAPTURE_WGPU \
      MAQUE_IMGUI MAQUE_IMGUI_POINTER MAQUE_IMGUI_SCALE MAQUE_IMGUI_FONT_SIZE MAQUE_KEEP_OPEN \
      MAQUE_ALWAYS_ON_TOP MAQUE_PACE MAQUE_TRACE_PACE MAQUE_DPI_SCALE MAQUE_SAFE_AREA \
      MAQUE_SHOT MAQUE_SHOT_FRAME MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_SHOT_EVERY MAQUE_SHOT_COUNT \
      MAQUE_SIM MAQUE_SIM_CLICK MAQUE_TRACE_INPUT MAQUE_TRACE_STATS 2>/dev/null || true

# 真实窗口/GPU 在 CI 负载下偶发进程级创建失败：最多尝试两次（两次都失败仍判失败）。
run_player() {
    local log="$1"; shift
    local n
    for n in 1 2; do
        if "$@" >"$log" 2>&1; then return 0; fi
        if [ "$n" -eq 1 ]; then
            cp "$log" "$log.retry1" 2>/dev/null || true
            echo "label_resize_e2e: 第 1 次运行失败，重试一次（日志 $log）" >&2
        fi
    done
    return 1
}

# ---- ① run A：800x600 启动，第 6 帧 resize 到 1600x1200 ----
run_player "$WORK/a.log" env \
    MAQUE_WINDOW=800x600 MAQUE_WINDOW_EVENT_AT="6:1600x1200" MAQUE_MAX_FRAMES=12 \
    timeout 120 "$PLAYER" "$SCENE" "$WORK/frames" \
    || fail "run A 失败（日志 $WORK/a.log）"

# ---- ② run B：直接 1600x1200 启动（不同初始 ss）----
run_player "$WORK/b.log" env \
    MAQUE_WINDOW=1600x1200 MAQUE_MAX_FRAMES=8 \
    timeout 120 "$PLAYER" "$SCENE" "$WORK/framesB" \
    || fail "run B 失败（日志 $WORK/b.log）"

# ---- ③ 断言：metrics 两阶段/两 run 完全一致 + resize 日志 + 归一化 ink bbox ----
"$PY" - "$WORK" <<'PY' || exit 1
import glob
import json
import re
import sys

work = sys.argv[1]
metric_keys = ['text_w', 'text_h', 'line_count', 'fit_font_size',
               'font_size', 'x', 'y', 'max_width', 'max_height']
bad = []
def ck(cond, msg):
    if not cond:
        bad.append(msg)

def read_text(p):
    return open(p, 'rb').read().decode('utf-8', 'replace')

def summaries(path):
    out = []
    for line in read_text(path).splitlines():
        m = re.search(r'label_resize_summary (\{.*\})\s*$', line)
        if m:
            out.append(json.loads(m.group(1)))
    return out

a = summaries(f'{work}/a.log')
b = summaries(f'{work}/b.log')
ck(len(a) == 2, f'run A 应有 2 行 label_resize_summary（stage3/stage9），实际 {len(a)}')
ck(len(b) >= 1, f'run B 应有 label_resize_summary，实际 {len(b)}')
if len(a) != 2 or not b:
    print('label_resize_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
s3 = next((x for x in a if x.get('stage') == 3), None)
s9 = next((x for x in a if x.get('stage') == 9), None)
b3 = next((x for x in b if x.get('stage') == 3), None)
ck(s3 is not None and s9 is not None, 'run A 缺 stage3 或 stage9')
ck(b3 is not None, 'run B 缺 stage3')
if s3 is None or s9 is None or b3 is None:
    print('label_resize_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)

ck(s3.get('design') == [640, 360], f"设计分辨率应 [640,360]（run A stage3: {s3.get('design')}）")
ck(s9.get('design') == [640, 360], f"设计分辨率应 [640,360]（run A stage9: {s9.get('design')}）")
ck(b3.get('design') == [640, 360], f"设计分辨率应 [640,360]（run B stage3: {b3.get('design')}）")

def cmp_cases(ref, other, label):
    rc, oc = ref.get('cases', {}), other.get('cases', {})
    ck(sorted(rc) == sorted(oc), f'{label}: 用例集合不同 {sorted(rc)} vs {sorted(oc)}')
    for name in sorted(set(rc) & set(oc)):
        for k in metric_keys:
            rv, ov = rc[name].get(k), oc[name].get(k)
            if isinstance(rv, (int, float)) and isinstance(ov, (int, float)):
                if abs(float(rv) - float(ov)) > 1e-6:
                    bad.append(f'{label}: {name}.{k} 不一致 {rv} vs {ov}')
            elif rv != ov:
                bad.append(f'{label}: {name}.{k} 不一致 {rv} vs {ov}')

cmp_cases(s3, s9, 'run A resize 前后（stage3 vs stage9）')
cmp_cases(s3, b3, 'run A stage3 vs run B stage3（不同初始 ss）')
cmp_cases(s9, b3, 'run A stage9 vs run B stage3（不同初始 ss）')

# resize 日志：窗口 800x600 → 1600x1200，render target 800x450 → 1600x900，ss 1.25 → 2.5
alog = read_text(f'{work}/a.log')
m = re.search(r'window resized\s+800x600\s*(?:→|->)\s*1600x1200'
              r'.*?render target\s+800x450\s*(?:→|->)\s*1600x900'
              r'.*?ss\s+1\.250\s*(?:→|->)\s*2\.500', alog)
ck(m is not None, 'run A 日志缺 `window resized 800x600 → 1600x1200: render target 800x450 → 1600x900 (ss 1.250 → 2.500 ...)`')
m_ev = re.search(r'window event frame=6 resize 1600x1200', alog)
ck(m_ev is not None, 'run A 日志缺 `window event frame=6 resize 1600x1200`')

# 归一化 ink bbox：wrap 黄色 ink 在 resize 前后相对 canvas 不变（尺寸/位置差 <0.01）
def load_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    if len(parts) < 4 or parts[0].strip() != b'P6':
        bad.append(f'不是合法 PPM: {path}')
        return None
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def yellow_bbox(path):
    ppm = load_ppm(path)
    if not ppm:
        return None
    w, h, px = ppm
    x0 = y0 = 1 << 30; x1 = y1 = -1
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            if r > 120 and g > 0.5 * r and g < 1.02 * r and b < 0.6 * r:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 < 0:
        return None
    return w, h, x0, y0, x1, y1

frames = sorted(glob.glob(f'{work}/frames/frame_*.ppm'))
ck(len(frames) >= 10, f'run A frames 数 {len(frames)}（应 ≥10）')
before = after = None
sizes = set()
for p in frames:
    ppm = load_ppm(p)
    if not ppm:
        continue
    w, h, _ = ppm
    sizes.add((w, h))
    if (w, h) == (800, 450):
        before = p
    if (w, h) == (1600, 900):
        after = p
ck((800, 450) in sizes, f'frames 缺 resize 前尺寸 800x450（实际 {sorted(sizes)}）')
ck((1600, 900) in sizes, f'frames 缺 resize 后尺寸 1600x900（实际 {sorted(sizes)}）')
if before and after:
    bb = yellow_bbox(before)
    ba = yellow_bbox(after)
    ck(bb is not None, 'resize 前帧找不到 wrap 黄色 ink')
    ck(ba is not None, 'resize 后帧找不到 wrap 黄色 ink')
    if bb and ba:
        def norm(t):
            w, h, x0, y0, x1, y1 = t
            return (x0 / w, y0 / h, (x1 - x0 + 1) / w, (y1 - y0 + 1) / h)
        nb, na = norm(bb), norm(ba)
        diffs = [abs(p - q) for p, q in zip(nb, na)]
        ck(max(diffs) < 0.01,
           f'归一化 ink bbox resize 前后差 {max(diffs):.4f}（>0.01）：before={tuple(round(v,4) for v in nb)} '
           f'after={tuple(round(v,4) for v in na)}')
        print(f'label_resize_e2e: ③ 归一化 ink bbox ok（before={tuple(round(v,4) for v in nb)} '
              f'after={tuple(round(v,4) for v in na)} max_diff={max(diffs):.5f}；'
              f'PPM {bb[0]}x{bb[1]} → {ba[0]}x{ba[1]}）')
else:
    if not before:
        bad.append('未找到 resize 前帧（800x450）')
    if not after:
        bad.append('未找到 resize 后帧（1600x900）')

if bad:
    print('label_resize_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)

w0, h0 = s3['cases']['wrap']['text_w'], s3['cases']['wrap']['text_h']
sh = s3['cases']['shrink']
mx = s3['cases']['mixed']
print(f"label_resize_e2e: ① resize 前后 metrics 完全相等 ok（wrap {w0:.0f}x{h0:.0f} lines={s3['cases']['wrap']['line_count']}；"
      f"shrink h={sh['text_h']:.0f} fit={sh['fit_font_size']:.2f}；mixed h={mx['text_h']:.0f} lines={mx['line_count']}）")
print(f"label_resize_e2e: ② 1600x1200 起始 vs 800x600+resize metrics 完全相等 ok（stage3/9 一致）")
PY

echo "label_resize_e2e: PASS"
exit 0
