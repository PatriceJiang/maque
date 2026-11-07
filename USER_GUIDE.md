# maque 用户指南 · 使用引擎的最佳实践

> **这份文档写给谁**：用 maque 做游戏 / 交互应用的人（以及驱动这些项目的 AI）。
> 引擎贡献者请先读 [`AGENTS.md`](AGENTS.md)；设计动机与逐模块契约见 [`docs/`](docs/README.md)。
>
> **基线**：当前 `main` 分支上**已实现、有样例或测试覆盖**的能力。文中每个结论都尽量给出
> 可执行的验证方式；标了“预览 / 当前不要依赖”的能力，请先看第 1 节再决定要不要用。
>
> **用法**：第一次接触 maque 从第 2 节开始；已经能跑起来的人直接看第 3–7 节（心智模型、
> 组织方式、场景、脚本、确定性），再按需要跳到渲染 / 资源 / 调试 / 测试章节。

---

## 目录

0. [先看这 12 条](#0-先看这-12-条)
1. [稳定面：哪些能用，哪些别依赖](#1-稳定面哪些能用哪些别依赖)
2. [五分钟跑通](#2-五分钟跑通)
3. [心智模型](#3-心智模型)
4. [项目与场景组织](#4-项目与场景组织)
5. [场景编写](#5-场景编写)
6. [脚本](#6-脚本)
7. [确定性与回放](#7-确定性与回放)
8. [输入与 UI](#8-输入与-ui)
9. [渲染](#9-渲染)
10. [资源与 glTF](#10-资源与-gltf)
11. [音频](#11-音频)
12. [存储与存档](#12-存储与存档)
13. [调试与观测](#13-调试与观测)
14. [测试与 CI](#14-测试与-ci)
15. [平台与发布](#15-平台与发布)
16. [反模式清单](#16-反模式清单)
17. [发布前检查清单](#17-发布前检查清单)
18. [常用配方](#18-常用配方)
19. [文档地图](#19-文档地图)

---

## 0. 先看这 12 条

1. **游戏逻辑全在资源里**：`.scn` + `.js`/`.lua`；宿主只负责启动。不要给新游戏加 `main.cpp`。
2. **一个 `.scn` = 一个行为单元**：关卡/敌人/塔/弹窗用 `SceneInstance`；子弹/瓦片等大量同类对象用
   prefab 池 + 根脚本批量驱动，别每个一个 VM。
3. **新项目、全 JS → 包 + ESM**；Lua/混合语言走旧路径。包场景热重载是 P1，改包脚本要重启。
4. **只挂场景级 `script`/`scripts`**；`script.Script` 组件、`this.props`、`Script.exposed` 当前不要用。
5. **状态放世界**：组件字段 / `global_*` / `storage`；闭包和顶层变量只放瞬态，热更/快照会丢。
6. **确定性是硬约束**：固定 `dt`、`rng()`、不读墙钟、不做帧内异步、按 id/声明序遍历。
7. **热更/快照后重新 `on_start`**：`node_on` 注册写在 `on_start`，不要写顶层。
8. **2D/UI**：小图才进动态图集（RGBA8/单 mip/clamp/边 ≤256）；大图单独画；排序用 `z_index`；
   长列表用 `ListView`；`Mask` 只有矩形。
9. **3D**：材质显式 `side`；`render.Light` 有重复 `"type"` 键（别用标准 JSON 库改写）；
   最多 8 盏灯；环境/阴影/RT 按需开，别全量铺 4x。
10. **资源路径相对场景目录**；glTF 可运行时直读或离线导入；不手改生成的 `.mesh.json`/`README`。
11. **先测量再优化**：`--shot`/`--frames`/`MAQUE_TRACE_STATS`/`MAQUE_RENDER3D_INFO` 定位，
    再上 Tracy/perf。
12. **每个场景都要有测试**：加载冒烟 + 确定性 checksum + 关键帧截图；跨平台只做结构性断言。

---

## 1. 稳定面：哪些能用，哪些别依赖

maque 的能力不是“同一成熟度”的。**稳定**在本指南里的定义是：

1. 运行时有真实实现（不是只有 schema 声明或设计稿）；
2. 仓库里有样例场景 / 端到端测试；
3. 行为与文档一致，且没有“看起来能用、其实静默降级”的已知坑。

### 1.1 可以直接依赖的稳定能力

| 领域 | 能力 | 入口 / 样例 |
|------|------|-------------|
| 场景 | 节点树、组件、父子/层级、`node_*` 与 `entity_*` API | `scenes/nodes/`、`scenes/prefab/` |
| 场景组合 | 静态/动态 `scn.SceneInstance`、实例参数、`scene_emit`、暂停/卸载/重载 | `scenes/nested/`、`scenes/td/`、`samples/tank/` |
| 脚本 | JS（quickjs）与 Lua 5.4；同一套 API 与回调 | `scenes/ball.js`、`scenes/lua/ball.lua` |
| 2D | Sprite、Layer/z 排序、Sprite 合批、字体、UI 控件、ListView、Mask、九宫格 | `scenes/ui*.scn`、`scenes/ui_list/`、`scenes/ui_patch/` |
| 3D | Camera3D、MeshRenderer、PBR 材质（10 种 shading model）、多光源、阴影、IBL / HDRI、glTF 直读 | `scenes/pbr/`、`scenes/khronos/` |
| RenderTexture | 相机离屏纹理、同帧采样、脚本截图、逐目标 MSAA | `scenes/render_texture/` |
| 物理 | 2D 固定步长求解器（球/盒）、射线、接触对查询 | `scenes/ball.scn`、`scenes/contact/`、`samples/flappy/` |
| 动画/粒子 | `scn.AnimationPlayer`、`scn.ParticleEmitter`、glTF 骨骼/蒙皮/morph、Spine 2D | `scenes/anim/`、`samples/spine/` |
| 导航 | 网格寻路、追踪、跟随 | `scenes/nav/` |
| 输入 | 指针/键盘/文本、UI 命中/拖拽滚动、输入模拟器 | `scenes/ui.scn`、`tests/data/sim/` |
| 音频 | WAV/OGG/MP3、总线、空间化、离线抓音 | `tests/audio_e2e.sh`、`samples/tank/`（BGM/音效） |
| 存储 | `file://` / `mem://` / `http(s)://` / `ws(s)://` 键值存档 | `scenes/storage/` |
| VFS | 目录挂载、`zip`、`builtin:/`、`http(s)://`、Android `asset://` | `docs/04`、`android/README.md` |
| 调试/工具 | 截图、批量截图、F2/F4、Inspector、控制面、Tracy、Linux perf | 本文第 13 节 |
| 平台 | Linux、Windows x64、Android arm64；WASM P0 headless 门 | 本文第 15 节 |

### 1.2 能用，但要知道边界

| 能力 | 当前边界 / 最佳实践 |
|------|---------------------|
| 脚本包（`maque.package.json`） | **P0 只覆盖“全 JS 场景”**：单 JS VM + 原生 ESM + 每挂载点 context。Lua / 混合语言仍走旧路径；包内 Lua `require` 未实现。 |
| 包场景热重载 | **未实现**（P1）。改包内脚本请重启；`MAQUE_WATCH=1` 只对旧路径（无清单）脚本按文件原子热更。 |
| 场景级脚本的持久状态 | 热重载 = 新建 VM/模块代际，**脚本内变量不跨重载保留**；跨重载状态放组件字段、`global_*` 或 `storage`。 |
| `global_*` | 当前是 **Engine 级全局表**，所有实例/包共享；包作用域隔离（P1）尚未落地。键名请自行加前缀。 |
| 3D 拾取 | `scene_pick` / `scene_raycast` 用 **网格本地包围盒的 OBB**，不是三角形级；蒙皮/morph 用绑定姿态。 |
| glTF | 运行时直读覆盖核心 PBR + 动画/蒙皮/morph + 多数 KHR 扩展；**不支持 Draco / meshopt / WebP / AVIF / KTX2 块格式 / cubemap**。 |
| 透明/玻璃 | transmission/volume/diffuse transmission 是**屏幕空间近似**，不是真实多次折射；不要把它当光学仿真。 |
| RenderTexture | 固定 `RGBA8Unorm`、单层、单 mip；不能做 cubemap/array/深度纹理；后处理只作用于主输出。 |
| 阴影 | 方向光 1024² shadow map + 3×3 PCF；最多 8 盏灯；默认投硬阴影。性能敏感时给补光设 `shadow_mode=Off`。 |
| UI 文本 | 字形栅格化/布局是设计像素；`text_w/text_h` 在**排版后**才有值，不要在 `on_start` 里立刻拿它做布局。 |
| Android | 默认 null 音频、默认不弹软键盘；AVD 走 CPU fallback，不代表真机性能。 |

### 1.3 当前明确不要依赖

| 不要依赖 | 原因 / 替代 |
|----------|-------------|
| `script.Script` 组件挂脚本 | 组件脚本与 `props` 是 P1，运行期尚未接线。**场景级 `script` / `scripts` 才是稳定入口。** |
| `this.props.*` / `Script.exposed` 持久属性 | 同上（P1）。持久状态用组件字段、`global_*`、`storage`。 |
| 包场景的 `MAQUE_WATCH` 热更 | 未实现（P1）；重启验证。 |
| Lua 的 `require` / 跨模块 import | P2；Lua 脚本按场景级文件组织。 |
| 脚本指令预算 / 沙箱 / realm | P2；本地脚本默认可信，远程内容不要直接跑原生/不受信脚本。 |
| WASM 可见渲染 | P1 未完成；当前 P0 仅 headless 确定性门，不提供 WebGL/CPU 光栅回退。 |
| 3D 物理 / CCD | 物理只有 2D 球/盒；高速薄物体会隧穿，用脚本 AABB 或钳速度。 |
| 三角形级 3D 拾取 | 只有 OBB；精确点击用几何/自建射线检测。 |
| `Meta.stable_id` 做引用锚点 | 运行期未赋值；引用请用节点名/路径/实例句柄。 |
| `asset.RenderTarget` / `render.RenderLayer.target` | 声明存在但 runtime 尚未消费；相机离屏请用 `render.RenderTexture`。 |
| `ui.Mask` 圆角/纹理/stencil | 当前只有矩形裁剪（含 reverse）；圆角用图片/九宫格。 |
| `ui.Image` 的 `KeepAspect*` / `Tile` | 未实现；按 Scale 处理。 |
| `ui.ListView.item_template` 虚拟化 | 未实现；P0 用单个 Label 承载大文本。 |
| KTX2 块格式 / cubemap / 动画可见性 / 节点·贴图 pointer | 见 `plans/24` 能力矩阵。 |

> 这一节是“当前快照”。若你在更新的提交上工作，以 `docs/24`、`docs/32 §14`、
> `docs/34 §13`、`plans/24` 的状态表为准。

---

## 2. 五分钟跑通

### 2.1 构建

```bash
# 首次：codegen 依赖
cd modules/core/data && npm install && cd -

# 配置 + 构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 全量测试
ctest --test-dir build --output-on-failure
```

Windows 用 `tools\win\build.cmd`（默认 Debug + ASan），细节见 `AGENTS.md`。
WASM / Android 不是“先跑起来”的必选项，见第 15 节。

### 2.2 运行

```bash
./build/samples/player/maque_player scenes/ball.scn
./build/samples/player/maque_player scenes/sokoban/sokoban.scn
./build/samples/player/maque_player samples/tank/tank.scn
```

**只有一个宿主**：`samples/player`。所有 sample 都用它打开场景，游戏逻辑全在
`.scn` + `.js`/`.lua` 里；不要给新的游戏再加一个 `main.cpp`。

### 2.3 一张图、一组图

```bash
# 第 30 帧一张 PNG（无窗口也能跑）
MAQUE_WINDOW=0x0 MAQUE_PRESENT=off \
  ./build/samples/player/maque_player --shot out/f30.png --shot-frame 30 scenes/ball.scn

# 动画/物理：按帧号批量截图
./build/samples/player/maque_player \
  --shot-seq out/f{}.png --shot-frames 1,30,60 scenes/anim/anim.scn

# 只跑 120 帧后正常退出（退出码 0，适合冒烟）
./build/samples/player/maque_player --frames 120 scenes/ball.scn
```

- `--shot` 默认第 1 帧；场景脚本里 `quit()` 可能提前结束，必要时加 `MAQUE_KEEP_OPEN=1`。
- PPM 是原始像素，适合 CI 逐字节比对；PNG 适合人看 / 预览插件。
- 无窗口截图的尺寸基准是场景设计分辨率 × 视图数 × `render_scale`（默认 1）。

### 2.4 窗口与呈现

| 变量 | 作用 |
|------|------|
| `MAQUE_WINDOW=WxH` | 窗口尺寸；`0x0` = 不建窗口（CI/无显示环境）。默认 800×650，首次建窗按逻辑屏自动放大。 |
| `MAQUE_PRESENT=auto\|sdl\|wgpu\|off` | 呈现路径。`auto` 在无像素消费者时优先 wgpu 直呈；`off` 不呈现（离屏/截图）。 |
| `MAQUE_RENDER_SCALE=N` | 相对窗口 drawable 的渲染倍率；`0.5` = 两轴各半、约 1/4 像素。 |
| `MAQUE_PACE=0\|1` | 墙钟限速。无窗口/截图/录制默认不限速；真窗口默认 1x 游戏时间。 |
| `MAQUE_MAX_FRAMES=N` | 最多跑 N 帧；到上限是正常结束（退出码 0）。 |

### 2.5 第一次验证什么

```bash
ctest --test-dir build -R 'player_run|determinism|ui_e2e|render3d_scene_e2e' --output-on-failure
```

如果这些过了，说明构建、场景加载、脚本、渲染、确定性主干都在。

---

## 3. 心智模型

### 3.1 引擎是“浏览器”，游戏是“资源”

```text
maque_player  <scene.scn>
      │
      ├─ VFS：场景目录 = 资源根（本地 / zip / http / Android asset://）
      ├─ Scene：节点树 + 行为（脚本集合）+ 场景配置
      ├─ 脚本 VM：JS / Lua，模拟线程、固定步长
      └─ 渲染：只读提取 → WebGPU
```

因此：

- **宿主不写游戏逻辑**。要加默认场景/参数，写在脚本或启动命令里。
- 游戏参数（重力、波次、数值）属于资源，放 `.scn` / 脚本 / JSON 资产，不要塞进 C++。
- 任何一个 `.scn` 都可以被直接打开运行；这是调试和 CI 的基础。

### 3.2 节点树是唯一模型

- 节点 = `{ name, components[...], children[...], parent }`；组件 = 带 `type` 的反射记录。
- **没有 ECS / archetype / system 调度**。每帧只有一次树遍历（前序 DFS）把局部变换累积成
  `world_x/world_y`、`world_m`、`visible/world_*` 等**派生缓存**。
- 位置真相在 `Transform2D` / `scene.Transform3D` 组件。改 `entity_x/entity_y` 的派生缓存会被下一帧覆盖。
- 渲染 / 物理 / UI / 命中都按**组件角色**从树里只读提取；引擎不认识“玩家”“子弹”这些游戏概念。

### 3.3 Scene = 节点树 + 行为 + 配置

结构示意（`entities` 省略，实际文件必须是合法 JSON）：

```jsonc
{
  "title": "my-game",
  "script": "game.js",
  "width": 320,
  "height": 240,
  "seed": 12345,
  "entities": [ /* nodes... */ ]
}
```

- `script` / `scripts` 是**场景级行为**：按声明顺序创建 VM/context。
- `global_script`：跨 `director_load_scene` 保留的**服务层**（进度、存档、跨场景规则）。
- `overlay_script`：跨场景保留的**外壳/HUD/导航**。
- 嵌套场景（`scn.SceneInstance`）自带自己的脚本与生命周期；父场景看不到子场景的局部变量，
  只能通过组件字段、`scene_emit`、`params`、`global_*` 通信。

### 3.4 SceneInstance：行为单元，不是万能节点

```text
根场景（实例 0，根脚本）
├─ 静态节点 / prefab 展开（纯数据）
├─ SceneInstance → 关卡 / 敌人 / 塔 / 弹窗（各自 .scn + 脚本）
└─ SceneInstance → HUD（独立 VM，读 global_*）
```

- **静态**：`.scn` 节点上挂 `scn.SceneInstance{source, params}`，加载期实例化。
- **动态**：`scene_instantiate(parent, path, params)`；句柄立即返回，实体/VM/`on_start`
  在**下一个帧边界**完成。
- **卸载**：`scene_unload(instance)` 帧边界执行 `on_destroy` 后销毁子树/VM。
- 子场景脚本 `scene_self()` 拿自己的实例 id，`scene_self_params()` 读父场景传入的 JSON；
  `scene_instance_parent()` / `scene_self_root()` 拿父子关系与根实体。

### 3.5 渲染是模拟的只读消费者

```text
脚本 update ──► 组件字段（唯一真相）
                   │
                   ├─ update_world()：世界矩阵/可见性/透明度（派生）
                   ├─ extract()：只读 → 绘制项（2D/3D/UI）
                   └─ render：GPU
```

- 渲染阶段**不得加载资源、不得回写模拟字段**。资源在装配期预热（`preload_scene_assets`）。
- 动态生成的实体要用 `node_set_string(..., "material"/"texture", ...)` 当场解析资源；
  渲染期不会替你加载。

### 3.6 生命周期与顺序（确定性的一部分）

```text
创建：实例创建 → 子先父后 on_start
每帧：父先子后 update(dt) → 物理/动画/粒子 → update_world → extract → render
销毁：子先父后 on_destroy → 取消定时器/补间/事件注册 → 销毁 VM/实体
```

| 事件 | 顺序 |
|------|------|
| 场景实例 `on_start` | **后序**（子先父）；实例内节点后序；场景级根实例最后 |
| `update` | **前序**（父先子）；实例内节点前序；根实例最先 |
| `on_destroy` | **后序**；根实例最后 |
| 节点自定义事件 | 目标 → 父 → … → 根（冒泡）；每个 handler 在注册它的实例上调用 |
| 键盘/文本 | 所有 active 实例 + 根 + overlay（root → 实例树先序 → overlay） |
| 指针 | 根实例 → target 所属实例及其祖先 → overlay |

---

## 4. 项目与场景组织

### 4.1 推荐目录

```text
mygame/
  maque.package.json        # 包清单（推荐；全 JS 场景才会走新模块路径）
  main.scn                  # 入口场景：编排 + 根脚本
  scripts/
    main.js
    lib/util.js
  levels/
    level1.scn + level1.js  # 关卡是行为单元
    level2.scn + level2.js
  actors/
    enemy.scn + enemy.js    # 有行为的可复用单元
    bullet.prefab.json      # 无行为的纯数据 prefab
  ui/
    hud.scn + hud.js        # 独立 VM 的 HUD
  art/  textures/  audio/  fonts/
  assets/                   # .mesh.json / .gltf / .glb / .skel.json / .clip.json
```

- **包根 = 最近的、包含 `maque.package.json` 的祖先目录。** 搬走整个目录就是搬走一个可运行单元。
- 没有清单的场景仍能跑（旧路径），但**不承诺可移植**；加载时会打 warning。
- 资源路径默认相对**场景文件目录**（运行时 VFS 根 = 场景目录）。共享包内用 `pkg:/...`。
- 不要提交派生资产（`scenes/khronos/*/assets`、`textures/`、`preview.png`）；提交
  `.scn`、手写脚本、README、manifest。构建产物与 `.maque/` 不入库。

### 4.2 包清单

```json
{
  "id": "mygame",
  "version": "1.2.0",
  "api": 1,
  "backends": ["js"]
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `id` | 是 | `[a-z0-9_.-]`，包身份 |
| `version` | 是 | 语义化版本；`id@version` 是模块键的一部分 |
| `api` | 是 | 依赖的脚本 API 主版本 |
| `backends` | 否 | 缺后端时明确报错 |
| `isolation` | 否 | `shared`（默认）/ `realm`（P2） |

**最佳实践**：

- 新项目、全 JS → 用包 + ESM；一个包一个 `id@version`。
- 需要 Lua 或 JS+Lua 混用 → 暂时不要指望包的可移植性；按场景目录组织，走旧路径。
- 不要把两个项目的 `lib/util.js` 放在同一个共享 VM 路径下靠“同名碰运气”隔离。

### 4.3 场景粒度：一个 `.scn` = 一个行为单元

推荐：

| 粒度 | 放什么 | 脚本？ |
|------|--------|--------|
| 根场景 | 编排、全局状态入口、输入/相机 | 1 个根脚本（+ global/overlay） |
| 关卡 | 地形、出生点、波次配置 | 有关卡行为时放脚本；纯数据可不放 |
| HUD / 菜单 | 独立 UI 与生命周期 | 独立 `.scn` + 脚本，读 `global_*` |
| 敌人/塔/弹窗 | 少量、独立行为、可复用 | `SceneInstance` + 自己的 `.scn` |
| 子弹/粒子/瓦片 | 大量、同构、生命周期短 | **prefab 池 + 根脚本批量驱动**，不要每个一个 VM |

**为什么不要“每个对象一个场景实例”**：每个 `SceneInstance` 有自己的脚本 context/VM 组；
大量同类对象会让 VM 数、内存、事件派发成本线性增长。`samples/tank` 的做法是：

- 46 个 actor 用 prefab 在装配期建池，运行期只 `set_active`/换纹理，不 spawn/destroy；
- 关卡地形是纯数据 `.scn`，根脚本遍历一次建碰撞网格；
- HUD 是独立嵌套场景（独立 VM），只通过 `global_*` 读数据；
- 切关只卸载/实例化 stage，根场景、HUD、全局状态不重建。

### 4.4 prefab、SceneInstance、global/overlay 怎么选

| 需求 | 选择 |
|------|------|
| 纯数据模板、批量复用、无行为 | `.prefab.json`（或纯数据 `.scn`） |
| 有行为、独立生命周期、少量 | `.scn` + `scn.SceneInstance` |
| 需要父 → 子配置 | `SceneInstance.params`（JSON 文本）+ `scene_self_params()` |
| 需要子 → 父/兄弟通信 | `node_emit_number`（冒泡）/ `scene_emit(instance, event, value)` |
| 跨场景保留的 HUD/外壳 | `overlay_script` |
| 跨场景保留的服务（进度/存档/规则） | `global_script` |
| 同一份代码的 N 个实例 | 包模块 + 每挂载点 context（全 JS），或 prefab 池 + 根脚本 |

### 4.5 命名即数据

场景里的名字会被脚本按名查找，建议把它当成**稳定的逻辑标识**：

- 路径点：`WP0`、`WP1`…（`node_find_path(-1, "WP" + i)`）；
- 瓦片：`tile_c09_r10_brick`（根脚本遍历一次解析坐标/类型）；
- UI：`ScoreLabel`、`SpawnButton`、`HUD`；
- 关卡：`LevelName`、`Hint`、`player_start`、`spawn_1`。

改名会破坏脚本查找；改名时全局搜索 `.js`/`.lua`/`.scn`。

### 4.6 不要做什么

- 不要把 676 个瓦片每个都写成 `SceneInstance` 或挂脚本。
- 不要把同一份地图在 `.scn` 和 Lua 里各抄一遍（`samples/tank` 明确说“关卡是数据”）。
- 不要让所有逻辑挤在一个根脚本里，同时让子场景“看起来有行为”却没有脚本。
- 不要在 `on_start` 里做重 IO / 生成大量实体；装配期已经预加载，运行期批量操作放帧边界。
- 不要用 `director_load_scene` 做“关内小切换”；它替换整个世界（根脚本会重建）。关内切块用 `SceneInstance`。

---

## 5. 场景编写

### 5.1 最小 2D 场景

```json
{
  "title": "hello",
  "script": "hello.js",
  "width": 320,
  "height": 240,
  "seed": 12345,
  "entities": [
    {
      "name": "player",
      "components": [
        { "type": "scn.Transform2D", "x": 160.0, "y": 60.0 },
        { "type": "scn.Sprite", "radius": 12.0, "r": 1.0, "g": 0.8, "b": 0.2 }
      ]
    }
  ]
}
```

```js
'use strict';
const S = { vy: 0 };

function on_start() { log('hello'); }

function update(dt) {
  const id = entity_find('player');
  if (id < 0) return;
  S.vy -= 900.0 * dt;                          // 世界 y 向上；向下掉 = 负加速度
  let y = entity_y(id) + S.vy * dt;
  if (y <= 12.0) { y = 12.0; S.vy = -S.vy * 0.86; }
  entity_set_pos(id, entity_x(id), y);
}
```

要点：

- 2D 用 `scn.Transform2D` + `scn.Sprite`；3D 用 `scene.Transform3D` + `render.*`。**命名空间不同**。
- `scn.Sprite` 的 `width/height > 0` 画矩形；否则按 `radius` 画圆。
- `entity_find` 按名字找节点；`entity_*` 是 `node_*` 的按 id 扁平别名。
- 改位置用 `entity_set_pos`/`node_set_position`，不要直接改 `world_x/world_y`（派生缓存）。

### 5.2 组件配对

| 组件 | 必须/建议搭配 | 说明 |
|------|---------------|------|
| `scn.Sprite` / `render.TileMap` / `render.Camera2D` | `scn.Transform2D` | 缺 Transform 会被提取阶段跳过 |
| `render.Camera3D` / `render.MeshRenderer` / `render.Light` | `scene.Transform3D` | 同上 |
| `ui.Control` 与 `ui.Button` / `ui.TextLabel` / `ui.Image` 等 | 同节点 | UI 控件的布局/命中基座 |
| `scn.Button` / `scn.Label` | 可单独使用（旧式简单 UI） | 适合 HUD；复杂布局用 `ui.*` |
| `render.RenderTexture` | 同节点必须有一个相机 | 否则 `ready=false` |
| `audio.AudioSource` | 空间化时放在有世界位置的节点 | 当前按节点 `world_x/world_y` 做 2D 空间化 |
| `asset.PbrMaterial` | `render.MeshRenderer` | 材质挂在同一个节点 |

### 5.3 `render.Light` 的重复 `"type"` 键（必读）

`.scn` 里灯光组件同时有**组件类型**和**数值类型**，所以它有两个 `"type"` 键：

```json
{ "type": "render.Light", "type": 0, "intensity": 1.2,
  "color": { "r": 1.0, "g": 0.95, "b": 0.85, "a": 1.0 } }
```

- 第一个 `"type"` 是组件类型名；第二个 `"type"` 是 `LightType`（0=Directional、1=Point、2=Spot）。
- **用 `json.load` / `json.dump` / 任何标准 JSON 库读写 `.scn` 会静默丢掉一个 `type`**：
  灯光会变成默认类型或直接丢组件。仓库里的迁移工具用 `_JsonPairs` 保留重复键。
- 手工编辑可以；脚本化修改请用仓库工具（`tools/migrate_gltf_models.py` 的 `_JsonPairs` 思路），
  改完用 `MAQUE_RENDER3D_INFO=1` 检查灯数/类型。

### 5.4 材质与 `side`

```json
{ "type": "asset.PbrMaterial",
  "shading_model": 0,
  "base_color": { "r": 0.9, "g": 0.75, "b": 0.3, "a": 1.0 },
  "metallic": 1.0,
  "roughness": 0.25,
  "side": 0 }
```

- `shading_model`：0 Standard、1 Physical、2 Basic、3 Lambert、4 Phong、5 Toon、6 Normal、
  7 Matcap、8 Depth、9 Shadow。
- **Culling 只认 `side`**（0=Front、1=Back、2=Double）。默认是 2（兼容旧场景），
  但闭合不透明网格建议显式 `"side": 0`，减少双面绘制。
- 贴图槽：albedo/emissive 走 sRGB；MR/AO/normal 走线性。手写时不要把数据贴图丢进颜色槽。
- 透明：`alpha_mode` = Opaque / Mask / Blend；玻璃/透射用 `transmission_factor` 等近似参数。
- 改材质数值用类型化字段或 `node_set_number(id, "asset.PbrMaterial", "base_color.r", v)`；
  改贴图/材质字符串用 `node_set_string`（会当场解析并缓存）。

### 5.5 层级与 z 排序

- 2D 排序键：`(layer, z, 原始序)`；`z_index` 越小越先画（越靠底）。
- UI 图层 `layer=1`；文字默认 `z=100+`，所以 Label 会盖在普通 UI 图元上。
- 常见分层（`samples/tank`）：地形 10、坦克 20、子弹 30、树 40、特效 50、HUD 100、文字 9000。
- 父子节点会累积 `angle/scale/opacity`：子节点的角度是**相对父节点**的局部角。
- `node_set_active(id, 0)` 隐藏整棵子树（含渲染/命中）；`node_set_opacity` 会乘到子树。

### 5.6 UI：两套写法，别混用

| 写法 | 组件 | 适合 | 注意 |
|------|------|------|------|
| 简单 UI | `scn.Button` / `scn.Label` | HUD、分数、几个按钮 | 坐标是相对节点的偏移；不同控件的 y 锚点不统一（Label 顶部偏移、Button 底部偏移），**用截图校准** |
| 布局 UI | `ui.Control` + `ui.Button`/`ui.TextLabel`/`ui.Image`/`ui.ListView`/`ui.Mask` | 面板、列表、九宫格、裁剪 | 锚点/offset 语义见 `docs/components/04`；复杂场景参考 `scenes/ui_patch/`、`scenes/ui_list/` |

**最佳实践**：

- HUD 用独立 `SceneInstance`（独立 VM），只读 `global_*`；不要在根脚本里直接抓 HUD 子节点。
- 长列表用 `ui.ListView`：设置视口 `Control.offset_max`、`item_count/item_extent`、`clip_content=true`；
  需要淡出时用 `fade_top/bottom` + `fade_unit=Percent`。
- 图片进 UI 动态图集的条件：RGBA8、单 mip、clamp、任一边 ≤ 图集/4（默认 1024 → ≤256），
  且不是 mesh/字体页。**整屏背景/大图不会进图集**，不要用一张 4K 图铺满 UI。
- 九宫格 `nine_patch_margin` 的单位是**源图像素**；目标过小时边距会压缩，角不重叠。
- `ui.Mask` 当前只有矩形裁剪（含 reverse）；圆角/纹理/stencil 未实现。
- 文本度量（`ui_text_width/height`、`text_w/h`）在排版后才有值；`on_start` 里先不要依赖。

### 5.7 坐标速查

| 用途 | 坐标空间 | 备注 |
|------|----------|------|
| 2D 世界 / 物理 / `entity_x/y` / `node_position_*` | 设计世界坐标，**y 向上** | 向下掉 = 负重力；屏幕显示时引擎翻转 |
| `input_x` / `input_y` | 世界坐标（与 2D 世界同一套） | 设备事件在平台边界转换；模拟事件也按世界坐标 |
| `scene_pick(x, y)` | 设计坐标（同 `input_x/input_y`） | 3D 屏幕射线 → OBB 命中 |
| `scene_raycast(...)` | 3D 世界坐标 | 返回最近命中的 MeshRenderer 实体 |
| `scn.Label.x/y` | 相对节点位置的偏移 | 文本向下排版；`text_w/h` 是设计像素 |
| `scn.Button.x/y` | 相对节点位置的偏移 | 矩形向上扩展（当前实现） |
| `ui.Control` 锚点/offset | UI 布局空间 | 见 `docs/components/04`；不要和世界坐标混算 |
| `rendertexture://name` | 纹理 URI | 根场景可写短形式；子场景按消费者实例解析 |

**经验法则**：游戏逻辑一律用设计/世界坐标；窗口尺寸只用于“展示层自适应”
（`window_w/h`、`canvas_scale`、`canvas_offset_*`），不要用它驱动玩法判定。

### 5.8 资源路径

- `.scn` 的 `script` / `scripts` / `overlay_script` / `global_script`：相对**场景文件目录**；
  包内必须落在包根内。
- 组件资源路径（`texture`、`material`、`mesh_path`、`clip`、`font`、`skeleton`、`atlas`、
  `*_texture`）：相对**场景目录**（运行时 VFS 根）。
- `builtin:/...`：引擎内建资源（shader、程序化纹理）。
- `pkg:/...`：包内共享资源根（推荐共享包使用）。
- Android：场景在 APK assets 里，路径仍是相对场景目录；VFS 后端读 `asset://`。
- 远程：`https://host/path/game.scn`；场景内资源相对远程目录解析；`MAQUE_MOUNT` 可加本地覆盖。

**坑**：嵌套场景内组件路径会按实例 base 改写，但**材质文件内部引用的贴图**仍按根场景目录解析
（`docs/32 §14` 的已知边缘）。跨场景复用的材质文件，贴图路径尽量写成稳定的 VFS/`pkg:/` 路径。

### 5.9 场景级字段

| 字段 | 最佳实践 |
|------|----------|
| `width` / `height` | 设计分辨率；窗口缩放/留边由引擎处理，不要在脚本里按窗口改玩法坐标 |
| `seed` | 固定种子；配 `rng()` 做确定性随机 |
| `gravity_y` | 默认 `-900`（世界 y 向上）；要“向下掉”就保持负值 |
| `restitution` | 弹性 0..1；默认 0.86 |
| `render_scale` | 主输出倍率；性能不足时 0.5/0.75，不要改设计分辨率 |
| `views` / `eye_sep` | 多视图/VR；普通游戏保持 1 |
| `env_color` / `env_intensity` | 3D 影棚环境光；默认值就是引擎旧硬编码，老场景零回归 |
| `env_texture` / `env_cubemap` | HDRI IBL；`env_background=false` 时只做 IBL 不画天空 |
| `msaa_samples` | 0/1 关、4 开；逐 RenderTexture 另有 `msaa_samples` |
| `global_script` / `overlay_script` | 只在根场景声明；嵌套场景声明会被忽略并警告 |

### 5.10 保存与生成物

- 运行期修改场景后，用控制面 `Scene.save {path}` 落盘（反射驱动、连续保存逐字节一致）。
- `Scene.bake_instances` 会把实例标成 `detached` 并写出自包含子树；之后不再按 source 实例化。
- **不要手改生成物**：`scenes/khronos/README.md`、`manifest.json`、`assets/*.mesh.json` 等由工具生成；
  要改就改源/工具，或加 `--exclude` 保护手调场景。
- 用 `.scn` 版本字段（v2）做向前兼容；旧场景没写 `version` 仍可加载。

---

## 6. 脚本

### 6.1 选型

| 场景 | 推荐 |
|------|------|
| 新项目、全 JS、需要模块/可移植 | **包 + ESM**（`maque.package.json`） |
| 单场景 demo、快速验证、Lua | 场景级顶层脚本（旧路径） |
| JS 玩法 + Lua AI 混用 | 旧路径（`scripts: ["game.js", "ai.lua"]`） |
| 需要 C/C++ | native guest（信任内容；P0 每脚本一个 VM，组件/实例面未全覆盖） |

- 语言由扩展名决定：`.js` / `.lua` / `.c/.cc/.cpp`。
- 所有语言共用同一套 API 名、回调名、事件语义。包模块（当前仅 JS）里 `this` 是实例上下文；Lua 模块化是 P2，旧路径的 Lua 仍是顶层全局函数。
- 默认后端是 JS；Lua 可用 `-DMAQUE_ENABLE_LUA=OFF` 裁剪，未编入时明确报错。

### 6.2 两种脚本形态

#### A. 场景级顶层脚本（旧路径，稳定、热更可用）

```js
'use strict';
const S = { t: 0 };

function on_start() { S.t = 0; log('ready'); }
function update(dt) { S.t += dt; }
function on_pointer_down(target, x, y) { /* ... */ }
```

- 回调是**顶层函数声明**；引擎按名字从 VM 全局对象取。
- 每个脚本槽一个 VM；同一场景多个脚本按 `scripts` 声明顺序投递。
- 热重载会**新建 VM**：`S` 不跨重载保留；`on_start` 会重跑，记得重新 `node_on`。

#### B. 包模块（全 JS，P0）

```js
// scripts/enemy.js
export default function () {
  const S = { hp: 20, t: 0 };                 // 每挂载点闭包状态

  function on_start() {
    // 场景级：this.node === -1；子场景：this.node === 根实体 id
    log('enemy start node=' + this.node + ' package=' + this.package);
    const body = entity_find('Body');          // 不要对 -1 注册节点事件
    if (body >= 0) node_on(body, 'damage', 'on_damage');
  }
  function on_damage(id, amount) { S.hp -= amount; }
  function update(dt) { S.t += dt; }
  return { on_start, on_damage, update };
}
```

```js
// 也可以导出对象 / class
export default {
  on_start() { /* this.node / this.params */ },
  update(dt) { /* ... */ },
};
```

```js
export default class Enemy {
  on_start() { /* this 是实例；构造函数早于上下文注入，别在 constructor 用 this.node */ }
  update(dt) { /* ... */ }
}
```

- 模块在装配期求值一次；每个挂载点一个 context。`this.node/scene/package/params` 是保留名。
- **模块顶层无副作用**：不要读世界、读墙钟、发事件、注册 handler、`schedule`；只放定义/常量/纯缓存。
- **不要用 `this.props`**（P1 未接线）。持久状态见 6.3。
- `this.params` 是只读对象（来自 `SceneInstance.params`）；无参数时 `{}`。
- 同一模块的多个实例共享定义/原型，但 `this`/闭包字段每实例独立。

**两种形态的差异**：

| 项 | 顶层脚本 | 包模块 |
|----|----------|--------|
| 模块求值 | 每脚本 VM 一次 | 每 VM 每模块一次 |
| 实例状态 | VM 全局/闭包 | `this`/工厂闭包，每挂载点 |
| 上下文 `this` | 无（全局函数） | 实例 |
| 热重载 | 支持（按文件、原子） | **未支持（P1）** |
| 适用 | 快速 demo / Lua / 混合语言 | 新项目、全 JS、可移植 |

### 6.3 状态放在哪里

| 状态 | 生命周期 | 用途 |
|------|----------|------|
| 脚本顶层变量 / 闭包 / `this` 字段 | 当前 VM / context；**热重载、快照恢复会丢** | 帧内临时状态、缓存 |
| 组件字段（Transform、Sprite、自定义字段等） | 场景/世界；进快照 | 世界状态唯一真相 |
| `global_*` | Engine；跨 `director_load_scene`；进世界快照 | 跨场景进度、金币、解锁 |
| `storage.*` | 跨进程；磁盘/服务端 | 存档、设置、玩家数据 |
| `this.props` / `Script.exposed` | — | **当前不要用**（P1） |

**规则**：

- 需要“读档/热更/回滚后还在”的，放组件字段或 `storage`；不要放闭包。
- 需要“跨场景但不需要跨进程”的，放 `global_*`；键名加前缀（如 `td.gold`）。
- 需要“玩家下次启动还在”的，放 `storage`，并带 schema 版本号。
- 不要用 `global_*` 存大数据/二进制；它是数值/文本的小表。

### 6.4 生命周期与回调

当前只有这些回调（生成自 `schema/script.yaml`）：

| 回调 | 参数 | 说明 |
|------|------|------|
| `on_start` | — | 场景实例装配后；热重载换入新 VM 后也会再派发一次 |
| `update` | `dt` | 每模拟帧 |
| `on_pointer_down` / `on_pointer_up` / `on_pointer_move` | `target, x, y` | 指针；坐标是世界坐标 |
| `on_wheel` | `target, x, y` | 滚轮 |
| `on_click` | `target, x, y` | 点击（down+up 语义） |
| `on_key_down` / `on_key_up` | `target, key` | 键码见 8.2 |
| `on_text` | `target, text` | 文本输入/IME |
| `on_destroy` | — | 场景实例卸载前；用于清理 |

没有 `ready` / `process` / `physics_process` / `on_enter_tree` / `on_exit_tree` 这些回调。
物理/动画/粒子/渲染由引擎在固定点推进，不需要脚本再驱动（2D 物理示例里的 `physics_step()` 是显式推进旧式球模拟的 API，不是通用生命周期）。

**最佳实践**：

- `on_start` 只做查找/注册/初始化；重活放装配期或帧边界。
- `node_on` 注册放在 `on_start`，因为热重载会先摘旧注册再重派 `on_start`。
- `on_destroy` 里取消定时器/补间/音频 voice（如果它们不随节点自动清理）。
- 不要在回调里阻塞、sleep、同步网络、读墙钟。

### 6.5 事件

```js
// 注册：只能注册当前实例子树内的节点（根实例 / global / overlay 可任意）
node_on(enemy, 'damage', 'on_damage');

// 触发：沿父链冒泡；handler 收到 (target_id, value)
node_emit_number(enemy, 'damage', 12);

// 跨实例：显式路由到目标 SceneInstance 中同名的 handler，收 (0, value)
scene_emit(levelInstance, 'enemy_dead', uid);

// 全局脚本：调用 global_script 中名为 level_finished 的 handler，收 (0, value)
global_emit('level_finished', 1);
```

- 载荷只有标量/字符串；不要试图跨 VM 传对象/表。
- 同名 handler 分属不同实例时互不串扰（记录带 `owner_instance`）。
- 未注册的目标不报错（静默跳过）；handler 抛错只影响该 context，其他脚本继续。
- 事件注册顺序 = 派发顺序；父链冒泡深度有上限（防递归）。
- 指针事件只路由到 target 所属实例及祖先；键盘/文本给所有 active 实例。

### 6.6 定时器与补间

```js
schedule_once('on_timer_once', 0.5);     // 0.5s 后一次，handler(dt)
schedule('on_timer_repeat', 0.25);       // 每 0.25s，handler(dt)
unschedule('on_timer_repeat');

tween_start(node, 'scn.Sprite.radius', 20.0, 0.5, 0);  // 0.5s 线性到 20
tween_stop(node);
```

- 定时器/补间有 owner 归属；节点/实例销毁时应取消（引擎会尽量清理，但显式清理更稳）。
- 不要用 `schedule` 做每帧逻辑；每帧逻辑用 `update`。
- 补间的字段路径是 `组件类型.字段`（可嵌套，如 `scn.Sprite.radius`）。

### 6.7 热重载

```bash
MAQUE_WATCH=1 ./build/samples/player/maque_player scenes/ball.scn
```

- 按文件增量：只重建被改脚本的槽位；其它脚本的 VM/状态保留。
- **原子**：新 VM eval 成功才换入；失败保留旧 VM 并打印错误。
- 换入后只对新 VM 派发一次 `on_start`；旧 `node_on` 会先被摘除。
- **状态不迁移**：脚本顶层变量、闭包、`globalThis` 都不跨重载保留。
- **包模块热重载未实现（P1）**：有 `maque.package.json` 的全 JS 场景改脚本请重启。
- 嵌套场景脚本也在监视列表；改嵌套 `.scn` 会帧边界重建该实例。

### 6.8 错误与日志

- 脚本异常/受保护调用失败：该 context 记录错误，其他脚本继续；看 stderr / 崩溃上下文。
  场景**实例化**失败（路径/循环/预算）另查 `scene_last_error()`。
- 用 `log(msg)` 输出结构化可读日志；不要每帧刷屏（会拖慢）。
- 所有句柄都可能失效：`node_valid(id)` / `scene_instance_valid(inst)` / `entity_valid(id)` 再操作。
- 资源加载失败通常返回 `-1` 或空串；不要假设成功。例如 `mesh_path` 失败会回退内建 cube，
  日志里是 `mesh=-1`。
- 用 `assert` 风格：脚本里发现不变量不成立就 `log` + 停用该逻辑，不要静默继续。

### 6.9 脚本性能

- 不要在 `update` 里做 `entity_find`/`node_find_path` 全树查找；在 `on_start` 缓存 id。
- 不要每帧 `JSON.parse/stringify` 大对象；存档/配置在加载/保存时做。
- 不要每帧创建大量临时字符串/数组；尽量复用。
- 内置组件用类型化 API（`node_set_number`、`ui_set_value`）而不是反射慢路径；
  反射路径适合脚本/自定义组件。
- 不要每帧 `log`；需要观察时用计数器 + 每 N 帧输出。
- 渲染/物理是主循环的大头；脚本热点先用 `MAQUE_TRACE_STATS=1` 定位，不要猜。
- 目前没有指令预算（P2）；死循环会卡住模拟线程，写循环要保证退出条件。

### 6.10 安全边界

- 本地脚本默认可信；远程内容（`https://`）不要直接执行不受信的原生 C/C++。
- 包隔离 `realm` 是 P2；当前单 VM 共享堆/GC/异常域，“一个包吃爆内存会影响全部”。
- 不要把 `storage` 当安全边界（`uid` 只是命名空间，不是鉴权）。

---

## 7. 确定性与回放

### 7.1 硬规则（写游戏时逐条对照）

| 规则 | 做法 | 违反后果 |
|------|------|----------|
| 固定步长 | 用引擎 `update(dt)` 的 `dt`；不要自己按墙钟算 | 不同机器速度不同 |
| 引擎 RNG | 用 `rng()`；种子来自场景 `seed` | 回放序列不同 |
| 禁墙钟 | 不读 `Date`/`performance.now`/系统时间 | 回放漂移 |
| 单线程模拟 | 不在脚本里开线程/worker 改世界 | 顺序不定 |
| 帧内无异步 | 不 `await`、不 `fetch`、不动态 `import()` | 帧时序不确定 |
| 按 id 排序 | 遍历实体/实例用 id/声明序，不用对象键序/指针 | 顺序漂移 |
| 不依赖窗口尺寸 | 玩法用设计坐标；窗口只做展示 | 分辨率不同结果不同 |
| 事件驱动优先 | 按键/点击改变状态，而不是每帧轮询时间 | 帧率耦合 |
| 热更/快照后重建 | 不把持久状态放闭包 | 状态丢失/漂移 |
| 浮点范围 | 默认只保证同机同构建（L1）；跨平台别承诺逐位 | L2/L3 差异 |

### 7.2 输入即数据

- 每帧输入事件按到达顺序进入固定处理点；回放喂同样的事件序列 → 相同分发。
- 设备事件与模拟事件走同一条路径（`mq_input_inject` → 输入模块 → 命中 → 回调）。
- 录制/回放等价：模拟输入会被录制覆盖；回放模式下模拟器不喂事件，只吃记录。
- 时间戳只用于渲染插值，不参与模拟判定。

### 7.3 快照、校验和与回放

```bash
# 逐帧校验和（默认关闭，避免每帧遍历全场景）
MAQUE_CHECKSUM=1 ./build/samples/player/maque_player --frames 300 scenes/ball.scn

# 轨迹输出 / 录制 / 回放 / 崩溃报告
MAQUE_TRAIL_OUT=trail.txt MAQUE_MAX_FRAMES=300 ./build/samples/player/maque_player scenes/ball.scn
MAQUE_RECORD=run.rec ./build/samples/player/maque_player --frames 300 scenes/ball.scn
MAQUE_REPLAY=run.rec ./build/samples/player/maque_player scenes/ball.scn
MAQUE_CRASH_REPORT=crash.json ./build/samples/player/maque_player scenes/ball.scn
```

- 快照保存实体/组件 + 实例表 + 全局状态；**不保存脚本 VM/闭包**。恢复 = 重建 context + 重派 `on_start`，
  数据状态从组件读回。
- 默认退出只算一次终局指纹；需要逐帧轨迹时显式开 `MAQUE_CHECKSUM=1`。
- 失步时先找**首个分歧帧**，再用快照/检查点二分；不要从最后一帧猜。
- 确定性等级：L1 同机构建保证；L2 同平台跨构建努力；L3 跨平台不承诺（WASM 更稳）。

### 7.4 常见破坏确定性的写法

```js
// ✗ 系统随机
const r = Math.random();

// ✓ 引擎 RNG
const r = rng();

// ✗ 墙钟
const t = Date.now() / 1000;

// ✓ 模拟帧
const t = director_frame() / 60.0;

// ✗ 每帧按窗口尺寸改玩法
const scale = window_w() / 800.0;

// ✓ 设计坐标
const scale = 1.0;

// ✗ 对象键序
for (const k in enemies) { ... }

// ✓ 排序后的键/数组
const ids = Object.keys(enemies).map(Number).sort((a, b) => a - b);
```

### 7.5 回放/回归的推荐姿势

- 场景固定 `seed`；不要在 `on_start` 里用当前时间初始化。
- 冒烟测试用 `--frames N` 或控制面 `Runtime.step {frames:N}`，不要用墙钟等待。
- 视觉回归用 PPM 逐字节；跨 GPU/驱动只做结构性断言（非背景占比/中心色/区域差异）。
- 性能回归用固定长度 + 中位数；不要在 Debug/ASan 上比绝对毫秒（见第 13 节）。

---

## 8. 输入与 UI

### 8.1 坐标

- `input_x` / `input_y`：**世界坐标**。设备事件在平台边界转换；模拟事件也按世界坐标注入。
- 2D 世界 y 向上；屏幕/图片 y 向下由引擎翻转。不要自己再翻一次。
- `scene_pick(x, y)` 收设计坐标（同 input）；`scene_raycast` 收 3D 世界坐标。
- 窗口坐标只用于宿主/工具；游戏逻辑用设计坐标。

### 8.2 键码（引擎规范键空间）

| 键 | 值 |
|----|----|
| Esc | 1 |
| Space | 2 |
| Enter | 3 |
| Tab | 4 |
| Backspace | 5 |
| Left / Right | 6 / 7 |
| Up / Down | 8 / 9 |
| 字母/数字 | **大写 ASCII**（`A`=65、`W`=87、`0`=48） |

真实设备事件、`.sim.json` 注入、脚本 `input_key()` 三条路径最终用同一套码。
SDL 风格码在 `.sim.json` 里也可写，会被归一化。

### 8.3 回调与轮询

```js
function on_key_down(target, key) {
  if (key === 32) jump();          // Space
}
function update(dt) {
  if (input_key(65) > 0) moveLeft();   // A 按住
}
function on_click(target, x, y) {
  if (target === spawnBtn) spawn(x, y);
}
```

- 事件适合“一次动作”（跳跃、开火、切页）；`input_key` 适合“按住/持续移动”。
- `target` 是命中的 UI/节点 id（未命中为 -1）。
- `input_target()` / `input_down()` / `input_focus()` 是当前帧的派生状态。
- `ui_hit` / `ui_hover` / `ui_pressed(id)` 用于 UI 自省；复杂交互优先用 `on_click` + `target`。

### 8.4 UI 命中与 `mouse_filter`

- 命中先看 UI 层（z-order + 树深），再是显式 3D 拾取/根处理器。需要“模态”时用全屏 `ui.Control`
  + `mouse_filter=Stop` 的遮罩层，而不是指望内置 Dialog。
- `ui.Control.mouse_filter`：`Stop`（消费，默认）、`Pass`（自身处理后继续冒泡）、`Ignore`（不参与命中）。
- 纯展示的覆盖层设 `Ignore`，否则会挡住下面的按钮。
- 3D 世界没有自动 hit-test 链；需要点击 3D 对象时，在 `on_pointer_down` 里显式 `scene_pick`。
- 指针捕获/拖拽由 UI 控件内部处理；脚本侧用 `on_pointer_down/up/move` 实现自定义拖拽时，
  记得用 `input_target()` 判断是否命中自己的控件，并在 `up` 时清理状态。

### 8.5 ListView / Mask

- `ui.ListView`：视口 = `Control.offset_max`；内容滚动只移动 `content_root`，子节点局部坐标不变。
- 大文本：`text` 非空时自动生成 `scn.Label` 承载全文；纯子节点模式用 `item_count/item_extent/spacing`。
- 视口裁剪：`clip_content=true`；边缘淡出：`fade_top/bottom/left/right` + `fade_unit`（Pixels/Percent）。
- 滚动条/百分比默认可见；内容淡出不影响滚动条。
- `ui.Mask`：矩形裁剪 + reverse；嵌套求解；命中测试与裁剪使用同一矩形集合。
- 不要用 Mask 做圆角；用带透明角的九宫格图。

### 8.6 文本输入与移动端

- 文本输入走 `on_text(target, text)`；IME 组合中间态不进 `text`。
- Android 默认不 `SDL_StartTextInput`，需要时设 `MAQUE_TEXT_INPUT=1`。
- 3D 主相机在移动端支持：单指环绕、双指平移、双指捏合缩放；走同一套 pointer 事件，
  不影响 2D 场景的脚本/UI 回调。
- 移动端 UI 用 `ui.Canvas` 的 `fit_mode`（ShowAll/NoBorder/Exact…）+ 安全区
  `canvas_safe_area(edge)`；不要按物理像素硬编码。

### 8.7 输入模拟器（开发/CI/演示）

```json
{
  "name": "click-smoke",
  "space": "window",
  "events": [
    { "frame": 10, "type": "click", "x": 160, "y": 120 },
    { "delay": 5, "type": "pointer_move", "x": 180, "y": 140 },
    { "frame": 30, "type": "key_down", "key": 32 }
  ]
}
```

```bash
MAQUE_SIM=tests/data/sim/ball_clicks.sim.json \
  ./build/samples/player/maque_player --frames 60 scenes/ball.scn

# 只点几下（窗口坐标，内联场景）
MAQUE_SIM_CLICK="10:160:120;20:200:150" ./build/samples/player/maque_player --frames 40 scenes/ball.scn
```

- `frame` 绝对优先；`delay` 相对上一条；同帧保持声明顺序。
- `space`：`window`（宿主映射到世界）或 `world`；事件级可覆盖。
- 模拟事件走与真实设备完全相同的路径，可被录制/回放。
- CI 里优先用 `.sim.json`，不要依赖真实鼠标/窗口焦点。

---

## 9. 渲染

### 9.1 分层与顺序

```text
3D 层：ShaderPass（背景）→ 天空/3D PBR/阴影 → （可选）PostEffect
2D 层：Sprite / 2D mesh / 粒子
UI 层：ui.* / scn.Label / 文字
ImGui：视图层调试面板（不进快照/校验和）
```

- 后处理默认只处理 **3D 层**；2D/UI 叠在结果上。勾选 `post_2d=1` 才整帧处理。
- 渲染是模拟的只读消费者；渲染选项（F4）不写场景、不进快照/校验和。
- `render_scale` 是主输出倍率；窗口缩放/留边由 present 处理。

### 9.2 2D

- Sprite：`width/height > 0` 画矩形，否则画圆；`texture` 为空用颜色。
- 排序：`(layer, z, 原始序)`；`node_set_z_index` 立即生效，不用重建场景。
- 合批：同贴图/采样器的相邻图元合并；UI 层按状态分组（顺序安全用 AABB 粗网格检查）。
- 动态图集：只收小图（RGBA8/单 mip/clamp/边 ≤256）；字体图集与 2D 图集分开；
  spine/ImGui/mesh 不进自动合图。整屏背景/大图不进图集，避免挤掉小图。
- 诊断：`MAQUE_TRACE_BATCH=1` 看 `items/draws/segments/atlas`；`MAQUE_UI_BATCH=0` / `MAQUE_UI_ATLAS=0` 做 A/B。
- 回归要求：开/关合批的 PPM 逐字节一致；改 2D 贴图路径时跑 `ui_batch|ui_atlas|text_pixel|spine`。

### 9.3 3D

**最小 PBR 场景**：

```json
{
  "title": "pbr",
  "width": 640,
  "height": 360,
  "env_intensity": 1.0,
  "entities": [
    { "name": "camera", "components": [
      { "type": "scene.Transform3D", "local": {
          "position": { "x": 0.0, "y": 1.0, "z": 4.4 },
          "rotation": { "x": -0.22, "y": 0.0, "z": 0.0 } } },
      { "type": "render.Camera3D", "fov_y": 55.0, "near": 0.1, "far": 100.0,
        "clear_color": { "r": 0.07, "g": 0.08, "b": 0.11, "a": 1.0 } }
    ]},
    { "name": "sun", "components": [
      { "type": "scene.Transform3D", "local": { "rotation": { "x": -0.75, "y": 0.6, "z": 0.0 } } },
      { "type": "render.Light", "type": 0, "intensity": 1.2,
        "color": { "r": 1.0, "g": 0.95, "b": 0.85, "a": 1.0 } }
    ]},
    { "name": "cube", "components": [
      { "type": "scene.Transform3D", "local": {
          "position": { "x": 0.0, "y": 0.0, "z": 0.0 },
          "scale": { "x": 1.0, "y": 1.0, "z": 1.0 } } },
      { "type": "render.MeshRenderer" },
      { "type": "asset.PbrMaterial", "metallic": 0.9, "roughness": 0.25,
        "base_color": { "r": 0.9, "g": 0.75, "b": 0.3, "a": 1.0 },
        "side": 0 }
    ]}
  ]
}
```

**材质/光照建议**：

- 金属用 `metallic=1` + 低 `roughness`；绝缘体 `metallic=0`；不要靠“提高环境光”救暗部。
- 环境：`env_texture`（HDRI/EXR/PNG/JPEG）或 `env_cubemap`（6 面基名）；没有环境时用
  `env_color/env_sky_color/env_ground_color/env_highlight/env_rotation` 调 analytic studio。
- 光照：最多 8 盏同时生效；方向光优先。补光/装饰光设 `shadow_mode` 为 Off，别让每盏灯都投 1024² 阴影。
- 阴影：默认硬阴影 1024² + 3×3 PCF；`MAQUE_SHADOW=0` 可做 A/B。确认地面/接收面的 `receive_shadow` 没被关。
- 透明排序：`alpha_mode=Blend` 的物体按稳定顺序绘制；大量透明片会掉帧，能 Mask 就 Mask。
- transmission/volume 是近似；玻璃不要依赖真实折射/多次散射。体积厚度会乘节点世界缩放
  （`cbrt(|sx·sy·sz|)`）；小尺度资产的 `thicknessFactor` 不要按米猜。
- `render.MeshRenderer.mesh_path` 支持 `.mesh.json` / `.gltf` / `.glb`；路径相对场景目录。
- 蒙皮/动画用 `render.MeshRenderer` 的 `clip/playing/loop/speed/time/duration`；同一模型多实例
  各自播放不同时间。
- 相机：`fov_y/near/far/clear_mode/clear_color/culling_mask`；`clear_mode=0 Skybox` 才画环境天空，
  `clear_mode=1 Color` 用 `clear_color`（不画天空）。
- 3D 拾取：`scene_pick`/`scene_raycast` 返回最近 OBB 命中；不是三角形级，细长/镂空物体会偏粗。

### 9.4 RenderTexture

**小地图/监控/预览**：

```json
{
  "entities": [
    { "name": "minimap_camera", "components": [
      { "type": "scn.Transform2D", "x": 80.0, "y": 120.0 },
      { "type": "render.Camera2D", "layer_mask": 1,
        "clear_color": { "r": 0.02, "g": 0.03, "b": 0.05, "a": 1.0 } },
      { "type": "render.RenderTexture", "name": "minimap", "width": 160, "height": 120,
        "update_mode": 0, "filter": 0, "enabled": true }
    ]},
    { "name": "monitor", "components": [
      { "type": "scn.Transform2D", "x": 235.0, "y": 65.0 },
      { "type": "scn.Sprite", "width": 160.0, "height": 120.0,
        "texture": "rendertexture://minimap" }
    ]}
  ]
}
```

```js
const cam = entity_find('minimap_camera');
const uri = camera_texture(cam);        // rendertexture://0/minimap
camera_update(cam);                     // OnDemand 标脏
camera_capture(cam, 'out/minimap.png'); // 当前帧渲染后落盘（冷路径）
```

**最佳实践**：

- UI 头像/静态预览用 `update_mode=OnDemand`；镜子/监控用 `EveryFrame`。
- 目标尺寸按**实际显示尺寸**建，不要默认跟随大窗口；多张目标尽量同尺寸（附件缓存 8 组，
  尺寸种类多会互相驱逐）。
- `include_ui=false` 默认；不需要游戏 UI 进小地图就别开。
- 3D 目标才支持逐目标 MSAA（`0` 跟随场景、`1` 关、`4` 4x）；2D 目标恒单采样。
- 4x 附件 = 颜色 + 深度各 `w×h×4B×4`（128²≈0.5MB；1080p 两张附件约 66MB）；只给真正需要的目标开。
- 目标对自身采样会被后端剔除；跨目标依赖环按相机 id 固定顺序，较早目标读较晚目标上一帧。
- 不要每帧 `camera_capture`（GPU→CPU 回读是冷路径）；截图/抓帧用控制面或显式请求。

### 9.5 自定义 shader

| 组件 | 用途 | 契约 |
|------|------|------|
| `render.ShaderPass` | 全屏背景（Shadertoy/程序化天空） | 只写 `@fragment fs_main`；`group(0) binding(0)` 32B uniform；无纹理/无 depth/blend |
| `render.ShaderGraph` | 多通道/反馈（上一帧、中间缓冲） | program JSON 声明缓冲与有序阶段；最后阶段画到背景 |
| `render.ShaderSprite` | 节点矩形内的 shader | 与 ShaderPass 同契约，或 program JSON |

- ShaderPass 的 `frag_coord` 原点左上、y 向下；搬 Shadertoy 时翻 y。
- 3D 相机的 clear 会覆盖 ShaderPass 背景；有 Camera3D 时不要指望它可见。
- `.wgsl` **不热重载**；改完重启/重载场景。
- 自定义 shader 是场景资源（相对场景目录），随场景分发；失败会报错但场景其余部分继续。

### 9.6 后处理

- 默认关闭；F4 → 引擎选项 → 后处理，或 `MAQUE_ENGINE_OPTIONS="post=1,bloom=1,tonemap=aces,exposure=1.2"`。
- 默认只处理 3D；2D/UI 原样叠在结果上（保证 UI 清晰）。纯 2D 场景默认不跑。
- 是**视图层**效果：不写场景、不进快照/校验和、不影响模拟；RenderTexture 不叠后处理。
- 采样序列固定，同参数同帧逐字节确定；关掉时零成本。
- 真正的 HDR 中间目标 + 后置 tonemap 尚未实现；这里的曝光/ACES 是“成片上再叠一层”的调试语义。

### 9.7 性能开关与预算

| 开关 | 用途 |
|------|------|
| `MAQUE_TRACE_STATS=1` | 每 30 帧打印 `logic/render/present/draws/tris/target` |
| `MAQUE_TRACE_BATCH=1\|2` | 2D 批次数/逐批明细/图集统计 |
| `MAQUE_UI_BATCH=0` | 关闭 UI 分组合批（A/B） |
| `MAQUE_UI_ATLAS=0` | 关闭 2D 动态图集（A/B） |
| `MAQUE_UI_ATLAS_SIZE=N` | 改图集边长（默认 1024） |
| `MAQUE_RENDER_SCALE=N` | 降主输出分辨率 |
| `MAQUE_MSAA=0\|1\|4` | 主输出 MSAA |
| `MAQUE_SHADOW=0` | 关阴影做对照 |
| `MAQUE_TEX_INFO=1` / `MAQUE_MESH_INFO=1` | 装配期纹理/网格预加载统计 |
| `MAQUE_TEX_PRELOAD=0` / `MAQUE_MESH_PRELOAD=0` | 关闭并行预加载（测量/对照） |
| `MAQUE_TRACE_GPU_ALLOC=1` | 看 RenderTexture 附件创建/驱逐 |

**预算经验**：

- 2D/UI 的目标是“状态数”级别的 drawcall；如果 draws 随控件数线性涨，先查图集/合批。
- 单步总耗时 >16.7ms 就上不了 60fps；Debug+ASan 比 Release 慢约 2 倍，属预期。
- 3D 阴影/MSAA/RenderTexture 是乘法成本；先关阴影/降 MSAA/减目标尺寸定位。
- 纹理按实际显示尺寸做；不要给 64px 图标用 2K 贴图。
- 场景切换会重建资源缓存；`MAQUE_TEX_THREADS` 默认并行，除非测量不要关。

### 9.8 像素/截图确定性

- 同机同构建、同场景同帧的 `--shot` 应逐字节一致（PPM/PNG）。
- 跨 GPU/驱动/构建不保证逐像素；视觉回归用结构性指标（非背景占比、中心色、区域差异）。
- 动画/物理场景用 `--shot-frames` 指定帧，不要依赖墙钟。
- 截图不要开 `MAQUE_CAPTURE`（会强制 sdl 呈现）；直呈抓帧用 `MAQUE_CAPTURE_WGPU`。

---

## 10. 资源与 glTF

### 10.1 VFS 路径

| 形式 | 含义 |
|------|------|
| `textures/a.png` | 相对场景目录（运行时 VFS 根） |
| `/textures/a.png` | VFS 绝对（仍受挂载点约束） |
| `builtin:/textures/checker` | 引擎内建 |
| `pkg:/textures/a.png` | 包资源根（共享包推荐） |
| `asset://...` | Android APK assets |
| `https://...` | 远程目录/文件（可整游戏远程加载） |

挂载：

```bash
MAQUE_MOUNT="/cdn=https://cdn.example.com/assets/,/shared=../shared" \
  ./build/samples/player/maque_player scenes/game.scn
```

后挂载优先 → 可以“远程游戏 + 本地覆盖”。远程场景没有文件监视热重载。

### 10.2 glTF 运行时直读

`render.MeshRenderer.mesh_path` 可直接写 `.gltf` / `.glb`：

```json
{ "type": "render.MeshRenderer", "mesh_path": "assets/model.glb",
  "clip": "Idle", "playing": true, "loop": true, "speed": 1.0 }
```

- 相对**模型文件所在目录**解析外部 `.bin`/贴图；也支持 `data:` URI、GLB 内嵌 BIN。
- 多 primitive → 多 draw；每个 primitive 一份材质。
- 支持蒙皮/动画/morph、节点层级、KHR_lights_punctual、KHR_materials_variants、
  EXT_mesh_gpu_instancing、sparse accessor、静态 KHR_node_visibility、KTX2（BasisU）。
- 不支持：Draco / meshopt / WebP / AVIF；KTX2 块格式/cubemap；动画可见性；
  节点/贴图/可见性 pointer；非三角 primitive（点/线）。
- `MAQUE_ANIM=0` / `MAQUE_SKIN=0` / `MAQUE_MORPH=0` 可做 A/B；`MAQUE_RENDER3D_INFO=1`
  打印模型/skin/clip/morph/材质扩展/灯光相机日志。
- 场景已有 `render.Light` 时**不会**追加模型自带 KHR 灯光（显式覆盖）；否则模型灯光会叠加过曝。
- 外部 URI 按模型目录经 VFS 解析，远程挂载同样适用。

### 10.3 离线导入（`.mesh.json`）

```bash
python tools/gltf_import.py <model.gltf|model.glb> --out <dir> --name <name> --compact --scene
```

- 产物：`<name>.mesh.json`（48/56B 顶点布局 + indices + bounds + `asset.PbrMaterial` 材质数组 + 贴图引用）、
  `textures/*.png`、可选场景。
- **固定 `--compact`**：同一输入逐字节确定；默认缩进输出与 compact 有 6 位小数舍入差异，
  混用会让“资产漂移”测试假报。
- 有 skins/animations/morph 的模型输出层级场景 + `assets/skin/*.skel.json` + `assets/anim/*.clip.json`；
  其余模型平级烘焙。
- 非三角 primitive（点/线/strip/fan）离线导入会展开/跳过；运行时 GLB 直读跳过非三角。
- **不要手改生成物**：`scenes/khronos/<Model>/assets`、`textures`、`preview.png` 是派生资产；
  手调 `.scn` 要加入 `--exclude`，否则 `--force` 会覆盖。

### 10.4 贴图

- 普通贴图：PNG/JPEG；KTX2/BasisU 运行时支持；WebP/AVIF 无解码器。环境贴图另支持 EXR/HDR（RGBE）。
- sRGB 贴图：base color / emissive / specular color / sheen color；
  线性：MR / AO / normal / thickness / 数据贴图。放错槽会偏色/发灰。
- `#wrap=repeat|clamp|mirror&filter=nearest|linear` 后缀可逐贴图覆盖采样器。
- 小图进 UI 图集（RGBA8/单 mip/clamp/边 ≤256）；大图/背景不进。
- 需要像素硬边（像素画/UI 图标）用 `#filter=nearest`；窗口整倍缩放时 present 走最近邻，保持锐利（非整倍仍可能插值）。
- 装配期并行 decode+mip（`MAQUE_TEX_THREADS`）；GPU 上传仍主线程；mip 与旧实现逐字节一致。
- 不要把 normal/MR/AO 当颜色贴图传给 Sprite/PBR color 槽。

### 10.5 场景切换与预加载

- `director_load_scene` 会清空场景资源缓存（纹理/材质/网格/环境）并重建；这是为了避免
  “同一相对路径复用上一个场景的贴图”。
- 场景装配期会预热 Sprite/材质/网格/环境；运行期提取只查表，不加载。
- 动态生成的实体如果赋 `material`/`texture` 字符串，`node_set_string` 会当场解析并缓存；
  这是运行期唯一能补齐的时机。
- 嵌套场景组件的资源路径会按实例 base 改写；材质文件内部贴图仍按根场景目录解析（已知边缘）。
- 切场景/热重载/快照恢复后，常驻脚本（global/overlay）会收到 `on_start`，需要重建自己的 UI 实体。

### 10.6 Android / 远程

- Android：`scenes/` 全部内容打进 APK assets；场景路径 `asset://...`；脚本/子场景/音频按场景目录解析。
  新增场景直接重建 APK，不需要手写清单。
- 远程：`maque_player https://host/game.scn`；场景/脚本/资源全走 VFS；`storage` 默认落到 XDG 兜底。
- 远程内容没有本地文件监视；不要依赖热重载。
- 发布前检查 APK 体积：`scenes/khronos/*/assets` 等未入库派生资源如果存在，也会被“全部内容”规则打进去。

---

## 11. 音频

**默认是 null 后端**：逻辑/脚本/`playhead` 可用，但不出声。这是 CI/headless 的默认。

```bash
# 打开 SDL 音频设备
MAQUE_AUDIO=sdl ./build/samples/player/maque_player samples/tank/tank.scn

# 静音（逻辑照常）
MAQUE_AUDIO=off ./build/samples/player/maque_player scenes/ball.scn
```

```js
const voice = audio_play('audio/coin.wav', 0.8, 0, 'sfx'); // path, volume, loop, bus
audio_set_listener(x, y, 1, 0);                            // 位置 + 方向
audio_master_volume(0.8);
audio_bus_volume('sfx', 0.6);
if (voice) audio_stop(voice);
```

**最佳实践**：

- 短音效走 `audio_play`；循环/环境声用 `audio.AudioSource` 组件（`autoplay/loop/volume/pitch`）。
- 路径相对场景目录；WAV/OGG/MP3 都支持；解码在加载期，回调线程只读样本。
- `playhead` 按模拟 dt 推进（不是墙钟）；不要用它做玩法判定。
- 音频不写回模拟、不进 checksum；不要因为“音频没声”就以为逻辑没跑（先看 `audio_enabled()`）。
- CI 用 `MAQUE_AUDIO_CAPTURE=out.wav` 强制 null 后端并写 PCM，断言 RMS 非静音。
- 移动端/浏览器需要用户手势解锁音频（P2 浏览器音频尚未完成）；Android 默认 null，启动器显式设 `MAQUE_AUDIO=sdl`。

---

## 12. 存储与存档

`storage.*` 是“浏览器 localStorage 类比”，但模式可配：

| spec | 后端 | 默认目标 |
|------|------|----------|
| `file://<目录>` | 本地文件 | `<目录>/<uid>/<session>/<key>` |
| `file://`（无路径） | 本地文件 | `$MAQUE_STORAGE_ROOT` → XDG → `~/.local/share/maque/storage` |
| `mem://` | 进程内，不落盘 | 测试/临时档（模块 spec 为空时默认；runtime 会显式给 `file://<场景目录>`） |
| `http(s)://` | REST | 4 条固定路由 |
| `ws(s)://` | WebSocket JSON 帧 | `maque.storage.v1` |

运行期默认：`<场景目录>/.maque`（场景在 CWD 时 `<CWD>/.maque`）。
命名空间：`MAQUE_UID`（默认 `default`）/ `MAQUE_SESSION`（默认 `main`）。

```js
const KEY = 'save/v1';

function loadGame() {
  if (!storage.exists(KEY)) return null;
  const raw = storage.get(KEY);
  if (!raw) return null;
  try {
    const s = JSON.parse(raw);
    if (s.version !== 1) return null;        // 版本不匹配：迁移或丢弃
    return s;
  } catch (e) {
    log('save corrupt: ' + e);
    return null;
  }
}

function saveGame(s) {
  s.version = 1;
  storage.set(KEY, JSON.stringify(s));
}
```

**最佳实践**：

- 存档带 `version`；读取时校验/迁移；损坏时回退默认，不要崩。
- 键加前缀（`save/`、`settings/`），`storage.keys()` 返回 JSON 数组。
- 值适合配置/进度（KB 级）；**不要存大资源/二进制**（> 数十 MB 走 VFS + 内容哈希）。
- `uid` 只是命名空间，不是鉴权；多租户/安全边界要自己做。
- `file://` 是原子写（tmp + rename）；并发写按单写者假设，多进程同档需上层加锁。
- 控制面 `Storage.get/list/info` 只读，`put/remove` 变更类；AI/CI 可核对存档。

---

## 13. 调试与观测

### 13.1 日志与常用环境变量

| 变量 | 用途 |
|------|------|
| `MAQUE_TRACE_STATS=1` | 每 30 帧打印逻辑/渲染/呈现/draws/tris/target |
| `MAQUE_TRACE_INPUT=1` | 打印 SDL 事件与平台队列 |
| `MAQUE_HITS=1` | 打印命中测试 |
| `MAQUE_TRACE_UI=1` | UI 布局/命中诊断 |
| `MAQUE_TRACE_BATCH=1\|2` | 2D 合批/图集统计 |
| `MAQUE_RENDER3D_INFO=1` | 3D 模型/skin/clip/morph/材质扩展/灯光相机日志 |
| `MAQUE_PICK_INFO=1` | 屏幕→NDC→世界射线中间量 |
| `MAQUE_TEX_INFO=1` / `MAQUE_MESH_INFO=1` | 纹理/网格预加载统计 |
| `MAQUE_TRACE_PACE=1` | 墙钟/仿真/速率/睡觉/重锚定 |
| `MAQUE_TRACE_PHYSICS=1` | 物理诊断 |
| `MAQUE_TRACE_AUDIO=1` | 音频诊断 |
| `MAQUE_TRACE_ORBIT=1` | 3D 相机 orbit 状态 |
| `MAQUE_CRASH_REPORT=<file>` | 崩溃上下文落盘（值是路径，不是布尔） |
| `MAQUE_CHECKSUM=1` | 逐帧世界校验和 |
| `MAQUE_SNAPSHOT` / `MAQUE_REPLAY` / `MAQUE_RECORD` / `MAQUE_TRAIL_OUT` | 快照/回放/录制/轨迹 |

日志一律 `std::print`（C++）/`log()`（脚本）；不要用裸 `printf` 作为唯一出口。

### 13.2 F2 / F4 / Inspector

- **F2**：引擎 stats 面板（FPS、logic/render、draws、tris、frame、target）。默认不显示；
  `MAQUE_IMGUI=1` 启动即开。
- **F4**：引擎选项（rendered/textured/solid/wireframe、3D/2D 图层、MSAA、RenderTexture MSAA、
  默认太阳光、主相机 near/far、后处理）。只在成帧时生效，不写场景/快照。
- **Inspector（独立进程）**：

```bash
MAQUE_INSPECT=7777 ./build/samples/player/maque_player scenes/sokoban/sokoban.scn
./build/samples/inspector/maque_inspector --connect 7777
```

  - 默认**只读**；变更类方法返回 `-32001`，需要 `MAQUE_INSPECT_RW=1`。
  - 面板：场景树 / 属性（反射驱动）/ 控制台 / 字体图集 / 预览 / 模式。
  - 无显示器 CI：`maque_inspector --connect 7777 --headless Scene.info Runtime.frame`。
  - 游戏窗口与 inspector 同时用时，不要给游戏加 `MAQUE_ALWAYS_ON_TOP`，否则会盖住 inspector。

- **控制面 JSON-RPC**（与 inspector/MCP 同源）：

```bash
MAQUE_INSPECT=7777 ./build/samples/player/maque_player scenes/ball.scn
# 另一个终端
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"World.checksum"}' | nc 127.0.0.1 7777
```

  常用方法：`Runtime.step`、`Runtime.frame`、`Scene.info/entities/get/set`、`Scene.instances`、
  `World.checksum/trail/snapshot/restore`、`Replay.log`、`Input.sim_*`、`Capture.screen`、
  `Capture.camera*`、`Perf.get`、`Reload.script/scene`。变更类遵守只读会话规则。

### 13.3 截图与序列

```bash
# 单帧
MAQUE_WINDOW=0x0 MAQUE_PRESENT=off \
  ./build/samples/player/maque_player --shot out/a.png --shot-frame 2 scenes/pbr/cube.scn

# 动画/物理多帧
./build/samples/player/maque_player --shot-seq out/f{}.png --shot-frames 1,30,60 scenes/anim/anim.scn
./build/samples/player/maque_player --shot-every 20 --shot-count 4 scenes/anim/anim.scn

# 直呈交换链抓帧（需要 surface 支持 CopySrc）
MAQUE_CAPTURE_WGPU=out/wgpu MAQUE_CAPTURE_FRAMES=3 \
  ./build/samples/player/maque_player scenes/pbr/cube.scn
```

- `--shot` 默认第 1 帧；`--shot-frame` 渲染到该帧；`--shot-seq` 拍完最大帧退出。
- 无窗口截图 = 虚拟屏幕（设计分辨率 × 视图数）× render_scale。
- 视觉回归优先 PPM；PNG 由 image 模块写（zlib 可用则真压缩）。
- `MAQUE_CAPTURE` 从 SDL renderer 后台缓冲抓帧（含 ImGui），会强制 sdl 呈现。

### 13.4 性能分析

- **Tracy**（时间线/zone）：F3 或 `MAQUE_TRACY=1`；GUI 二进制不入库，先跑 `tools/tracy/...` 下载/构建。
  埋点只用 `MQ_TRACY_*`，加在外层入口，不进热内循环。
- **Linux perf**（函数/源码行热点）：`MAQUE_PROFILE=1 [MAQUE_PROFILE_FRAMES=N]`；F5 开关；
  产物在 `.maque/profile/run-<时间戳>`；`python3 tools/perf/maque_perf.py record/report/compare/bench`。
  权限：`perf_event_paranoid<=1` 或 `CAP_PERFMON`。
- 两者分工：Tracy 看“一帧内各段”；perf 看“几百帧 CPU 花在哪些函数/源码行”。不要互相替代。

### 13.5 排错表

| 现象 | 先查什么 | 修复 |
|------|----------|------|
| 3D 全黑 / 只有背景 | `MAQUE_RENDER3D_INFO=1` 的 `cams/meshes/lights`；相机 near/far；`clear_mode`；mesh_path | 修相机/路径；`clear_mode=1` 看纯色；临时 `near=0.001,far=10000` |
| 模型显示成灰 cube | 日志 `mesh=-1`；路径是否相对场景目录；JSON 是否坏 | 修路径/文件；`MAQUE_RENDER3D_INFO=1` |
| 贴图缺失/全白 | 贴图槽是否放错（sRGB/线性）；路径；格式是否受支持 | 按槽放图；KTX2 用 runtime 直读；WebP/AVIF 先转 PNG/JPEG |
| 透明物体全黑/背面黑 | 是否写了 `side`；`double_sided` 是否被忽略 | 显式 `"side": 2` 或 0 |
| 阴影没有/像没开 | `MAQUE_SHADOW=0`？地面是否 `receive_shadow`；灯光 `shadow_mode` | 开阴影；加地面；补光设 Off |
| 点击 UI 没反应 | `MAQUE_HITS=1`、`MAQUE_TRACE_INPUT=1`；`input_target`；`mouse_filter` | 把遮挡层设 `Ignore`；确认 target id |
| 3D 点击不准 | `MAQUE_PICK_INFO=1`；是否用 OBB；相机 view/proj 是否成帧 | 用 `scene_raycast`；细长物体自建检测 |
| 没有声音 | `audio_enabled()`；`MAQUE_AUDIO` | `MAQUE_AUDIO=sdl`；Android 启动器显式开 |
| 窗口里游戏不动/太快 | `MAQUE_TRACE_PACE=1`；单步耗时 | 降 render_scale/MSAA/阴影；看 `[stats]` |
| 热重载不生效 | 是否有 `maque.package.json`（P1 不支持）；`MAQUE_WATCH=1` | 重启；旧路径才支持按文件热更 |
| 热更后事件丢失 | `on_start` 是否重新 `node_on` | 在 `on_start` 注册，不要放顶层 |
| RenderTexture 空白 | 相机与 RT 是否同节点；`ready`；URI；`update_mode` | 同节点；`camera_update`；检查 name 重复 |
| 截图尺寸不对 | 窗口 vs 无窗口基准；`render_scale` | 用 `MAQUE_WINDOW=0x0` + 设计分辨率；显式 `render_scale` |
| 切场景后贴图串了 | 是否同相对路径；缓存是否清 | 用不同路径/包前缀；引擎已清缓存，仍建议唯一路径 |
| 中文文本乱码/缺字 | `font` 路径；字体图集；`MAQUE_TRACE_TEXT=1` | 指定 TTF；等一帧后再量文本；检查字体覆盖 |

---

## 14. 测试与 CI

### 14.1 本地测试

```bash
# 全量
ctest --test-dir build --output-on-failure

# 按主题
ctest --test-dir build -R 'ui_batch|ui_atlas|text_pixel|spine' --output-on-failure
ctest --test-dir build -R 'render_texture_e2e|render3d_scene_e2e|pbr' --output-on-failure
ctest --test-dir build -R 'determinism|replay|snapshot' --output-on-failure
ctest --test-dir build -R 'scene_instance_e2e|td_e2e|mixed_lang_e2e' --output-on-failure
```

### 14.2 给新场景加测试

1. **加载冒烟**：`maque_player --frames N`，断言退出码 0 + 关键日志。
2. **确定性**：同 seed 跑两次，比较 `world_trail`/`World.checksum`；或用 `MAQUE_TRAIL_OUT`。
3. **视觉**：`--shot` 出 PPM，断言非背景占比/中心色/区域差异；不要跨 GPU 逐字节。
4. **交互**：`.sim.json` 注入点击/按键，断言脚本日志/状态。
5. **性能**：固定帧数 + 多次中位数；必要时 `MAQUE_TRACE_STATS` 解析。

```bash
# 例：冒烟 + 确定性 + 截图
MAQUE_WINDOW=0x0 MAQUE_PRESENT=off \
  ./build/samples/player/maque_player --frames 120 --shot out/a.ppm --shot-frame 60 scenes/ball.scn
```

### 14.3 CI 注意事项

- 用绝对路径/仓库相对路径；不要依赖 CWD 之外的临时目录。
- Windows 用 Git Bash（CMake 自动探测）；不要用 `C:\WINDOWS\system32\bash.exe`（WSL 启动器）。
- Python 用 `maque_python.sh` 探测（避免 Windows Store 占位 `python3`）。
- e2e 临时目录用 `tests/maque_test_fs.h` 的 helper，不要裸 `fs::temp_directory_path()`。
- 视觉基线同机同构建生成；跨平台只做结构断言。
- 性能门用 Release + 固定采样数；Debug/ASan 只做功能。
- wasm/Android 有独立 CI job，不要用本机 headless stub 冒充真实构建。

---

## 15. 平台与发布

### 15.1 桌面

- 默认窗口 800×650，自动适配逻辑屏；`MAQUE_WINDOW=WxH` 覆盖；`0x0` 无窗口。
- 呈现：`auto` 优先直呈；`sdl` 走 SDL_Renderer（截图/ImGui 需要）；`off` 不呈现（离屏/截图）。
- 墙钟限速：真窗口默认 1x；无窗口/截图/录制不限速；`MAQUE_PACE=0/1` 覆盖。
- 多实例：不同 `Engine` 实例隔离；控制面可按 instance 驱动。

### 15.2 Android

```bash
sh android/setup-deps.sh
cd android && ./gradlew :app:assembleDebug
```

- `scenes/` 全部内容作为 APK assets 根；`asset://` 只读后端读取；脚本/纹理/音频/子场景按场景目录解析。
- 真机默认 wgpu 直呈；AVD 因 WSI acquire timeout 回退 CPU（约 15Hz，仅功能调试）。
- 默认不弹软键盘；文本输入场景显式 `MAQUE_TEXT_INPUT=1`。
- 触摸手势：单指环绕、双指平移、捏合缩放；2D 场景仍走普通 pointer 回调。
- 发布前关注 APK 体积（未入库派生资产也会被打进去）。

### 15.3 WASM / 浏览器

```bash
# 先 source emsdk_env.sh（官方 toolchain；不要用宿主 clang 冒充）
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAQUE_WASM=ON
cmake --build build-wasm --target maque_web --parallel
node tests/wasm_p0_smoke.mjs "$PWD/build-wasm/samples/web/maque_web.mjs" /scenes/anim/anim.scn 60
```

- 当前 **P0 headless 确定性门**：null renderer + MEMFS 预载 + `mq_host_step` + checksum；
  页面只显示 frame/checksum，不是可玩渲染。
- 正式浏览器渲染只走 WebGPU（`emdawnwebgpu`），不做 WebGL/SDL_Renderer/CPU 光栅回退；
  P1 才交付可见 2D。
- 浏览器必须经 HTTP(S) 打开，不能 `file://`。
- P0 关闭：native TCP/UDS inspector、libcurl URL VFS、libwebsockets、文件 watcher、真实音频、
  Tracy、Lua、动态模块卸载、GPU capture。

### 15.4 远程内容

```bash
./build/samples/player/maque_player https://microgames.example/game.scn
```

- 场景/脚本/资源全走 VFS；无文件监视热重载。
- `MAQUE_MOUNT` 可加本地覆盖（后挂载优先）。
- `storage` 远程场景默认落到 XDG 兜底（仍持久）；要服务端存档显式 `http(s)://` / `ws(s)://`。
- 远程脚本不是安全边界；不要执行不受信原生代码。

### 15.5 Windows 差异

- UDS（`uds:`）不支持；控制端点/Inspector 只用 `tcp:端口`。
- libwebsockets 的 Windows 构建 TLS 关闭 → 只有 `ws://`，没有 `wss://`。
- 模块卸载默认不 `FreeLibrary`（wgpu 类库退出路径问题）；`MAQUE_MODULE_FORCE_UNLOAD=1` 才强制。
- 构建用 clang-cl + Ninja；不要加载 `vcvars64.bat`（会让 CMake 挂起）。

---

## 16. 反模式清单

| 反模式 | 为什么错 | 正确做法 |
|--------|----------|----------|
| 每个子弹/敌人一个 `SceneInstance` + 脚本 | VM/context 数线性增长，内存/派发变慢 | prefab 池 + 根脚本批量驱动（`samples/tank`） |
| 所有逻辑塞进根脚本，子场景当纯数据 | 组合性/复用性差，改一处影响全局 | 行为单元用 `.scn` + 脚本；父脚本只编排 |
| 顶层变量存进度/存档 | 热重载/快照恢复丢失 | 组件字段 / `global_*` / `storage` |
| `Math.random()` / `Date.now()` | 回放漂移 | `rng()` / `director_frame()` |
| 用标准 JSON 库改写 `.scn` | 丢掉 `render.Light` 的重复 `type` 键 | 文本编辑或 `_JsonPairs` 工具；改完检查灯 |
| 每帧 `entity_find`/`node_find_path` | O(n) 查找，帧率抖 | `on_start` 缓存 id |
| 每帧 `log` / 每帧截图 / 每帧 readback | IO/GPU→CPU 是冷路径，严重掉帧 | 计数器 + 每 N 帧；截图用控制面/显式请求 |
| 在渲染提取阶段才发现资源没加载 / 每帧加载新贴图 | 提取只读，不会加载；每帧 IO 会卡顿 | 装配期引用；动态实体用 `node_set_string` 当场解析并缓存 |
| 整屏 4K 背景想靠 UI 图集合批 | 图集准入 ≤256/边，大图不会被收进去（也不会挤掉小图），只是不合批 | 大图单独画；小图标/九宫格才进图集 |
| 所有灯都投 1024² 阴影 + 全开 4x MSAA | 成本乘法增长 | 只给主光开阴影；只给需要的 RT 开 4x |
| 混用世界坐标和 UI 布局坐标 | 命中/排列错位 | 逻辑用世界/设计坐标；UI 用 Control；截图校准 |
| 依赖 3D 三角形级拾取 | 只有 OBB | 粗拾取用 `scene_pick`；精确自建检测 |
| 高速薄物体靠物理碰撞 | 无 CCD，会隧穿 | 脚本 AABB 或钳速度 |
| 依赖 `script.Script`/`props` | P1 未接线 | 场景级 `script` + 组件/global/storage |
| 包场景开 `MAQUE_WATCH` 等热更 | P1 未实现 | 重启；旧路径才支持 |
| 用 `SceneInstance` 做整局切场景 | 根脚本/全局状态语义不同 | 整局切换用 `director_load_scene`；关内切块用实例 |
| 在 `on_start` 里量文本宽度 | 排版尚未完成，值为 0 | 等一帧/在 `update` 里量 |
| 直接改 `world_x/world_y` | 下一帧被覆盖 | `entity_set_pos`/`node_set_position` |
| 信任远程脚本/原生代码 | 单 VM 共享堆，native 非沙箱 | 只跑受信内容；远程用 JS + 能力边界 |

---

## 17. 发布前检查清单

- [ ] **每个场景**在 `MAQUE_WINDOW=0x0 MAQUE_PRESENT=off` 下能加载并跑 N 帧（无崩溃/无脚本错误）。
- [ ] `ctest --test-dir build --output-on-failure` 全绿；新增场景有对应测试。
- [ ] 同 seed + 同输入跑两次，`world_trail`/checksum 一致；没有墙钟/系统随机。
- [ ] 所有 `.scn` 灯光组件仍保留重复 `type` 键（抽查 + `MAQUE_RENDER3D_INFO=1`）。
- [ ] 资源路径相对场景目录；没有绝对路径/本机路径；大小写与平台一致。
- [ ] 派生资产没有入库；生成物可一条命令重建；手调资产有 `--exclude` 保护。
- [ ] APK/发布包里没有 `.deps`、`build/`、`preview.png`、`.maque/` 等无关内容。
- [ ] 音频：默认 null 不出声是有意为之？发布版显式 `MAQUE_AUDIO=sdl` 或启动器设置。
- [ ] 存档：有版本号/迁移/损坏回退；测试过跨会话读取。
- [ ] 热重载/包：发布版不依赖热重载；包场景不会误开 `MAQUE_WATCH`。
- [ ] UI：多分辨率/窗口缩放/安全区（移动端）下不遮挡、不越界；文本度量在排版后。
- [ ] 性能：目标平台 Release 下稳定帧率；draws/纹理/阴影/RT 在预算内。
- [ ] 3D：材质 `side`、透明排序、阴影接收/投射、相机 near/far 都验证过。
- [ ] 平台：Linux/Windows/Android（按目标）都跑过冒烟；没有任何“只在我机器上”的路径。
- [ ] 关闭调试开关（`MAQUE_IMGUI`/`MAQUE_INSPECT`/`MAQUE_TRACE_*`/`MAQUE_KEEP_OPEN` 等）。

---

## 18. 常用配方

### 18.1 最小 2D + 脚本

见 5.1。运行：

```bash
MAQUE_WINDOW=800x600 ./build/samples/player/maque_player scenes/hello.scn   # 先把 5.1 存成 scenes/hello.scn
```

### 18.2 父场景传参给子场景

```json
{ "name": "EnemyA", "components": [
  { "type": "scn.Transform2D", "x": 100.0, "y": 120.0 },
  { "type": "scn.SceneInstance", "source": "enemy.scn",
    "params": "{\"hp\":30,\"speed\":40}" }
]}
```

```js
// enemy.js（子场景）
const S = { hp: 10, speed: 30, uid: -1 };

function on_start() {
  let p = {};
  try { p = JSON.parse(scene_self_params() || '{}'); } catch (e) { log('bad params: ' + e); }
  S.hp = p.hp || 10;
  S.speed = p.speed || 30;
  S.uid = scene_self();
}
function on_hit(id, dmg) {
  S.hp -= dmg;
  if (S.hp <= 0) scene_emit(scene_instance_parent(scene_self()), 'enemy_dead', S.uid);
}
```

### 18.3 动态生成/卸载关卡块

```js
let stage = -1;

function loadStage(n) {
  if (stage >= 0) scene_unload(stage);
  stage = scene_instantiate(-1, 'stages/stage' + n + '.scn', '{"tier":2}');
  if (stage < 0) log('stage load failed: ' + scene_last_error());
}

function update(dt) {
  // 句柄立即返回，但实体/VM 在下一个帧边界才就绪
  if (stage >= 0 && !scene_instance_valid(stage)) return;
  // ...
}
```

### 18.4 prefab 池（大量同类对象）

```js
const POOL = [];
function on_start() {
  for (let i = 0; i < 32; ++i) {
    const id = entity_find('pool_slot_' + i);
    node_set_active(id, 0);
    POOL.push(id);
  }
}
function spawnAt(x, y) {
  const id = POOL.find((e) => node_active(e) === 0);
  if (id === undefined) return -1;
  node_set_position(id, x, y);
  node_set_active(id, 1);
  return id;
}
```

- 池对象在装配期创建，运行期只切 `active`/位置/纹理；不要每帧 spawn/destroy。
- 池大小按同屏峰值 + 余量；溢出时丢弃/复用最早对象。

### 18.5 3D RenderTexture 小地图

见 9.4。脚本：

```js
function on_start() {
  const cam = entity_find('minimap_camera');
  log('minimap=' + camera_texture(cam));
  const path = env('MAQUE_RT_SHOT');
  if (path) camera_capture(cam, path);
}
```

### 18.6 音频 + 存储

```js
function on_coin() {
  audio_play('audio/coin.wav', 0.7, 0, 'sfx');
  const n = (parseInt(storage.get('save/coins') || '0', 10) || 0) + 1;
  storage.set('save/coins', String(n));
}
```

### 18.7 输入模拟 + 截图回归

```bash
MAQUE_WINDOW=0x0 MAQUE_PRESENT=off \
MAQUE_SIM=tests/data/sim/ball_clicks.sim.json \
  ./build/samples/player/maque_player --frames 60 --shot out/smoke.ppm --shot-frame 40 scenes/ball.scn
```

### 18.8 Inspector / 控制面

```bash
MAQUE_INSPECT=7777 ./build/samples/player/maque_player scenes/sokoban/sokoban.scn
./build/samples/inspector/maque_inspector --connect 7777
# headless
./build/samples/inspector/maque_inspector --connect 7777 --headless Scene.info World.checksum
```

### 18.9 glTF 导入

```bash
python tools/gltf_import.py assets/model.glb --out scenes/mymodel --name mymodel --compact --scene
python tools/gltf_import.py --help
```

### 18.10 调试环境变量速查

```bash
# 统计 / 输入 / 命中
MAQUE_TRACE_STATS=1 MAQUE_TRACE_INPUT=1 MAQUE_HITS=1 ./build/samples/player/maque_player scenes/ball.scn

# 3D / 拾取 / 加载
MAQUE_RENDER3D_INFO=1 MAQUE_PICK_INFO=1 MAQUE_TEX_INFO=1 MAQUE_MESH_INFO=1 \
  ./build/samples/player/maque_player scenes/khronos/ToyCar/ToyCar.scn

# 确定性 / 崩溃
MAQUE_CHECKSUM=1 MAQUE_CRASH_REPORT=crash.json MAQUE_TRAIL_OUT=trail.txt \
  ./build/samples/player/maque_player --frames 300 scenes/ball.scn
```

---

## 19. 文档地图

| 你想知道 | 看 |
|----------|----|
| 设计哲学、ADR、确定性/快照原则 | [`docs/00`](docs/00-设计哲学与约束.md)、[`docs/10`](docs/10-确定性快照与回放.md) |
| 模块/ABI/多实例 | [`docs/02`](docs/02-模块系统-ABI与多实例.md) |
| VFS / 路径 / 远程 | [`docs/04`](docs/04-虚拟文件系统与多协议.md) |
| 场景/节点/实例 | [`docs/12`](docs/12-场景与世界.md)、[`docs/32`](docs/32-场景实例与事件作用域.md) |
| 脚本/多语言/热重载 | [`docs/14`](docs/14-脚本.md)、[`docs/34`](docs/34-脚本模块与实例上下文.md) |
| 输入/UI/命中 | [`docs/19`](docs/19-输入与事件分发系统.md)、[`docs/components/04`](docs/components/04-ui.md) |
| 渲染/PBR/RenderTexture | [`docs/13`](docs/13-渲染（WebGPU）.md)、[`docs/33`](docs/33-RenderTexture相机离屏纹理.md) |
| 3D 组件字段 | [`docs/components/03`](docs/components/03-render.md)、[`docs/components/14`](docs/components/14-render-extra-3d.md) |
| 动画/物理/粒子/导航 | [`docs/15`](docs/15-动画物理粒子.md)、[`docs/components/06`](docs/components/06-animation-particle.md)、[`docs/components/07`](docs/components/07-script-audio-nav.md) |
| 音频 | [`docs/30`](docs/30-音频集成方案.md) |
| 存储/存档 | [`docs/25`](docs/25-本地存储与远程存档.md) |
| ImGui / Tracy / perf | [`docs/28`](docs/28-ImGui调试面板.md)、[`docs/29`](docs/29-Tracy性能分析.md)、[`docs/35`](docs/35-Linux采样剖析（perf）.md) |
| glTF 迁移/坑 | [`docs/31`](docs/31-gltf-model-migration.md)、[`plans/24`](plans/24-runtime-gltf-capabilities.md) |
| 自动生成 API（脚本/控制面/事件/ABI） | [`modules/core/data/generated/API.md`](modules/core/data/generated/API.md)、[`TYPES.md`](modules/core/data/generated/TYPES.md) |
| 场景组织标杆 | [`samples/tank/README.md`](samples/tank/README.md) |
| Android | [`android/README.md`](android/README.md) |
| WASM | [`docs/plan-wasm.md`](docs/plan-wasm.md) |
| 引擎贡献/构建/约定 | [`AGENTS.md`](AGENTS.md) |

---

> **最后一条建议**：把这份指南里的“稳定面”当成契约，把“不要依赖”当成待办清单。
> 每次要用一个没把握的能力，先找对应 `scenes/` 样例和 `ctest -R` 测试；没有样例/测试的能力，
> 先按预览功能对待，别把它放进发布路径。
