#!/usr/bin/env bash
# PBR/迁移渲染回归（docs/31 §11 P3）：把「参考图对比」沉淀成可重复的断言，防渲染缺陷回潮。
#   ① 迁移基线数据完整性 + 关键模型 compare.json 指标下限（无需 GPU）；
#   ② Normal oracle / 镜像 front-face oracle（绝对像素值/可见性，抓 S1、负行列式类缺陷）；
#   ③ 已提交 preview.jpg 渲染基线：重渲染 3 个代表场景，与提交预览逐像素均值比较
#      （JPEG 噪声容差小；任何渲染行为变化都会超阈值 → 需重跑 migrate 更新基线）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$2"
fail() { echo "pbr_regression_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

if [ ! -f "$ROOT/scenes/khronos/manifest.json" ]; then
    echo "pbr_regression_e2e: skip（缺 scenes/khronos/manifest.json，未做迁移）"
    exit 77
fi

# ---- ① 基线数据 + 指标下限 ----
"$PY" - "$ROOT" <<'PY' || exit 1
import json, os, sys
root = sys.argv[1]
man = json.load(open(os.path.join(root, "scenes/khronos/manifest.json"), encoding="utf-8"))
recs = {r["model"]: r for r in man.get("records", [])}
assert man.get("count") == 148, man.get("count")   # ToyCar 手工调校，不在自动 manifest
for name, r in recs.items():
    assert r.get("status") in ("ok", "imported"), (name, r.get("status"))
    assert r.get("compare"), name
    assert os.path.isfile(os.path.join(root, "scenes/khronos", name, "preview.jpg")), name
# 指标下限：覆盖 glTF 相机、env/PMREM、specular 贴图三条通路的代表模型（历史最佳附近留余量）。
# 指标以提交的 comparison.json 为准（含手工调校的 ToyCar）。
cmp_all = {r["model"]: r for r in json.load(
    open(os.path.join(root, "reports/khronos-comparison/comparison.json"), encoding="utf-8"))["models"]}
floors = {
    "ToyCar": 0.55,                  # env PMREM + 天空 + 手调机位
    "Duck": 0.55,                    # glTF 自带相机（第 18 轮）
    "CompareSpecular": 0.60,         # specularTexture/specularColorTexture
    "IridescentDishWithOlives": 0.40,  # glTF 相机 + transmission 近似
}
for model, floor in floors.items():
    c = cmp_all[model]
    hist = float(c.get("hist") or 0.0)
    assert hist >= floor, (model, hist, floor)
print("pbr_regression_e2e: manifest/compare 基线 OK（149 条，指标下限满足）")
PY
[ $? -eq 0 ] || exit 1

# ---- ② probe oracle：Normal 编码 + 镜像负行列式 ----
unset MAQUE_PRESENT MAQUE_WINDOW MAQUE_ENV_IBL MAQUE_MSAA MAQUE_SHADOW 2>/dev/null || true
(timeout 90 env MAQUE_PRESENT=off MAQUE_WINDOW=0x0 "$PLAYER" --shot "$WORK/probe_normal.ppm" --frames 2 \
    "$ROOT/scenes/pbr/probe_normal.scn" >"$WORK/probe_normal.txt" 2>&1) \
    || { cat "$WORK/probe_normal.txt"; fail "probe_normal 渲染失败"; }
(timeout 90 env MAQUE_PRESENT=off MAQUE_WINDOW=0x0 "$PLAYER" --shot "$WORK/probe_mirror.ppm" --frames 2 \
    "$ROOT/scenes/pbr/probe_mirror.scn" >"$WORK/probe_mirror.txt" 2>&1) \
    || { cat "$WORK/probe_mirror.txt"; fail "probe_mirror 渲染失败"; }
"$PY" - "$WORK/probe_normal.ppm" "$WORK/probe_mirror.ppm" <<'PY' || exit 1
import sys
def load(p):
    d = open(p, 'rb').read(); m, dims, _x, px = d.split(b'\n', 3)
    assert m == b'P6', m
    w, h = map(int, dims.split())
    return w, h, px
w, h, px = load(sys.argv[1])
def rgb(x, y):
    i = (y * w + x) * 3
    return px[i], px[i+1], px[i+2]
# Normal shading：视图空间法线前向 (0,0,1) → 编码 0.5/0.5/1.0（sRGB OETF 后 ≈188/188/255）。
c = rgb(w // 2, h // 2)
assert all(abs(c[k] - (188, 188, 255)[k]) <= 3 for k in range(3)), c
assert rgb(2, 2) == (0, 0, 0), rgb(2, 2)
# 镜像（scale.x=-1）+ side=0 仍可见；且与未镜像立方体覆盖面积接近。
w2, h2, px2 = load(sys.argv[2])
def fg(x0, x1):
    return sum(1 for y in range(h2) for x in range(int(w2 * x0), int(w2 * x1))
               if px2[(y * w2 + x) * 3] + px2[(y * w2 + x) * 3 + 1] + px2[(y * w2 + x) * 3 + 2] > 20)
left, right = fg(.15, .45), fg(.55, .85)
assert left > 5000 and right > 5000, (left, right)
assert abs(left - right) <= max(500, left // 20), (left, right)
print("pbr_regression_e2e: Normal oracle + 镜像 front-face OK")
PY
[ $? -eq 0 ] || exit 1

# ---- ③ preview.jpg 渲染基线（需 PIL；无 PIL 时跳过这一节）----
if "$PY" -c 'import PIL' >/dev/null 2>&1; then
    for model in ToyCar Duck CompareSpecular; do
        scn="$ROOT/scenes/khronos/$model/$model.scn"
        ( cd "$ROOT" && timeout 120 env MAQUE_PRESENT=off MAQUE_WINDOW=0x0 "$PLAYER" \
            --shot "$WORK/$model.png" --frames 2 "scenes/khronos/$model/$model.scn" ) \
            >"$WORK/$model.log" 2>&1 || { cat "$WORK/$model.log"; fail "$model 基线重渲染失败"; }
    done
    "$PY" - "$ROOT" "$WORK" <<'PY' || exit 1
import os, sys
from PIL import Image, ImageChops
root, work = sys.argv[1], sys.argv[2]
for model in ("ToyCar", "Duck", "CompareSpecular"):
    a = Image.open(os.path.join(work, model + ".png")).convert("RGB")
    w, h = a.size
    a = a.resize((800, 450), Image.LANCZOS)          # preview.jpg = preview.png 下采样 q82
    b = Image.open(os.path.join(root, "scenes/khronos", model, "preview.jpg")).convert("RGB")
    assert b.size == (800, 450), (model, b.size)
    d = ImageChops.difference(a, b)
    hist = d.histogram()
    n = 800 * 450
    mean = sum(i * (hist[i] + hist[256 + i] + hist[512 + i]) for i in range(256)) / (3 * n)
    mx = max(i for i in range(256) if any(hist[c * 256 + i] for c in range(3)))
    print(f"pbr_regression_e2e: {model} 预览基线 mean={mean:.2f} max={mx}")
    # 同机同构建应接近 0；JPEG 下采样噪声给到 mean<6/max<140；渲染行为变化会显著超过。
    assert mean < 6.0 and mx < 140, (model, mean, mx)
print("pbr_regression_e2e: preview.jpg 渲染基线 OK")
PY
    [ $? -eq 0 ] || exit 1
else
    echo "pbr_regression_e2e: 无 PIL → 跳过 preview.jpg 基线比较（① ② 仍有效）"
fi

echo "pbr_regression_e2e: PASS"
