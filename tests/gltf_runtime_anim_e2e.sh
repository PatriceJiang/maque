#!/usr/bin/env bash
# 运行时直读 glTF 的**蒙皮 / 动画 / morph**端到端（plans/24 P1–P3）。
#
# 背景：runtime 的 C++ 直读加载器现在支持 skins / animations / morph targets——
#   · 蒙皮：节点层级 + JOINTS_0/WEIGHTS_0 + IBM → palette[j] = world(joint_j) × IBM_j；
#           bind pose 下 palette≈I → 默认渲染与 MAQUE_SKIN=0（identity palette）应逐字节一致。
#   · 动画：TRS（translation/rotation/scale，STEP/LINEAR）通道**自动播放第一个 clip**（按实体维护时间），
#           固定 dt 推进 → 1/20/40 帧画面不同、同帧重复截图逐字节一致；MAQUE_ANIM=0 → 不采样（各帧一致）。
#   · morph：targets（POSITION/NORMAL 增量）+ 默认权重 + weights 动画通道 → CPU 重算顶点；
#           默认渲染与 MAQUE_MORPH=0（基础网格）不同；1/20/40 帧因权重变化而不同。
#
# 夹具（从 tests/data/gltf/{skinned,animated,morph}.gltf 原样复制，data: URI 内嵌、自包含）：
#   skinned.gltf   2 joints（root_joint → child_joint，IBM 使 bind pose palette=I）+ 带蒙皮的 quad
#   animated.gltf  pivot → spinner；clip "spin" duration=1s：pivot.rotation LINEAR（0→90°Y）、
#                  spinner.translation LINEAR（(1,0,0)→(1,1,0)）、spinner.scale STEP（t≥0.5 → 2x）
#   morph.gltf     2 targets（POSITION/NORMAL 增量）、默认 weights [0.5,0.25]；
#                  clip "morph_anim" duration=1s 的 weights 通道（[0.5,0.25]→[1,0]→[0,1]）
#
# 断言：
#   A. 蒙皮：日志 skins=1 + skin joints=2；默认 vs MAQUE_SKIN=0 两张 PPM **逐字节一致**；
#            PPM 结构（非背景/中心有内容）。
#   B. 动画：日志 anims=1 + clip 'spin'；--shot-seq 1/20/40 三帧**两两不同**（像素差 ≥ 阈值）；
#            同帧重复截图逐字节一致；MAQUE_ANIM=0 时三帧一致（不采样）；每帧结构合理。
#   C. morph：日志 morph_prims=1 + `runtime: gltf morph`；默认 vs MAQUE_MORPH=0 **明显不同**；
#            --shot-seq 1/20/40 因权重动画而不同；同帧确定性；默认帧结构合理。
#   所有场景：无 `runtime: gltf 加载失败`、无 ASan 报告、截图已写出。
#
# 参数（与 samples/player/CMakeLists.txt 的 add_test 一致）：
#   $1 = maque_player；$2 = 夹具目录（runtime_anim，含 *.scn / *.gltf）；$3 = 工作目录
# 无显示环境可跑：全程 --shot / --shot-seq 离屏 + MAQUE_WINDOW=0x0（不建窗口）。
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"
FIXDIR="$2"
WORK="$3"

fail() { echo "gltf_runtime_anim_e2e: FAIL $*" >&2; exit 1; }
[ -n "${PY:-}" ] || { echo "gltf_runtime_anim_e2e: skip（无 python3，无法解析 PPM）"; exit 77; }
[ -f "$PLAYER" ] || { echo "gltf_runtime_anim_e2e: skip（缺 player: $PLAYER）"; exit 77; }
[ -d "$FIXDIR" ] || { echo "gltf_runtime_anim_e2e: skip（缺夹具目录: $FIXDIR）"; exit 77; }
SCN_SKIN="$FIXDIR/skinned.scn"
SCN_ANIM="$FIXDIR/animated.scn"
SCN_MORPH="$FIXDIR/morph.scn"
for f in "$SCN_SKIN" "$SCN_ANIM" "$SCN_MORPH" \
         "$FIXDIR/skinned.gltf" "$FIXDIR/animated.gltf" "$FIXDIR/morph.gltf"; do
    [ -f "$f" ] || { echo "gltf_runtime_anim_e2e: skip（缺夹具 $f）"; exit 77; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
# 清掉可能干扰离屏渲染/诊断的环境；强制无窗口 + 纯离屏
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_SHOT_FRAMES MAQUE_ENV_IBL \
      MAQUE_RENDER_SCALE MAQUE_MSAA MAQUE_SHADOW MAQUE_ORBIT MAQUE_RENDER3D_INFO MAQUE_WINDOW \
      MAQUE_WINDOW_EVENT_AT MAQUE_ANIM MAQUE_SKIN MAQUE_MORPH 2>/dev/null || true
export MAQUE_PRESENT=off
export MAQUE_WINDOW=0x0

# PPM 检查器：结构 / 逐字节一致 / 明显不同（阈值集中在这里）
cat >"$WORK/ppm_check.py" <<'PY'
import sys

BG_TOL = 25          # 与渲染清屏色的 L1 距离阈值（与 render3d_scene_e2e 一致）
DIFF_TOL = 8         # 单通道差 > 8 视为"该像素不同"


def load(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    assert len(parts) == 4 and parts[0] == b'P6', f'{path}: 不是 P6 PPM'
    w, h = map(int, parts[1].split())
    px = parts[3]
    assert len(px) == w * h * 3, f'{path}: 像素字节数 {len(px)} != {w}x{h}x3'
    return w, h, px


def cmd_struct(path, label, min_nonbg, need_center):
    w, h, px = load(path)
    assert (w, h) == (640, 360), f'{label}: 尺寸 {w}x{h} != 设计分辨率 640x360'
    def rgb(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]
    bg = rgb(2, 2)
    diff = lambda c: abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2])
    nonbg = center = 0
    for y in range(h):
        for x in range(w):
            if diff(rgb(x, y)) > BG_TOL:
                nonbg += 1
                if w // 3 <= x < w * 2 // 3 and h // 3 <= y < h * 2 // 3:
                    center += 1
    ratio = nonbg / (w * h)
    print(f"gltf_runtime_anim_e2e: {label} {w}x{h} bg={bg} nonbg={ratio:.3f} center={center}")
    ok = min_nonbg < ratio < 0.85 and (center > 500 or need_center == '0')
    if not ok:
        print(f'gltf_runtime_anim_e2e: FAIL {label}: PPM 结构断言不满足 '
              f'(min_nonbg={min_nonbg}, need_center={need_center})', file=sys.stderr)
        return 1
    return 0


def cmd_same(a_path, b_path, label):
    wa, ha, a = load(a_path)
    wb, hb, b = load(b_path)
    assert (wa, ha) == (wb, hb), f'{label}: 两图尺寸不同 {wa}x{ha} vs {wb}x{hb}'
    if a != b:
        nd = sum(1 for i in range(0, len(a), 3)
                 if abs(a[i] - b[i]) > 0 or abs(a[i + 1] - b[i + 1]) > 0
                 or abs(a[i + 2] - b[i + 2]) > 0)
        md = max(abs(a[i] - b[i]) for i in range(len(a)))
        print(f'gltf_runtime_anim_e2e: FAIL {label}: 期望逐字节一致，但 {nd} 像素不同 '
              f'(max_channel_diff={md})', file=sys.stderr)
        return 1
    print(f'gltf_runtime_anim_e2e: {label} 逐字节一致 OK')
    return 0


def cmd_differs(a_path, b_path, min_px, label):
    wa, ha, a = load(a_path)
    wb, hb, b = load(b_path)
    assert (wa, ha) == (wb, hb), f'{label}: 两图尺寸不同 {wa}x{ha} vs {wb}x{hb}'
    nd = 0
    md = 0
    for i in range(0, len(a), 3):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        if d > DIFF_TOL:
            nd += 1
        if d > md:
            md = d
    print(f'gltf_runtime_anim_e2e: {label} 差异像素={nd} max_channel_diff={md}')
    if nd < int(min_px):
        print(f'gltf_runtime_anim_e2e: FAIL {label}: 差异像素 {nd} < {min_px}', file=sys.stderr)
        return 1
    return 0


def main():
    cmd = sys.argv[1]
    if cmd == 'struct':
        return cmd_struct(sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5])
    if cmd == 'same':
        return cmd_same(sys.argv[2], sys.argv[3], sys.argv[4])
    if cmd == 'differs':
        return cmd_differs(sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5])
    raise SystemExit(f'未知模式 {cmd}')


if __name__ == '__main__':
    sys.exit(main())
PY
CHECK() { "$PY" "$WORK/ppm_check.py" "$@"; }

# 成功渲染的公共日志断言：有 shot、无加载失败、无 ASan
check_ok_log() {   # check_ok_log <log> <label>
    local log="$1" label="$2"
    grep -q "runtime: shot" "$log" || { cat "$log"; fail "$label: 截图未写入"; }
    if grep -q "gltf 加载失败" "$log"; then cat "$log"; fail "$label: gltf 加载失败"; fi
    if grep -q "AddressSanitizer" "$log"; then cat "$log"; fail "$label: ASan 报告（崩溃/越界）"; fi
}

# ---- A. 蒙皮（P1）----
timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/skin.ppm" --frames 2 \
    "$SCN_SKIN" >"$WORK/skin.log" 2>&1 || { cat "$WORK/skin.log"; fail "A 蒙皮场景渲染失败"; }
check_ok_log "$WORK/skin.log" "A 蒙皮"
grep -Eq "runtime: gltf model .*skins=1" "$WORK/skin.log" \
    || { cat "$WORK/skin.log"; fail "A 蒙皮: 模型日志缺 skins=1（蒙皮未被识别）"; }
grep -Eq "runtime: gltf skin .*joints=2" "$WORK/skin.log" \
    || { cat "$WORK/skin.log"; fail "A 蒙皮: 缺 'runtime: gltf skin ... joints=2' 日志"; }
CHECK struct "$WORK/skin.ppm" "A 蒙皮" 0.03 1 || exit 1

# bind pose palette≈I：默认渲染与 MAQUE_SKIN=0（identity palette）逐字节一致
timeout 120 env MAQUE_RENDER3D_INFO=1 MAQUE_SKIN=0 "$PLAYER" --shot "$WORK/skin_skin0.ppm" \
    --frames 2 "$SCN_SKIN" >"$WORK/skin_skin0.log" 2>&1 \
    || { cat "$WORK/skin_skin0.log"; fail "A 蒙皮 MAQUE_SKIN=0 渲染失败"; }
check_ok_log "$WORK/skin_skin0.log" "A 蒙皮(SKIN=0)"
CHECK same "$WORK/skin.ppm" "$WORK/skin_skin0.ppm" "A 蒙皮 bind pose palette≈I" || exit 1
echo "gltf_runtime_anim_e2e: A 蒙皮（skins=1/joints=2 + bind pose 与 MAQUE_SKIN=0 逐字节一致）OK"

# ---- B. 动画（P2）----
mkdir -p "$WORK/anim" "$WORK/anim_rep" "$WORK/anim_off"
timeout 180 env MAQUE_RENDER3D_INFO=1 "$PLAYER" \
    --shot-seq "$WORK/anim/f{}.ppm" --shot-frames 1,20,40 \
    "$SCN_ANIM" >"$WORK/anim.log" 2>&1 || { cat "$WORK/anim.log"; fail "B 动画渲染失败"; }
check_ok_log "$WORK/anim.log" "B 动画"
grep -Eq "runtime: gltf model .*anims=1" "$WORK/anim.log" \
    || { cat "$WORK/anim.log"; fail "B 动画: 模型日志缺 anims=1（动画未被识别）"; }
grep -q "runtime: gltf anim" "$WORK/anim.log" && grep -q "spin" "$WORK/anim.log" \
    || { cat "$WORK/anim.log"; fail "B 动画: 缺动画/clip 日志（期望含 'spin'）"; }
for n in 1 20 40; do
    # --shot-seq 的日志形如 "runtime: shot[1] '<path>' 640x360 frame=1"（序号在方括号里），
    # 所以这里不能写成 "shot .*frame="（"shot" 后面紧跟的是 '['）。
    grep -q "runtime: shot.*frame=$n" "$WORK/anim.log" \
        || { cat "$WORK/anim.log"; fail "B 动画: 未拍第 $n 帧"; }
    [ -s "$WORK/anim/f$n.ppm" ] || { cat "$WORK/anim.log"; fail "B 动画: 缺第 $n 帧 PPM"; }
done
# 三帧两两不同（动画真的在推进）+ 每帧都有内容（相机取景覆盖整段动画扫过的区域，
# 三帧都落在画面内且中心区有内容 —— 实测 nonbg≈0.06/0.03/0.03、center≈4800/5800/1200）
CHECK struct "$WORK/anim/f1.ppm" "B 动画 frame1" 0.02 1 || exit 1
CHECK struct "$WORK/anim/f20.ppm" "B 动画 frame20" 0.02 1 || exit 1
CHECK struct "$WORK/anim/f40.ppm" "B 动画 frame40" 0.02 1 || exit 1
CHECK differs "$WORK/anim/f1.ppm" "$WORK/anim/f20.ppm" 200 "B 动画 f1 vs f20" || exit 1
CHECK differs "$WORK/anim/f20.ppm" "$WORK/anim/f40.ppm" 200 "B 动画 f20 vs f40" || exit 1
CHECK differs "$WORK/anim/f1.ppm" "$WORK/anim/f40.ppm" 200 "B 动画 f1 vs f40" || exit 1

# 同帧重复截图逐字节一致（固定 dt / 无随机）
timeout 180 env MAQUE_RENDER3D_INFO=1 "$PLAYER" \
    --shot-seq "$WORK/anim_rep/f{}.ppm" --shot-frames 1,20,40 \
    "$SCN_ANIM" >"$WORK/anim_rep.log" 2>&1 || { cat "$WORK/anim_rep.log"; fail "B 动画重复渲染失败"; }
for n in 1 20 40; do
    cmp -s "$WORK/anim/f$n.ppm" "$WORK/anim_rep/f$n.ppm" \
        || { cat "$WORK/anim_rep.log"; fail "B 动画: 第 $n 帧两次截图不一致（非确定性）"; }
done
echo "gltf_runtime_anim_e2e: B 动画（anims=1/clip spin，1/20/40 互相不同 + 同帧确定）OK"

# MAQUE_ANIM=0 → 不采样动画：三帧应逐字节一致
timeout 180 env MAQUE_RENDER3D_INFO=1 MAQUE_ANIM=0 "$PLAYER" \
    --shot-seq "$WORK/anim_off/f{}.ppm" --shot-frames 1,20,40 \
    "$SCN_ANIM" >"$WORK/anim_off.log" 2>&1 || { cat "$WORK/anim_off.log"; fail "B 动画 MAQUE_ANIM=0 渲染失败"; }
CHECK same "$WORK/anim_off/f1.ppm" "$WORK/anim_off/f20.ppm" "B 动画 MAQUE_ANIM=0 f1 vs f20" || exit 1
CHECK same "$WORK/anim_off/f20.ppm" "$WORK/anim_off/f40.ppm" "B 动画 MAQUE_ANIM=0 f20 vs f40" || exit 1
echo "gltf_runtime_anim_e2e: B 动画 MAQUE_ANIM=0（不采样 → 各帧一致）OK"

# ---- C. morph（P3）----
timeout 120 env MAQUE_RENDER3D_INFO=1 "$PLAYER" --shot "$WORK/morph.ppm" --frames 2 \
    "$SCN_MORPH" >"$WORK/morph.log" 2>&1 || { cat "$WORK/morph.log"; fail "C morph 场景渲染失败"; }
check_ok_log "$WORK/morph.log" "C morph"
grep -Eq "runtime: gltf model .*morph_prims=1" "$WORK/morph.log" \
    || { cat "$WORK/morph.log"; fail "C morph: 模型日志缺 morph_prims=1"; }
grep -q "runtime: gltf morph" "$WORK/morph.log" \
    || { cat "$WORK/morph.log"; fail "C morph: 缺 'runtime: gltf morph' 日志"; }
CHECK struct "$WORK/morph.ppm" "C morph" 0.03 1 || exit 1

# 默认（应用默认 weights [0.5,0.25]）vs MAQUE_MORPH=0（基础网格）必须不同 → morph 真生效
timeout 120 env MAQUE_RENDER3D_INFO=1 MAQUE_MORPH=0 "$PLAYER" --shot "$WORK/morph_nomorph.ppm" \
    --frames 2 "$SCN_MORPH" >"$WORK/morph_nomorph.log" 2>&1 \
    || { cat "$WORK/morph_nomorph.log"; fail "C morph MAQUE_MORPH=0 渲染失败"; }
check_ok_log "$WORK/morph_nomorph.log" "C morph(MORPH=0)"
CHECK differs "$WORK/morph.ppm" "$WORK/morph_nomorph.ppm" 200 "C morph 默认 vs MAQUE_MORPH=0" || exit 1

# weights 动画：1/20/40 帧权重不同 → 截图不同
mkdir -p "$WORK/morph_seq" "$WORK/morph_rep"
timeout 180 env MAQUE_RENDER3D_INFO=1 "$PLAYER" \
    --shot-seq "$WORK/morph_seq/f{}.ppm" --shot-frames 1,20,40 \
    "$SCN_MORPH" >"$WORK/morph_seq.log" 2>&1 || { cat "$WORK/morph_seq.log"; fail "C morph 序列渲染失败"; }
for n in 1 20 40; do
    [ -s "$WORK/morph_seq/f$n.ppm" ] || { cat "$WORK/morph_seq.log"; fail "C morph: 缺第 $n 帧"; }
done
CHECK differs "$WORK/morph_seq/f1.ppm" "$WORK/morph_seq/f20.ppm" 100 "C morph f1 vs f20" || exit 1
CHECK differs "$WORK/morph_seq/f20.ppm" "$WORK/morph_seq/f40.ppm" 100 "C morph f20 vs f40" || exit 1
CHECK differs "$WORK/morph_seq/f1.ppm" "$WORK/morph_seq/f40.ppm" 100 "C morph f1 vs f40" || exit 1

# 同帧确定性
timeout 180 env MAQUE_RENDER3D_INFO=1 "$PLAYER" \
    --shot-seq "$WORK/morph_rep/f{}.ppm" --shot-frames 1,20,40 \
    "$SCN_MORPH" >"$WORK/morph_rep.log" 2>&1 || { cat "$WORK/morph_rep.log"; fail "C morph 重复渲染失败"; }
for n in 1 20 40; do
    cmp -s "$WORK/morph_seq/f$n.ppm" "$WORK/morph_rep/f$n.ppm" \
        || { cat "$WORK/morph_rep.log"; fail "C morph: 第 $n 帧两次截图不一致（非确定性）"; }
done
echo "gltf_runtime_anim_e2e: C morph（morph_prims=1，默认 vs MAQUE_MORPH=0 不同 + 权重动画帧不同 + 同帧确定）OK"

echo "gltf_runtime_anim_e2e: PASS"
