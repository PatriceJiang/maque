#!/usr/bin/env bash
# 小小塔防（scn 组合示例）端到端：
#   ① 菜单 → 第 1 关：独立 menu/level 场景 + 全局脚本常驻 + 静态塔 params（tier）；
#   ② 第 1 关直接运行 → 动态敌人场景（params: hp/path）+ 全局状态（gold/kills/score）
#      → 通关 → global_emit('level_finished') → 全局脚本解锁第 2 关 → 切回菜单；
#   ③ 确定性：同一关两跑 world_trail 一致；
#   ④ 菜单锁定 / 重置进度；
#   ⑤ 第 2/3 关独立装载（场景自己的路径点/预置塔）；
#   ⑥ 3 个塔 + 4 个敌人场景可**单独运行**（demo 模式）并各自 --shot 出图。
# 全程 headless（--shot 也走离屏），无需 DISPLAY。
set -u

PLAYER="$1"
SCENE="$2"
WORK="$3"
ROOT="${SCENE%/*}"
[ -n "$ROOT" ] && [ "$ROOT" != "$SCENE" ] || ROOT="."

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "td_e2e: FAIL $*" >&2; exit 1; }

run_scene() {  # run_scene <log> <frames> <scene> [env...]
  local log="$1" frames="$2" scene="$3"; shift 3
  env "$@" MAQUE_MAX_FRAMES="$frames" "$PLAYER" "$scene" > "$log" 2>&1 || true
}

# ---- ① 菜单 → 第 1 关（键盘 1；全局脚本/关卡/塔/敌人 params 全部就位）----
cat > "$WORK/menu.sim.json" <<'EOF'
{ "name": "td_menu_start", "events": [ { "frame": 5, "type": "key_down", "key": 49 } ] }
EOF
run_scene "$WORK/a.log" 240 "$SCENE" MAQUE_SIM="$WORK/menu.sim.json"
A="$WORK/a.log"
grep -q 'td_menu ready unlocked=1' "$A" || fail "菜单未就绪/初始解锁数不对"
grep -q 'td_menu start level=1' "$A" || fail "按 1 未启动第 1 关"
grep -q 'td_level start level=1 name="松林小径" waves=3 waypoints=6' "$A" \
  || fail "第 1 关场景数据（波次/路径点）未装配"
grep -q 'td_tower ready type=arrow .*tier=1' "$A" || fail "静态塔 params.tier=1 未生效"
grep -q 'td_tower ready type=arrow .*tier=2' "$A" || fail "第二座箭塔 params.tier=2 未生效"
grep -q 'td_tower ready type=cannon' "$A" || fail "炮台场景未实例化"
grep -q 'td_wave wave=1 total=3' "$A" || fail "第 1 波未开始"
grep -q 'td_enemy ready type=slime .*uid=0 hp=26 speed=30 path=6' "$A" \
  || fail "动态敌人未拿到父场景 params（hp/speed/path）"
grep -q 'td_global ready .*menu=1' "$A" || fail "全局脚本未在菜单 on_start"
grep -q 'td_global ready .*menu=0' "$A" || fail "全局脚本未在关卡 on_start"
grep -q 'td_global hud' "$A" || fail "全局 HUD 未创建"
[ "$(grep -c "global script 'global.js' active" "$A")" = "1" ] \
  || fail "全局脚本应只加载一次（跨场景保留）"

# ---- ② 直接运行第 1 关 → 通关 → 全局脚本解锁并切回菜单 ----
run_scene "$WORK/b.log" 2300 "$ROOT/level1.scn"
B="$WORK/b.log"
grep -q 'td_enemy_dead .*gold=' "$B" || fail "击杀未写回全局金币"
grep -q 'td_level finish level=1 result=1' "$B" || fail "第 1 关没有通关（result=1）"
grep -q 'td_global result=1 level=1' "$B" || fail "global_emit('level_finished') 未到达全局脚本"
grep -q 'td_global result=1 .*unlocked=2' "$B" || fail "通关后未解锁第 2 关"
grep -q 'td_global return path=.*menu.scn' "$B" || fail "结算后未切回菜单"
grep -q 'td_menu ready unlocked=2' "$B" || fail "回到菜单后解锁状态没有保留"
grep -q 'td_level finish level=1 result=1 .*kills=11' "$B" || fail "第 1 关击杀数不符合预期（11）"

# ---- ③ 确定性：同一关两跑 trail 完全一致 ----
run_scene "$WORK/d1.log" 700 "$ROOT/level1.scn"
run_scene "$WORK/d2.log" 700 "$ROOT/level1.scn"
T1=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/d1.log" | tail -1)
T2=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/d2.log" | tail -1)
[ -n "$T1" ] && [ "$T1" = "$T2" ] || fail "两跑 world_trail 不一致：$T1 != $T2"
S1=$(grep 'td_state ' "$WORK/d1.log" | tail -1)
S2=$(grep 'td_state ' "$WORK/d2.log" | tail -1)
[ -n "$S1" ] && [ "$S1" = "$S2" ] || fail "两跑关卡状态摘要不一致"

# ---- ④ 菜单：锁定关不可进入 / 重置进度 ----
cat > "$WORK/locked.sim.json" <<'EOF'
{ "name": "td_locked", "events": [ { "frame": 5, "type": "key_down", "key": 51 } ] }
EOF
run_scene "$WORK/locked.log" 30 "$SCENE" MAQUE_SIM="$WORK/locked.sim.json"
grep -q 'td_menu locked level=3' "$WORK/locked.log" || fail "未解锁的第 3 关不应可进入"

cat > "$WORK/reset.sim.json" <<'EOF'
{ "name": "td_reset", "events": [ { "frame": 5, "type": "key_down", "key": 82 } ] }
EOF
run_scene "$WORK/reset.log" 30 "$SCENE" MAQUE_SIM="$WORK/reset.sim.json"
grep -q 'td_menu reset unlocked=1 best=0' "$WORK/reset.log" || fail "重置进度未生效"

# ---- ⑤ 第 2/3 关是独立场景（各自路径点/预置塔）----
run_scene "$WORK/l2.log" 150 "$ROOT/level2.scn"
grep -q 'td_level start level=2 name="冰河岔路" waves=4 waypoints=8' "$WORK/l2.log" \
  || fail "第 2 关场景未正确装载"
grep -q 'td_tower ready type=frost' "$WORK/l2.log" || fail "第 2 关缺少冰塔"
run_scene "$WORK/l3.log" 150 "$ROOT/level3.scn"
grep -q 'td_level start level=3 name="王城终局" waves=5 waypoints=8' "$WORK/l3.log" \
  || fail "第 3 关场景未正确装载"
grep -q 'td_tower ready type=cannon .*tier=2' "$WORK/l3.log" || fail "第 3 关塔 params 未生效"

# ---- ⑥ 塔/敌人独立渲染：单独跑 demo + --shot 出图 ----
check_shot() {  # check_shot <scene-rel> <tag> <demo-tag>
  local rel="$1" tag="$2" demo="$3"
  "$PLAYER" --shot "$WORK/$tag.png" --shot-frame 2 "$ROOT/$rel" > "$WORK/$tag.log" 2>&1 || true
  [ -s "$WORK/$tag.png" ] || fail "$rel 单独渲染失败（无截图）"
  [ "$(wc -c < "$WORK/$tag.png")" -gt 100 ] || fail "$rel 截图过小"
  grep -q "$demo" "$WORK/$tag.log" || fail "$rel 未进入独立运行 demo 模式"
}
check_shot "towers/arrow/arrow.scn"   tower_arrow   'td_tower_demo type=arrow'
check_shot "towers/cannon/cannon.scn" tower_cannon  'td_tower_demo type=cannon'
check_shot "towers/frost/frost.scn"   tower_frost   'td_tower_demo type=frost'
check_shot "enemies/slime/slime.scn"  enemy_slime   'td_enemy_demo type=slime'
check_shot "enemies/runner/runner.scn" enemy_runner 'td_enemy_demo type=runner'
check_shot "enemies/tank/tank.scn"    enemy_tank    'td_enemy_demo type=tank'
check_shot "enemies/flyer/flyer.scn"  enemy_flyer   'td_enemy_demo type=flyer'

echo "td_e2e: ok（菜单/关卡组合 · 塔与敌人 params · 全局脚本/状态 · 通关解锁 · 确定性 · 独立渲染）"
exit 0
