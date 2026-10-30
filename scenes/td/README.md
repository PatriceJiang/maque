# 小小塔防（Tiny Tower Defense）

用一个小游戏验证 **`.scn` 组合机制**：场景 = 节点树 + 行为，场景可以嵌套场景，
子场景可以接收参数，全局脚本与全局状态跨场景存活。

```
scenes/td/
  menu.scn / menu.js                 # 菜单：选关 / 继续 / 重置
  global.js                          # 全局脚本：HUD + 进度 + 结算 + 全局热键
  level1.scn / level1.js             # 第 1 关：松林小径（3 波，2 箭 1 炮）
  level2.scn / level2.js             # 第 2 关：冰河岔路（4 波，加冰塔/飞行）
  level3.scn / level3.js             # 第 3 关：王城终局（5 波，全兵种）
  towers/arrow/arrow.scn + arrow.js     # 箭塔：单体、快速
  towers/cannon/cannon.scn + cannon.js  # 炮台：慢速、范围伤害
  towers/frost/frost.scn + frost.js     # 冰塔：低伤害、减速
  enemies/slime/slime.scn + slime.js    # 史莱姆：慢速、低血
  enemies/runner/runner.scn + runner.js # 疾跑者：快速、低血
  enemies/tank/tank.scn + tank.js       # 重甲：慢速、高血
  enemies/flyer/flyer.scn + flyer.js    # 飞行者：直线飞向终点
```

## 运行

```bash
# 整个游戏（菜单 → 关卡）
./build/samples/player/maque_player scenes/td/menu.scn
# 单独跑某一关（不经过菜单；关卡会自己初始化金币/生命）
./build/samples/player/maque_player scenes/td/level1.scn
# 单独渲染一个塔/敌人（demo 模式：塔旋转、敌人绕圈）
./build/samples/player/maque_player scenes/td/towers/arrow/arrow.scn
./build/samples/player/maque_player scenes/td/enemies/slime/slime.scn
# 截图
./build/samples/player/maque_player --shot td.png --shot-frame 300 scenes/td/level1.scn
```

操作：`1/2/3` 选关（需先解锁）、`Enter` 继续最高关、`R` 重开当前关 / 在菜单重置进度、
`Esc` 返回选关。塔自动索敌开火；敌人到终点扣 1 生命；生命归零失败，打完所有波次通关。

## 组合关系（实现要点）

| 机制 | 用法 |
|------|------|
| 静态嵌套 | 关卡 `.scn` 里的 `scn.SceneInstance{source:"towers/..."}` 预置塔 |
| 实例参数 | `scn.SceneInstance.params` / `scene_instantiate(..., params)` → 子场景 `scene_self_params()` |
| 动态实例化 | 关卡脚本 `scene_instantiate(-1, "enemies/...", params)` 生成敌人（帧边界构建） |
| 子 → 父事件 | `scene_emit(scene_instance_parent(scene_self()), "on_enemy_dead", uid)` |
| 塔 → 敌事件 | `scene_emit(enemy_inst, "on_damage", dmg)` / `"on_slow"` |
| 实例扫描 | 塔用 `scene_instance_count/at/source/root` 找 `/enemies/` 实例并读挂载壳位置 |
| 全局脚本 | `scn.Scene.global_script: "global.js"`，跨 `director_load_scene` 保留 |
| 全局状态 | `global_set/get/set_text/get_text/...`，所有 VM 共享；金币/生命/波次/进度 |
| 全局事件 | 关卡 `global_emit('level_finished', 1\|2)` → 全局脚本解锁/结算/切回菜单 |
| 生命周期 | `on_start`（子先父）/ `update`（父先子）/ `on_destroy`（子先父），宏/敌各自独立 VM |

`docs/32-场景实例与事件作用域.md` §16 是这些机制的权威说明。
回归：`ctest -R td_e2e`。
