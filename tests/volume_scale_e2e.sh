#!/usr/bin/env bash
# KHR_materials_volume 厚度单位回归（第 28 轮）：
# thicknessFactor/thicknessTexture 给的是 **mesh 空间** 厚度，随节点变换缩放到世界空间；
# attenuationDistance 是 **世界空间**。漏乘节点缩放时，小尺度资产（USDShaderBallForGltf 节点 scale=0.01）
# 的光学厚度会被放大 100 倍 → 玻璃发黑。
#
# 场景 scenes/pbr/volume_scale.scn 两个 cube 材质/厚度完全相同：
#   左：scale=1.0（世界厚度 1.0 → 光学厚度被 clamp 到 3 → 几乎全黑）
#   右：scale=0.1（世界厚度 0.1 → 介质较薄 → 明显更亮）
# 若运行时漏乘节点缩放，两者亮度会一样（都变黑）→ 断言失败。
set -u
. "$(dirname "$0")/maque_python.sh"
PLAYER="$1"
SCENE="$2"
WORK="$3"
[ -n "${PLAYER:-}" ] && [ -x "$PLAYER" ] || { echo "volume_scale_e2e: 缺少 player" >&2; exit 77; }
[ -f "$SCENE" ] || { echo "volume_scale_e2e: 缺少场景 $SCENE" >&2; exit 77; }
mkdir -p "$WORK"
fail() { echo "volume_scale_e2e: FAIL $*" >&2; exit 1; }

export MAQUE_PRESENT=off MAQUE_WINDOW=0x0
SHOT="$WORK/volume_scale.png"
"$PLAYER" --shot "$SHOT" --shot-frame 1 "$SCENE" >"$WORK/render.log" 2>&1 || fail "渲染失败"
[ -f "$SHOT" ] || fail "没有截图输出"

"$PY" - "$SHOT" <<'PY' || exit 1
import sys
import numpy as np
from PIL import Image

im = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.float32) / 255.0
h, w = im.shape[:2]
bg = im[2, 2]
fg = np.abs(im - bg).sum(axis=2) > 0.04
lum = 0.2126 * im[..., 0] + 0.7152 * im[..., 1] + 0.0722 * im[..., 2]
stats = {}
for name, x0, x1 in (("left", 0, w // 2), ("right", w // 2, w)):
    m = fg[:, x0:x1]
    v = lum[:, x0:x1][m]
    stats[name] = (int(m.sum()), float(np.median(v)) if len(v) else -1.0)
assert stats["left"][0] > 500, ("左 cube 不可见", stats)
assert stats["right"][0] > 500, ("右 cube 不可见", stats)
# 世界厚度差 10×，光学厚度被 clamp 后仍有显著亮度差
assert stats["right"][1] > stats["left"][1] + 0.10, \
    ("厚度未按节点缩放（两 cube 亮度应差 >0.10）", stats)
print("volume_scale_e2e: 厚度按节点缩放 OK 左=%.3f 右=%.3f" % (stats["left"][1], stats["right"][1]))
PY
[ $? -eq 0 ] || exit 1

# 确定性：同场景两次截图逐字节一致
"$PLAYER" --shot "$WORK/volume_scale_b.png" --shot-frame 1 "$SCENE" >/dev/null 2>&1 || fail "二次渲染失败"
cmp -s "$SHOT" "$WORK/volume_scale_b.png" || fail "同场景两次截图不一致"
echo "volume_scale_e2e: PASS"
