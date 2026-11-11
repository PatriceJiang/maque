# maque 3D 街机赛车（4 场景 / 8 车）

用 [maque](https://github.com/PatriceJiang/maque) 引擎做的低多边形 3D 街机赛车：
**8 台车 · 4 条赛道 · 3 圈**，键盘（可选手柄）操作，60fps。

> 引擎只当"浏览器"用：没有任何 C++ 改动，全部是 `.scn` + `.js` + 生成的 `.mesh.json`。

## 跑起来

```powershell
cd D:\Github\maque
# 1) 首次/改过引擎源码：先构建（本工程依赖 plans/36 的 typed 3D 变换与碰撞查询）
cmake --build build-win-release

# 2) 启动（工程根 = games\race，场景路径相对仓库根）
.\build-win-release\samples\player\maque_player.exe games\race\menu.scn
# 或者双击 / 命令行跑这条（内部会 cd 到仓库根）：
games\race\run.cmd
```

> **必须从仓库根目录启动**：引擎按 **cwd** 解析默认字体，换目录会退化成系统字体——
> 实测同一帧从 `logic 3ms` 掉到 `logic 20ms+`（20→45fps）。`run.cmd` 已经处理好这件事。

无窗口出图（CI/回归）：

```powershell
.\build-win-release\samples\player\maque_player.exe --shot games\race\shots\out.png --shot-frame 200 games\race\race_desert.scn
```

## 操作

| 键 | 作用 |
|---|---|
| `W` / `↑` | 油门 |
| `S` / `↓` | 刹车 / 倒车 |
| `A` `D` / `←` `→` | 转向 |
| `Space` | 手刹（甩尾） |
| `C` | 切换第三人称 / 车头视角 |
| `R` | 重开本场 |
| `Esc` | 回主菜单 |

手柄（有则自动生效）：左摇杆转向，`LT` 刹车，`RT` 油门。
`MAQUE_GAMEPAD="lx,ly,rx,ry,lt,rt"` 可注入模拟量（CI 用）。

菜单：`↑↓` 选择，`←→` 在"车/赛道"两列间切换，`Enter` 开始，`Esc` 退出。

## 内容

| 赛道 | 长度 | 宽度 | 特征 |
|---|---:|---:|---|
| Dune Run（沙漠） | 676 m | 16 m | 长弧 + 开阔视野，最宽最好开 |
| Surf Line（海滩） | 592 m | 13 m | 临水窄道，连续弯 |
| Cedar Loop（森林） | 605 m | 12 m | 树墙夹道，雾最重 |
| Neon Grid（城市） | 731 m | 11 m | 夜间霓虹、直角弯、最长 |

8 台车（`scripts/data/cars.js`）：VIPER GT / COMET RS / BANDIT V8 / DUNE BUGGY /
KITE TURBO / HOUND 4WD / NOVA EV / BRUTE PU —— 极速 51–67 m/s，抓地 14–19，外形由
尺寸/尾翼/进气口/配色区分，全部用引擎程序化 Cube + 一个共享轮子网格拼出来。

## 结构

```text
games/race/
  maque.package.json          # 包清单（全 JS + ESM）
  run.cmd                     # 启动器（cd 到仓库根再跑，避开字体/cwd 坑）
  menu.scn                    # 主菜单（2D）
  race_<theme>.scn            # 4 个比赛场景（生成）：相机/太阳/补光/赛道网格
  themes/<theme>.js           # 3 行胶水：boot('<theme>')
  scripts/
    menu.js                   # 菜单逻辑
    race_scene.js             # 比赛运行时（物理/AI/相机/HUD/圈速）——唯一一份逻辑
    data/cars.js              # 8 车数据表
    gen/tracks.js             # 生成：采样后的中心线/法线/曲率/主题参数
    lib/{color,ui}.js
  assets/tracks/<theme>/*.mesh.json   # 生成：路面/路肩/地面/护栏/中心线/道具/水
  assets/shared/wheel.mesh.json       # 生成：轮子（轴沿 X，脚本只需绕 X 滚）
  data/tracks/<theme>.track.json      # 手写：中心线控制点 + 配色 + 太阳/雾/环境 + 道具规则
  tools/gen_track.mjs         # 生成器（确定性；改数据后重跑）
  sims/*.json                 # 输入回放脚本
  shots/                      # 截图产物
  reports/perf.md             # 性能实测记录
  verify.ps1                  # 一条命令跑完验收
```

改内容只改这两处，然后重新生成：

```powershell
cd D:\Github\maque\games\race
node tools/gen_track.mjs      # → assets/**、scripts/gen/tracks.js、race_<theme>.scn、themes/*.js
```

## 实现要点（为什么这样做）

- **3D 画面，平面车辆运动**：maque 没有 3D 刚体物理，所以车在 XZ 平面上做
  自行车模型 + 速度方向追赶车头的抓地模型（`gripRate`），手刹把 `gripRate` 压到 1.45
  就能做出可恢复的漂移。赛道约束用中心线闭式解（`lat` 夹回 + 速度方向掰回切线），
  不做物理引擎碰撞。
- **静态内容全部合并**：引擎 3D 是逐实体一次 draw。路面/路肩/地面/护栏/中心线/道具
  在离线生成时按材质烘成 6–8 个 `.mesh.json`，运行时每套场景只有 7–9 个赛道 draw。
- **车辆也是数据**：8 台车 × 6–8 个 Cube/轮子，颜色/尺寸/尾翼/进气口来自 `cars.js`。
- **一份逻辑四套主题**：`race_scene.js` 按 `TRACKS[id]` 取中心线与主题参数；
  场景差异（配色/雾/太阳/道具/水面）全在数据里。
- **确定性**：只读 `update(dt)`，不用墙钟与 `Math.random`；AI 只按位置/曲率决策，
  卡住自救只用本车计时器。同一 `.sim.json` 两次运行 `world_trail` 一致。

## 灯光与材质

**车漆**（每台车 3 种材质 + 自发光小件）：

| 部件 | 参数 |
|---|---|
| 车身 | `metallic 0.30` / `roughness 0.35` / **`clearcoat 0.72` + `clearcoat_roughness 0.22`** / `env_intensity 1.4`（对齐 threejs-impl） |
| 座舱玻璃 | `#07131c` / `metallic 0.72` / `roughness 0.09` / `clearcoat 1.0` |
| 进气口 / 尾翼 | `metallic 0.65–0.70` / `roughness 0.28–0.30` / `clearcoat 0.6` |
| 前灯 ×2 / 尾灯 ×1 | `emissive_intensity 2.2 / 1.6`（自发光，不占 8 盏灯预算） |
| 轮胎 / 轮毂 | 胎 `#111214` `roughness 0.92`；轮毂独立网格 `rim.mesh.json`（`metallic 0.85` / `roughness 0.28`，颜色按车） |

**场景灯光**：每套主题 1 盏方向光（软阴影 `shadow_mode=2`，90m 拟合）+ 1 盏冷色补光 +
**天空盒 IBL**（`env_texture` 等距柱状 PNG，引擎做 PMREM 预过滤）+ 场景雾；相机带独立 `exposure`
（森林 1.05 / 海滩 1.02 / 城市 1.10）。太阳角度按主题走：沙漠暖白、海滩金橙、
森林冷绿漫射、城市黄昏品红。城市楼体窗带用 `emissive_intensity=1.8` 做霓虹。

**天空盒**（`tools/gen_tex.mjs` 程序化生成，1024×512 等距柱状 PNG）：

- 每套主题一份 `assets/sky/<id>.png`：天顶→地平线的渐变 + 太阳盘与辉光 + 云（fbm）+ 星星（城市）；
- 场景里 `env_texture` + `env_background: true` + 相机 `clear_mode: 0`（skybox），
  同一张图同时用于**背景**和 **IBL**（引擎日志：`env PMREM ... 11 mips + irradiance 64x32`，约 0.8s 启动成本）；
- 不是静态图：颜色来自 `data/tracks/<id>.track.json` 的 `sky` 段，改数据重跑生成器即可。

**路面起伏**：

- 数据：`elevation: { amp, waves: [[freq, weight, phase], ...] }`，`freq` 取整数 → 闭环；
  `h(s) = amp · Σ w·sin(2π·f·s/L + φ)`，生成器采样成 `h[]` 写进 `scripts/gen/tracks.js`；
- 网格：路面/路肩/护栏/地面/路灯/道具全部按 `h` 抬升；地面改成**跟随高程的条带**
  （宽度自动夹到 0.72 / 最大曲率，否则内侧会自我折叠出摩尔纹）；法线按高度场解析求，
  逐顶点写入，避免每 2m 一条亮带；
- 物理：`c.y = h[idx]`，`slope = Δh/step`，直接当重力分量 `a -= 9.81·slope`（上坡掉速、下坡加速）；
  车体按 `atan(slope)` 俯仰，相机高度与注视点都跟随 `c.y`。

**路面材质**（`tools/gen_tex.mjs` 程序化 + 真 UV）：

- 路面/地面/路肩都带 UV：`u = 横向偏移 / uvScale`、`v = 沿赛道里程 / uvScale`（路面 6m 一轮、地面 10m）；
- 贴图是**亮度图**（均值 0.9），乘在 `base_color` 上只提供颗粒 → 换主题只改调色板，不用重画贴图；
- 沥青：多尺度周期噪声 + 补丁 + 麻点；地面：按主题色调的噪声；
- 周期化噪声（晶格取模）保证无缝平铺，否则每 6m 一条接缝。

**阴影**：这个 build 的网格不投实时阴影（实测 `cast_shadow` 对 MeshRenderer 不生效），所以：

- 每台车脚下挂一块半透明暗面（`alpha_mode=Blend`, `opacity 0.42`）当接地阴影；
- 生成器给每个道具/楼体在脚下烘一块 **假 AO**（`trk_ao`，颜色 = 地面色 ×0.62，
  一个 draw 覆盖全部道具），换回整片场景的落地感。

**夜景（城市）**：

- **路灯**：生成器沿赛道每 26m 左右交替摆一根灯杆（`trk_pole`，合并成 1 个 draw），
  灯头进 `props_b`（自发光），灯下再加一块暖色 alpha 光斑（`trk_pool`，`opacity 0.3`，
  同样只占 1 个 draw）。不用真实点光——8 盏灯根本铺不满一条街。**间距 18m、左右交替**，
  跑起来是连续的光带而不是零星几盏。
- **楼体基础光**：除了高层窗带，每栋楼底部加一条 1.1m 高的"商业灯带"+ 一条 0.3m 腰线，
  随机再挂 1 条竖窗带；`emissive_intensity` 按主题走（城市 2.0）。
- **夜间环境光**：`env_texture`（夜景天空）的 IBL 强度提到 **1.25**，再加一盏冷色补光
  （`fill: 0.75`）——夜里照样有环境光，楼体不会被压成纯黑剪影。
- **车灯**：每台车前 2 白后 1 红都是自发光小件；玩家车额外挂一块梯形 **headlight pool**
  （`assets/shared/beam.mesh.json`，`emissive` + `alpha 0.26`，1 个 draw），向车头铺 20m。
  spot light 在动态节点上实测无效（改强度画面不变），所以光斑走贴地面片。
- 天空：`sky` 段现在是紫调渐变 + 星点（`stars 0.03`），地平线有城市辉光；
  雾 `#302742`、太阳低角度品红、路面提亮到 `0.36 sRGB`。

## 验收

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\Github\maque\games\race\verify.ps1
```

跑完会打印 `[ok]/[FAIL]` 清单并写 `reports/perf.md`（含 draw/tris/帧时间）。

## 已知限制

- **没有 3D 物理**：车辆是 2.5D 街机模型，不能上坡/腾空/翻滚；撞墙是"夹回 + 掉速"，
  不是真实碰撞响应。
- **没有存档**：本 build 的脚本层没有 `storage` 绑定（只有 C ABI），最佳圈速只在
  本次进程内（跨场景保留，重启丢失）。
- **没有手柄震动 / 力反馈**；没有 3D 世界空间文字（HUD 是 2D 叠层）。
- **没有地形/水面运行时**：地面是平面 + 合并网格，海水是一块平面（靠环境色出高光），
  没有反射与折射。
- **没有实时阴影**：本 build 的 MeshRenderer `cast_shadow` 实测不产出阴影（光源级
  `shadow_mode` 有效，网格级被忽略），所以用"车底暗面 + 道具脚下假 AO"替代；
  没有阴影级联、没有接触硬化。
- **没有音频**：本次未接音效（引擎音频是 WAV/OGG，需要自备素材），HUD 里有
  `MAQUE_AUDIO=off` 的空实现。
- **包场景不热重载**：改 `scripts/**` 要重启进程。
