#!/usr/bin/env bash
# runtime oracle —— runtime.cpp 拆分的"纯搬运"证据（docs/10 确定性）
#
# 拆 17k 行的单一编译单元时，唯一能证明"只搬不改"的东西是**逐帧校验和 + 像素**。
# 每个场景取两路互补信号：
#   ① 模拟状态：`--frames N` 退出行里的 world_trail（覆盖组件/脚本/物理/粒子/实例）
#   ② 渲染像素：`--shot` 的 PNG 逐字节 sha256（覆盖提取/文本/骨骼/3D/后处理）
#
# 用法：
#   runtime_oracle.sh <maque_player> <repo_root> <workdir>                    # selfcheck（默认）
#   runtime_oracle.sh <maque_player> <repo_root> <workdir> record <manifest>
#   runtime_oracle.sh <maque_player> <repo_root> <workdir> check  <manifest>
#
# selfcheck = 跑两遍并互相比较：验证"本构建是确定的"（可移植，进 ctest）。
# record/check = 跨构建比较：拆分前 record，每一步之后 check（同一台机器，重构的护栏）。
#
# 环境变量：
#   ORACLE_FAST=1       只跑 8 个场景（快跑的每步门禁，默认全量）
#   ORACLE_NO_SHOTS=1   跳过像素（只比 world_trail）
#   ORACLE_FRAMES=N     每个场景跑多少帧（默认 60）
#   ORACLE_SHOT_FRAME=N 取第几帧的像素（默认 30）
set -u

PLAYER="${1:-}"; REPO="${2:-}"; WORK="${3:-}"; MODE="${4:-selfcheck}"; MANIFEST="${5:-}"
if [ -z "$PLAYER" ] || [ -z "$REPO" ] || [ -z "$WORK" ]; then
    echo "usage: runtime_oracle.sh <maque_player> <repo_root> <workdir> [record|check|selfcheck] [manifest]" >&2
    exit 2
fi
[ -x "$PLAYER" ] || { echo "runtime_oracle: SKIP（player 不存在：$PLAYER）"; exit 77; }
command -v sha256sum >/dev/null 2>&1 || { echo "runtime_oracle: SKIP（无 sha256sum）"; exit 77; }

FRAMES="${ORACLE_FRAMES:-60}"
SHOT_FRAME="${ORACLE_SHOT_FRAME:-30}"
SHOTS=1
[ "${ORACLE_NO_SHOTS:-0}" = "1" ] && SHOTS=0

# 场景清单：覆盖 runtime 的各个关注点（拆分时每块都至少被一个场景踩到）。
#   ball      2D 精灵 + 物理 + tween/schedule
#   anim      2D 关键帧动画（clip 采样写回反射字段）
#   label     文本排版/字形图集/装饰
#   nodes     节点/组件/事件 API（最近改动最多的面）
#   ui        UI 控件 + 命中测试
#   pbr/*     3D 提取 + PBR 材质/环境光/阴影/网格资产
#   rt        相机离屏 RenderTexture + MSAA
#   seascape  场景多通道 shader 图（ShaderGraph）
#   nested    场景实例（docs/32）
#   td        实例 + 全局脚本 + 全局状态（docs/32 §16 / docs/34）
#   spine     Spine 2D 骨骼网格
#   khronos   运行时直读 glTF：静态（DamagedHelmet）/ 蒙皮动画（CesiumMan）
#   multilang 多 VM 事件总线
#   imgui     引擎侧 ImGui 面板网格链
SCENES_ALL="
scenes/ball.scn
scenes/anim/anim.scn
scenes/label/label.scn
scenes/nodes/nodes.scn
scenes/ui.scn
scenes/pbr/cube.scn
scenes/pbr/materials.scn
scenes/pbr/env_ibl.scn
scenes/pbr/imported.scn
scenes/render_texture/render_texture.scn
scenes/shadertoy/seascape.scn
scenes/nested/parent.scn
scenes/td/level1.scn
samples/spine/spine.scn
scenes/khronos/DamagedHelmet/DamagedHelmet.scn
scenes/khronos/CesiumMan/CesiumMan.scn
scenes/multilang/multilang.scn
scenes/imgui_probe.scn
"
SCENES_FAST="
scenes/ball.scn
scenes/label/label.scn
scenes/nodes/nodes.scn
scenes/pbr/materials.scn
scenes/render_texture/render_texture.scn
scenes/nested/parent.scn
samples/spine/spine.scn
scenes/khronos/CesiumMan/CesiumMan.scn
"
if [ "${ORACLE_FAST:-0}" = "1" ]; then SCENES="$SCENES_FAST"; else SCENES="$SCENES_ALL"; fi

cd "$REPO" || exit 2
mkdir -p "$WORK" || exit 2
TMP="$WORK/shots"
rm -rf "$TMP"; mkdir -p "$TMP"

# 无窗口 / 离屏 / 不限速：与 CI 的确定性门同一套环境（docs/10）
export MAQUE_WINDOW=0x0
export MAQUE_PRESENT=off
export MAQUE_PACE=0

fail=0
pass_lines=""

# run_one <scene> → 往 MANIFEST（或 pass_lines）写一行
# 两次进程：① `--frames N` 取标准路径的 world_trail（模拟覆盖最全）
#           ② `--shot` 取像素（截图路径会在该帧后退出，所以不参与 trail 断言）
run_one() {
    local scene="$1"
    local log="$TMP/log.txt"
    local png="$TMP/one.png"
    rm -f "$png"
    timeout 300 "$PLAYER" --frames "$FRAMES" "$scene" >"$log" 2>&1
    local rc=$?
    local pngsum="-"
    if [ "$SHOTS" = "1" ]; then
        timeout 300 "$PLAYER" --shot "$png" --shot-frame "$SHOT_FRAME" "$scene" >>"$log" 2>&1
        [ -s "$png" ] || { echo "runtime_oracle: FAIL '$scene' 没有产出 PNG" >&2; echo "$scene NO_PNG"; return 1; }
        pngsum=$(sha256sum "$png" | cut -d' ' -f1)
    fi
    local line
    # 日志里两次运行各有一行退出摘要：**取第一次**（`--frames N` 的标准路径）；
    # 截图那次在 SHOT_FRAME 之后即退出，trail 不具代表性。
    line=$(grep -oE "exited after [0-9]+ frames \(quit=[0-9]\) world_trail=0x[0-9a-f]+" "$log" | head -1)
    if [ -z "$line" ]; then
        echo "runtime_oracle: FAIL '$scene' 没有退出摘要（rc=$rc）" >&2
        sed -n '1,40p' "$log" >&2
        echo "$scene FAILED rc=$rc"
        return 1
    fi
    if grep -qE 'ERROR: AddressSanitizer|stack-overflow|panic|Assertion failed' "$log"; then
        echo "runtime_oracle: FAIL '$scene' 日志里有 ASan/断言" >&2
        grep -E 'ERROR: AddressSanitizer|stack-overflow|panic|Assertion failed' "$log" | head -5 >&2
        echo "$scene CRASHED"
        return 1
    fi
    local frames quit trail
    frames=$(printf '%s' "$line" | sed -E 's/.*exited after ([0-9]+) frames.*/\1/')
    quit=$(printf '%s' "$line" | sed -E 's/.*\(quit=([0-9])\).*/\1/')
    trail=$(printf '%s' "$line" | sed -E 's/.*world_trail=(0x[0-9a-f]+)/\1/')
    echo "$scene frames=$frames quit=$quit trail=$trail png=$pngsum"
}

: > "$WORK/current.txt"
for scene in $SCENES; do
    [ -f "$scene" ] || { echo "runtime_oracle: SKIP 缺场景 $scene"; continue; }
    out=$(run_one "$scene") || { fail=1; printf '%s\n' "$out" >> "$WORK/current.txt"; continue; }
    printf '%s\n' "$out" >> "$WORK/current.txt"
done

sort -o "$WORK/current.txt" "$WORK/current.txt"

case "$MODE" in
    record)
        [ -n "$MANIFEST" ] || { echo "runtime_oracle: record 需要 manifest 路径" >&2; exit 2; }
        cp "$WORK/current.txt" "$MANIFEST"
        echo "runtime_oracle: 记录 $(wc -l < "$MANIFEST") 个场景 → $MANIFEST"
        [ "$fail" = "0" ] || exit 1
        ;;
    check)
        [ -n "$MANIFEST" ] || { echo "runtime_oracle: check 需要 manifest 路径" >&2; exit 2; }
        [ -f "$MANIFEST" ] || { echo "runtime_oracle: 缺基线 $MANIFEST（先 record）" >&2; exit 2; }
        if diff -u "$MANIFEST" "$WORK/current.txt" > "$WORK/diff.txt"; then
            echo "runtime_oracle: OK（$(wc -l < "$MANIFEST") 个场景逐字段一致）"
        else
            echo "runtime_oracle: FAIL 与基线不一致：" >&2
            cat "$WORK/diff.txt" >&2
            exit 1
        fi
        [ "$fail" = "0" ] || exit 1
        ;;
    selfcheck)
        # 再跑一遍：两次必须逐字段一致（证明当前构建是确定的）
        cp "$WORK/current.txt" "$WORK/pass1.txt"
        : > "$WORK/current.txt"
        fail=0
        for scene in $SCENES; do
            [ -f "$scene" ] || continue
            out=$(run_one "$scene") || { fail=1; printf '%s\n' "$out" >> "$WORK/current.txt"; continue; }
            printf '%s\n' "$out" >> "$WORK/current.txt"
        done
        sort -o "$WORK/current.txt" "$WORK/current.txt"
        if diff -u "$WORK/pass1.txt" "$WORK/current.txt" > "$WORK/diff.txt"; then
            echo "runtime_oracle: OK（$(wc -l < "$WORK/pass1.txt") 个场景，两遍逐字段一致）"
        else
            echo "runtime_oracle: FAIL 两遍不一致（非确定性）：" >&2
            cat "$WORK/diff.txt" >&2
            exit 1
        fi
        [ "$fail" = "0" ] || exit 1
        ;;
    *)
        echo "runtime_oracle: 未知模式 '$MODE'（record|check|selfcheck）" >&2
        exit 2
        ;;
esac
