#!/usr/bin/env bash
# 场景自定义全屏 shader pass 端到端（render.ShaderPass + scenes/shadertoy/seascape）：
#   WGSL 是**场景资源**（VFS 根 = 场景目录），不是 assets/builtin/shaders 内建 shader；
#   引擎提供全屏三角形 VS，资源只提供 `fs_main` + 32B uniform（group0 binding0）。
#   ① --shot：日志有 shader pass 加载行 + "runtime: shot"；PPM 640x360 结构断言
#      （颜色丰富/非纯色、顶部天空比底部海面亮、非平坦像素占比高）
#   ② 动画：第 1 帧 vs 第 45 帧 显著像素差（组件 time 真的推进进 shader）
#   ③ 确定性：同一帧跑两次逐字节一致（渲染不得依赖墙钟/不回写模拟）
#   ④ 源码真相：WGSL 在 scenes/shadertoy/，场景 JSON 挂着 render.ShaderPass 且引用该 .wgsl
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
SCN="$2"
WORK="$3"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# 全屏 shader 走离屏目标即可，CI 无显示器；清理可能干扰截图路径的宿主环境变量。
export MAQUE_WINDOW=0x0
unset MAQUE_SHOT MAQUE_SHOT_FRAME MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_MAX_FRAMES \
      MAQUE_PRESENT MAQUE_HEADLESS MAQUE_CAPTURE MAQUE_IMGUI 2>/dev/null || true

[ -f "$PLAYER" ] || { echo "shadertoy_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -f "$SCN" ] || { echo "shadertoy_e2e: skip（缺场景: $SCN）"; exit 77; }
[ "${HAVE_PY:-0}" = "1" ] || { echo "shadertoy_e2e: skip（无可用的 Python 3）"; exit 77; }
fail() { echo "shadertoy_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

# ---- ① 离屏渲染：加载行 + 截图 + PPM 结构 ----
timeout 120 "$PLAYER" --shot "$WORK/f1.ppm" --frames 2 "$SCN" >"$WORK/run1.txt" 2>&1 \
    || { cat "$WORK/run1.txt"; fail "场景运行/截图失败（退出码非 0）"; }
grep -qiE "shader pass.*loaded" "$WORK/run1.txt" \
    || { cat "$WORK/run1.txt"; fail "缺 shader pass 加载行（.wgsl 未加载/编译失败/组件未被提取）"; }
if grep -qE "shader pass.*(失败|未找到|fail|error|missing|not found)" "$WORK/run1.txt"; then
    cat "$WORK/run1.txt"; fail "shader pass 加载/编译报错"
fi
grep -q "runtime: shot" "$WORK/run1.txt" || { cat "$WORK/run1.txt"; fail "截图未写入（缺 runtime: shot）"; }

"$PY" - "$WORK/f1.ppm" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
assert (w, h) == (640, 360), (w, h)
n = w * h
def lum(i):
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]
lums = [lum(i) for i in range(0, len(px), 3)]
mean = sum(lums) / n
sd = (sum((v - mean) ** 2 for v in lums) / n) ** 0.5
# 颜色丰富度：5bit/通道量化后的唯一颜色数（平滑渐变应该上千，纯色/故障画面只有个位数）
colors = set((px[i] >> 3, px[i + 1] >> 3, px[i + 2] >> 3) for i in range(0, len(px), 3))
# 结构：顶部 20%（天空）应明显亮于底部 20%（海面），照 Shadertoy 原图（y 翻转后）
top = sum(lums[y * w + x] for y in range(h // 5) for x in range(0, w, 2))
top_n = (h // 5) * len(range(0, w, 2))
bottom = sum(lums[y * w + x] for y in range(h - h // 5, h) for x in range(0, w, 2))
bottom_n = (h // 5) * len(range(0, w, 2))
top_m, bottom_m = top / top_n, bottom / bottom_n
# 非平坦像素：与全图均值亮度差 > 10（全屏 shader 应有大面积明暗结构，而非一整块近似常数）
nonflat = sum(1 for v in lums if abs(v - mean) > 10) / n
print(f'shadertoy_e2e: f1 {w}x{h} colors={len(colors)} sd={sd:.1f} '
      f'top={top_m:.1f} bottom={bottom_m:.1f} nonflat={nonflat:.3f}')
ok = (len(colors) >= 200 and sd > 10.0
      and top_m > bottom_m + 20.0 and nonflat > 0.5)
sys.exit(0 if ok else 1)
PY
echo "shadertoy_e2e: ① 离屏全屏 shader（640x360 / 颜色丰富 / 天空亮于海面）OK"

# ---- ② 动画：第 1 帧 vs 第 45 帧 显著不同 ----
timeout 120 "$PLAYER" --shot "$WORK/f45.ppm" --shot-frame 45 "$SCN" >"$WORK/run45.txt" 2>&1 \
    || { cat "$WORK/run45.txt"; fail "第 45 帧截图失败"; }
grep -q "runtime: shot" "$WORK/run45.txt" || { cat "$WORK/run45.txt"; fail "第 45 帧未截图"; }
"$PY" - "$WORK/f1.ppm" "$WORK/f45.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    assert (w, h) == (640, 360), (w, h)
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
ratio = diff / (w * h)
print(f'shadertoy_e2e: frame1 vs frame45 diff={diff} ({ratio:.3f})')
sys.exit(0 if ratio > 0.05 else 1)
PY
echo "shadertoy_e2e: ② 动画（frame 1 vs 45 像素差 > 5%）OK"

# ---- ③ 确定性：同一帧跑两次，PPM 逐字节一致 ----
timeout 120 "$PLAYER" --shot "$WORK/f45b.ppm" --shot-frame 45 "$SCN" >"$WORK/run45b.txt" 2>&1 \
    || { cat "$WORK/run45b.txt"; fail "第 45 帧第二次截图失败"; }
cmp -s "$WORK/f45.ppm" "$WORK/f45b.ppm" \
    || fail "同一帧两次渲染不一致（确定性破坏：shader 通路不得依赖墙钟）"
echo "shadertoy_e2e: ③ 确定性（同帧两次逐字节一致）OK"

# ---- ④ 源码真相：WGSL 属场景资源，不是内建 shader ----
WGSL="$ROOT/scenes/shadertoy/seascape.wgsl"
[ -f "$WGSL" ] || fail "缺场景资源 $WGSL"
[ ! -e "$ROOT/assets/builtin/shaders/seascape" ] \
    || fail "seascape 不得是内建 shader（assets/builtin/shaders/seascape 存在）"
grep -q "render.ShaderPass" "$SCN" || fail "场景未挂 render.ShaderPass 组件"
grep -q "seascape.wgsl" "$SCN" || fail "场景未引用 seascape.wgsl"
grep -q "fs_main" "$WGSL" || fail "$WGSL 缺 fragment 入口 fs_main"
echo "shadertoy_e2e: ④ 场景资源 WGSL（scenes/shadertoy/，非内建 shader）OK"

echo "shadertoy_e2e: PASS"
