#!/usr/bin/env bash
# glTF / GLB **运行时直接加载**端到端（render.MeshRenderer.mesh_path → 模型文件）。
#
# 背景：runtime 用 C++ 直接解析 `.gltf` / `.glb`（不再依赖离线 .mesh.json）——
#   `mesh_path` 相对**场景目录**（运行时 VFS 根 = 场景目录），支持外部 .bin + 外部贴图、
#   data: URI、GLB 内嵌 BIN/bufferView 贴图；静态网格（多 primitive）+ 核心 PBR 材质/贴图。
#   首版不支持蒙皮/动画（夹具故意不含 skins/animations）。
#
# 覆盖（夹具 tests/data/gltf/runtime/，由 gen_fixtures.py 确定性生成）：
#   model.gltf         1 node / 1 mesh / 2 primitive / 2 material / 1 texture，
#                      buffer=外部 model.bin、贴图=外部 textures/checker.png（红白棋盘）
#                      左盒=贴图材质、右盒=纯蓝材质（颜色断言可区分「贴图/材质真的生效」）
#   model.glb          同几何/材质，BIN chunk 内嵌 buffer + PNG 贴图（image 用 bufferView）
#   model_data_uri.gltf 同几何/材质，buffer 与 image 都用 data: URI（内嵌 base64）
#
# ① `.gltf`：MAQUE_RENDER3D_INFO=1 + --shot → 日志 `runtime: gltf model ... primitives>=2
#    materials>=2 textures>=1 nodes=...`、`runtime: gltf texture ...`；PPM 结构（非背景占比/
#    中心有内容/顶部空/红白贴图区 + 蓝纯色区）——同时证明材质与贴图走了渲染路径。
# ② `.glb`：同样成功 + 同样的结构与材质断言（内嵌 BIN + bufferView 贴图）。
# ③ `.gltf` 与 `.glb` 两张 PPM **逐像素比较**（同几何/材质/相机，两个容器形式应等价；
#    max channel diff ≤ 2，余量只留给理论上的采样/舍入差异）。
# ④ 失败路径：复制场景 + model.gltf 到临时目录但**不给 model.bin** → 进程不崩
#    （日志含正常退出）、stderr 有 `runtime: gltf 加载失败 '<path>': <原因>`、无 ASan 报告、
#    截图像素不异常（非背景占比合理小，不会出现满屏错误色块）。
# ⑤ data: URI：模型能加载、贴图能从内嵌 base64 解出、渲染结果与 ① 的 .gltf PPM 一致（≤2）。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player 可执行文件；$2 = 夹具目录（含 model*.scn）；$3 = 工作目录
# 无显示环境可用：全程 `--shot` 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_e2e: skip（无 python3，无法解析 PPM/JSON）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN_GLTF="$FIXDIR/model_gltf.scn"
SCN_GLB="$FIXDIR/model_glb.scn"
SCN_DATA="$FIXDIR/model_data_uri.scn"
for f in "$FIXDIR/model.gltf" "$FIXDIR/model.bin" "$FIXDIR/model.glb" \
         "$FIXDIR/textures/checker.png" "$SCN_GLTF" "$SCN_GLB"; do
    [ -f "$f" ] || { echo "gltf_runtime_e2e: skip（缺夹具 $f）"; exit 77; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
# 清掉可能干扰离屏渲染的外部环境，并强制「无窗口 + 纯离屏」（CI/无显示环境可用）
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_ENV_IBL MAQUE_RENDER_SCALE \
      MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO MAQUE_WINDOW MAQUE_WINDOW_EVENT_AT \
      2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# PPM/日志检查器：所有阈值集中在这里（bash 里只做 grep 日志断言）。
cat >"$WORK/ppm_check.py" <<'PY'
import sys

BG_TOL = 25              # 与渲染清屏色的 L1 距离阈值（与 render3d_scene_e2e 一致）


def load(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{path}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{path}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


def measure(w, h, px):
    def rgb(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]
    bg = rgb(2, 2)
    diff = lambda c: abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2])
    nonbg = center = top = 0
    for y in range(h):
        for x in range(w):
            if diff(rgb(x, y)) > BG_TOL:
                nonbg += 1
                if w // 3 <= x < w * 2 // 3 and h // 3 <= y < h * 2 // 3:
                    center += 1
                if y < h // 10:
                    top += 1
    red = blue = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        # 红白棋盘（贴图）→ 明显红主导；纯蓝材质 → 明显蓝主导。
        # 暖白方向光下的白面实测 (213,213,215)（b 略高于 r），不会被误判成红/蓝。
        if r > b + 35 and r > 120:
            red += 1
        if b > r + 35 and b > 100:
            blue += 1
    return {"w": w, "h": h, "bg": bg,
            "nonbg": nonbg / (w * h), "center": center,
            "top": top / max(1, (h // 10) * w), "red": red, "blue": blue}


def cmd_structural(path, label, require_materials):
    w, h, px = load(path)
    assert (w, h) == (640, 360), f'{label}: 尺寸 {w}x{h} != 设计分辨率 640x360'
    m = measure(w, h, px)
    print(f"gltf_runtime_e2e: {label} {w}x{h} bg={m['bg']} nonbg={m['nonbg']:.3f} "
          f"center={m['center']} top={m['top']:.3f} red={m['red']} blue={m['blue']}")
    ok = 0.05 < m['nonbg'] < 0.85 and m['center'] > 500 and m['top'] < 0.05
    if require_materials:
        # 贴图（红白）+ 纯蓝材质都出现 → 材质数组与 baseColorTexture 真的进了渲染。
        ok = ok and m['red'] > 1000 and m['blue'] > 1000
    if not ok:
        print(f'gltf_runtime_e2e: FAIL {label}: PPM 结构断言不满足', file=sys.stderr)
        return 1
    return 0


def cmd_failure(path):
    w, h, px = load(path)
    assert (w, h) == (640, 360), f'失败路径: 尺寸 {w}x{h} != 640x360'
    m = measure(w, h, px)
    print(f"gltf_runtime_e2e: 失败路径截图 {w}x{h} bg={m['bg']} nonbg={m['nonbg']:.3f} "
          f"center={m['center']}")
    # 允许"回落内建 cube"或"什么都不画"，但不允许异常大块内容（满屏错误色）。
    if m['nonbg'] >= 0.6:
        print(f"gltf_runtime_e2e: FAIL 失败路径像素异常（nonbg={m['nonbg']:.3f} >= 0.6）",
              file=sys.stderr)
        return 1
    return 0


def cmd_diff(a_path, b_path, limit):
    wa, ha, a = load(a_path)
    wb, hb, b = load(b_path)
    assert (wa, ha) == (wb, hb), f'两图尺寸不同: {wa}x{ha} vs {wb}x{hb}'
    md = max(abs(a[i] - b[i]) for i in range(len(a)))
    nd = sum(1 for i in range(0, len(a), 3)
             if abs(a[i] - b[i]) > limit or abs(a[i + 1] - b[i + 1]) > limit
             or abs(a[i + 2] - b[i + 2]) > limit)
    print(f'gltf_runtime_e2e: 像素比较 {wa}x{ha} max_channel_diff={md} '
          f'px_over_{limit}={nd}')
    if md > limit:
        print(f'gltf_runtime_e2e: FAIL 两图差异超限（max={md} > {limit}）', file=sys.stderr)
        return 1
    return 0


def main():
    cmd = sys.argv[1]
    if cmd == 'structural':
        return cmd_structural(sys.argv[2], sys.argv[3], sys.argv[4] == '1')
    if cmd == 'failure':
        return cmd_failure(sys.argv[2])
    if cmd == 'diff':
        return cmd_diff(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    raise SystemExit(f'未知模式 {cmd}')


if __name__ == '__main__':
    sys.exit(main())
PY
CHECK() { "$PY" "$WORK/ppm_check.py" "$@"; }

# ---- 公共断言：成功加载一个模型 ----
check_model_log() {   # check_model_log <log> <label>
    local log="$1" label="$2"
    # 契约：runtime: gltf model '<path>' primitives=<N> materials=<M> textures=<K> nodes=<T>
    # （引号形式不写死，只锁定字段顺序与最小计数）
    grep -Eq "runtime: gltf model .*primitives=([2-9]|[1-9][0-9]+).*materials=([2-9]|[1-9][0-9]+).*textures=[1-9][0-9]*.*nodes=[0-9]+" "$log" \
        || { cat "$log"; fail "$label: 缺模型加载成功日志（期望 primitives>=2 materials>=2 textures>=1 nodes=*）"; }
    # 贴图（外部 PNG / GLB bufferView / data URI）必须走到 image → create_texture
    grep -q "runtime: gltf texture" "$log" \
        || { cat "$log"; fail "$label: 贴图未加载（缺 runtime: gltf texture 日志）"; }
    # 成功路径不允许出现加载失败回退
    if grep -q "gltf 加载失败" "$log"; then
        cat "$log"; fail "$label: 成功路径出现 gltf 加载失败"
    fi
    if grep -q "AddressSanitizer" "$log"; then
        cat "$log"; fail "$label: ASan 报告（越界/崩溃）"
    fi
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "$label: 截图未写入"; }
}

run_shot() {   # run_shot <scene> <out.ppm> <log>
    timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$2" --frames 2 "$1" >"$3" 2>&1
}

# ---- ① .gltf（外部 model.bin + 外部 textures/checker.png）----
run_shot "$SCN_GLTF" "$WORK/gltf.ppm" "$WORK/gltf.log" \
    || { cat "$WORK/gltf.log"; fail "① .gltf 渲染失败"; }
check_model_log "$WORK/gltf.log" "① .gltf"
CHECK structural "$WORK/gltf.ppm" "① .gltf" 1 || exit 1
echo "gltf_runtime_e2e: ① .gltf（外部 bin + 外部贴图）加载/渲染 OK"

# ---- ② .glb（BIN chunk 内嵌 buffer + bufferView 贴图）----
run_shot "$SCN_GLB" "$WORK/glb.ppm" "$WORK/glb.log" \
    || { cat "$WORK/glb.log"; fail "② .glb 渲染失败"; }
check_model_log "$WORK/glb.log" "② .glb"
CHECK structural "$WORK/glb.ppm" "② .glb" 1 || exit 1
echo "gltf_runtime_e2e: ② .glb（内嵌 BIN + bufferView 贴图）加载/渲染 OK"

# ---- ③ .gltf 与 .glb 逐像素比较 ----
# 同一份几何/材质/相机，只是容器形式不同：两条输入路径必须产生同一张图。
# 阈值 2 只留理论上的采样/舍入余量（实测通常为 0）。
CHECK diff "$WORK/gltf.ppm" "$WORK/glb.ppm" 2 || exit 1
echo "gltf_runtime_e2e: ③ .gltf vs .glb 逐像素一致 OK"

# ---- ⑤ data: URI（buffer + image 都内嵌 base64）----
if [ -f "$SCN_DATA" ] && [ -f "$FIXDIR/model_data_uri.gltf" ]; then
    run_shot "$SCN_DATA" "$WORK/data.ppm" "$WORK/data.log" \
        || { cat "$WORK/data.log"; fail "⑤ data: URI 渲染失败"; }
    check_model_log "$WORK/data.log" "⑤ data: URI"
    CHECK structural "$WORK/data.ppm" "⑤ data: URI" 1 || exit 1
    CHECK diff "$WORK/gltf.ppm" "$WORK/data.ppm" 2 || exit 1
    echo "gltf_runtime_e2e: ⑤ data: URI（内嵌 buffer/贴图）加载/渲染 OK"
fi

# ---- ④ 失败路径：缺 model.bin ----
# 复制场景 + glTF 到临时目录（VFS 根 = 该目录），但**不放 model.bin / textures**。
# 期望：加载失败有明确 stderr 日志、进程不崩（正常跑完 2 帧退出）、截图仍是合法 PPM。
mkdir -p "$WORK/broken"
cp "$FIXDIR/model.gltf" "$WORK/broken/"
cp "$SCN_GLTF" "$WORK/broken/"
rm -f "$WORK/broken/model.bin"
timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/broken.ppm" --frames 2 \
    "$WORK/broken/model_gltf.scn" >"$WORK/broken.log" 2>&1
# 不写 `|| fail`：失败路径允许非 0 退出码，但必须能走到正常结束（崩溃 = 没有 exited after 行）。
grep -q "runtime: gltf 加载失败" "$WORK/broken.log" \
    || { cat "$WORK/broken.log"; fail "④ 缺 .bin 时没有报 'runtime: gltf 加载失败'"; }
grep -q "exited after 2 frames" "$WORK/broken.log" \
    || { cat "$WORK/broken.log"; fail "④ 缺 .bin 时进程未走到正常退出（疑似崩溃）"; }
if grep -q "AddressSanitizer" "$WORK/broken.log"; then
    cat "$WORK/broken.log"; fail "④ 缺 .bin 触发 ASan（崩溃/越界）"
fi
[ -s "$WORK/broken.ppm" ] || { cat "$WORK/broken.log"; fail "④ 缺 .bin 时未写出截图"; }
CHECK failure "$WORK/broken.ppm" || exit 1
echo "gltf_runtime_e2e: ④ 缺 .bin 失败路径（不崩/报错/像素正常）OK"

echo "gltf_runtime_e2e: PASS"
