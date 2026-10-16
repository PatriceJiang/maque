# 27 · Spine 2D 运行时接入（已完成 + 官方 demo 全量移植）

> 目标：集成官方 **spine-runtime**（`spine-cpp`），交付**独立**的 `samples/spine/` 工程，
> 并把**官方全部 demo** 移植进来。
> 状态：**已完成** —— 模块参与默认构建、runtime 已接入渲染管线、
> **13 个官方 demo 场景** + 总览全部可跑、测试全绿。
> 单一真相：`modules/core/data/schema/world/anim.yaml` 的 `module_abi`（设计来源 `docs/assets/06 §2`）。

## 官方 demo 全量移植（`samples/spine/`）

用户要求"我要的是官方 runtime 的 demo"。核查结论：官方 demo 集在
**`spine-runtimes` 4.2 的 `spine-cocos2dx/example/Classes/*Example.cpp`**（各平台移植共用的官方参考实现，
11 个），另有官方 C++ runtime 的两个入口 demo（`spine-glfw/example/main.cpp`、`spine-sdl/example/main.cpp`）。
（`EsotericSoftware/spine-cpp-demos` 仓库**不存在**；`spine-cpp/` 目录只含库本体，不含 demo。）

| 场景 | 素材（官方缩放） | 复刻要点 |
|---|---|---|
| `demo-spineboy.scn` | spineboy-pro.json + spineboy.atlas（0.6） | 5 类动画事件 + 用户事件 + 排队 walk→jump→run + `setMix` |
| `demo-separator.scn` | 同上 | 槽区间分层 back=[19..24]/front=[25..-1] |
| `demo-goblins.scn` | goblins-pro.json + goblins.atlas（1.5） | `setSkin("goblin")` |
| `demo-raptor.scn` | raptor-pro.json + raptor.atlas（0.5） | track0 walk + track1 延迟 `gun-grab` |
| `demo-tank.scn` | tank-pro.skel + tank.atlas（0.5） | `shoot` |
| `demo-coin.scn` | coin-pro.skel + coin.atlas（1.0） | `animation` |
| `demo-mixandmatch.scn` | mix-and-match-pro.skel + atlas（0.5） | **运行时合成 9 件皮肤 → 46 附件** + `dance` |
| `demo-batching.scn` | spineboy-pro.json + atlas（0.6） | **同屏 50 个骨架** |
| `demo-sequence.scn` | dragon-ess.skel + **dragon-pma.atlas**（1.0，5 页） | **Sequence 序列帧**（wings 各 9 帧） |
| `demo-ik.scn` | spineboy-pro.json + atlas（0.6） | track0 walk + **track1 aim** + crosshair 骨骼随指针 |
| `demo-physics.scn` | celestial-circus-pro.skel + atlas（0.2） | **物理约束（30 个）** |
| `demo-glfw.scn` | spineboy-pro.skel + **spineboy-pma.atlas**（0.3） | 官方 C++ 入口：`defaultMix 0.2`、portal→run |
| `demo-sdl.scn` | spineboy-pro.json + **spineboy-pma.atlas**（0.5） | 同上（SDl 版底色 (94,93,96)） |
| `all.scn` | 全部 | **总览**：12 个骨架同屏（一张图证明全都集成） |

测试：`spine_demos`（13 个场景各跑 30 帧）、`spine_all`（出图 + 骨架数==格数 + **逐格**像素断言）。
**逐格断言是刻意的**——它实测抓到过两次"日志一切正常但画面错"：
Goblins 漏设皮肤（只剩长矛+盾）与 MixAndMatch 0 顶点（整格空白）。

### 诚实清单（复刻 vs 简化）

- **Separator 实现有差异**：官方是"一个骨架 + 两个渲染节点"，我们是**两个实体各持一个实例**镜像播放
  （引擎没有"分次绘制同一实例"的 API）→ 观感一致，代价是骨架数据/内存各一份。
- **调试骨骼绘制**（官方点击第一击）引擎无此能力 → 明确打印 `UNSUPPORTED debugBones`，不假装。
- 位置微调两处：Tank 官方 `x = width/2 + 400` 超出 800px 画布 → 挪到 560；
  Batching 官方用设备尺寸，50 个 0.6 缩放于 800x600 糊成一团 → 总览用 1280x960（位置仍按官方百分比公式）。
- **未用真实指针验证**：headless 无真实指针事件，`on_pointer_move` 驱动的 IK/物理分支只做了代码级 + 日志级核对。

## 已具备的 spine 能力（ABI 33 → 79 个函数）

| 能力 | 说明 |
|---|---|
| 动画事件 | push（回调）+ pull（`spine_event_*`）两条面，逐条对拍一致 |
| 多轨 | 每轨独立动画/时间/时间缩放；混合语义实测（`walk+aim` 280 顶点 vs `walk` 276 + 184 个位移） |
| 皮肤 | 按名取皮肤、**运行时 `addSkin` 合成**、按名取附件设到槽 |
| 骨骼 / IK | `bone_find` / 世界位置读写 / IK mix |
| 槽区间 | 按 `getData().getIndex()`（不随 draw order 漂移），用于 Separator |
| Sequence | 序列帧（帧下标/路径/UV 逐帧不同） |
| 物理 | `PhysicsConstraint`；**修了 `skeleton->update(dt)` 缺失**（此前物理永远冻结） |
| 两色着色 | dark 通道（见下） |
| 裁剪遮罩 | `SkeletonClipping` |
| 默认混合 / 时间缩放 | `setDefaultMix` / `setTimeScale` |

### 接入时修掉的真 bug（都带证据）

1. **Sequence 附件此前根本画不出来**：附件的 `_region` 在 `computeWorldVertices`
   （内部 `Sequence::apply`）之前一直是 NULL → 旧顺序下该槽被整槽跳过；多页序列还会取到上一帧的页。
   已改为"先求值、后取 region/UV"。
2. **索引越界自相矛盾**：批次数上限 break 路径上 `vtx += vcount` 漏加 →
   `out.vertex_count` 小于索引用到的顶点数（下游按 vertex_count 建缓冲会越界读）。
3. **spine 物理永远冻结**：`mq_spine_instance_step` 未调 `skeleton->update(dt)`。

## 版本选择（含取舍理由）

用 **4.2 分支**，并**打开 C++ RTTI**。

- **为什么不用 4.3**：4.3 把附件内部结构收成 friend-only（`RegionAttachment` 不再有
  `getRegion()/getUVs()`、`computeWorldVertices` 要外部传 vertexOffsets）；姿势数值全搬到
  `*Pose` 类；约束收进统一 `getConstraints()`；RTTI 只剩 `getClassName()`。与
  `docs/assets/06 §2` 描述的接法不符，胶水极多。
- **4.2 的公开面与设计文档一致**：`RegionAttachment::getRegion()/getUVs()`、
  `computeWorldVertices(slot,verts,offset,stride)`、`Skeleton::setToSetupPose()`、
  `getData()` 返回指针、`getIkConstraints()`、`IkConstraint::setMix()`、内存构造 `Atlas` 全在。
- **打开 RTTI**：附件类型判别用 `dynamic_cast`（spine 自研 RTTI 只给类名字符串）。
  异常仍关闭（spine 用 `getError()` 返回错误）。标志在 `third_party/spine-cpp/CMakeLists.txt`
  的 target 级，不污染其它目标。

## 架构：三层分工

```
spine-cpp（vendored，4.2）          官方运行时：解析骨架/图集、播放求值
      ↓ C ABI（abi/spine2d.h）
maque::spine2d                    ★只产出绘制数据（顶点/UV/颜色/索引 + 页号 + 批次）
      │                            不碰 GPU、不解码纹理
      ↓ C ABI（maque_ifaces.h）
runtime + maque::gfx              纹理页解码上传（image 模块）；网格图元提交与渲染
```

**关键取舍**：`spine2d` 不依赖 `gfx`/`image`。骨架与图集从**内存**进（调用方用 VFS 读），
图集页纹理经回调交引擎解码上传后回填句柄。跨模块只有 C ABI（docs/02）。

## 落地内容

| 项 | 位置 |
|---|---|
| 官方运行时 vendored | `third_party/spine-cpp/`（4.2 分支，160 文件 0.84 MiB，随附 LICENSE） |
| 模块实现 | `modules/anim/spine2d/src/spine2d.cpp` + `spine_extension.cpp`（宿主扩展点 `getDefaultExtension`） |
| C ABI | `generated/abi/spine2d.h`（骨架数据/实例/图集页/绘制批次/绘制数据 + 40 余函数） |
| runtime 接线 | `Engine::spine_setup/spine_step/spine_extract` + 12 个 `spine_*` 脚本 API（`schema/runtime/host.yaml`） |
| **网格图元** | `mq_renderer_v1::render_mesh_views(...)` + 内建 shader `assets/builtin/shaders/mesh/shader.wgsl`（顶点 stride 32B：pos.xy/uv.xy/col.rgba；与 textured 共用 48B 相机 uniform；网格与精灵**统一按 (layer,z) 排序**，可互相遮挡） |
| 独立工程 | `samples/spine/`（`main.cpp` 50 行薄宿主 / `spine.scn` / `dragon.scn` / `spine.js` / `art/` / `README.md`） |
| 测试 | 模块单测 `spine2d`；e2e `spine_smoke` / `spine_e2e` / `spine_dragon`（`tests/spine_e2e.sh`）；gfx 单测新增网格图元用例 |
| 官方素材 | `tests/data/spine/` 与 `samples/spine/art/`（含 `LICENSE-spine-runtimes.txt`） |

**许可**：Spine Runtimes License 允许集成，但再分发必须随附其许可与版权声明，且使用者需自备
Spine Editor 许可。**不要删** LICENSE 文件。

## 接入时踩到并修掉的坑（都写进了代码注释）

1. **`page->texture` 是"渲染器的贴图对象"，不是页对象**。spine 4.2 `Atlas.cpp:290` 会做
   `region->rendererObject = page->texture`；往它里面塞引擎纹理句柄**整数**是对的，
   但渲染侧**不能** `static_cast<AtlasRegion*>` 再解引用（实测 ASan `access-violation on 0x41`）。
   本模块把句柄当不透明贴图对象，页号用 `AtlasPage::index`，另维护"句柄 → 页号"表供批次划分。
2. **`AtlasAttachmentLoader` 必须活到骨架数据销毁**（附件在解析期留下它的指针）。
   用栈上局部对象会导致绘制期读已释放内存（ASan 报 `freed` 填充 `0xf2f2...`）→ 改堆分配并由
   `mq_spine_data` 持有；销毁顺序：骨架数据 → attachmentLoader → atlas → loader。
3. **`Vector` 扩容后指针失效**：先 `setSize` 再取 `buffer()`（ASan 抓到越界读）。
4. **网格 UV 必须取 `getUVs()`**（归一化到整张图集页），**不是** `getRegionUVs()`（区域局部 0..1）。
   用错会让每个网格都去采样图集左上角空白 → **整只角色不可见**。
   ⚠️ 模块单测的"UV ∈ [0,1]"对这条**没有鉴别力**（两种取法都在范围内），
   真正的防线是 e2e 的**像素断言**（角色框内非背景像素数）。
5. **`mq_renderer_mesh.vertex_count` 必须填**：schema 后加的字段，漏填则渲染器按 0 顶点**静默跳过**
   （没有任何报错）。用 `MAQUE_TRACE_SPINE=1` 的统计行定位。
6. **组件资源引用要用 `string` 路径**，不是 `asset_ref<T>`：后者是内容哈希句柄，场景里只能写
   `"0x…"`，无法表达路径。按引擎既有约定（`scn.Sprite.texture`）改；并加了显式 `atlas` 字段
   （解析顺序：`atlas` → `material` → 与骨架同名的 `.atlas`）。
7. **`SpineSkeleton` 需要 role**（`MQ_ROLE_SPINE = 1u<<20`）+ 图集页需要 `pma` 标志：
   官方 pma 图集必须用 `src = ONE`（预乘 alpha），用错会让边缘发暗。
8. **spine 绘制顶点在骨架子空间、y 向上**（spineboy 站立包围盒 y∈[-8,654]，脚在 0）——
   **不是**"y 向下"。所以 y 翻转由 runtime 做（`screen_y = H - world_y - ly`），
   模块保持骨架空间语义；`schema/world/anim.yaml` 的 `mq_spine_draw` 文档已改正并重新生成。
9. **顶点色 = 实例色 × 槽色 × 附件色**（官方 `SkeletonRenderer` 同款三项相乘）。
   一开始漏了**附件自身颜色** —— 那会让动画/皮肤里设的"变红/变暗"（受击反馈、状态染色）失效。
10. **必须显式应用默认皮肤**：`SkeletonData` 的 setup 附件并不全在 `slots` 里，
    皮肤附件（含**遮罩**）存在 skin 下；不 `setSkin(defaultSkin)` 就一起缺失
    （实测：不设皮肤时 `getSkin()==null`，spineboy 的 `clipping` 槽没有附件）。

## 验证（实测）

```bash
ctest --test-dir build-win -R spine      # 4/4 通过
ctest --test-dir build-win -R tank       # 3/3（gfx 改动未影响坦克）
ctest --test-dir build-win               # 62 项（唯一偶发是 text_e2e，单跑 3/3 通过）
```

- **动画清单**（脚本启动即 log）：spineboy-pro 11 个
  `[aim, death, hoverboard, idle, idle-turn, jump, portal, run, run-to-idle, shoot, walk]`；
  dragon-ess 1 个 `flying`、图集 **5 页**（1024×1024，pma=1）。
- **切换链路三层证据**：① `MAQUE_SIM` 注入 ↓/↑/空格 → 日志序列
  `idle → idle-turn → idle → idle-turn → idle` 与注入顺序一致，每行断言
  `engine=<脚本请求的名字>`（`spine_current_animation` 读回**引擎实例**的当前动画）；
  ② 同帧不同动画截图**不同**；③ **像素级**断言角色框 (50,26)-(195,218) 内非背景像素 9051 个。
- **确定性**：同一 `MAQUE_SIM` 跑两次 → `world_trail=0x80c911285900f466` 一致、
  截图 sha256 逐字节一致、切换序列一致。

## 已支持 / 仍未支持（如实记录）

### ✅ 双色着色（Tint Black）已支持

- 绘制数据多输出一路 dark colors（`mq_spine_draw.dark_colors`；`NULL` → 等价 `(0,0,0,1)`）。
- mesh shader 加 `@location(3) col2`，**顶点 stride 32B → 48B**。
- **公式取自官方**（不是推导）：`spine-runtimes` 4.2 `spine-glfw/src/spine-glfw.cpp:292-297` 的内嵌 GLSL：
  ```glsl
  float alpha = texColor.a * lightColor.a;
  fragColor.rgb = ((texColor.a - 1.0) * darkColor.a + 1.0 - texColor.rgb) * darkColor.rgb
                  + texColor.rgb * lightColor.rgb;
  ```
  dark 的**取值**语义见 `SkeletonRenderer.cpp:212-221`（`0xff000000` = 未启用 → rgba(0,0,0,1)，alpha 恒 1）。
- **关键性质**：`dark=(0,0,0,1)` 时该式**精确退化**成旧行为 `rgb = tex.rgb*light.rgb`、`a = tex.a*light.a`
  → 未启用 TintBlack 的素材（tank / flappy / spineboy）**逐字节不回归**（截图 sha256 实测相同）。
  这条退化性质是常驻回归防线（`ctest -R gfx_twocolor`）。

⚠️ **一次被纠正的错误前提（留档）**：本任务最初写的是"**Batching 与 Raptor 用到两色着色**"，
后来**扫了官方 4.2 全部 50 个 JSON 素材**才发现**不成立**：

| 素材 | `dark` 键 | 结论 |
|---|---|---|
| `raptor-pro.json` | **0 个** | `setTwoColorTint(true)` 走默认黑 → 开关**像素完全一样** |
| `spineboy-pro.json`（Batching 实际加载的） | **0 个** | 且两处 `setTwoColorTint(true)` **在上游就是注释掉的** |
| `alien-pro.json` | 3 槽，`eyeball` light=`dfa921ff` / **dark=`c57c1e`** | ← **唯一有鉴别力的官方素材** |
| `coin-pro.json` / `tank-pro.json` | dark 全 `000000` | 与默认黑等价 → 零差异 |

**教训**：拿"素材其实没启用的特性"去做像素断言 = 等于没测（同一个坑在裁剪遮罩上已经踩过一次）。
**先扫素材、再写断言**。

### ❌ 仍未支持 / 未验证

- **multiply / screen 混合回退为 normal**：本机 wgpu-native 的 `WGPUBlendFactor` 没有
  dst-color 类因子，gfx 会打印一行提示。用到这两种模式的素材会偏亮。
- **Separator 的"共享骨架"未复刻**：官方是两个渲染节点共享**同一个** Skeleton/AnimationState；
  我们给的是"槽区间 + 每个实体各自实例"。数值上两段合起来 = 全量，但"共享一份状态"没做。
- **`spine_set_two_color` 是诚实的空操作**：本引擎网格恒走两色路径，无 dark 的素材 = `(0,0,0,1)`，
  与关闭逐字节等价。它只把值写进组件字段，**不改变输出**。
- **`mq_spine_instance_draw` 内部仍会 `updateWorldTransform(Physics_Update)`**（同帧 IK 需要），
  语义上"绘制导出"会重算一次世界变换 —— **不是纯只读**，已在注释写明。
- **物理只验证了"约束存在 + 真的推进 + 冲量生效 + 接口安全"**，**没有**与官方 celestial-circus
  做视觉一致对照，也没有 wind/gravity/inertia 的逐项数值断言。
- **事件容量溢出路径**（>64 条/帧的 `events_dropped`）未覆盖（计数器实现了，但没构造出溢出场景）。
- **真实窗口 + 真键盘未验证**：只跑了 `MAQUE_SIM` 注入路径与无头截图；
  headless 无真实指针事件，`on_pointer_move` 驱动的 IK/物理分支**未被真实指针驱动过**。
- **无官方截图的逐像素对照**：只做到"数据面 + 逐格像素断言"。

## 裁剪遮罩（mask）—— 已支持，但要注意它**不是自动生效的**

已接 `SkeletonClipping`（`clipStart` → 逐附件 `clipTriangles` → `clipEnd`），
与官方 `SkeletonRenderer::render` 同一条算法；裁剪后的几何走
`getClippedVertices/Triangles/UVs`。

⚠️ **两个容易误解的点**（都是实测踩出来的）：

1. **遮罩不会自己生效**。`Skeleton::setSkin` 只给"setup 姿势里声明了 `attachment` 名"的槽
   挂皮肤附件（读 spine-cpp 源码 `Skeleton.cpp:setSkin` 确认）。官方 spineboy 的 `clipping`
   槽在 setup 里**没有**附件名，而且它的 **11 个动画没有一个**驱动该槽 ——
   所以默认播放时遮罩**根本不参与渲染**（这也解释了为什么开关它渲染结果一样）。
   要用就靠动画时间轴或代码显式 `Slot::setAttachment`。
2. **接入时踩的坑**：`SkeletonClipping::clipTriangles` 的第 3 个参数是**索引个数**，
   **不是字节数**。传字节数会让它多读一倍索引并越界
   （ASan 实测：`SkeletonClipping.cpp:180` access-violation）。
   另外索引必须是**局部 0 基**，不能预先加顶点偏移。

### 怎么验证（`ctest -R spine_mask`）

因为官方素材默认不启用遮罩，专门加了显式入口 `MAQUE_SPINE_MASK="槽名:皮肤附件名"`
（默认关闭、不影响正常渲染）与诊断 `MAQUE_TRACE_SPINE_DRAW=1`（输出裁剪计数）。
`spine_mask` 测试做三层断言：

1. 遮罩真的挂上了（日志「遮罩已启用」）；
2. 裁剪路真的被走到**且几何确实被改变**（`clipStarts≥1`、`被裁剪附件≥1`、裁剪前后顶点数不同）；
3. **最强判据**：开启与不开启遮罩的截图必须**不同**（`cmp`）。

实测（spineboy，idle）：`clipStarts=1 被裁剪附件=18 顶点 276→515` ——
顶点数**变多**是正常的：遮罩多边形与几何求交会产生新的顶点。

## 后续可做

1. 真实窗口 + 真键盘/真指针的端到端验证（补 `MAQUE_CAPTURE` 回读路径）；
   重点验证 `on_pointer_move` 驱动的 IK 与物理分支。
2. 补 `PhysicsConstraint` 的 wind/gravity/inertia 逐项数值断言，并与官方 celestial-circus 对照。
3. 事件容量溢出（`events_dropped`）的单测。
4. `phys_timestep`/`update_mode`/`PlayMode` 的其余枚举；multiply/screen 混合（受 wgpu-native 因子限制）。
5. Separator 的"共享骨架"（若真要省一份模拟）。

