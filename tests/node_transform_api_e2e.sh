#!/usr/bin/env bash
# 3D 变换 typed API 端到端（plans/27 M1-4）：
#   ① 值类型返回值 = JS {x,y,z}；局部/世界位置读写（父链 yaw/scale 参与）
#   ② node_set_local_position 之后立即读 world（不等下一帧 update_world3d）
#   ③ localToWorld / worldToLocal 互逆；值参数既有对象也有"3 个数字"写法
#   ④ node_look_at：本地 -Z 指目标；在旋转父节点下补偿出正确的世界朝向
#   ⑤ 与反射路径（node_get_number("local.position.x")）读数一致；无组件节点自动补挂
# 纯脚本/场景逻辑，不需要 GPU；MAQUE_HEADLESS=1。
set -u
. "$(dirname "$0")/maque_python.sh"
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"

PLAYER="$1"
SCENE="$2"
WORK="$3"
LUA_SCENE="${4:-}"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1
unset MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_IMGUI 2>/dev/null || true

fail() { echo "node_transform_api_e2e: FAIL $*" >&2; exit 1; }

timeout 120 "$PLAYER" --frames 4 "$SCENE" > "$WORK/run.log" 2>&1
RC=$?
[ "$RC" -eq 0 ] || { cat "$WORK/run.log"; fail "player exit=$RC"; }

if grep -q "transform_api: FAIL" "$WORK/run.log"; then
    grep "transform_api: FAIL" "$WORK/run.log" >&2
    fail "脚本自检失败"
fi
grep -q "transform_api: PASS" "$WORK/run.log" || { cat "$WORK/run.log"; fail "没有 transform_api: PASS"; }

# 反射路径与 typed API 必须指向同一份存储（脚本已断言，这里再确认脚本确实跑了）
grep -q "\[game\] transform_api: PASS" "$WORK/run.log" || { cat "$WORK/run.log"; fail "脚本未执行"; }

# Lua 侧值类型（table）与同一批动词（plans/36）：单独场景/脚本，避免混语言干扰。
if [ -n "$LUA_SCENE" ] && [ -f "$LUA_SCENE" ]; then
    timeout 120 "$PLAYER" --frames 4 "$LUA_SCENE" > "$WORK/lua.log" 2>&1
    RC=$?
    [ "$RC" -eq 0 ] || { cat "$WORK/lua.log"; fail "lua player exit=$RC"; }
    grep -q "\[game\] transform_api_lua: PASS" "$WORK/lua.log" \
        || { cat "$WORK/lua.log"; fail "Lua 值类型/变换 API 自检失败"; }
fi

echo "node_transform_api_e2e: PASS（JS/Lua 局部/世界读写、lookAt 父链补偿、localToWorld 互逆、值类型编组）"
