#!/usr/bin/env bash
# 节点/组件 API 端到端（docs/18）：层级 / 节点属性 / 组件 / 事件 / UI 控件 / 物理组件 / Director。
#   ① 节点层级：setParent、childCount、childAt、getChildByName
#   ② 节点属性：scale/angle/opacity/zIndex/contentSize（写回组件，非仅缓存）
#   ③ 组件：addComponent/getComponent(has)/property 读写/componentCount/componentAt
#   ④ 事件：node.on 注册 → 引擎按名字回调；node.emit 沿父链冒泡
#   ⑤ UI 语义：VBox 布局排布、ProgressBar 值、Slider 拖拽改值、CheckBox 点击切换
#   ⑥ 物理组件：RigidBody2D + Collider2D 自动步进并回写变换
#   ⑦ Director：控制面 pause/resume（暂停期间脚本 update 不再执行）
#   ⑧ 确定性：同输入两跑 world_trail 一致
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）

PLAYER="$1"
SCENE="$2"
WORK="$3"

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "nodes_e2e: FAIL $*" >&2; exit 1; }

# ---- 第一跑：驱动交互（滑块拖拽 + 复选框点击 + Director 暂停）----
run() {   # $1=out file
  printf '%s\n' \
   '{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":2}}' \
   '{"jsonrpc":"2.0","id":2,"method":"Scene.entities"}' \
   '{"jsonrpc":"2.0","id":3,"method":"Director.pause"}' \
   '{"jsonrpc":"2.0","id":4,"method":"Runtime.step","params":{"frames":5}}' \
   '{"jsonrpc":"2.0","id":5,"method":"Director.resume"}' \
   '{"jsonrpc":"2.0","id":6,"method":"Runtime.step","params":{"frames":1}}' \
   | "$PLAYER" "$SCENE" 2>&1 > "$1.part" &
  PID=$!
  # 等实体表就绪后取滑块/复选框坐标，注入交互事件
  sleep 0.4
  COORDS=$(grep -F '"id":2,"result"' "$1.part" | head -1)
  "$PLAYER" --version >/dev/null 2>&1 || true
  wait $PID 2>/dev/null || true
  mv "$1.part" "$1"
}

# 交互需要用控制面在同一进程内完成：改用「先读坐标 → 再注入」的两段式不可行，
# 因此这里用一条流水线：先 step 让脚本建树，再用 Scene.entities 的坐标注入事件。
cat > "$WORK/req.txt" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":2,"method":"Scene.entities"}
{"jsonrpc":"2.0","id":3,"method":"Scene.info"}
EOF
MAQUE_CONTROL=- MAQUE_MAX_FRAMES=4 "$PLAYER" "$SCENE" < "$WORK/req.txt" > "$WORK/coords.txt" 2>&1 || true
ENT=$(grep -F '"id":2,"result"' "$WORK/coords.txt" | head -1)
[ -n "$ENT" ] || fail "could not read entity table"
SCREEN_H=$(grep -F '"id":3,"result"' "$WORK/coords.txt" | head -1 | "$PY" -c "
import json,sys
d=json.JSONDecoder().raw_decode(sys.stdin.read().split('\"result\":',1)[1])[0]
print(int(d.get('height', 240)))")

pos() { printf '%s' "$ENT" | "$PY" -c "
import json,sys
txt=sys.stdin.read().split('\"result\":',1)[1]
d,_=json.JSONDecoder().raw_decode(txt)          # 行内可能含后续输出 → 只解第一个 JSON
name=sys.argv[1]
for e in d:
    if e['name']==name: print(f\"{e['x']:.1f} {e['y']:.1f}\"); break
else: print('')" "$1"; }
VOL=$(pos Volume); MUTE=$(pos Mute)
[ -n "$VOL" ] && [ -n "$MUTE" ] || fail "Volume/Mute not found"
# 输入事件用屏幕坐标（y 向下）；节点 y 为世界坐标（y 向上）→ y_screen = H - y_world
VX=$(echo "$VOL" | cut -d' ' -f1); VY=$(echo "$VOL" | cut -d' ' -f2)   # 注入坐标 = 世界坐标（y 向上）
MX=$(echo "$MUTE" | cut -d' ' -f1); MY=$(echo "$MUTE" | cut -d' ' -f2)

# 滑块拖拽：按下（左端偏左）→ 移动到（右端偏右）→ 抬起；复选框：点击
VX_L=$("$PY" -c "print(f'{$VX-50:.1f}')")
VX_R=$("$PY" -c "print(f'{$VX+60:.1f}')")
cat > "$WORK/req2.txt" <<EOF
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":2,"method":"Input.inject","params":{"type":0,"x":$VX_L,"y":$VY,"button":0}}
{"jsonrpc":"2.0","id":3,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":4,"method":"Input.inject","params":{"type":2,"x":$VX_R,"y":$VY,"button":0}}
{"jsonrpc":"2.0","id":5,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":6,"method":"Input.inject","params":{"type":1,"x":$VX_R,"y":$VY,"button":0}}
{"jsonrpc":"2.0","id":7,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":8,"method":"Input.inject","params":{"type":0,"x":$MX,"y":$MY,"button":0}}
{"jsonrpc":"2.0","id":9,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":10,"method":"Input.inject","params":{"type":1,"x":$MX,"y":$MY,"button":0}}
{"jsonrpc":"2.0","id":11,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":12,"method":"Director.pause"}
{"jsonrpc":"2.0","id":13,"method":"Runtime.step","params":{"frames":5}}
{"jsonrpc":"2.0","id":14,"method":"Director.resume"}
{"jsonrpc":"2.0","id":15,"method":"Runtime.step","params":{"frames":1}}
{"jsonrpc":"2.0","id":16,"method":"Director.status"}
{"jsonrpc":"2.0","id":17,"method":"Runtime.step","params":{"frames":45}}
{"jsonrpc":"2.0","id":18,"method":"World.trail"}
EOF
MAQUE_CONTROL=- MAQUE_MAX_FRAMES=60 "$PLAYER" "$SCENE" < "$WORK/req2.txt" > "$WORK/out.txt" 2>&1 || true

SUMMARY=$(grep -o 'nodes_summary .*' "$WORK/out.txt" | head -1)
[ -n "$SUMMARY" ] || fail "script summary not found (scene did not reach frame 40)"
TRAIL1=$(grep -F '"id":18,"result"' "$WORK/out.txt" | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
STATUS=$(grep -F '"id":16,"result"' "$WORK/out.txt" | head -1)

"$PY" - "$SUMMARY" "$STATUS" <<'PY' || exit 1
import json, sys
raw = sys.argv[1][len('nodes_summary '):]
d = json.loads(raw)
st = json.JSONDecoder().raw_decode(sys.argv[2].split('"result":', 1)[1])[0] if sys.argv[2] else {}
bad = []
def ck(name, cond, got=None):
    if not cond: bad.append(f"{name} (got {got})")

ck('层级：Panel 有 3 个子节点', d.get('panel_children') == 3, d.get('panel_children'))
ck('层级：Canvas 有 2 个子节点', d.get('canvas_children') == 2, d.get('canvas_children'))
ck('层级：parent(bar)==panel', d.get('parent_of_bar') == d['ids']['panel'], d.get('parent_of_bar'))
ck('层级：childAt(panel,0) 命中', d.get('child0_of_panel') == d['ids']['bar'], d.get('child0_of_panel'))
ck('层级：getChildByName(Volume)', d.get('find_child') == d['ids']['volume'], d.get('find_child'))
p = d.get('spawned_props') or []
ck('节点属性：scale/angle/opacity/z/contentSize', p[:4] == [1.5, 30, 0.5, 3] and p[4] > 39 and p[5] > 19, p)
ck('组件：addComponent 成功', d.get('has_sprite') == 1, d.get('has_sprite'))
ck('组件：property 读写（radius=9）', d.get('sprite_radius') == 9, d.get('sprite_radius'))
ck('组件：componentCount/At', d.get('component_count', 0) >= 1 and d.get('component0') == 'scn.Sprite', (d.get('component_count'), d.get('component0')))
ck('UI：ProgressBar 初值 0.25', abs(d.get('bar_value', 0) - 0.25) < 1e-6, d.get('bar_value'))
ck('UI：ProgressBar 推进', d.get('bar_value_end', 0) > 0.6, d.get('bar_value_end'))
ck('UI：CheckBox 初始未选中', d.get('mute_checked') == 0, d.get('mute_checked'))
lay = d.get('layout_after') or [0, 0]
ck('UI：VBox 布局排布（子节点 y 递增）', lay[1] > lay[0] > 0, lay)
ck('物理组件：RigidBody2D 下落', (d.get('fall_y_40', 0) - d.get('fall_y_3', 0)) > 50, (d.get('fall_y_3'), d.get('fall_y_40')))
ck('事件：Slider changed 已派发', (d.get('slider_events') or 0) >= 1, d.get('slider_events'))
ck('事件：Slider 值被拖拽改变', abs((d.get('slider_value') or 5.0) - 5.0) > 0.5, d.get('slider_value'))
ck('事件：CheckBox toggled 已派发', (d.get('mute_events') or 0) >= 1, d.get('mute_events'))
ck('事件：CheckBox 已切换为选中', d.get('muted') in (True, 1), d.get('muted'))
eng, scr = d.get('engine_frame_end', 0), d.get('frames', 0)
ck('Director：暂停期间脚本 update 未执行', eng - scr >= 4, (eng, scr))
ck('Director：恢复后仍在跑', st.get('paused') is False, st.get('paused'))
ck('Tween：半径插值到目标值', abs(d.get('tween_radius', 0) - 20.0) < 0.01, d.get('tween_radius'))
ck('Tween：完成后自动移除', d.get('tween_left') == 0, d.get('tween_left'))
ck('Tween：完成事件已派发', (d.get('tween_done') or 0) >= 1, d.get('tween_done'))
ck('scheduleOnce：只触发一次', d.get('timer_once') == 1, d.get('timer_once'))
ck('schedule：周期性触发', (d.get('timer_repeat') or 0) >= 2, d.get('timer_repeat'))
ck('unschedule：已移除定时器', d.get('schedules_before_unschedule') == 1 and d.get('schedules_after_unschedule') == 0,
   (d.get('schedules_before_unschedule'), d.get('schedules_after_unschedule')))
ck('find(path)：按路径命中节点', d.get('found_path') == d['ids']['volume'], d.get('found_path'))
ck('destroy：调用时仍有效（帧末销毁）', d.get('valid_right_after_destroy_call') == 1, d.get('valid_right_after_destroy_call'))
ck('destroy：帧边界后已销毁', d.get('destroyed') == 1, d.get('destroyed'))
if bad:
    print('nodes_e2e: FAIL ' + '; '.join(bad), file=sys.stderr)
    sys.exit(1)
print(f"nodes_e2e: 语义全部通过（children={d.get('panel_children')} layout={lay} "
      f"fall={d.get('fall_y_3')}→{d.get('fall_y_40')} slider->{d.get('slider_value'):.2f} "
      f"muted={d.get('muted')} paused_skipped={eng-scr})")
PY

# ---- 确定性：同输入两跑一致 ----
MAQUE_CONTROL=- MAQUE_MAX_FRAMES=60 "$PLAYER" "$SCENE" < "$WORK/req2.txt" > "$WORK/out2.txt" 2>&1 || true
TRAIL2=$(grep -F '"id":18,"result"' "$WORK/out2.txt" | grep -o '"trail":"[0-9]*"' | grep -o '[0-9]*')
[ -n "$TRAIL1" ] && [ "$TRAIL1" = "$TRAIL2" ] || fail "determinism broken: $TRAIL1 != $TRAIL2"

echo "nodes_e2e: ok (trail=$TRAIL1)"
exit 0
