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

# ⑤ shader 精灵（render.ShaderSprite，docs/13）：节点矩形里跑场景 .wgsl（同一份 WGSL 契约）。
#    `sprite-shader.scn` 左右并排两个精灵（左 mandelbulb / 右 seascape）：
#    · 两条 "shader sprite ... loaded" 日志（各自一张离屏纹理）；
#    · 640x360 离屏：左右两块内部区域颜色多样/非平坦，且**左暖右冷**（是两个不同作品）；
#    · 两个精灵之间的中缝是场景背景色（精灵只画自己的矩形，不铺满整屏）；
#    · 动画（frame 1 vs 60）+ 同帧两次逐字节一致（确定性）。
check_sprite_scene() {
    local scn="$ROOT/scenes/shadertoy/sprite-shader.scn"
    local d="$WORK/sprite"
    [ -f "$scn" ] || { echo "shadertoy_e2e: SKIP sprite-shader（场景未入库）"; return 0; }
    mkdir -p "$d"
    echo "shadertoy_e2e: ---- [sprite] $scn"
    timeout 120 "$PLAYER" --shot "$d/f1.ppm" --frames 2 "$scn" >"$d/run1.txt" 2>&1 \
        || { cat "$d/run1.txt"; fail "[sprite] 场景运行/截图失败（退出码非 0）"; }
    local loaded
    loaded="$(grep -c "shader sprite .* loaded" "$d/run1.txt" || true)"
    [ "${loaded:-0}" -ge 2 ] || { cat "$d/run1.txt"; fail "[sprite] 应加载 2 个 shader 精灵，实际 $loaded"; }
    grep -qiE "shader sprite.*(失败|未找到|error|missing|not found)" "$d/run1.txt" \
        && { cat "$d/run1.txt"; fail "[sprite] shader 精灵加载/编译报错"; }
    timeout 120 "$PLAYER" --shot "$d/f60.ppm" --shot-frame 60 "$scn" >"$d/run60.txt" 2>&1 \
        || { cat "$d/run60.txt"; fail "[sprite] 第 60 帧截图失败"; }
    timeout 120 "$PLAYER" --shot "$d/f60b.ppm" --shot-frame 60 "$scn" >"$d/run60b.txt" 2>&1 \
        || { cat "$d/run60b.txt"; fail "[sprite] 第 60 帧第二次截图失败"; }
    cmp -s "$d/f60.ppm" "$d/f60b.ppm" \
        || fail "[sprite] 同一帧两次渲染不一致（确定性破坏）"

    "$PY" - "$d/f1.ppm" "$d/f60.ppm" <<'PY' || fail "[sprite] PPM 结构断言失败"
import sys
def load(p):
    d = open(p, 'rb').read()
    magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6', magic
    w, h = map(int, dims.split())
    assert (w, h) == (640, 360), (w, h)
    return w, h, px
w, h, a = load(sys.argv[1])
_, _, b = load(sys.argv[2])
def rgb(px, x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]
def block(px, x0, x1):
    return [rgb(px, x, y) for y in range(20, h - 20, 6) for x in range(x0, x1, 6)]
def stats(pix):
    n = len(pix)
    lum = [0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] for c in pix]
    m = sum(lum) / n
    sd = (sum((v - m) ** 2 for v in lum) / n) ** 0.5
    colors = len({(c[0] >> 3, c[1] >> 3, c[2] >> 3) for c in pix})
    r = sum(c[0] for c in pix) / n
    bl = sum(c[2] for c in pix) / n
    return m, sd, colors, r, bl
lm, lsd, lc, lr, lb = stats(block(a, 20, 290))     # 左精灵内部
rm, rsd, rc, rr, rb = stats(block(a, 350, 620))    # 右精灵内部
gap = rgb(a, 320, 180)                             # 两精灵之间的中缝
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
anim = diff / (w * h)
print(f'  [sprite] left(m,b)={lm:.1f},{lsd:.1f} colors={lc} warm={lr - lb:+.1f} | '
      f'right(m,sd)={rm:.1f},{rsd:.1f} colors={rc} cool={rb - rr:+.1f} | gap={gap} anim={anim:.3f}')
ok = (lc >= 100 and rc >= 100 and lsd > 10.0 and rsd > 10.0)
ok = ok and (lr - lb) > 10.0 and (rb - rr) > 10.0          # 左暖（fractal）右冷（海面）
ok = ok and max(gap) < 40                                  # 中缝 = 场景背景色，未被精灵覆盖
ok = ok and anim > 0.05                                    # time 真的推进进两个 shader
sys.exit(0 if ok else 1)
PY
    echo "shadertoy_e2e: [sprite] 左右两个 shader 精灵（mandelbulb / seascape）+ 动画 + 确定性 OK"
}

# ⑥ 多通道 shader 图（render.ShaderGraph，docs/13 §多通道 shader）：
#    `mountains.scn` = 960x540，program.json 声明 3 个中间缓冲 + 4 个阶段
#    （buffer_a 自反馈 → buffer_b 侵蚀 → buffer_c 噪声 → image 主目标）。
#    断言：加载行 + 4 个阶段无报错 / 960x540 结构（颜色多样、非平坦、天空亮于底部）/
#    frame 1 vs 120 差异 / 同帧逐字节确定性 / 阶段与 program 都是场景资源（非内建 shader）/
#    鼠标输入真的写进缓冲（MAQUE_SIM 按住拖动 vs 不注入，同一帧可见差异）。
check_graph_scene() {
    local scn="$ROOT/scenes/shadertoy/mountains.scn"
    local prog="$ROOT/scenes/shadertoy/mountains/program.json"
    local d="$WORK/graph"
    [ -f "$scn" ] || { echo "shadertoy_e2e: SKIP mountains（场景未入库）"; return 0; }
    mkdir -p "$d"
    echo "shadertoy_e2e: ---- [graph] $scn"
    [ -f "$prog" ] || fail "[graph] 缺 $prog"
    [ ! -e "$ROOT/assets/builtin/shaders/mountains" ] \
        || fail "[graph] 不得是内建 shader（assets/builtin/shaders/mountains 存在）"
    grep -q "render.ShaderGraph" "$scn" || fail "[graph] 场景未挂 render.ShaderGraph 组件"
    grep -q "mountains/program.json" "$scn" || fail "[graph] 场景未引用 mountains/program.json"
    [ -f "$ROOT/scenes/shadertoy/mountains/common.wgsl" ] || fail "[graph] 缺共享源码 common.wgsl"
    grep -q "common.wgsl" "$prog" || fail "[graph] program.json 未引用 common.wgsl"
    for f in buffer_a buffer_b buffer_c image; do
        [ -f "$ROOT/scenes/shadertoy/mountains/$f.wgsl" ] || fail "[graph] 缺阶段源码 $f.wgsl"
        grep -q "fs_main" "$ROOT/scenes/shadertoy/mountains/$f.wgsl" \
            || fail "[graph] $f.wgsl 缺 fragment 入口 fs_main"
        grep -q "$f.wgsl" "$prog" || fail "[graph] program.json 未引用 $f.wgsl"
    done

    # ① 离屏渲染：加载行 + 4 个阶段 + 截图
    timeout 180 "$PLAYER" --shot "$d/f1.ppm" --frames 2 "$scn" >"$d/run1.txt" 2>&1 \
        || { cat "$d/run1.txt"; fail "[graph] 场景运行/截图失败（退出码非 0）"; }
    grep -q "shader graph .*mountains/program.json.*passes=4" "$d/run1.txt" \
        || { cat "$d/run1.txt"; fail "[graph] 缺 shader graph 加载行 / 阶段数不是 4"; }
    if grep -qE "shader graph.*(失败|未找到|编译失败|error|missing)" "$d/run1.txt"; then
        cat "$d/run1.txt"; fail "[graph] 阶段加载/编译报错"
    fi
    grep -q "runtime: shot" "$d/run1.txt" || { cat "$d/run1.txt"; fail "[graph] 截图未写入"; }

    "$PY" - "$d/f1.ppm" <<'PY' || fail "[graph] PPM 结构断言失败"
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
assert (w, h) == (960, 540), (w, h)
n = w * h
def lum(i):
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]
lums = [lum(i) for i in range(0, len(px), 3)]
mean = sum(lums) / n
sd = (sum((v - mean) ** 2 for v in lums) / n) ** 0.5
colors = set((px[i] >> 3, px[i + 1] >> 3, px[i + 2] >> 3) for i in range(0, len(px), 3))
nonflat = sum(1 for v in lums if abs(v - mean) > 10) / n
top = sum(lums[y * w + x] for y in range(h // 6) for x in range(0, w, 2))
top_n = (h // 6) * len(range(0, w, 2))
bottom = sum(lums[y * w + x] for y in range(h - h // 6, h) for x in range(0, w, 2))
bottom_n = (h // 6) * len(range(0, w, 2))
top_m, bottom_m = top / top_n, bottom / bottom_n
print(f'  [graph] {w}x{h} colors={len(colors)} sd={sd:.1f} nonflat={nonflat:.3f} '
      f'top={top_m:.1f} bottom={bottom_m:.1f}')
ok = (len(colors) >= 300 and sd > 10.0 and nonflat > 0.5)
ok = ok and top_m > bottom_m + 15.0        # 顶部天空亮于底部海面/岩层
sys.exit(0 if ok else 1)
PY
    echo "shadertoy_e2e: [graph] ① 离屏 4 阶段（960x540 / 颜色多样 / 非平坦 / 天空更亮）OK"

    # ② 动画：相机自转（frame 1 vs 120）
    timeout 180 "$PLAYER" --shot "$d/f120.ppm" --shot-frame 120 "$scn" >"$d/run120.txt" 2>&1 \
        || { cat "$d/run120.txt"; fail "[graph] 第 120 帧截图失败"; }
    "$PY" - "$d/f1.ppm" "$d/f120.ppm" <<'PY' || fail "[graph] 动画像素差不足（time 未推进）"
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
ratio = diff / (w * h)
print(f'  [graph] frame1 vs frame120 diff={diff} ({ratio:.3f})')
sys.exit(0 if ratio > 0.05 else 1)
PY
    echo "shadertoy_e2e: [graph] ② 动画（相机自转，frame 1 vs 120）OK"

    # ③ 确定性：同一帧跑两次逐字节一致
    timeout 180 "$PLAYER" --shot "$d/f120b.ppm" --shot-frame 120 "$scn" >"$d/run120b.txt" 2>&1 \
        || { cat "$d/run120b.txt"; fail "[graph] 第 120 帧第二次截图失败"; }
    cmp -s "$d/f120.ppm" "$d/f120b.ppm" \
        || fail "[graph] 同一帧两次渲染不一致（确定性破坏：多阶段/自反馈不得依赖墙钟）"
    echo "shadertoy_e2e: [graph] ③ 确定性（同帧两次逐字节一致）OK"

    # ④ 指针通路：MAQUE_SIM 按住拖动 → 与不注入的同一帧有可见差异
    #    （验证 Buffer A 的自反馈 + uniform 里的屏幕鼠标坐标真的写进中间缓冲）
    local sim="$ROOT/tests/data/sim/mountains_paint.sim.json"
    if [ -f "$sim" ]; then
        MAQUE_SIM="$sim" timeout 180 "$PLAYER" --shot "$d/paint.ppm" --shot-frame 60 "$scn" \
            >"$d/paint.txt" 2>&1 || { cat "$d/paint.txt"; fail "[graph] 注入鼠标后运行失败"; }
        timeout 180 "$PLAYER" --shot "$d/nopaint.ppm" --shot-frame 60 "$scn" >"$d/nopaint.txt" 2>&1 \
            || { cat "$d/nopaint.txt"; fail "[graph] 基准帧截图失败"; }
        "$PY" - "$d/paint.ppm" "$d/nopaint.ppm" <<'PY' || fail "[graph] 鼠标绘制没有改变画面（自反馈/指针通路未生效）"
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
ratio = diff / (w * h)
print(f'  [graph] paint vs base diff={diff} ({ratio:.4f})')
sys.exit(0 if ratio > 0.002 else 1)
PY
        echo "shadertoy_e2e: [graph] ④ 鼠标绘制（自反馈缓冲 + 指针 uniform）OK"
    else
        echo "shadertoy_e2e: SKIP [graph] 鼠标绘制（缺 $sim）"
    fi
    echo "shadertoy_e2e: [graph] ⑤ 场景资源（program.json + 4 个 .wgsl，非内建 shader）OK"
}

# ⑦ 多通道 shader 图的精灵形态：`render.ShaderSprite.shader` 直接指向 program JSON
#    （与 .wgsl 共用同一个字段；引擎按扩展名判定）。`mountain-shader-sprite.scn` =
#    单个实体、720x405 精灵，背后是场景背景色。
#    断言：加载行走的是 "shader sprite graph" 分支 / 960x540 结构（精灵内颜色多样、四角为背景色、
#    中心与角落不同）/ 精灵内动画 / 同帧逐字节确定性 / 鼠标在精灵矩形内绘制会改变画面。
check_graph_sprite_scene() {
    local scn="$ROOT/scenes/shadertoy/mountain-shader-sprite.scn"
    local d="$WORK/graph_sprite"
    [ -f "$scn" ] || { echo "shadertoy_e2e: SKIP graph-sprite（场景未入库）"; return 0; }
    mkdir -p "$d"
    echo "shadertoy_e2e: ---- [graph-sprite] $scn"
    grep -q "render.ShaderSprite" "$scn" || fail "[graph-sprite] 场景未挂 render.ShaderSprite"
    grep -q '"shader": *"mountains/program.json"' "$scn" \
        || fail "[graph-sprite] ShaderSprite.shader 未指向 program.json"
    ! grep -q "render.ShaderGraph" "$scn" \
        || fail "[graph-sprite] 精灵形态不应再需要 render.ShaderGraph 组件"

    timeout 180 "$PLAYER" --shot "$d/f1.ppm" --frames 2 "$scn" >"$d/run1.txt" 2>&1 \
        || { cat "$d/run1.txt"; fail "[graph-sprite] 场景运行/截图失败（退出码非 0）"; }
    grep -q "shader sprite graph .*mountains/program.json.*passes=4" "$d/run1.txt" \
        || { cat "$d/run1.txt"; fail "[graph-sprite] 缺 'shader sprite graph' 加载行 / 阶段数不是 4"; }
    if grep -qE "shader graph.*(失败|未找到|编译失败|error|missing)" "$d/run1.txt"; then
        cat "$d/run1.txt"; fail "[graph-sprite] 阶段加载/编译报错"
    fi
    if grep -qE "shader 图精灵绘制失败" "$d/run1.txt"; then
        cat "$d/run1.txt"; fail "[graph-sprite] 精灵离屏渲染失败"
    fi
    if grep -qE "shader sprite .*编译失败" "$d/run1.txt"; then
        cat "$d/run1.txt"; fail "[graph-sprite] program JSON 被误当成 WGSL 编译"
    fi

    "$PY" - "$d/f1.ppm" <<'PY' || fail "[graph-sprite] PPM 结构断言失败"
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
assert (w, h) == (960, 540), (w, h)
def rgb(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]
corner = rgb(8, 8)
# 精灵矩形 = 960x540 里居中的 720x405：x∈[120,840] y∈[67.5,472.5]
inner = [rgb(x, y) for y in range(120, 420, 6) for x in range(220, 740, 6)]
colors = len({(c[0] >> 3, c[1] >> 3, c[2] >> 3) for c in inner})
lum = [0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] for c in inner]
m = sum(lum) / len(lum)
sd = (sum((v - m) ** 2 for v in lum) / len(lum)) ** 0.5
center = rgb(480, 270)
print(f'  [graph-sprite] corner={corner} center={center} colors={colors} sd={sd:.1f} mean={m:.1f}')
ok = colors >= 200 and sd > 10.0
ok = ok and max(corner) < 60                       # 角落 = 场景背景色，未被精灵覆盖
ok = ok and abs(center[0] - corner[0]) + abs(center[1] - corner[1]) + abs(center[2] - corner[2]) > 60
sys.exit(0 if ok else 1)
PY
    echo "shadertoy_e2e: [graph-sprite] ① 离屏精灵（720x405 矩形 / 背景色保留 / 颜色多样）OK"

    # ② 精灵内动画 + ③ 确定性
    timeout 180 "$PLAYER" --shot "$d/f120.ppm" --shot-frame 120 "$scn" >"$d/run120.txt" 2>&1 \
        || { cat "$d/run120.txt"; fail "[graph-sprite] 第 120 帧截图失败"; }
    timeout 180 "$PLAYER" --shot "$d/f120b.ppm" --shot-frame 120 "$scn" >"$d/run120b.txt" 2>&1 \
        || { cat "$d/run120b.txt"; fail "[graph-sprite] 第 120 帧第二次截图失败"; }
    cmp -s "$d/f120.ppm" "$d/f120b.ppm" \
        || fail "[graph-sprite] 同一帧两次渲染不一致（确定性破坏）"
    "$PY" - "$d/f1.ppm" "$d/f120.ppm" <<'PY' || fail "[graph-sprite] 精灵内动画不足（time 未推进）"
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = 0; total = 0
for y in range(120, 420, 2):
    for x in range(220, 740, 2):
        i = (y * w + x) * 3
        total += 1
        if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12:
            diff += 1
ratio = diff / total
print(f'  [graph-sprite] frame1 vs frame120 inner diff={ratio:.3f}')
sys.exit(0 if ratio > 0.05 else 1)
PY
    echo "shadertoy_e2e: [graph-sprite] ② 精灵内动画 + ③ 确定性 OK"

    # ④ 鼠标在精灵矩形内绘制（复用 mountains 的绘制脚本；世界坐标在精灵范围内）
    local sim="$ROOT/tests/data/sim/mountains_paint.sim.json"
    if [ -f "$sim" ]; then
        MAQUE_SIM="$sim" timeout 180 "$PLAYER" --shot "$d/paint.ppm" --shot-frame 60 "$scn" \
            >"$d/paint.txt" 2>&1 || { cat "$d/paint.txt"; fail "[graph-sprite] 注入鼠标后运行失败"; }
        timeout 180 "$PLAYER" --shot "$d/nopaint.ppm" --shot-frame 60 "$scn" >"$d/nopaint.txt" 2>&1 \
            || { cat "$d/nopaint.txt"; fail "[graph-sprite] 基准帧截图失败"; }
        "$PY" - "$d/paint.ppm" "$d/nopaint.ppm" <<'PY' || fail "[graph-sprite] 精灵内鼠标绘制未生效"
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
ratio = diff / (w * h)
print(f'  [graph-sprite] paint vs base diff={diff} ({ratio:.4f})')
sys.exit(0 if ratio > 0.002 else 1)
PY
        echo "shadertoy_e2e: [graph-sprite] ④ 精灵内鼠标绘制（局部像素 + 自反馈）OK"
    else
        echo "shadertoy_e2e: SKIP [graph-sprite] 鼠标绘制（缺 $sim）"
    fi
    echo "shadertoy_e2e: [graph-sprite] ⑤ 精灵形态 = ShaderSprite.shader 指向 program.json OK"
}

# ⑧ quartz cave（render.ShaderGraph，5 阶段 / 4 缓冲，其中 3 个是**非方形**缓冲）：
#    `quartz_cave.scn` = 960x540，program.json 声明
#      state 1x6（拖拽状态，自反馈）→ scene 960x540（HDR 渲染）→ cells 64x24（光照单元）
#      → centres 6x1（最亮光心）→ image（tone map + 星芒）。
#    断言：加载行 + 5 阶段无报错 / 960x540 结构（颜色多样、非平坦、顶部暗于底部：洞顶暗、
#    岩石地面亮）/ frame 1 vs 120 有动画 / 同帧逐字节确定性 /
#    鼠标拖拽真的通过自反馈缓冲改变了视角（MAQUE_SIM 按下拖动 vs 不注入）/
#    阶段与 program 都是场景资源（非内建 shader）。
#    这节同时是「非方形中间缓冲（width/height）」的唯一回归。
check_quartz_scene() {
    local scn="$ROOT/scenes/shadertoy/quartz_cave.scn"
    local prog="$ROOT/scenes/shadertoy/quartz/program.json"
    local d="$WORK/quartz"
    [ -f "$scn" ] || { echo "shadertoy_e2e: SKIP quartz（场景未入库）"; return 0; }
    mkdir -p "$d"
    echo "shadertoy_e2e: ---- [quartz] $scn"
    [ -f "$prog" ] || fail "[quartz] 缺 $prog"
    [ ! -e "$ROOT/assets/builtin/shaders/quartz" ] \
        || fail "[quartz] 不得是内建 shader（assets/builtin/shaders/quartz 存在）"
    grep -q "render.ShaderGraph" "$scn" || fail "[quartz] 场景未挂 render.ShaderGraph 组件"
    grep -q "quartz/program.json" "$scn" || fail "[quartz] 场景未引用 quartz/program.json"
    [ -f "$ROOT/scenes/shadertoy/quartz/common.wgsl" ] || fail "[quartz] 缺共享源码 common.wgsl"
    grep -q "common.wgsl" "$prog" || fail "[quartz] program.json 未引用 common.wgsl"
    grep -q '"width"' "$prog" || fail "[quartz] program.json 未使用非方形缓冲（width/height）"
    for f in buffer_a buffer_b buffer_c buffer_d image; do
        [ -f "$ROOT/scenes/shadertoy/quartz/$f.wgsl" ] || fail "[quartz] 缺阶段源码 $f.wgsl"
        grep -q "fs_main" "$ROOT/scenes/shadertoy/quartz/$f.wgsl" \
            || fail "[quartz] $f.wgsl 缺 fragment 入口 fs_main"
        grep -q "$f.wgsl" "$prog" || fail "[quartz] program.json 未引用 $f.wgsl"
    done

    # ① 离屏渲染：加载行 + 5 个阶段 + 截图
    timeout 240 "$PLAYER" --shot "$d/f1.ppm" --frames 2 "$scn" >"$d/run1.txt" 2>&1 \
        || { cat "$d/run1.txt"; fail "[quartz] 场景运行/截图失败（退出码非 0）"; }
    grep -q "shader graph .*quartz/program.json.*passes=5.*buffers=4" "$d/run1.txt" \
        || { cat "$d/run1.txt"; fail "[quartz] 缺 shader graph 加载行 / 阶段或缓冲数不对（应 5 阶段 4 缓冲）"; }
    if grep -qE "shader graph.*(失败|未找到|编译失败|error|missing)" "$d/run1.txt"; then
        cat "$d/run1.txt"; fail "[quartz] 阶段加载/编译报错"
    fi
    grep -q "runtime: shot" "$d/run1.txt" || { cat "$d/run1.txt"; fail "[quartz] 截图未写入"; }

    "$PY" - "$d/f1.ppm" <<'PY' || fail "[quartz] PPM 结构断言失败"
import sys
d = open(sys.argv[1], 'rb').read()
magic, dims, _max, px = d.split(b'\n', 3)
assert magic == b'P6', magic
w, h = map(int, dims.split())
assert (w, h) == (960, 540), (w, h)
n = w * h
def lum(i):
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]
lums = [lum(i) for i in range(0, len(px), 3)]
mean = sum(lums) / n
sd = (sum((v - mean) ** 2 for v in lums) / n) ** 0.5
colors = set((px[i] >> 3, px[i + 1] >> 3, px[i + 2] >> 3) for i in range(0, len(px), 3))
nonflat = sum(1 for v in lums if abs(v - mean) > 10) / n
# 洞顶（上 1/6）暗、湿岩地面（下 1/6）亮：晶体簇从暗洞顶里被光柱照亮
top = sum(lums[y * w + x] for y in range(h // 6) for x in range(0, w, 2))
top_n = (h // 6) * len(range(0, w, 2))
bottom = sum(lums[y * w + x] for y in range(h - h // 6, h) for x in range(0, w, 2))
bottom_n = (h // 6) * len(range(0, w, 2))
top_m, bottom_m = top / top_n, bottom / bottom_n
print(f'  [quartz] {w}x{h} colors={len(colors)} sd={sd:.1f} nonflat={nonflat:.3f} '
      f'top={top_m:.1f} bottom={bottom_m:.1f}')
ok = (len(colors) >= 300 and sd > 10.0 and nonflat > 0.5)
ok = ok and top_m < bottom_m - 5.0
sys.exit(0 if ok else 1)
PY
    echo "shadertoy_e2e: [quartz] ① 离屏 5 阶段（960x540 / 颜色多样 / 非平坦 / 洞顶暗于地面）OK"

    # ② 动画：太阳漂移 + 尘埃（frame 1 vs 120）
    timeout 240 "$PLAYER" --shot "$d/f120.ppm" --shot-frame 120 "$scn" >"$d/run120.txt" 2>&1 \
        || { cat "$d/run120.txt"; fail "[quartz] 第 120 帧截图失败"; }
    "$PY" - "$d/f1.ppm" "$d/f120.ppm" <<'PY' || fail "[quartz] 动画像素差不足（time 未推进）"
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
ratio = diff / (w * h)
print(f'  [quartz] frame1 vs frame120 diff={diff} ({ratio:.3f})')
sys.exit(0 if ratio > 0.05 else 1)
PY
    echo "shadertoy_e2e: [quartz] ② 动画（太阳漂移 + 尘埃，frame 1 vs 120）OK"

    # ③ 确定性：同一帧跑两次逐字节一致
    timeout 240 "$PLAYER" --shot "$d/f120b.ppm" --shot-frame 120 "$scn" >"$d/run120b.txt" 2>&1 \
        || { cat "$d/run120b.txt"; fail "[quartz] 第 120 帧第二次截图失败"; }
    cmp -s "$d/f120.ppm" "$d/f120b.ppm" \
        || fail "[quartz] 同一帧两次渲染不一致（确定性破坏：多阶段/自反馈不得依赖墙钟）"
    echo "shadertoy_e2e: [quartz] ③ 确定性（同帧两次逐字节一致）OK"

    # ④ 指针通路：MAQUE_SIM 拖拽 → 相机绕着晶体转，画面必须有可见差异
    #    （验证 state 缓冲的自反馈：拖拽偏移跨帧保留、image 复用它）
    local sim="$ROOT/tests/data/sim/quartz_orbit.sim.json"
    if [ -f "$sim" ]; then
        MAQUE_SIM="$sim" timeout 240 "$PLAYER" --shot "$d/orbit.ppm" --shot-frame 70 "$scn" \
            >"$d/orbit.txt" 2>&1 || { cat "$d/orbit.txt"; fail "[quartz] 注入拖拽后运行失败"; }
        timeout 240 "$PLAYER" --shot "$d/base.ppm" --shot-frame 70 "$scn" >"$d/base.txt" 2>&1 \
            || { cat "$d/base.txt"; fail "[quartz] 基准帧截图失败"; }
        "$PY" - "$d/orbit.ppm" "$d/base.ppm" <<'PY' || fail "[quartz] 拖拽没有改变视角（自反馈/指针通路未生效）"
import sys
def load(p):
    d = open(p, 'rb').read(); magic, dims, _m, px = d.split(b'\n', 3)
    assert magic == b'P6'
    w, h = map(int, dims.split())
    return w, h, px
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
diff = sum(1 for i in range(0, len(a), 3)
           if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12)
ratio = diff / (w * h)
print(f'  [quartz] orbit vs base diff={diff} ({ratio:.3f})')
sys.exit(0 if ratio > 0.2 else 1)
PY
        echo "shadertoy_e2e: [quartz] ④ 拖拽环绕（state 自反馈缓冲 + 指针 uniform）OK"
    else
        echo "shadertoy_e2e: SKIP [quartz] 拖拽环绕（缺 $sim）"
    fi
    echo "shadertoy_e2e: [quartz] ⑤ 场景资源（program.json + 5 个 .wgsl，非内建 shader）OK"
}

# 主场景（CTest 参数）= seascape；mandelbulb 由 shaderport 并行产出，缺失时只 SKIP 该节。
check_scene seascape "$SCN" "$ROOT/scenes/shadertoy/seascape.wgsl" 1
if [ -f "$ROOT/scenes/shadertoy/mandelbulb.scn" ] && [ -f "$ROOT/scenes/shadertoy/mandelbulb.wgsl" ]; then
    check_scene mandelbulb "$ROOT/scenes/shadertoy/mandelbulb.scn" \
        "$ROOT/scenes/shadertoy/mandelbulb.wgsl" 0
else
    echo "shadertoy_e2e: SKIP mandelbulb（scenes/shadertoy/mandelbulb.scn/.wgsl 尚未入库）"
fi
check_sprite_scene
check_graph_scene
check_graph_sprite_scene
check_quartz_scene

echo "shadertoy_e2e: PASS"
