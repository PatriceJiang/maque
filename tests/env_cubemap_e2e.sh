#!/usr/bin/env bash
# cubemap env 端到端（docs/31，第 26 轮）：6 面 cube 资产 → GGX 预过滤 cube + irradiance cube → IBL/天空。
#   ① 同一 studio 环境分别以 equirect（env_ibl.scn）与 cubemap（env_cubemap.scn）渲染：
#      cube 日志必须有 "env CUBEMAP ... mips (GGX) + irradiance"；equirect 有 "env PMREM"。
#   ② 两张渲染的主体/天空应接近（同一环境 → hist_similarity ≥ 0.9，平均像素差 ≤ 4；无 PIL 时跳过比较）。
#   ③ 确定性：cube 场景连渲两次逐字节一致（装配期 CPU PMREM 无随机）。
#   ④ 回退鲁棒性：env_cubemap 指向不存在的 6 面 → 只 warn + 回退 analytic，进程仍 exit 0 且出图。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$2"
fail() { echo "env_cubemap_e2e: FAIL $*" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"

[ -f "$ROOT/scenes/pbr/env_cubemap.scn" ] || fail "缺 scenes/pbr/env_cubemap.scn"
for s in px nx py ny pz nz; do
    [ -f "$ROOT/scenes/pbr/env/studio_cube/studio_${s}.hdr" ] || fail "缺 cube 面 studio_${s}.hdr"
done

unset MAQUE_PRESENT MAQUE_WINDOW MAQUE_ENV_IBL MAQUE_MSAA MAQUE_SHADOW 2>/dev/null || true
run_shot() {  # $1=scene(相对 ROOT) $2=out.png $3=log
    ( cd "$ROOT" && timeout 120 env MAQUE_PRESENT=off MAQUE_WINDOW=0x0 "$PLAYER" \
        --shot "$2" --frames 2 "$1" ) >"$3" 2>&1
}

# ---- ① 两种 env 各渲一张 ----
run_shot "scenes/pbr/env_ibl.scn" "$WORK/equirect.png" "$WORK/equirect.log" \
    || { cat "$WORK/equirect.log"; fail "equirect env 渲染失败"; }
run_shot "scenes/pbr/env_cubemap.scn" "$WORK/cube.png" "$WORK/cube.log" \
    || { cat "$WORK/cube.log"; fail "cubemap env 渲染失败"; }
grep -q 'env PMREM' "$WORK/equirect.log" || { cat "$WORK/equirect.log"; fail "equirect 未走 PMREM"; }
grep -q 'env CUBEMAP' "$WORK/cube.log" || { cat "$WORK/cube.log"; fail "cube 未走 CUBEMAP 装配"; }
grep -q 'mips (GGX) + irradiance' "$WORK/cube.log" || { cat "$WORK/cube.log"; fail "cube 缺 GGX mip/irradiance 日志"; }
[ -s "$WORK/cube.png" ] || fail "cube 截图为空"
[ -s "$WORK/equirect.png" ] || fail "equirect 截图为空"
echo "env_cubemap_e2e: cube 装配日志 OK（$(grep -o 'env CUBEMAP[^)]*' "$WORK/cube.log" | head -1)）"

# ---- ③ 确定性：再渲一次逐字节一致 ----
run_shot "scenes/pbr/env_cubemap.scn" "$WORK/cube2.png" "$WORK/cube2.log" \
    || { cat "$WORK/cube2.log"; fail "cube 第二次渲染失败"; }
cmp -s "$WORK/cube.png" "$WORK/cube2.png" || fail "cube 渲染不确定（两次 PNG 不一致）"
echo "env_cubemap_e2e: cube 渲染确定性 OK"

# ---- ④ 回退：env_cubemap 指向不存在的面 ----
"$PY" - "$ROOT/scenes/pbr/env_cubemap.scn" "$WORK/missing.scn" <<'PY' || exit 1
import json, sys
src, dst = sys.argv[1], sys.argv[2]
j = json.load(open(src, encoding="utf-8"))
j["env_cubemap"] = "env/no_such_cube/x"
json.dump(j, open(dst, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
PY
run_shot "$WORK/missing.scn" "$WORK/missing.png" "$WORK/missing.log"
rc=$?
[ $rc -eq 0 ] || { cat "$WORK/missing.log"; fail "cube 缺失时应回退并正常退出（rc=$rc）"; }
[ -s "$WORK/missing.png" ] || fail "cube 缺失回退后无截图"
grep -q '6 面加载失败' "$WORK/missing.log" || { cat "$WORK/missing.log"; fail "cube 缺失无告警日志"; }
echo "env_cubemap_e2e: 缺失 cube 回退 analytic OK"

# ---- ② equirect vs cube 渲染接近（同一环境）----
if "$PY" -c 'import PIL, numpy' >/dev/null 2>&1; then
    "$PY" - "$WORK/equirect.png" "$WORK/cube.png" <<'PY' || exit 1
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.float64)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB"), dtype=np.float64)
assert a.shape == b.shape, (a.shape, b.shape)
d = np.abs(a - b)
la = (0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]) / 255.0
lb = (0.2126 * b[..., 0] + 0.7152 * b[..., 1] + 0.0722 * b[..., 2]) / 255.0
ha, _ = np.histogram(la, bins=16, range=(0.0, 1.0))
hb, _ = np.histogram(lb, bins=16, range=(0.0, 1.0))
hist = float(np.minimum(ha, hb).sum() / max(1, ha.sum()))
mean = float(d.mean())
print(f"env_cubemap_e2e: equirect vs cube hist={hist:.4f} mean_abs_diff={mean:.3f}")
assert hist >= 0.90, hist                     # 同一环境：cube 应显著接近 equirect
assert mean <= 4.0, mean
PY
    [ $? -eq 0 ] || exit 1
else
    echo "env_cubemap_e2e: 无 PIL/numpy → 跳过 equirect-vs-cube 像素比较（①②③④ 仍有效）"
fi

echo "env_cubemap_e2e: PASS"
