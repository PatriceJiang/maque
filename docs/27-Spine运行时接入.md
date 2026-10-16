# 27 · Spine 2D 运行时接入（已完成）

> 目标：集成官方 **spine-runtime**（`spine-cpp`），并交付**独立**的 `samples/spine/` 工程：
> 渲染官方素材 + 上/下键切换动画。
> 状态：**已完成** —— 模块参与默认构建、runtime 已接入渲染管线、sample 可跑、测试全绿。
> 单一真相：`modules/core/data/schema/world/anim.yaml` 的 `module_abi`（设计来源 `docs/assets/06 §2`）。

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

## 已知限制（如实记录）

- **双色着色（Tint Black）未支持**：spine 4.2 有完整的双色接口
  （`Slot::getDarkColor()` / `hasDarkColor()` / `SlotData::setHasDarkColor()` / `TwoColorTimeline`，
  官方参考结构 `SkeletonRenderer::RenderCommand` 里也有一路 `darkColors`），
  本实现**只取 light 色**（`Slot::getColor()`），没有第二路顶点色、也没有光照插值。
  要做需：绘制数据多输出一路 dark colors + mesh shader 加 `@location(3)` 与光照参数
  （顶点 stride 32B → 48B）。
  ℹ️ **官方 spineboy / dragon 都没开 Tint Black**（实测：`spineboy-pro.json` 的 `skins` 下
  `dark` 字段出现 **0 次**），所以当前样例渲染**没有可见缺陷**；但用到该特性的素材会画错。
- **`Sequence`（序列帧附件，4.2 新增）未接**。
- **multiply / screen 混合回退为 normal**：本机 wgpu-native 的 `WGPUBlendFactor` 没有
  dst-color 类因子，gfx 会打印一行提示。用到这两种模式的素材会偏亮。
- 组件里的 `phys_timestep` / `update_mode` / `SpineBlend` / `PlayMode` 的 PingPong/ClampForever
  **未接**（`step` 固定 `Physics_Update`，`play_mode` 只区分 loop/once）。
- **真实窗口 + 真键盘的 ↑↓ 未验证**：只跑了 `MAQUE_SIM` 注入路径与无头截图；
  `key_from_sdl` 的归一化只做了代码级核对，没有真实按键实证。

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

1. 接**双色着色**（dark color 通道 + mesh shader 光照）——注意官方样例没用到，
   需要自备开 Tint Black 的素材，否则又会做成"写了但没法证明对"的东西。
2. 接 `Sequence`（序列帧附件）。
3. 接 `phys_timestep`/`update_mode`/`PlayMode` 的其余枚举；multiply/screen 混合。
4. 真实窗口 + 真键盘的端到端验证（补 `MAQUE_CAPTURE` 回读路径）。
