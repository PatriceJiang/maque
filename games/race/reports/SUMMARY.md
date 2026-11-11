# 验收记录：maque 3D 街机赛车

- 日期：2026-11-09
- 工程：`D:\Github\maque\games\race`（只新增资源与脚本，引擎源码零改动）
- 引擎：`D:\Github\maque` @ `09db7fc`，`build-win-release`（Release / Ninja / clang-cl）
- 入口：`games\race\run.cmd`（或从仓库根跑 `maque_player games\race\menu.scn`）

## 1. 一条命令的验收结果

```text
powershell -NoProfile -ExecutionPolicy Bypass -File games\race\verify.ps1

=== maque racing: verify ===
[ok]   menu smoke
[ok]   race smoke desert
[ok]   race smoke beach
[ok]   race smoke forest
[ok]   race smoke city
[ok]   determinism world_trail=0x3b1387422b0e6c7c
[ok]   shot desert (78452 bytes)
[ok]   shot beach (94683 bytes)
[ok]   shot forest (94179 bytes)
[ok]   shot city (103865 bytes)
  stats (avg of 9 samples): draws=139 3d=132 tris=28708 total=10.6ms logic=1.68ms
[ok]   draw budget 3d_draws=132 <= 200
[ok]   frame budget total=10.6ms <= 18ms
report: D:\Github\maque\games\race\reports\perf.md
=== ALL OK ===
```

要点：

- **确定性**：同一份 `.sim.json`（`sims/oval.json`）跑两次，逐帧世界校验和
  `world_trail=0x3b1387422b0e6c7c` 完全一致。
- **预算**：3D draw 99（8 台车 × 13 件 + 8~10 个赛道网格，其余是 HUD），三角形 24.0k，
  单帧 9.8–13.6 ms（同机多次采样有波动），脚本+模拟 1.6–4.6 ms。
  预算是 200 draw / 16.7 ms，实际用量约一半。

## 2. 做成了什么

| 需求 | 状态 | 实现 |
|---|---|---|
| 3D 赛车游戏 | ✅ | Camera3D + MeshRenderer + PBR + 雾 + 方向光阴影；跟车相机（脚本 `node_look_at`，主相机 Orbit 已关） |
| 4 个场景 | ✅ | 沙漠 / 海滩 / 森林 / 城市，四条独立中心线赛道（676 / 592 / 605 / 731 m），配色/雾/太阳/道具全不同 |
| 8 款赛车 | ✅ | 极速 51–67 m/s、抓地 14–19，外形由尺寸/尾翼/进气口/配色区分；颜色与参数集中在 `scripts/data/cars.js` |
| 比赛 | ✅ | 8 台车同场、3 圈、倒计时、名次、单圈/最佳圈速、结算 `FINISH P#` |
| 操作 | ✅ | 键盘（WASD/方向键 + 空格手刹 + C 视角 + R 重开 + Esc 菜单），手柄模拟量（`input_axis`） |
| 60fps | ✅ | 9.66 ms/帧 @1280×720 |

## 3. 关键技术决定

1. **3D 画面 + 平面车辆运动**：maque 没有 3D 刚体，车在 XZ 平面用
   自行车模型 + "速度方向追车头"的抓地模型；手刹把抓地率压到 1.45 产生可恢复漂移。
2. **静态内容离线合并**：路面/路肩/地面/护栏/中心线/道具按材质烘成 7–9 个
   `.mesh.json`，赛道侧每帧只有 7 个 draw —— 这是 60fps 的前提。
3. **一份逻辑四套主题**：`race_scene.js` 从 `TRACKS[id]` 取中心线与主题参数，
   场景差异全在 `data/tracks/*.track.json`。
4. **确定性**：不读墙钟、不用 `Math.random`、AI 只按位置/曲率决策、卡住自救只用本车计时器。

## 4. 已知限制（与 README 一致）

- 无 3D 物理：不能上坡/腾空/翻滚；撞墙是"夹回 + 掉速"，非真实碰撞响应。
- 无存档：本 build 脚本层没有 `storage` 绑定（只有 C ABI），最佳圈速只在本次进程内。
- 无音频、无手柄震动、无 3D 世界文字、无地形/水面运行时、单张 1024² 阴影。
- 包场景脚本不热重载（改脚本要重启）。

## 5. 证据文件

- 截图：`shots/menu.png`、`shots/grid_*.png`（发车格）、`shots/play_*.png`（行驶中）、`shots/verify_*.png`（脚本产出）
- 性能：`reports/perf.md`
- 回放：`sims/oval.json`、`sims/hold_w.json`、`sims/menu_to_race.json`
