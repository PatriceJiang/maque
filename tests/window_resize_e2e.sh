#!/usr/bin/env bash
# 窗口 resize / 最大化端到端（docs/05）：真实窗口 + 屏幕分辨率渲染目标。
#   设计分辨率 320x240 只作为世界/UI 坐标参考；默认 render_scale=1 时目标跟随 drawable。
#   在 1x DPI 上：800x650 窗口 → 800x650 目标，resize 到 1000x700 → 1000x700 目标。
#
# 覆盖：
#   ① 运行期 resize 一次：目标与窗口设备像素同步变化，抓帧尺寸同步且非黑；
#   ② 启动即最大化：目标跟随最大化后的 drawable；
#   ③ MAQUE_RENDER_SCALE=0.5：目标始终为屏幕设备像素的一半，仍随 resize 变化；
#   ④ 连续两次 resize：最后目标等于最后 drawable，退出正常且无 ASan 迹象。
#
# 参数：$1 = maque_player 可执行、$2 = 场景（scenes/ball.scn）、$3 = WORK 目录。
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"

# 无显示环境判定（Linux/Wayland 看 DISPLAY / WAYLAND_DISPLAY；Windows 始终有窗口系统，
# 见 window_present_e2e：旧写法会让 Windows 上的窗口用例"假跳过"）。
_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "window_resize_e2e: skip（无显示环境）"
  exit 77                                   # CTest SKIP_RETURN_CODE
fi

fail() { echo "window_resize_e2e: FAIL $*" >&2; exit 1; }

[ -f "$PLAYER" ] || fail "player 可执行不存在: $PLAYER"
[ -f "$SCENE" ] || fail "场景不存在: $SCENE"
[ "${HAVE_PY:-0}" = "1" ] || fail "找不到可用的 Python 3（MAQUE_PYTHON 可覆盖）"

rm -rf "$WORK"; mkdir -p "$WORK"

# 跑前清掉相关 MAQUE_*：CI 与服务/交互式 shell 共用环境时最容易"串味"出假通过/假失败。
unset MAQUE_HEADLESS MAQUE_VIEWS MAQUE_RENDER_SCALE MAQUE_MAX_FRAMES MAQUE_PRESENT \
      MAQUE_WINDOW MAQUE_WINDOW_MAXIMIZED MAQUE_WINDOW_EVENT_AT MAQUE_WINDOW_POS MAQUE_TITLE \
      MAQUE_CAPTURE MAQUE_CAPTURE_FRAMES MAQUE_CAPTURE_EVERY MAQUE_CAPTURE_WGPU \
      MAQUE_IMGUI MAQUE_IMGUI_POINTER MAQUE_IMGUI_SCALE MAQUE_IMGUI_FONT_SIZE MAQUE_KEEP_OPEN \
      MAQUE_ALWAYS_ON_TOP MAQUE_PACE MAQUE_TRACE_PACE MAQUE_DPI_SCALE MAQUE_SAFE_AREA \
      MAQUE_SHOT MAQUE_SHOT_FRAME MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_SHOT_EVERY MAQUE_SHOT_COUNT \
      MAQUE_SIM MAQUE_SIM_CLICK MAQUE_TRACE_INPUT MAQUE_TRACE_STATS 2>/dev/null || true

# 所有场景统一：窗口 800x650、单视图；默认目标即 800x650 drawable。
BASE=(MAQUE_WINDOW=800x650 MAQUE_VIEWS=1)

# 真实窗口 + GPU 在 CI 负载下偶发**进程级**创建失败（实测约 1/6：同一命令重跑即过）。
# 每次运行最多两次尝试；两次都失败仍判失败（不把真实回归掩盖成"偶发"），并保留第一次日志。
run_player() {
    local log="$1"; shift
    local n
    for n in 1 2; do
        if "$@" >"$log" 2>&1; then return 0; fi
        if [ "$n" -eq 1 ]; then
            cp "$log" "$log.retry1" 2>/dev/null || true
            echo "window_resize_e2e: 第 1 次运行失败，重试一次（日志 $log）" >&2
        fi
    done
    return 1
}

# ---- ① 运行期 resize 一次：800x650 → 1000x700（帧 4），目标跟随 drawable ——
run_player "$WORK/resize.log" env "${BASE[@]}" \
    MAQUE_CAPTURE="$WORK/resize" MAQUE_CAPTURE_EVERY=1 MAQUE_CAPTURE_FRAMES=8 \
    MAQUE_MAX_FRAMES=12 MAQUE_WINDOW_EVENT_AT="4:1000x700" \
    timeout 90 "$PLAYER" "$SCENE" || fail "resize 运行失败（日志 $WORK/resize.log）"

# ---- ② 启动即最大化 ----
run_player "$WORK/max.log" env "${BASE[@]}" MAQUE_WINDOW_MAXIMIZED=1 \
    MAQUE_CAPTURE="$WORK/max" MAQUE_CAPTURE_EVERY=1 MAQUE_CAPTURE_FRAMES=8 \
    MAQUE_MAX_FRAMES=12 \
    timeout 90 "$PLAYER" "$SCENE" || fail "最大化运行失败（日志 $WORK/max.log）"

# ---- ③ 独立输出倍率：目标始终是 drawable 的 0.5 倍 ----
run_player "$WORK/fixed.log" env "${BASE[@]}" MAQUE_RENDER_SCALE=0.5 \
    MAQUE_CAPTURE="$WORK/fixed" MAQUE_CAPTURE_EVERY=1 MAQUE_CAPTURE_FRAMES=8 \
    MAQUE_MAX_FRAMES=12 MAQUE_WINDOW_EVENT_AT="4:1000x700" \
    timeout 90 "$PLAYER" "$SCENE" || fail "输出倍率运行失败（日志 $WORK/fixed.log）"

# ---- ④ 连续两次 resize：1000x700（帧 3）→ 900x600（帧 6）----
run_player "$WORK/multi.log" env "${BASE[@]}" \
    MAQUE_CAPTURE="$WORK/multi" MAQUE_CAPTURE_EVERY=1 MAQUE_CAPTURE_FRAMES=12 \
    MAQUE_MAX_FRAMES=12 MAQUE_WINDOW_EVENT_AT="3:1000x700;6:900x600" \
    timeout 90 "$PLAYER" "$SCENE" || fail "连续 resize 运行失败（日志 $WORK/multi.log）"

# ---- 日志/抓帧统一断言（Python：解析 window resized 行、PPM 尺寸与画面内容）----
"$PY" - "$WORK" <<'PY' || exit 1
import glob
import re
import sys

work = sys.argv[1]


def fail(msg):
    print(f'window_resize_e2e: FAIL {msg}', file=sys.stderr)
    sys.exit(1)


def read_text(path):
    return open(path, 'rb').read().decode('utf-8', 'replace')


ARROW = r'(?:→|->)'
# 行格式：
#   runtime: window resized <oldW>x<oldH> → <newW>x<newH>: render target <oldTW>x<oldTH> → <newTW>x<newTH> (render_scale <S>, views=<n>)
RESIZED_RE = re.compile(
    r'window resized\s+(\d+)x(\d+)\s*' + ARROW + r'\s*(\d+)x(\d+)'
    r':?\s*render target\s+(\d+)x(\d+)\s*' + ARROW + r'\s*(\d+)x(\d+)'
    r'\s*\(\s*render_scale\s+([0-9.]+)\s*,\s*views\s*=\s*(\d+)\s*\)'
)


def parse_resized(log):
    """返回每行 window resized 解析结果（保持日志顺序）。"""
    out = []
    for line in read_text(log).splitlines():
        if 'window resized' not in line:
            continue
        m = RESIZED_RE.search(line)
        if not m:
            fail(f'window resized 行格式无法解析（实现与契约不一致）：{line!r}')
        g = m.groups()
        out.append({
            'old_w': int(g[0]), 'old_h': int(g[1]),
            'new_w': int(g[2]), 'new_h': int(g[3]),
            'old_tw': int(g[4]), 'old_sh': int(g[5]),
            'new_tw': int(g[6]), 'new_sh': int(g[7]),
            'render_scale': float(g[8]),
            'views': int(g[9]),
            'line': line.strip(),
        })
    return out


def capture_files(prefix):
    def idx(p):
        m = re.search(r'_(\d+)\.ppm$', p)
        return int(m.group(1)) if m else 1 << 30
    return sorted(glob.glob(prefix + '_*.ppm'), key=idx)


def load_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    if len(parts) < 4 or parts[0].strip() != b'P6':
        fail(f'不是合法 PPM: {path}')
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def frame_stats(path):
    """按 2px 步长采样：亮像素 / 非黑像素 / 颜色多样性。"""
    w, h, px = load_ppm(path)
    sampled = bright = nonblack = 0
    colors = set()
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            s = r + g + b
            sampled += 1
            if s > 90:
                bright += 1
            if s > 12:
                nonblack += 1
            colors.add((r, g, b))
    return w, h, sampled, bright, nonblack, len(colors)


def check_last_frame(prefix, label):
    caps = capture_files(prefix)
    if not caps:
        fail(f'{label}：未产生窗口抓帧（MAQUE_CAPTURE 没生效？预览日志）')
    w, h, sampled, bright, nonblack, colors = frame_stats(caps[-1])
    # ball.scn 背景 (15,15,23)，球是亮色 → 黑屏时三者都会塌到 ~0。
    if bright < sampled * 0.002:
        fail(f'{label}：最后抓帧几乎是空的（亮像素 {bright}/{sampled}，尺寸 {w}x{h}）')
    if nonblack < sampled * 0.4:
        fail(f'{label}：最后抓帧非黑像素不足（{nonblack}/{sampled}，尺寸 {w}x{h}——画面没上屏？）')
    if colors < 100:
        fail(f'{label}：颜色数 {colors}（纯色/黑屏？尺寸 {w}x{h}）')
    return caps, (w, h)


def capture_sizes(prefix):
    sizes = []
    for p in capture_files(prefix):
        w, h, _ = load_ppm(p)
        sizes.append((w, h))
    return sizes


# ================= ① 运行期 resize：目标跟随 drawable =================
resize_log = f'{work}/resize.log'
if not re.search(r'window event frame=4 resize 1000x700', read_text(resize_log)):
    fail('resize：日志缺 `window event frame=4 resize 1000x700`（MAQUE_WINDOW_EVENT_AT 未下发？）')

lines = parse_resized(resize_log)
if len(lines) != 1:
    fail(f'resize：窗口尺寸变化一次应恰好 1 行 window resized，实际 {len(lines)} 行：'
         + ' | '.join(x['line'] for x in lines))
ln = lines[0]
if (ln['old_w'], ln['old_h']) != (800, 650):
    fail(f"resize：旧窗口 {ln['old_w']}x{ln['old_h']}（应 800x650）")
if (ln['new_w'], ln['new_h']) != (1000, 700):
    fail(f"resize：新窗口 {ln['new_w']}x{ln['new_h']}（应 1000x700）")
if abs(ln['render_scale'] - 1.0) > 0.01:
    fail(f"resize：render_scale={ln['render_scale']:.3f}（默认应为 1.0）")
if ln['views'] != 1:
    fail(f"resize：views={ln['views']}（MAQUE_VIEWS=1 未生效）")

sizes = capture_sizes(f'{work}/resize')
if not sizes:
    fail('resize：未产生窗口抓帧')
old_target = (ln['old_tw'], ln['old_sh'])
new_target = (ln['new_tw'], ln['new_sh'])
if old_target not in sizes:
    fail(f'resize：抓帧尺寸 {sorted(set(sizes))} 缺 resize 前目标 {old_target}')
if new_target not in sizes:
    fail(f'resize：抓帧尺寸 {sorted(set(sizes))} 缺 resize 后目标 {new_target}')
extra = sorted(set(sizes) - {old_target, new_target})
if extra:
    fail(f'resize：抓帧出现非预期的中间尺寸 {extra}（应只有 {old_target} / {new_target}）')
if sizes[-1] != new_target:
    fail(f'resize：最后一张抓帧 {sizes[-1][0]}x{sizes[-1][1]}（应为 resize 后目标 {new_target}）')
check_last_frame(f'{work}/resize', 'resize')
print(f'window_resize_e2e: ① resize ok（目标 {old_target} → {new_target}，render_scale=1；'
      f'抓帧 {len(sizes)} 张：{sizes[0][0]}x{sizes[0][1]} … {sizes[-1][0]}x{sizes[-1][1]}）')

# ================= ② 启动即最大化 =================
max_log = f'{work}/max.log'
lines = parse_resized(max_log)
max_hit = [x for x in lines if (x['old_w'], x['old_h']) == (800, 650) and x['new_w'] > 800]
if not max_hit:
    detail = ' | '.join(x['line'] for x in lines) if lines else '（没有任何 window resized 行）'
    fail('最大化：日志没有 `window resized 800x650 → WxH`（W>800）——窗口可能没被 WM 最大化'
         f'（要求 SDL_WINDOW_RESIZABLE / MAQUE_WINDOW_MAXIMIZED 未生效）。日志：{detail}')
last = lines[-1]
if last['new_w'] <= 800:
    fail(f"最大化：最后一次窗口尺寸 {last['new_w']}x{last['new_h']}（W 应 >800）")
if abs(last['render_scale'] - 1.0) > 0.01:
    fail(f"最大化：render_scale {last['render_scale']:.3f}（默认应为 1.0）")
if last['views'] != 1:
    fail(f"最大化：views={last['views']}（MAQUE_VIEWS=1 未生效）")
caps, (cw, ch) = check_last_frame(f'{work}/max', '最大化')
if (cw, ch) != (last['new_tw'], last['new_sh']):
    fail(f"最大化：最后抓帧 {cw}x{ch} ≠ 渲染目标 {last['new_tw']}x{last['new_sh']}")
print(f"window_resize_e2e: ② 最大化 ok（800x650 → {last['new_w']}x{last['new_h']}，"
      f"目标 {last['new_tw']}x{last['new_sh']}，render_scale=1，抓帧 {cw}x{ch}）")

# ================= ③ 输出倍率：目标为 drawable 的 0.5 倍 =================
fixed_log = f'{work}/fixed.log'
lines = parse_resized(fixed_log)
if len(lines) != 1:
    fail(f'输出倍率：窗口尺寸变化一次应恰好 1 行 window resized，实际 {len(lines)} 行：'
         + ' | '.join(x['line'] for x in lines))
ln = lines[0]
if (ln['old_w'], ln['old_h'], ln['new_w'], ln['new_h']) != (800, 650, 1000, 700):
    fail(f"输出倍率：窗口 {ln['old_w']}x{ln['old_h']} → {ln['new_w']}x{ln['new_h']}"
         f"（应 800x650 → 1000x700）")
if abs(ln['render_scale'] - 0.5) > 0.01:
    fail(f"输出倍率：render_scale={ln['render_scale']:.3f}（应为 0.5）")

sizes = capture_sizes(f'{work}/fixed')
if not sizes:
    fail('输出倍率：未产生窗口抓帧')
old_surface, new_surface = sizes[0], sizes[-1]
want_old = (int(old_surface[0] * 0.5 + 0.5), int(old_surface[1] * 0.5 + 0.5))
want_new = (int(new_surface[0] * 0.5 + 0.5), int(new_surface[1] * 0.5 + 0.5))
if (ln['old_tw'], ln['old_sh']) != want_old or (ln['new_tw'], ln['new_sh']) != want_new:
    fail(f"输出倍率：0.5 目标应为 drawable 一半 {want_old} → {want_new}，实际 "
         f"{ln['old_tw']}x{ln['old_sh']} → {ln['new_tw']}x{ln['new_sh']}")
extra = sorted(set(sizes) - {old_surface, new_surface})
if extra:
    fail(f'输出倍率：抓帧出现非预期尺寸 {extra}')
check_last_frame(f'{work}/fixed', '输出倍率')
print(f'window_resize_e2e: ③ render_scale=0.5 ok（目标 {want_old} → {want_new}；抓帧 {len(sizes)} 张 '
      f'{sizes[0][0]}x{sizes[0][1]} … {sizes[-1][0]}x{sizes[-1][1]}）')

# ================= ④ 连续两次 resize =================
multi_log = f'{work}/multi.log'
lines = parse_resized(multi_log)
if len(lines) < 2:
    fail(f'连续 resize：window resized 行 {len(lines)} 条（应 ≥2）：'
         + ' | '.join(x['line'] for x in lines))
if not re.search(r'window event frame=3 resize 1000x700', read_text(multi_log)):
    fail('连续 resize：日志缺 `window event frame=3 resize 1000x700`')
if not re.search(r'window event frame=6 resize 900x600', read_text(multi_log)):
    fail('连续 resize：日志缺 `window event frame=6 resize 900x600`')
last = lines[-1]
if (last['new_w'], last['new_h']) != (900, 600):
    fail(f"连续 resize：最后一次窗口 {last['new_w']}x{last['new_h']}（应 900x600）")
multi_sizes = capture_sizes(f'{work}/multi')
if not multi_sizes:
    fail('连续 resize：未产生窗口抓帧')
if (last['new_tw'], last['new_sh']) != multi_sizes[-1]:
    fail(f"连续 resize：最后渲染目标 {last['new_tw']}x{last['new_sh']} ≠ 最后 drawable {multi_sizes[-1]}")
if abs(last['render_scale'] - 1.0) > 0.01:
    fail(f"连续 resize：render_scale={last['render_scale']:.3f}（默认应为 1.0）")

# 崩溃/内存错误迹象（Debug+ASan 构建；命中即真实缺陷）
for name, log in (('resize', resize_log), ('最大化', max_log), ('输出倍率', fixed_log), ('连续 resize', multi_log)):
    txt = read_text(log)
    for needle in ('AddressSanitizer', 'heap-buffer-overflow', 'SEGV'):
        if needle in txt:
            fail(f'{name}：日志含崩溃/ASan 迹象 `{needle}`（见 {log}）')
print(f'window_resize_e2e: ④ 连续 resize ok（{len(lines)} 行；最后逻辑窗口 900x600，目标 {multi_sizes[-1]}，'
      f'render_scale=1，exit 0、无 ASan）')
PY

echo "window_resize_e2e: PASS"
