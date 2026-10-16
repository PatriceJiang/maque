# Flappy Bird（samples/flappy）

用真实游戏反推引擎能力的第一个完整样例。**玩法全部在资源里**，宿主只有 40 行。

```
samples/flappy/
├── main.cpp         宿主：只调 mq_run_scene()，不含任何游戏逻辑
├── flappy.scn       场景：实体 + 组件 + 引用素材（引擎从这里提取渲染）
├── flappy.js        玩法：重力/拍翅/水管生成与回收/碰撞判定/计分/状态机/HUD
└── art/             素材：PNG + 材质 JSON（程序化生成，见 tools/make_flappy_art.py）
```

## 跑起来

```bash
# 从仓库根目录跑（场景目录会被挂到 VFS 的 /，所以素材按 /art/xxx 引用）
maque_player samples/flappy/flappy.scn          # 用通用宿主
maque_flappy samples/flappy/flappy.scn          # 用本工程宿主（等价）

# 无头冒烟 + 截图（CI/无显示器）
MAQUE_HEADLESS=1 maque_flappy --shot /tmp/f.png --shot-frame 120 samples/flappy/flappy.scn
```

窗口、缩放、输入模拟等运行期开关见仓库根 `AGENTS.md`。

## 解耦约定（本工程存在的意义）

1. **宿主不认识游戏**：`main.cpp` 只做参数解析 + `mq_run_scene()`；
   它链接的**唯一**引擎目标是 `maque::runtime`。若它的源码里出现了 flappy 之外的游戏概念，
   就是耦合漏进来了。
2. **玩法不编译进引擎**：改重力、缺口高度、难度曲线、碰撞盒大小，只改 `flappy.js`，
   不需要重编译任何 C++（改完直接跑；开 `MAQUE_WATCH=1` 还能热重载脚本）。
3. **素材属于游戏**：`art/` 是程序化生成的（`python tools/make_flappy_art.py`），
   引擎不需要认识任何一张图。新增素材 = 放 PNG + 写一行材质 JSON。
4. **引擎缺口才动引擎**：本游戏开发过程中实际补了引擎能力，见下节与
   `docs/26-用真实游戏驱动引擎（flappy）.md`。

## 玩法参数（都在 flappy.js 顶部）

| 常量 | 值 | 作用 |
|---|---|---|
| `GRAVITY` | −1400 px/s² | 负值 = 向下（世界坐标 y 向上） |
| `FLAP_VY` | 380 px/s | 一次拍翅的瞬时上冲 |
| `MAX_FALL` | −520 px/s | 下落速度上限（同时防隧穿） |
| `PIPE_SPACING` | 132 px | 相邻水管间距 |
| `GAP_MIN`/`GAP_MAX` | 78 / 96 px | 缺口高度区间，随分数收窄 |
| `SCROLL_SPEED`→`SPEED_MAX` | 62 → 96 px/s | 卷动速度随分数提升 |
| `BIRD_HIT_R` | 7 px | 判定半径（故意小于视觉半径 9 → 手感宽容） |

## 测试

```bash
ctest -R 'flappy'          # flappy_smoke（加载+40 帧不崩）、flappy_e2e（计分+确定性+撞管判死）
```

`flappy_e2e` 用三个**只影响测试、不跳过判定**的钩子做到确定性（都通过环境变量开关）：

- `MAQUE_FLAPPY_FOLLOW_GAP=1`：每帧把鸟吸到"与它在 x 上重叠的那根水管"的缺口正中
  → 稳定验证滚动/生成/计分链路。它只改鸟的 y，碰撞与计分照常走。
- `MAQUE_FLAPPY_CRASH_AT=<帧号>`：该帧把鸟送进水管 → 验证判死。
  **实测确认它走的是真实判定路径**（钩子返回 1 后由 update 的碰撞检测判死，而不是直接调 `die()`），
  所以判定失效时测试会红。
- `Scene.emit(<birdId>, "crash", 1)`：控制面触发同一个钩子（跨语言节点事件路径）。

为什么不直接断言"永不死亡"：随机缺口下那是在测"输入调得好不好"，必然脆弱。
本测试断言的是"链路对不对 + 可复现"（同输入 → 同 `world_trail`）。

## 已知限制（不是这个游戏的 bug，是引擎现状）

- **动态生成实体的材质/贴图不会自动解析**：渲染提取是只读的、只查装配期预热的缓存，
  脚本运行时 `entity_spawn` 出来的实体拿不到那次预热（`material` 赋了也不解析 →
  图元退化成 sprite 默认色）。所以水管用**纯色**（r/g/b）而不是贴图；
  场景内静态实体的贴图完全正常（已实测对照）。
- **物理求解器是离散窄相、没有 CCD**：物体单帧位移超过碰撞体厚度会穿透。
  所以本游戏的碰撞用脚本侧逐帧 AABB 判定（薄水管 + 高速穿行场景下这是正确选择）。
  物理模块的接触查询（`physics_contact_*`）用法见 `scenes/contact/`。
