# Tank Duel（samples/tank）

俯视 2D 坦克对战（World of Tanks 的轻量版）：驾驶车体、独立转向的炮塔、指针瞄准、波次敌人 AI、
炮弹池化、爆炸粒子、履带印、血条与 HUD。**玩法全部在资源里**，宿主只有 37 行。

工具链角色与 `samples/flappy` 完全一致 —— 那个工程验证过的解耦模式，这里原样再来一遍
（并且顺手把它没覆盖到的能力压出来了：多实体层级、UI 控件、键盘输入、更高的每帧实体数）。

```
samples/tank/
├── main.cpp         宿主：只调 mq_run_scene()，不含任何游戏概念
├── tank.scn         场景：地面/土斑/木箱 + 我方坦克（车体+炮塔+炮管）+ 5 辆敌坦 + 履带印池 + 残骸 + HUD
├── tank.js          玩法：驾驶/瞄准/开火/敌人 AI/炮弹池/命中判定/特效/波次/计分/HUD/测试钩子
├── art/             素材：PNG + 材质 JSON（程序化生成，见 tools/make_tank_art.py）
├── CMakeLists.txt   目标 + 3 个 CTest（tank_smoke / tank_e2e / tank_kill）
└── shot_*.png       截图自证（开局 / 开火中 / 击毁后 / 阵亡 / 履带印 / 指针瞄准）
```

## 跑起来

```bash
# 从仓库根目录跑（场景目录会被挂到 VFS 的 /，所以素材按 /art/xxx 引用）
maque_player samples/tank/tank.scn          # 用通用宿主
maque_player samples/tank/tank.scn            # 用本工程宿主（等价）

# 操作：W/S 前进后退、A/D 原地转车体、指针瞄准炮塔、左键按住或空格开火
```

无头冒烟 / 截图（CI、无显示器）：

```bash
MAQUE_HEADLESS=1 maque_player --shot /tmp/t.png --shot-frame 3 samples/tank/tank.scn
MAQUE_TANK_AUTOPLAY=1 maque_player --shot /tmp/t.png --shot-frame 300 samples/tank/tank.scn
MAQUE_TANK_AUTOPLAY=1 MAQUE_TANK_TRACE=1 maque_player --frames 900 samples/tank/tank.scn   # 每 60 帧打一行战况
```

改玩法不用重编译：`tank.js` 顶部就是全部手感常量；开 `MAQUE_WATCH=1` 还能热重载脚本，
`node_set_number(id,'scn.Sprite',…)` 之类的运行期改动不需要重新构建引擎。

## 解耦约定（本工程存在的意义）

1. **宿主不认识游戏**：`main.cpp` 只做参数解析 + `mq_run_scene()`，链接的**唯一**引擎目标是
   `maque::runtime`。它的源码里没有"坦克/炮弹/敌人"任何一个词。
2. **玩法不编译进引擎**：改射速、伤害、AI 视野、波次难度、HUD 文案，只改 `tank.js`。
3. **素材属于游戏**：`art/` 全部程序化生成（`python tools/make_tank_art.py`），
   引擎不认识任何一张图。新增素材 = 放 PNG + 写一行材质 JSON + 在 `.scn` 里引用。
4. **场景是数据的唯一真相**：出生点、掩体矩形、履带印池大小、HUD 布局都写在 `tank.scn` 里，
   脚本启动时**读场景**（`entity_find` + `entity_x/y` + `node_get_number`），不在代码里复制一份。
   例：掩体列表来自 `crate1..5` 的坐标与 sprite 宽高；出生点来自 5 辆敌坦的初始位置。

## 玩法参数（都在 tank.js 顶部）

| 常量 | 值 | 作用 |
|---|---|---|
| `P_MOVE` / `P_BACK` | 132 / 78 px/s | 前进 / 倒车极速 |
| `P_TURN` | 150 °/s | 车体原地转向 |
| `P_TURRET_TURN` | 280 °/s | 炮塔转向（独立于车体） |
| `B_SPEED` / `P_DMG` | 360 px/s / 20 | 玩家炮弹速度 / 伤害（敌坦 40 血 → 2 发） |
| `P_FIRE_CD` / `P_MAX_LIVE` | 0.42 s / 6 | 冷却 / 同时在飞的炮弹上限 |
| `E_BASE_HP` / `E_HP_PER_WAVE` | 40 / +12 | 敌坦血量 + 每波递增（难度曲线） |
| `E_SIGHT` / `E_RANGE` | 300 / 320 px | 发现距离 / 开火距离 |
| `E_FIRE_CD` | 1.5 s + rng 抖动 | 敌方射速 |
| `E_DMG` / `E_EB_SPEED` | 10 / 250 | 敌方炮弹伤害 / 速度（玩家 100 血 → 10 发） |
| `POOL_PB/EB/PART/WAVE` | 12 / 24 / 48 / 6 | 四类池化实体（炮弹/粒子/火球，**绝不每帧 spawn/destroy**） |
| `TRACK_EVERY` / `TRACK_LIFE` | 6 帧 / 4.5 s | 履带印落点间隔 / 淡出时长（环形覆盖 20 个） |

世界 640x480，y 向上（引擎渲染时翻到屏幕坐标），所以"向上"= 90°、"向右"= 0°、逆时针为正。
`HUD_TOP=428` 以上是 HUD 带，坦克不会开进去（观感干净）。

## 系统一览

- **玩家**：车体与炮塔是**两个实体**（炮塔是车体的子节点，旋转靠父链累积：炮塔存的是**相对车体**的角度）。
  W/S 沿车体朝向加减速（有加速度，不瞬停），A/D 原地转车体；炮塔指向指针，
  `input_down()`（左键按住）或空格开火，冷却 + 在飞炮弹数上限双限制。
- **敌人（5 辆）**：`巡逻 → 发现玩家 → 转向 + 保持 150~250px 距离 + 预判瞄准 → 对准后开火 → 冷却`。
  巡逻路点由引擎 `rng()` 选（可复现）；被击毁后消失、计分、留残骸；5 辆全清 → 2 秒后下一波（血量 +12）。
- **炮弹**：脚本侧直线运动 + **圆判定**（炮弹圆 vs 坦克圆 / 木箱 AABB / 世界边界），
  超时 2.2s 或出界回收。**池化复用**：`takeBullet()` 从不 spawn/destroy。
  为什么不用物理引擎：离散窄相无 CCD（薄掩体高速穿行会隧穿），而且玩法需要比视觉略小的宽容判定盒。
  物理接触查询（`physics_contact_*`）仍是对外能力，用法见 `scenes/contact/`。
- **观感**：地面草地 + 6 处土斑 + 5 个木箱掩体 + 车底软阴影 + 履带印（环形池，随时间淡出）+
  炮口焰 + 命中火花 + 击毁火球（扩散 + 淡出）+ 爆炸粒子（收缩 + 淡出）+ 残骸贴图 +
  命中闪白（乘法着色 1.0→2.0 再还原）。
- **HUD**：`HP n` + 血量条（**ui.ProgressBar**，脚本 `ui_set_value` 写、引擎钳制、脚本再按引擎回读值
  同步 sprite 宽度）、`SCORE`、`WAVE`、`ENEMY`（剩余敌人）、底部操作提示、
  中央状态行（`WAVE n CLEARED` / `GAME OVER - CLICK OR SPACE TO RESTART`）。阵亡后点击/空格重开。
- **确定性（docs/10）**：固定 dt（引擎给的 1/60）、只用引擎 `rng()`、无墙钟/无异步
  → 同一输入序列必得同一 `world_trail`（e2e 逐位断言）。

## 测试

```bash
ctest -R 'tank' --output-on-failure
```

| 测试 | 模式 | 断言 |
|---|---|---|
| `tank_smoke` | `smoke` | 加载 + 60 帧不崩 + `tank: ready` + 波次生成 + 渲染插件可用（否则 SKIP 77） |
| `tank_e2e` | `play` | ① autoplay 900 帧：≥1 次命中、≥1 次击毁、击毁计分 ≥100、有玩家被命中（AI 会还手）——实测 hits=8 kills=4 score=400 player_hits=8 ② 两次运行 `world_trail` 一致 + 末次击毁行逐字一致 ③ `KILL_AT=60`：钩子开火 → 40→20→0 血 → 击毁计分 |
| `tank_kill` | `kill` | 判定鉴别力单测：`KILL_AT` 整条链路（瞄准→开火→命中→扣血→击毁→计分→剩余敌人计数），且两次运行的**击杀帧逐位一致**；顺带断言静止玩家 260 帧内不会阵亡（保证断言稳定） |

### 测试钩子（只影响输入/朝向，判定与计分走真实代码路径）

- `MAQUE_TANK_AUTOPLAY=1`：脚本自驱动 —— 车体转向最近敌人、按"炮弹飞行时间 × 目标速度"**预判瞄准**、
  进入射程且炮口误差 < 9° 时开火、血量 < 35 时后撤、阵亡后自动重开。
  它改的只是"人怎么操作"，命中判定/扣血/击毁/计分全部照常。
- `MAQUE_TANK_KILL_AT=<帧号>`：该帧**立即**把炮口对准最近敌人的预判点并强制开火一次（绕过冷却），
  之后 150 帧继续盯住该敌人、按**正常冷却**补枪 → 单发 20 伤害、敌坦 40 血，两发必杀。
  它只改朝向与开火时机（"绕过冷却"仅限第一发）。
- `MAQUE_TANK_TRACE=1`：每 60 帧打一行战况（位置/朝向/血量/每辆敌坦的距离与瞄准误差）——
  开发时靠它定位问题，不参与任何判定。
- 只读自省（控制面，走 docs/19 §8 的节点事件路径）：玩家车体在场景里是 **id 33**，
  注册了 `state` 事件处理器 → 一条命令拿一行战况，**不用改脚本、不进入帧循环**：

  ```bash
  MAQUE_HEADLESS=1 MAQUE_INSPECT_RW=1 \
    MAQUE_CONTROL='{"jsonrpc":"2.0","id":1,"method":"Scene.emit","params":{"id":33,"event":"state","value":1}}' \
    maque_player samples/tank/tank.scn
  # [game] tank: state state=play score=0 wave=1 alive=5 hp=100 kills=0 frame=0
  # {"jsonrpc":"2.0","id":1,"result":{"ok":true,"event":"state","id":33,"value":1,"handlers":1}}
  ```

指针瞄准**走真实输入路径**的验证（sim → 输入模块 → 脚本回调，世界坐标）：

```json
// pointer.sim.json：把指针挪到世界 (540,130) 并点一下
{ "name": "pointer", "space": "world", "events": [
  { "frame": 40, "type": "pointer_move", "x": 540, "y": 130 },
  { "frame": 60, "type": "pointer_down", "x": 540, "y": 130, "button": 0 },
  { "frame": 63, "type": "pointer_up",   "x": 540, "y": 130, "button": 0 } ] }
```

```bash
MAQUE_HEADLESS=1 MAQUE_SIM=pointer.sim.json MAQUE_TANK_TRACE=1 \
  maque_player --shot out.png --shot-frame 66 samples/tank/tank.scn
# 实测：trace 里 aim 从 90 → **10**（= atan2(130−90, 540−320) = 10.3°，与指针方向一致），
# 帧 66 的截图里炮管朝右、一枚黄色炮弹正沿该方向飞出（shot_aim_pointer.png）。
```

### 实测数值（本机 clang-cl + ASan，2026-10-16）

```text
# ① autoplay 900 帧（ctest tank_e2e）
tank: hit enemy=4 hp=20 frame=49   → hit hp=0 frame=69   → kill idx=4 total=1 score=100 alive=4
tank: hit enemy=2 hp=20 frame=187  / hit enemy=3 hp=20 frame=212 → hit hp=0 frame=238 → kill total=2 score=200
tank: hit enemy=2 hp=0 frame=290   → kill total=3 score=300 alive=2
tank: hit enemy=1 hp=20 frame=853  → hit hp=0 frame=882  → kill total=4 score=400 alive=1
tank: player hit hp=90/80/70/60/50/40/30/20 frame=82..643
world_trail = 0xda42a0235835f8e7（两次运行一致）

# ② KILL_AT=60（ctest tank_kill）
tank: kill hook fired frame=60 fired=1 aim=150 target=4
tank: hit enemy=4 hp=20 frame=92  →  tank: hit enemy=4 hp=0 frame=118
tank: kill idx=4 total=1 score=100 total_score=100 alive=4 frame=118
world_trail = 0x12a90b60ed998918（两次运行一致）

# ③ 长跑 4000 帧（autoplay，非 CI）
tank: wave 1 cleared frame=988  →  tank: wave 2 start enemies=5 hp=52
tank: player dead score=600 best=600 total_kills=6 frame=1368  →  tank: restart frame=1428
```

截图自证（`--shot` 出图后逐张 `read_image` 看过，不是"应该能行"）：

| 文件 | 帧 | 图里看到了什么 |
|---|---|---|
| `shot_open.png` | 3 | 草地/土斑/木箱；5 辆红色敌坦（车体与炮管**各自转到自己的朝向**）带车底阴影与水平血条；玩家绿色坦克在下方中央，炮管**朝上**，车头标记在前 |
| `shot_fire.png` | 62（KILL_AT=60） | 玩家炮管已转到**左上 150°**（与 90° 的 shot_open 对比明显：炮管是**真正旋转过的贴图**，管口制退器可见），炮口焰方块 + 一枚橙色炮弹正沿该方向飞出 |
| `shot_kill.png` | 120 | `SCORE 100` / `ENEMY 4` / `HP 90`（HUD 血条右侧露出一截暗底）；被击毁的那辆变成**残骸**并被黄橙色火球 + 粒子包住；空中还有红色敌方炮弹 |
| `shot_dead.png` | 900（不操作） | `HP 0`（血条全空）、玩家处留下**残骸**、中央 `GAME OVER - CLICK OR SPACE TO RESTART` 完整不越界 |
| `shot_tracks.png` | 300（autoplay） | 玩家坦克**转着车身行驶**（车体/炮管同屏两个角度）、身后留下一串深色**履带印**；2 处残骸 + 爆炸粒子；`SCORE 300 / ENEMY 2 / HP 60` |
| `shot_aim_pointer.png` | 66（sim 指针事件） | **指针瞄准**：sim 把指针放到世界 (540,130) 后点了一下 → 炮管转到**朝右约 10°**（与 trace 的 `aim=10` 一致），一枚黄色玩家炮弹正沿该方向飞行 |

## 引擎现状（本版实测；踩过的坑都写在这）

写这个样例时用**探针场景 + 截图像素级比对**确认了 4 处引擎行为与直觉不符的地方，并向引擎侧登记。
其中 3 处已在提交 `3a6f87b`（补齐 `Scene::extract` 遗漏的节点属性）修好，**本工程已改用修好的能力**；
剩下 1 处（键码两套语义）仍开着，游戏侧用"两套都接受"绕过。

### 已修好、本工程正在用

| 能力 | 本工程怎么用 |
|---|---|
| `node_set_angle(id, deg)` 旋转精灵 | 车体 = `ang − 90`；炮塔 = `aim − ang`（**相对车体**，靠父链累积得到世界朝向）；炮管跟随炮塔 → 连续平滑转动 |
| `node_set_active(id, 0)` 隐藏 | 池化实体的显示/隐藏（炮弹/粒子/履带印/残骸）；整辆坦克的收放 |
| `node_set_scale(id, s)` 缩放 | 爆炸粒子收缩、火球扩散（不再手改 width/height/radius） |
| `node_set_opacity(id, o)` 透明度 | 粒子淡出、火球淡出、**履带印随时间淡出**、整棵子树的透明度累积 |

命中闪白 = 乘法着色（`setTintRGB` 把车体/炮塔/炮管的 r/g/b 从本色乘到 2.0，0.11s 后还原）。

### 三个必须知道的约定（实测，否则会踩）

1. **`node_set_angle` 的 0 是"精灵天然朝向"，而贴图的天然朝向是"上"（世界 90°）**，
   正角度是**逆时针**。实测：同一张朝上炮管贴图设 0/90/180/270，管口分别指向 **上/左/下/右**。
   → 想让精灵指向世界角 θ，必须设 **θ − 90**（脚本里的 `aimToSpriteAngle`）。
   对称的长条看不出正负，**带方向的一端（管口）才看得出** —— 只拿对称图形验证会漏掉这个约定。
2. **`node_set_active(id, 0)` 只隐藏实体自身，不级联到子节点**（实测：隐藏父节点后子节点照常绘制）。
   → 整辆坦克的显隐要**逐个部件**设（`setTankShown`），不能只关车体。
3. **层级只累积旋转/缩放/透明度，不旋转子节点位置**（`flatten_world` 仍是"父坐标 + 子坐标"）。
   → 挂在会转的父节点下的部件（炮管、血条），**位置**要脚本按朝向算，**朝向**才靠父链累积。
   反过来这也是个便利：血条设 `90 − ang` 就抵消了车体旋转，永远水平。

### 仍未修 / 不是 bug 但要知道（本工程绕开）

4. **键盘键码两套语义。** 真实设备事件经 `key_from_sdl`（`modules/platform/src/platform.cpp:31-48`）
   **归一化**：空格=2、回车=3、W=87、A=65、S=83、D=68；而 sim/回放注入的原始码是
   空格=32、W=119…（不经那条路）。→ 脚本对每个键**同时接受两套码**（`K_SPACE=[2,32]` 等）。
   `samples/flappy` 的 `on_key_down` 只认 raw SDL 码 32/1073741906，实际按空格不会触发
   （它的 e2e 只用点击，所以没暴露）。
5. **动态 spawn 的实体拿不到装配期资源预热**：`entity_spawn` 出来的实体没有那次预解析，
   `material`/`texture` 字段不会被解析（图元退化成 sprite 默认色）。
   → 池化实体（炮弹/粒子/履带印）一律**纯色 r/g/b**。
   （`node_set_string(id,'scn.Sprite','texture',…)` 走"赋值即解析"那条路，能用，但没必要。）
6. **`ui.ProgressBar` 的填充不会自动改变画面**：`ui_step` 只把填充子节点的
   `ui.Control.offset_max.x` 改成 `full×value`，而 sprite 提取只读 sprite 自己的
   `width/height`（两个字段不互通）。→ 脚本 `ui_set_value()` 之后**回读**
   `node_get_number(id,'ui.ProgressBar','value')`，再用它同步 sprite 宽度与左对齐位置：
   引擎组件是唯一真相，画面跟着它走，而不是各算一份。
7. **层级按实体 id**（先声明先画）：地面 → 土斑 → 木箱 → 车底阴影 → 坦克 → 残骸 → HUD。
   阴影之所以是**独立根实体**而不是坦克的子节点，就是因为子节点 id 比车体大、会被画在车上面。

## 已知限制

- 子节点位置不随父节点旋转（引擎层级只累积旋转/缩放/透明度），所以炮管/血条的**位置**是脚本按朝向
  算出来的近似（朝向本身是引擎算的，连续平滑）；如果引擎以后支持"2D 变换矩阵"，这几行可以删掉。
- 爆炸/炮口焰是纯色方块/圆（池化实体没有贴图），靠缩放 + 透明度做动感，没有逐帧序列帧。
- 履带印只给玩家坦克落（敌人不落）。
- 玩家阵亡后世界冻结（只有粒子/残余炮弹继续），不做观战/自动重生（`MAQUE_TANK_AUTOPLAY=1`
  时才会 1 秒后自动重开，那是测试钩子）。
