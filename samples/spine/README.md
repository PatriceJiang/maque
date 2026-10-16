# Spine 2D（samples/spine）

用**官方 spine-runtimes 素材**驱动引擎的骨骼动画渲染：一个带 `anim.SpineSkeleton` 组件的实体 →
引擎在装配期读骨架/图集/页纹理，每帧步进动画并把**网格图元**（逐顶点 pos/uv/color + 索引）提交给渲染器。
**↑/↓ 切换动画**，空格暂停。玩法与素材全在资源里，宿主只有 50 行。

```
samples/spine/
├── main.cpp          宿主：只解析参数 + mq_run_scene()，不含任何 spine/游戏概念
├── spine.scn         spineboy（单页图集 1024x256，11 个动画）—— 上下键切换的主场景
├── dragon.scn        dragon-ess（**5 页图集** 1024x1024，1 个动画）—— 多页批处理场景
├── spine.js          交互：列动画清单、↑/↓ 切换、空格暂停、HUD
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

## 测试

```bash
ctest -R spine          # spine2d（模块单测）+ spine_smoke / spine_e2e / spine_dragon
```

- `spine2d`（模块级）：动画/皮肤清单、图集页与 `pma`、批次完整覆盖索引、时间语义、坏输入不崩。
- `spine_smoke`：加载骨架/图集/页纹理 + 跑 40 帧不崩。
- `spine_e2e`：`MAQUE_SIM` 注入 ↑/↓/空格（**SDL 风格码**，引擎在模拟器边界归一化）→
  断言日志里的切换序列 == 注入顺序、脚本读回的"引擎当前动画" == 请求；
  再对比**同一帧不同动画**的截图（必须不同），并做**像素级**断言：角色区域内非背景像素 > 2000。
- `spine_dragon`：5 页图集逐页拿到纹理句柄 + 引擎页表 `pma=1` × 5。

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
