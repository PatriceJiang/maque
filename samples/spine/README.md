# Spine 2D（samples/spine）

用**官方 spine-runtimes 素材**驱动引擎的骨骼动画渲染：一个带 `anim.SpineSkeleton` 组件的实体 →
引擎在装配期读骨架/图集/页纹理，每帧步进动画并把**网格图元**（逐顶点 pos/uv/color + 索引）提交给渲染器。
**↑/↓ 切换动画**，空格暂停。玩法与素材全在资源里，宿主只有 50 行。

**本目录同时是官方 11 个 demo 的移植**（+ 官方两个 C++ 入口 demo）+ 一个 `all.scn` 总览 → 见
[官方 demo 移植](#官方-demo-移植11--2-个) 一节。

```
samples/spine/
├── main.cpp          宿主：只解析参数 + mq_run_scene()，不含任何 spine/游戏概念
├── spine.scn         spineboy（单页图集 1024x256，11 个动画）—— 上下键切换的主场景
├── dragon.scn        dragon-ess（**5 页图集** 1024x1024，1 个动画）—— 多页批处理场景
├── spine.js          交互：列动画清单、↑/↓ 切换、空格暂停、HUD（spine.scn / dragon.scn 用）
├── demo-*.scn        官方 demo 移植（每个一个场景，13 个）—— 共用 demos.js
├── all.scn           总览：12 个骨架同屏网格（一张截图看见全部）
├── demos.js          官方 demo 的行为表（缩放/起始动画/排队/皮肤/背景）+ 点击链路
└── art/              素材（官方 4.2 样例副本，随附 LICENSE-spine-runtimes.txt）
```

## 跑起来

```bash
# 从仓库根目录跑（场景目录被挂到 VFS 的 /，素材按 /art/... 引用）
./build-win/samples/spine/maque_spine samples/spine/spine.scn
./build-win/samples/spine/maque_spine samples/spine/dragon.scn

# 无头截图（CI/无显示器）：第 N 帧一张图
MAQUE_HEADLESS=1 MAQUE_PRESENT=off ./build-win/samples/spine/maque_spine \
    --shot /tmp/spineboy.png --shot-frame 40 samples/spine/spine.scn

# 指定起始动画 / 定格时间（截图回归用）
MAQUE_SPINE_ANIM=run MAQUE_SPINE_TIME=0.5 ./build-win/samples/spine/maque_spine \
    --shot /tmp/run.png --shot-frame 40 samples/spine/spine.scn
```

操作：**↑ / ↓ 上一个/下一个动画**（环绕），**空格 暂停/继续**。
也可在脚本里用引擎 API 驱动：`spine_play(id, "run", 1)`、`spine_set_time(id, 0.5)`、
或直接写组件字段 `node_set_number(id, "anim.SpineSkeleton", "animation", 7)`（改了就切）。

## 场景怎么写

```json
{ "name": "spineboy",
  "components": [
    { "type": "scn.Transform2D", "x": 100, "y": 20 },
    { "type": "anim.SpineSkeleton",
      "skeleton": "/art/spineboy-pro.skel",     // .skel = 二进制；.json = 文本
      "atlas":    "/art/spineboy-pma.atlas",    // 省略 → 用 material / 与骨架同名的 .atlas
      "animation": 3, "autoplay": true, "play_mode": "PlayMode::Loop", "speed": 1.0 } ] }
```

- **骨架单位 = 像素**（spineboy 站立约 663 单位高）→ 放进 320x240 的设计分辨率要缩：
  脚本用 `node_set_scale(id, 0.30)`，引擎按**累计节点缩放**设置实例变换（缩放是场景的事）。
- 图集页名（如 `spineboy-pma.png`）按 **atlas 所在目录**解析，所以素材可以整体换目录/换挂载点（含远程 VFS）。
- 页纹理如果是**预乘 alpha**（官方 pma 图集：`pma: true`），引擎会按 `src=ONE` 的混合方程选管线
  （`mq_spine_page.pma` → 渲染项 `pma` 字段），否则边缘会被乘两次 alpha 发暗。

## 引擎侧这条链路（排查问题时的地图）

| 环节 | 代码 | 产物 |
|---|---|---|
| 装配 | `runtime.cpp: Engine::spine_setup` | VFS 读 `.skel`/`.atlas` → `mq_spine_data_create`；页纹理回调里 `image` 解码 → `create_texture` → 回填句柄 |
| 帧内 | `Engine::spine_step` | `mq_spine_instance_step(dt*speed)` → `mq_spine_instance_draw`（CPU 顶点/UV/颜色/索引/批次）→ 变换到渲染目标像素空间 |
| 提取 | `Engine::spine_extract` | 每个批次 → 一条 `mq_renderer_mesh`（同页 + 同混合模式） |
| 渲染 | `gfx: render_impl`（`render_mesh_views`） | 交错顶点缓冲 + 索引缓冲 + 页纹理，逐批次 `drawIndexed`，与精灵按 (layer,z) 统一排序 |

诊断开关：`MAQUE_TRACE_SPINE=1` 打印每 20 帧的绘制规模/包围盒/UV 统计（定位"角色看不见"的第一手证据）。

## 官方 demo 移植（11 + 2 个）

上游 `spine-runtimes` 4.2 的 cocos2d-x 示例有 11 个 demo，另外官方 C++ runtime 有两个入口 demo。
本目录把**它们全部**做成独立场景；行为按 `.deps/official/DEMO-SPEC.md`（从上游源码提取）与
`.deps/official/cocos/*Example.cpp` 复刻，**素材也用官方那套**（注意：cocos 系列用的是**非 pma** 图集
`spineboy.atlas`，而两个 C++ 入口用 **pma** 图集 `spineboy-pma.atlas` —— 引擎按图集的 `pma: true`
自动选混合方程，两种都能正确出图）。

```bash
# 每个 demo 一个场景（800x600，灰色背景 = 官方 LayerColor(128,128,128)）
./build-win/samples/spine/maque_spine samples/spine/demo-spineboy.scn
./build-win/samples/spine/maque_spine samples/spine/demo-ik.scn
...
# 总览：12 个骨架同屏（1600x1300）
./build-win/samples/spine/maque_spine samples/spine/all.scn
```

| # | 场景 | 素材（缩放） | 动画清单（实测打印） | 复刻了什么 |
|---|---|---|---|---|
| 1 | `demo-spineboy` | spineboy-pro.json + spineboy.atlas（0.6） | 11：aim, death, hoverboard, idle, idle-turn, jump, portal, run, run-to-idle, shoot, walk | 事件回调 5 类 + 事件 + 排队 walk→jump→run + setMix(0.4) |
| 2 | `demo-separator` | 同上（0.6） | 11（同 spineboy） | 槽区间分层（后半身 / 前景）+ 中间红条 |
| 3 | `demo-goblins` | goblins-pro.json + goblins.atlas（1.5） | 1：walk | `setSkin("goblin")` |
| 4 | `demo-raptor` | raptor-pro.json + raptor.atlas（0.5） | 5：gun-grab, gun-holster, jump, roar, walk | track0 walk + track1 延迟 2s 的 gun-grab；官方还开两色着色 |
| 5 | `demo-tank` | tank-pro.skel + tank.atlas（0.5） | 2：drive, shoot | shoot |
| 6 | `demo-coin` | coin-pro.skel + coin.atlas（1.0） | 1：animation | 旋转金币（默认皮肤；点三次点击链） |
| 7 | `demo-mixandmatch` | mix-and-match-pro.skel + mix-and-match.atlas（0.5） | 6：aware, blink, dance, dress-up, idle, walk；**34 个皮肤** | 运行时 `Skin::addSkin` 合成 9 件部件皮肤 + dance |
| 8 | `demo-batching` | spineboy-pro.json + spineboy.atlas（0.6） | 11（同 spineboy） | **50 个骨架**同屏（官方 `NUM_SKELETONS=50`），位置用引擎 `rng()` 复刻随机分布 |
| 9 | `demo-sequence` | dragon-ess.skel + dragon-pma.atlas（1.0） | 1：flying | 序列帧（Sequence）附件 |
| 10 | `demo-ik` | spineboy-pro.json + spineboy.atlas（0.6） | 11（同 spineboy） | track0 walk + **track1 aim**，指针驱动 crosshair 骨骼 → IK 手臂跟随 |
| 11 | `demo-physics` | celestial-circus-pro.skel + celestial-circus.atlas（0.2） | 7：eyeblink, eyeblink-long, stars, swing, wind-idle, wing-flap, wings-and-feet | PhysicsConstraint（骨骼链下垂/摆动）+ 拖动平移 |
| 12 | `demo-glfw` | spineboy-pro.skel + **spineboy-pma.atlas**（0.3） | 11（同 spineboy） | 官方 `spine-glfw` 入口：defaultMix 0.2、`portal`→`run`、窗口 800x600、黑底 |
| 13 | `demo-sdl` | spineboy-pro.json + **spineboy-pma.atlas**（0.5） | 11（同 spineboy） | 官方 `spine-sdl` 入口：json scale 0.5、defaultMix 0.2、`portal`→`run`、背景 (94,93,96) |
| — | `all.scn` | 上述 12 个骨架（每格一个） | 逐格打印 | 总览：一张截图看见全部（1600x1300 网格） |

**交互（所有 demo 场景一致）**：`↑/↓` 上一个/下一个动画（沿用 `spine.scn` 的交互）、`空格` 暂停、
`N` 下一个 demo；**鼠标点击 = 官方 demo 的三段行为**：第 1 击开调试骨骼（引擎无此能力 → 打印
`UNSUPPORTED` 而不是假装）、第 2 击 `timeScale=0.3`（官方就这么写）、第 3 击切下一个 demo
（官方 `replaceScene` 链：Spineboy→Separator→Goblins→Raptor→Tank→Coin→MixAndMatch→Batching→
Sequence→IK→Physics→Spineboy，本工程把两个 C++ 入口接在 Physics 之后）。

**诚实清单**（哪些是复刻、哪些因引擎能力/素材而简化 —— 脚本会在 HUD 与日志里打 `UNSUPPORTED:...`）：

| 项 | 状态 |
|---|---|
| 缩放 / 起始动画 / 换皮肤（Goblins）/ 背景色 / 位置 / 动画清单 | ✅ 复刻 |
| 多页图集、非 pma 与 pma 两种图集 | ✅ 复刻（引擎按 `pma` 自动选混合方程） |
| **动画事件**（Spineboy 的 start/interrupt/end/complete/dispose/event） | ✅ 复刻：引擎把事件翻成脚本事件（`node_on(id,"spine_complete",…)`，另留 pull 面 `spine_event_*`）；实测日志与官方同格式（`0 start: jump` / `jumped!` / `0 event: footstep, 0, 0, `） |
| **排队动画**（Spineboy walk→jump→run、glfw/sdl portal→run、Raptor track1 gun-grab、Batching 随机延迟） | ✅ 复刻（`spine_add_animation`） |
| **混合**：`setMix(walk,jump,0.4)`、`setDefaultMix(0.2)` | ✅ 复刻（`spine_set_mix` / `spine_set_default_mix`） |
| **多轨**（IK：track0 walk + track1 aim） | ✅ 复刻（`spine_set_animation(id,track,…)`）。注：官方 `aim` 是单帧循环 → 每帧一次 complete（实测 60 帧 60 次），属正常 |
| **IK 指针跟随**（crosshair 骨骼） | ✅ 复刻（`spine_bone_find` + `spine_bone_set_world_position` + `spine_update_world_transform`）；指针移动即当帧跟随 |
| **运行时合成皮肤**（MixAndMatch） | ✅ 复刻（`spine_skin_compose`：9 件 → **46 个附件**，实测打印） |
| **槽区间分层**（Separator） | ✅ 复刻（`spine_set_slots_range`：back=`[19..24]`、front=`[25..-1]`）。实现差异：官方是"一个骨架 + 两个渲染节点"，我们是**两个实体各持一实例**镜像播放（引擎没有"分次绘制同一实例"的 API）→ 观感一致、内存各一份 |
| **Sequence 序列帧**（dragon） | ✅ 复刻：扫槽发现 `left-wing`/`right-wing` 各 **9 帧**，运行时打印"sequence 帧变了 → 5" |
| **PhysicsConstraint**（celestial-circus，30 个约束） | ✅ 支持（task-5 补了 `skeleton->update(dt)`，物理真的逐帧推进）；拖动 → `spine_physics_translate` |
| **两色着色 TintBlack**（Raptor / Batching 官方都开） | ✅ 语义满足：本引擎网格**恒定**两色（无 dark 通道的素材等价于关闭，task-6 有逐字节证据）；`spine_set_two_color` 是诚实空操作 |
| 调试骨骼绘制（官方第一击） | ❌ 引擎无骨骼调试绘制（如实打印 UNSUPPORTED，不假装） |
| MixAndMatch 的 default 皮肤**没有任何附件** | 已由 compose 解决（官方皮肤 = 9 件合成，46 附件）；若引擎缺 compose，脚本兜底用现成的 `full-skins/girl` |
| Batching 画布 | 官方用设备尺寸；50 个 0.6 缩放的 spineboy 在 800x600 里糊成一团（实测），故总览用 1280x960，位置仍按官方百分比公式 |
| 场景里声明槽区间 | 除了脚本按**槽名**设置（= 官方 `findSlot(name)->getData().getIndex()`），组件字段 `slots_from`/`slots_to` 也能在 `.scn` 里直接声明（声明序下标） |

## 测试

```bash
ctest -R spine    # spine2d + spine_smoke/spine_e2e/spine_dragon/spine_mask + spine_demos/spine_all
```

- `spine2d`（模块级）：动画/皮肤清单、图集页与 `pma`、批次完整覆盖索引、时间语义、坏输入不崩。
- `spine_smoke`：加载骨架/图集/页纹理 + 跑 40 帧不崩。
- `spine_e2e`：`MAQUE_SIM` 注入 ↑/↓/空格（**SDL 风格码**，引擎在模拟器边界归一化）→
  断言日志里的切换序列 == 注入顺序、脚本读回的"引擎当前动画" == 请求；
  再对比**同一帧不同动画**的截图（必须不同），并做**像素级**断言：角色区域内非背景像素 > 2000。
- `spine_dragon`：5 页图集逐页拿到纹理句柄 + 引擎页表 `pma=1` × 5。
- `spine_demos`：13 个官方 demo 场景各跑 30 帧（`ready` + 动画清单非空 + 无报错）。
- `spine_all`：`all.scn` 出图 + 骨架数 == 格数 + **逐格**像素断言（12 格各自 >300 非背景像素）。
  逐格是刻意的：整图统计会被别的格子掩盖 —— 这条断言实测抓到过"Goblins 漏设 skin 只剩长矛+盾"
  与"MixAndMatch 的 default 皮肤没附件 → 0 顶点"（两处都是真的画面上看不见，日志却一切正常）。

**为什么要有像素级断言**：单测里的 `uv ∈ [0,1]` 对"UV 取错 accessor（区域局部 vs 归一化到整页）"
**没有鉴别力**（两者都在 [0,1] 内，实测并集跨度 952x252 px vs 1024x256 px），
而用错时角色会整只采样到图集空白处 → 画面上**什么都看不见**。像素断言能抓住它。

## 已知限制（都不是这个样例的 bug，是引擎/上游现状）

1. **裁剪附件（`ClippingAttachment`）不产生几何** → 带遮罩的角色不会被裁切（spine2d 模块的既定取舍）。
2. **multiply / screen 混合模式回退为 normal**：本机 wgpu-native 的 `WGPUBlendFactor` 没有
   `dst-color` 类因子，无法表达 multiply/screen。gfx 会打印一行提示后按 normal 画（不静默假装支持）。
3. **dragon 只有 1 个动画**（`flying`），所以多页场景演示不了切换；切换演示用 spineboy（11 个动画）。
4. 骨架**旋转**来自节点的累计 angle（`node_set_angle`），骨架自身的 `phys_timestep` / `update_mode`
   字段尚未接（spine2d 的 `step` 固定用 `Physics_Update`）。
5. 骨架/图集是**内存解析**：spine 自己的 JSON 解析器遇到垃圾图集文本会断言中止（上游行为），
   所以引擎侧没有"远程脏数据"防御 —— 见 `docs/27` 的坑 5。
6. `art/dragon-ess.atlas` 是 `dragon-pma.atlas` 的**同内容副本**：专门用来覆盖"图集缺省解析规则"
   （组件不写 `atlas` 时取与骨架同名的 `.atlas`）——`dragon.scn` 走的就是这条路。

## ImGui 调参面板（`MAQUE_IMGUI` / **F2**）

引擎侧即时模式 UI（`modules/ui/imgui`）。**游戏内 HUD 仍用声明式 `ui.*`**（进场景/快照/可回放）；
这块面板是**开发工具**：不进快照、不进校验和、关掉不影响任何模拟结果。

### 怎么打开

**运行时按 `F2`**（引擎级热键，runtime 直接监听，不依赖脚本）——再按一次关闭。
也可以启动时就打开：

```bash
MAQUE_IMGUI=1 MAQUE_WINDOW=800x650 ./build-win/samples/spine/maque_spine samples/spine/all.scn
```

| 变量 | 作用 |
|---|---|
| `MAQUE_IMGUI=1` | 启动即开面板（不设则默认关，按 F2 可开） |
| `MAQUE_WINDOW=800x650` | 窗口尺寸（场景仍 1600x1300，画布等比缩放+留边） |
| `MAQUE_IMGUI_FONT_SIZE=20` | 面板字号（默认 18px；字体复用 `scenes/fonts/` 的中文字体） |

### 绘制位置（为什么能看到）

面板在所有引擎图层（含 `ui` pass 的 `layer=1`）**之后**、present **之前**提交，
网格 `layer=MQ_IMGUI_LAYER(100)` → 永远在最上层，且**写进同一像素缓冲**
→ `--shot` 截图里也有面板（这是选"复用引擎网格管线"而非 `imgui_impl_sdlrenderer2` 的主因：
后者只能画到窗口，截图看不到、无法自证）。

### 输入隔离

ImGui 要吃鼠标/键盘时，引擎**清空本帧设备事件** → 点面板不会同时打到游戏上。
脚本也可自行判断：`imgui_want_mouse()` / `imgui_hovered()`。

### 面板内容（`all.scn`）

全局：`Pause all` / `Reset` / `ImGui demo`（内建演示窗口，用来验证"整套控件都活着"）。
逐格：每个 `cell` 的动画下拉、播放头时间（拖动即定格）、缩放。

### ⚠️ 已知缺陷（未修）

面板**能渲染**（标题栏/按钮/复选框/下拉/文字都在），但**内部控件重叠、有白色条带**。
已排除：scissor 缺失（网格已支持 `clip_*` 并逐网格 `SetScissorRect`）、`VtxOffset`（已按
draw-list 偏移修正）、字体纹理未上传（已按动态图集 `WantUpdates` 重传）。**根因未定位**，
所以面板暂时**不能当控件用** —— 见 `docs/27` 的待办。
