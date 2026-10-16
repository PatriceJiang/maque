# Spine 2D 运行时接入 · 进行中（2026-10）

> 目标：集成官方 **spine-runtime**（`spine-cpp`），并在 `samples/` 下做几个播放官方样例动画的场景。
> 状态：**已落地基础设施，绘制导出未打通 —— 模块暂不参与默认构建**（见文末）。
> 单一真相：`modules/core/data/schema/world/anim.yaml` 的 `module_abi`（docs/assets/06 §2 是其设计来源）。

## 已完成并入库

| 项 | 位置 | 说明 |
|---|---|---|
| 官方运行时 vendored | `third_party/spine-cpp/` | 分支 **4.3**、203 文件、1.02 MiB；随附上游 `LICENSE`（**不是** MIT/BSD，见下） |
| 构建封装 | `third_party/spine-cpp/CMakeLists.txt` | 目标级 `-fno-exceptions -fno-rtti`、C++11、`/W0`；不污染其它目标（未用上游那份，它 include 了范围内的 `flags.cmake` 并改全局 `CMAKE_CXX_FLAGS`） |
| C ABI 声明 | `schema/world/anim.yaml` → `generated/abi/spine2d.h` | 骨架数据/实例/图集页/绘制批次/绘制数据 + 40 余个函数；`mq_spine2d` 命名空间与版本宏由 DSL 生成 |
| 模块骨架 | `modules/anim/spine2d/` | C ABI 实现（骨架/图集从内存进、纹理回调交引擎、动画/皮肤/IK/变换约束/绘制导出）+ 单测 |
| 官方样例素材 | `tests/data/spine/` | `spineboy-pro.skel`(1 页图集) 与 `dragon-ess.skel`(**6 页图集**，测多页批处理) + 上游 LICENSE |

**许可注意**：Spine Runtimes License 允许集成，但再分发必须随附其许可与版权声明，且使用者需自备
Spine Editor 许可。已把 `LICENSE` 放进 `third_party/spine-cpp/` 与 `tests/data/spine/`，**不要删**。

## 分层取舍（设计决定，值得保留）

本模块**只产出绘制数据**（顶点/UV/颜色/索引 + 页号），**不碰 GPU、不解码纹理**：

- 骨架与图集从**内存**进（调用方用 VFS 读）→ 数据可来自本地/远程/内存，符合 docs/04；
- 图集页纹理经 `mq_spine_texture_cb` 回调交给**引擎侧**读文件 + 解码 + 上传，再回填句柄；
- 因此 `spine2d` **不依赖** `gfx` / `image`，模块间只有 C ABI（docs/02）。

## 未打通的障碍（已查实，供下一次继续）

spine-cpp **4.3** 的内部结构比设计文档（`docs/assets/06 §2`，按更早版本描述）假设的要封闭：

1. **附件几何不再公开**：`RegionAttachment` 直接继承 `Attachment`，**没有** `getRegion()` / `getUVs()`；
   `computeWorldVertices(Slot&, float* vertexOffsets, float* worldVertices, size_t offset, size_t stride)`
   需要调用方自己传 `vertexOffsets`（`getOffsets(SlotPose&)` 取）。区域与 UV 存在 friend-only 成员里。
2. **RTTI 没有类型枚举**：`RTTI` 只有 `getClassName()`（`getType()` 不存在）→ 判别附件类型要用
   `dynamic_cast`（已改）。注意上游用 `-fno-rtti` 构建，`dynamic_cast` 需要**打开 RTTI**
   —— 这是当前封装的一个矛盾点，要在"打开 RTTI"和"照类名字符串判别"之间做选择。
3. **姿势数值在 Pose 类上**：`Slot::getAttachment()/getColor()`、`Bone::getWorldX()`、
   `IkConstraint::setMix()` 这些 4.2 风格接口都不在 4.3 的对象上，要走
   `getAppliedPose()` / 各自的 `*Pose` 类。
4. **约束是统一数组**：`getIkConstraints()/getTransformConstraints()` 不存在，
   只有 `getConstraints()` 返回 `Constraint*`（需 `dynamic_cast` 过滤）。
5. 加载器改**引用**：`SkeletonBinary(AttachmentLoader&)`、`SkeletonJson(AttachmentLoader&)`、
   `Skeleton(SkeletonData&)`、`AnimationStateData(SkeletonData&)`。

已改到只剩 9 个编译错误（`RegionAttachment` 的 region/UVs 取法、`Skeleton::setColor`、
`TransformConstraintData::getTarget`、`SlotData` 指针误用）。**没有**为了"能编过"而把绘制逻辑
写死成猜测值 —— 那样只会得到一个静默渲染错误的集成。

## 当前构建状态

`modules/anim/spine2d` **不参与默认构建**（根 `CMakeLists.txt` 里该行已注释并写明原因）。
打开那一行即可参与构建，但当前会编译失败（属已知进行中状态）。
仓库其余部分**全量 ctest 通过**（55 项，2 项 Windows 环境限制跳过）。

## 下一步（建议顺序）

1. 定下类型判别方案：打开 RTTI 用 `dynamic_cast`，或按 `getClassName()` 字符串判别（与上游
   `-fno-rtti` 一致）。这决定后面所有代码的写法。
2. 打通绘制导出：从 `RegionAttachment::getOffsets(pose)` + `updateSequence`/`Sequence` 取 UV；
   网格走 `MeshAttachment::getRegionUVs()/getTriangles()`；`computeWorldVertices` 传对参数。
3. 接 runtime：把 `SpineSkeleton` 组件（已在 `anim.yaml` 声明）在装配期实例化、每帧 step、
   提取绘制数据；纹理页经 `image` 模块解码 + `gfx` 上传。
4. 按 docs/assets/06 §2.6 做**确定性**校验：固定 dt 推进、同一时间点重放姿势逐字节一致
   （单测里已写好这类断言，等能编过即可用）。
5. `samples/spine/`：参考 `samples/flappy` 的解耦做法（薄宿主 + 资源），
   放 spineboy（含 `idle/walk/run/jump/aim/death` 等）与 dragon（多页图集、大量网格附件）两个场景，
   外加一个 IK 跟随（上游 `ik-following` 样例的用法）与一个换皮肤。
6. 逐场景截图核对（`--shot`）+ `ctest -R spine` 回归。
