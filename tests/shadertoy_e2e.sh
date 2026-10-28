#!/usr/bin/env bash
# 场景自定义全屏 shader pass 端到端（render.ShaderPass + Shadertoy demo 集）：
#   WGSL 是**场景资源**（VFS 根 = 场景目录），不是 assets/builtin/shaders 内建 shader；
#   引擎提供全屏三角形 VS，资源只提供 `fs_main` + 32B uniform（group0 binding0）。
#   当前 demo（scenes/shadertoy/）：
#     · seascape.scn   —— Shadertoy Ms2SD1 "Seascape"（TDM），天空亮/海面暗
#     · mandelbulb.scn —— Shadertoy MdXSWn "Mandelbulb"（Inigo Quilez），分形 + 渐变背景
#   逐场景断言：
#     ① --shot 日志有该 .wgsl 的 shader pass 加载行 + "runtime: shot"；PPM 640x360
#        （seascape：颜色丰富/非纯色/天空亮于海面；mandelbulb：颜色多样/非平坦）
#     ② 动画：第 1 帧 vs 第 60 帧 显著像素差（组件 time 真的推进进 shader）
#     ③ 确定性：同一帧跑两次逐字节一致（渲染不得依赖墙钟/不回写模拟）
#     ④ 源码真相：WGSL 在 scenes/shadertoy/，场景 JSON 挂着 render.ShaderPass 且引用该 .wgsl，
#        assets/builtin/shaders/<demo> 必须不存在。
#   mandelbulb 尚未入库时只 SKIP 该节，seascape 仍照常断言（两者可独立落地）。
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
[ -f "$SCN" ] || { echo "shadertoy_e2e: skip（缺主场景: $SCN）"; exit 77; }
[ "${HAVE_PY:-0}" = "1" ] || { echo "shadertoy_e2e: skip（无可用的 Python 3）"; exit 77; }
fail() { echo "shadertoy_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

# 单场景全套断言：$1 = demo 名（也作日志前缀），$2 = .scn，$3 = .wgsl，$4 = 1 时额外断言
# “顶部天空亮于底部海面”（seascape）；mandelbulb 无此语义，用 0。
check_scene() {
    local name="$1" scn="$2" wgsl="$3" top_check="$4"
    local stem="${wgsl##*/}"
    stem="${stem%.wgsl}"
    local d="$WORK/$name"
    mkdir -p "$d"
    echo "shadertoy_e2e: ---- [$name] $scn"
    [ -f "$wgsl" ] || fail "[$name] 缺场景资源 $wgsl"
    [ ! -e "$ROOT/assets/builtin/shaders/$stem" ] \
        || fail "[$name] 不得是内建 shader（assets/builtin/shaders/$stem 存在）"
    grep -q "render.ShaderPass" "$scn" || fail "[$name] 场景未挂 render.ShaderPass 组件"
    grep -q "${wgsl##*/}" "$scn" || fail "[$name] 场景未引用 $(basename "$wgsl")"
    grep -q "fs_main" "$wgsl" || fail "[$name] $wgsl 缺 fragment 入口 fs_main"

    # ① 离屏渲染：加载行 + 截图 + PPM 结构
    timeout 120 "$PLAYER" --shot "$d/f1.ppm" --frames 2 "$scn" >"$d/run1.txt" 2>&1 \
        || { cat "$d/run1.txt"; fail "[$name] 场景运行/截图失败（退出码非 0）"; }
    grep -qiE "shader pass.*${wgsl##*/}" "$d/run1.txt" \
        || { cat "$d/run1.txt"; fail "[$name] 缺 shader pass 加载行（.wgsl 未加载/编译失败）"; }
    if grep -qE "shader pass.*(失败|未找到|fail|error|missing|not found)" "$d/run1.txt"; then
        cat "$d/run1.txt"; fail "[$name] shader pass 加载/编译报错"
    fi
    grep -q "runtime: shot" "$d/run1.txt" || { cat "$d/run1.txt"; fail "[$name] 截图未写入"; }

    "$PY" - "$d/f1.ppm" "$top_check" <<'PY' || fail "[$name] PPM 结构断言失败"
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
# 颜色丰富度：5bit/通道量化后的唯一颜色数（平滑渐变应上千，纯色/故障画面只有个位数）
colors = set((px[i] >> 3, px[i + 1] >> 3, px[i + 2] >> 3) for i in range(0, len(px), 3))
# 非平坦像素：与全图均值亮度差 > 10（全屏 shader 应有大面积明暗结构，而非一整块近似常数）
nonflat = sum(1 for v in lums if abs(v - mean) > 10) / n
top_check = sys.argv[2] == '1'
top_m = bottom_m = 0.0
if top_check:
    # seascape：顶部 20%（天空）应明显亮于底部 20%（海面），照 Shadertoy 原图（y 翻转后）
    top = sum(lums[y * w + x] for y in range(h // 5) for x in range(0, w, 2))
    top_n = (h // 5) * len(range(0, w, 2))
    bottom = sum(lums[y * w + x] for y in range(h - h // 5, h) for x in range(0, w, 2))
    bottom_n = (h // 5) * len(range(0, w, 2))
    top_m, bottom_m = top / top_n, bottom / bottom_n
print(f'  [1] {w}x{h} colors={len(colors)} sd={sd:.1f} nonflat={nonflat:.3f}'
      + (f' top={top_m:.1f} bottom={bottom_m:.1f}' if top_check else ''))
ok = (len(colors) >= 200 and sd > 10.0 and nonflat > 0.5)
if top_check:
    ok = ok and top_m > bottom_m + 20.0
sys.exit(0 if ok else 1)
PY
    echo "shadertoy_e2e: [$name] ① 离屏全屏 shader（640x360 / 颜色多样 / 非平坦）OK"

    # ② 动画：第 1 帧 vs 第 60 帧 显著不同
    timeout 120 "$PLAYER" --shot "$d/f60.ppm" --shot-frame 60 "$scn" >"$d/run60.txt" 2>&1 \
        || { cat "$d/run60.txt"; fail "[$name] 第 60 帧截图失败"; }
    grep -q "runtime: shot" "$d/run60.txt" || { cat "$d/run60.txt"; fail "[$name] 第 60 帧未截图"; }
    "$PY" - "$d/f1.ppm" "$d/f60.ppm" <<'PY' || fail "[$name] 动画像素差不足（time 未推进进 shader）"
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
print(f'  [2] frame1 vs frame60 diff={diff} ({ratio:.3f})')
sys.exit(0 if ratio > 0.05 else 1)
PY
    echo "shadertoy_e2e: [$name] ② 动画（frame 1 vs 60 像素差 > 5%）OK"

    # ③ 确定性：同一帧跑两次，PPM 逐字节一致
    timeout 120 "$PLAYER" --shot "$d/f60b.ppm" --shot-frame 60 "$scn" >"$d/run60b.txt" 2>&1 \
        || { cat "$d/run60b.txt"; fail "[$name] 第 60 帧第二次截图失败"; }
    cmp -s "$d/f60.ppm" "$d/f60b.ppm" \
        || fail "[$name] 同一帧两次渲染不一致（确定性破坏：shader 通路不得依赖墙钟）"
    echo "shadertoy_e2e: [$name] ③ 确定性（同帧两次逐字节一致）OK"
    echo "shadertoy_e2e: [$name] ④ 场景资源 WGSL（scenes/shadertoy/，非内建 shader）OK"
}

# 主场景（CTest 参数）= seascape；mandelbulb 由 shaderport 并行产出，缺失时只 SKIP 该节。
check_scene seascape "$SCN" "$ROOT/scenes/shadertoy/seascape.wgsl" 1
if [ -f "$ROOT/scenes/shadertoy/mandelbulb.scn" ] && [ -f "$ROOT/scenes/shadertoy/mandelbulb.wgsl" ]; then
    check_scene mandelbulb "$ROOT/scenes/shadertoy/mandelbulb.scn" \
        "$ROOT/scenes/shadertoy/mandelbulb.wgsl" 0
else
    echo "shadertoy_e2e: SKIP mandelbulb（scenes/shadertoy/mandelbulb.scn/.wgsl 尚未入库）"
fi

echo "shadertoy_e2e: PASS"
