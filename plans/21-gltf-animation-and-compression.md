# 21 · glTF 动画/蒙皮/变形 与 压缩扩展解码 —— 评估与落地方案

> 状态：**已落地（plans/22，第 29 轮，2026-10-28）**：S1/S2/S3 按下文清单实现并通过
> `ctest -R 'gltf_(skin|anim|morph)_e2e'`；27 个 Khronos 动画/蒙皮/morph 模型已重新迁移。本文保留评估与设计依据
> （实现契约以 `plans/22-gltf-animation-implementation.md` 为准）。压缩扩展（B 节）仍按本文结论不做。
> 性质：为 `docs/31-gltf-model-migration.md`（Khronos glTF-Sample-Assets 迁移）的两个 P2 大项给出**实现级**方案、分阶段文件改动、可断言验收与工作量，供 Lead 排期。
> 关联：`plans/12-animation.md`（完整动画模块设计，未实现）、`docs/31 §5.4`（差距表）、`docs/31 §11`（后续工作表）、`plans/20-pbr-audit.md`（P1 剩余项）。

---

## 0. TL;DR（结论表）

| 大项 | 子项 | 结论 | 推荐下一步 | 工作量（人日，含测试/批量重跑） | 风险 |
|---|---|---|---|---|---|
| **A. 动画/蒙皮/变形** | **S1 骨架 + 蒙皮 + bind pose** | 当前顶点没有 `JOINTS_0/WEIGHTS_0`、没有蒙皮渲染路径；`.mesh.json` 无 skin 数据；7 个主变体模型（90 个 skin 实例/65 个蒙皮 primitive）只能静态烘焙。**必须新增**：skin 顶点数据 + 关节调色板 storage buffer + 新 vertex entry + 3D 层级世界矩阵。 | 先做 S1（Fox/BrainStem/CesiumMan/RiggedFigure/RiggedSimple/SimpleSkin/RecursiveSkeletons 出正确 bind pose）；同时把 3D 层级世界矩阵补上（现在 `flatten_world` 只累积 x/y） | **7–10**（乐观 7 / 保守 10） | 中高 |
| | **S2 关键帧采样 + 节点动画** | 已有 2D 属性轨道动画（`scn.AnimationPlayer` + `anim_step`），但无 glTF 节点/骨骼采样。主变体 **26 模型 / 38 片段 / 1319 通道**（rotation 1001、translation 119、pointer 108、scale 86、weights 5；LINEAR 1311 / STEP 5 / CUBICSPLINE 3）。 | S1 之后做 S2：importer 导出 `asset.AnimationClip`（复用既有 schema + 追加 Node/Morph 轨道种类）+ runtime 纯函数采样 quat/STEP/LINEAR；22/26 模型可完整动起来 | **5–9** | 中 |
| | **S3 morph（变形目标）** | 主变体 **4 模型 / 6 morph primitive**（目标数 1、2、8；默认权重 [0,0]、[0.5]、[0×8]、[0.5,0.5] 现在完全不生效）。 | S2 之后做 S3：CPU morph（顶点只有 3/24/30/1528 个，GPU morph 无必要）+ 权重采样；`update_mesh` ABI | **3–5** | 低中 |
| | **S4 混合/状态机** | 迁移不刚需（每个模型单主片段/自动播放即可验收）。 | 本轮**不做**；跨片段交叉淡入放到 `plans/12` 落地时一起做 | 3–5（若做） | 中 |
| | `KHR_animation_pointer` | **5 模型 / 108 通道**（104 个唯一指针：106 条材质/贴图变换 + 2 条节点可见性）。其中 4 个模型**纯 pointer**（AnimationPointerUVs 103 条、CubeVisibility、LightVisibility、PotOfCoalsAnimationPointer），S2 做完仍不动。 | 明确标注 gap；等材质动画通路（extract 每帧重建材质，技术上可复用）再补，P3 | 2–3（若做） | 中 |
| **B. 压缩扩展（KTX2/BasisU + meshopt/Draco）** | KTX2 / BasisU | 主变体 **0 模型/0 贴图**需要解码（KTX2 只出现在 7 个可选变体目录/6 模型；60 个 `.ktx2` 文件 45.21 MB，24 个 BasisLZ(ETC1S) + 36 个 UASTC+zstd，`vkFormat` 全为 0=需转码）。**不阻塞迁移**。 | **方案 C：保持跳过、只用主变体**；只做 P0 告警/文档（0.25–0.5 人日）。若要覆盖压缩变体再做方案 A（importer 侧离线转码，1.5–2.5 人日） | 0（本轮） | 低 |
| | meshopt | 主变体仅 `MeshoptCubeTest` 1 个模型"使用" meshopt，但数据是 **fallback 未压缩 buffer**（已导入成功，34 个 mesh 资产 bounds 全部 ±0.5，manifest `ok`）；全变体 6 目录/3 模型，其中真正需要 meshopt 解码的只有 BrainStem / DragonAttenuation 的 `glTF-Meshopt-EXT` 2 个模型。 | **不做解码器**；文档写清"主变体经 fallback 导入，无需 meshopt 解码" | 0 | 低 |
| | Draco | 主变体 **0 模型**（Draco 只在 18 个可选变体目录/18 模型）。 | **不做**；迁移已用主变体规避 | 0 | 低 |
| | `KHR_mesh_quantization` | 主变体 1 个模型（MeshoptCubeTest，`POSITION` 仍是 float，normal 为 normalized s8 但方向不受影响）；全变体 10 目录/7 模型。 | **不做**专门代码；在文档标注"仅压缩变体涉及" | 0 | 低 |

一句话：**先把 S1+S2+S3 作为一个整体大项（约 17–24 人日）排进计划；压缩三项都不排实现，只做 0.5 人日的告警/文档收尾。**

---

## 1. 本轮范围、统计口径与复现方法

### 1.1 范围

- **A（动画/蒙皮/变形）**：现状审计（顶点通道 / `.mesh.json` 通路 / runtime 与 gfx / ABI / shader / schema）；主变体资产统计；代表性模型；S1–S4 分阶段落地方案、文件级改动、可断言验收、工作量与风险；明确不做项。
- **B（压缩扩展解码）**：importer 当前处理方式、运行时贴图格式能力、主变体 vs 全变体统计；方案 A/B/C/D 对比、推荐、若实施的落地位置与构建/CI/确定性影响；本轮不做 + 可立即执行的 P0/P1。
- **不在本轮**：不写代码、不改 schema/assets/tools/tests、不构建、不跑 ctest、不做 git 操作。

### 1.2 统计口径（很重要，与既有文档数字对照时先看这里）

| 口径 | 定义 | 结果 |
|---|---|---|
| **主变体（main variant）** | 与 `tools/migrate_gltf_models.py` 的选源规则一致：优先 `Models/<M>/glTF/`（若存在同名 `.gltf`），否则 `glTF-Binary/` 下首个 `.glb`。迁移只导入这一份。 | **148 个模型**（149 个模型目录中含手调 ToyCar，不计入自动统计） |
| **全变体（all variants）** | 遍历 `Models/<M>/<变体目录>/` 下**所有** `.gltf/.glb`（同一模型多个变体、同一变体目录多个文件都计入文件数；按目录聚合时取该目录内最大计数）。 | **337 个 gltf/glb 文件、149 个模型目录、6～7 类压缩变体** |
| 动画/蒙皮/变形计数 | 每个文件读 `animations[].channels`、`skins[].joints`、`meshes[].primitives[].targets`；跨变体聚合时同目录内取最大值。 | 主变体 26/7/4；全变体 69/24/10（目录） |
| 图像编码计数 | 按 `images[]` 的 `mimeType`/uri magic/`.ktx2` 文件统计；同一贴图被多个变体引用时按 image 条目重复计。 | 主变体 705 条（697 + ToyCar 8）；全变体 1560 条 |
| `KHR_animation_pointer` | 通道指针在 `channels[].target.extensions.KHR_animation_pointer.pointer`（**不是** `target.pointer`，直接按 `target.pointer` 统计会得 0）。 | 5 模型 / 108 通道 / 104 唯一指针 |

> ⚠️ 与既有文档的数字差异（不建议互相覆盖，只做口径说明）：
> - `docs/31 §11` / `plans/20` 的"全变体 meshopt 2"= 本轮的"真正需要 meshopt 解码的 2 个模型"（BrainStem、DragonAttenuation 的 `glTF-Meshopt-EXT`）；本轮按变体目录统计为 6 目录/3 模型（多出 `MeshoptCubeTest` 的主变体与 fallback 变体，它们不需要解码）。
> - `plans/20` 的"KTX2 81"与"morph target 15"分别是"变体级 / morph primitive 计数"口径；本轮按 image 条目与目录计数分别为 107 条 / 10 目录，模型级为 4。
> - 主变体动画 26 / skin 7 / morph 4 与 `docs/31 §5.4` 完全一致，可直接引用。

### 1.3 复现（本轮实际执行）

统计脚本为**临时 Python（不在仓库内）**，关键逻辑见附录 A；运行环境 Python 3.11，只读 `.deps/gltf-assets/glTF-Sample-Assets-main/Models/**`（`.deps` 不入库）。资产总量约 1.09 GiB，扫描耗时 <1 min。核心命令形态：

```powershell
# 主变体统计（148 模型）
python $env:TEMP\maque_anim_final.py
# 全变体统计（337 文件）
python $env:TEMP\maque_variant_scan.py
# KTX2 文件头逐字节解析（scheme/vkFormat/levelCount）
python $env:TEMP\maque_ktx_scan.py
# 代码/资产核对（只读）
Select-String -Path modules/render/gfx/src/gfx.cpp -Pattern 'p3dMatPipeline|create_mesh|pack_draw_uniform'
```

### 1.4 明确"未验证"的项

- 未构建/未运行：所有"验收可断言"来自代码与现有测试基建的静态推断，非实测。
- 未探测本机 WebGPU 适配器的 `maxStorageBuffersPerShaderStage`/`maxVertexBuffers`/compressed texture features（代码在 `device` 创建时整体请求 adapter limits，desktop 通常远大于默认值，但未打印确认）。
- KTX-Software / basisu CLI 在 Windows 的可用性、体积、具体参数（`ktx extract` 对 BasisU 的行为 / `basisu -unpack` 是否输出 PNG）**未验证**。
- `KHR_meshopt_compression`（旧扩展名）与 `EXT_meshopt_compression` 的 fallback 语义差异只做了结构核对（BrainStem/DragonAttenuation 的 `*-EXT` 才真正压缩），未做像素对比。
- MeshoptCubeTest（quantization + KHR_meshopt fallback）当前渲染与参考图的偏差只按 bounds/结构判断，未逐像素对比。
- CUBICSPLINE 的 quat 语义（glTF 允许对四元数做三次样条）未实测；本方案先按 STEP/LINEAR 验证，cubic 走近似。
- 动画帧率/性能影响未测（已知 per-draw uniform/bind group 每帧重建是既有 P3 问题，见 `plans/20 §6`）。

---

# A · 动画 / 蒙皮 / 变形

## 2. 现状审计

### 2.1 当前顶点通道与 skin 的差量

`modules/render/gfx/src/gfx.cpp` 只支持两种顶点布局（`r_create_mesh`，1016–1044 行；`get_material_pipeline`，503–570 行）：

| 布局 | stride | 通道（按偏移） | 对应 shader 入口 | 备注 |
|---|---|---|---|---|
| 0 | 48 B | pos f32×3 @0, normal f32×3 @12, tangent f32×4 @24, uv0 f32×2 @40 | `vs` / `vs_shadow` | tangent 未被 shader 使用（法线切线基用屏幕导数，`plans/20` 修复） |
| 1 | 56 B | pos @0, normal @12, color f32×4 @24, uv0 @40, uv1 @48 | `vs1` / `vs_shadow1` | `plans/20` 的 COLOR_0 + TEXCOORD_1 |

- 顶点属性声明：主 pass `attrs[5]`（531–541 行），阴影 pass `sattrs[4]`（440–450 行）；`kMatVsEntry[layout]` 只按布局选 entry；管线缓存 `p3dMatPipeline[10][2][3][2][2]`（104 行：shading_model × blend × side × mirrored × layout）、阴影 `shadowPipeline[2]`（114 行）。
- **skin 需要的 `JOINTS_0`/`WEIGHTS_0` 完全不存在**：`tools/gltf_import.py` 只读 `POSITION/NORMAL/TEXCOORD_0/TEXCOORD_1/COLOR_0/indices`（928–1028 行），`skins`、`animations`、`targets` 三个字段一次都没读。
- 现有 PBR shader：`assets/builtin/shaders/pbr/40_vertex.wgsl` 入口 `vs`(66 行)/`vs1`(79 行)/`vs_shadow`(12 行)/`vs_shadow1`(22 行)，公共变换 `vs_common`(52 行) 只做 `model` + 逆转置法线；`00_types.wgsl` 的 `Draw` 结构 288B/72 floats（38 行），`VOut` 位置 0..4（77–82 行）。

### 2.2 数据通路（现状）

```
glTF --(tools/gltf_import.py, 离线)--> <M>.mesh.json (vertices/indices/bounds/materials[0])
     --(--scene)--> <M>.scn（每个 (node,primitive) 一个平级实体，node 世界矩阵**烘焙**进 local TRS）
       --(runtime load_mesh, runtime.cpp:1389-1453)--> gfx create_mesh (layout 0/1) --(extract_3d 每帧)
         --> mq_renderer_draw3d{mesh,material,model,sort_key} --> gfx 管线/绑定
```

- `.mesh.json` 的字段由 importer 写死：`name`、`vertex_stride`、`vertex_layout`、`vertices`、`indices`、`bounds_min/max`、`materials`（998–1007 行；`--compact` 用 `separators`，非 compact 用 `sort_keys=True`，1 个输入逐字节确定）。
- 场景发射 `emit_scene`（749–919 行）：DFS 遍历 glTF node，`world = parent × nodeMatrix`（`_node_matrix` 支持 `matrix` 或 TRS）→ 把**世界**分解成 Euler TRS 写进实体 `scene.Transform3D`；实体名 `nodeName_pi`；skinned mesh 也走这条路（因此 bind pose 会被节点世界矩阵位移/旋转，见 `scenes/khronos/Fox/README` 的"骨骼蒙皮未应用"）。
- 运行时 `load_mesh` 按路径缓存（失败缓存 -1）；`setup_3d`（1685 行）扫描 `render.MeshRenderer` 计数组件；`extract_3d`（1717 行）每帧为每个 MeshRenderer 读材质 → `load_mesh`（缓存命中）→ 组 draw，`m4_from_trs(trs, dr.model)`（2031 行）只用**实体本地** TRS，**不乘父节点**。
- 运行时**没有**任何 skeleton/skinned/morph 概念；`asset.SkeletonData`/`BoneB`、`anim.Skeleton`/`anim.Animator` 只是 schema 声明（`asset.yaml` 90–98/180–185 行、`world/anim.yaml` 122–160 行），无代码消费（`spine2d` 的 `SkeletonData` 是 spine-cpp 的类型，不是同一条通路）。

### 2.3 运行时已有动画（2D 属性轨道）与 glTF 骨骼动画的差异

现有能力（可复用/需要绕开的点）：

- `scn.AnimationPlayer`（`world/scn.yaml` 73 行，`roles: [anim_player]`）+ `MQ_ROLE_ANIM_PLAYER` + `Engine::anim_step(dt)`（2640 行）：
  - `load_clip`（2604 行）从 JSON 反序列化 `asset.AnimationClip`（`asset.yaml` 172–179 行），`Track`（111–120 行）/`Key`（104–110 行，28B：time + Vec4 + 两个标量切线）。
  - 采样仅"线性 + Vec4"，写回字段（`resolve_field_path` 只在本实体组件内解析），时间步进用**固定 dt**，无墙钟（确定性 ✅）。
  - `anim_step` 在每模拟步调用（2292 行），`extract_3d` 在渲染前调用（2358 行）。
- **差异**：glTF 动画通道目标是**节点**（跨实体、需层级世界矩阵）、值是 quat/vec3、插值有 STEP/LINEAR/CUBICSPLINE、需要骨骼调色板而非写标量字段；现有 `field_path` 机制只写本实体标量/Vec4，无法直接复用采样/写回逻辑。**可复用的是 `asset.AnimationClip` 容器格式、JSON 反序列化、确定性时间推进与固定 dt 调用点**。

### 2.4 3D 层级世界矩阵缺失（必须先补）

`Scene::flatten_world`（3652–3672 行）只累积 2D 位置（`world_x/world_y`）；`extract_3d` 只用实体局部 TRS。当前所有导入场景都是**平级实体 + 世界烘焙**，所以没暴露问题；一旦为了骨骼层级保留 glTF 节点树，必须：
- 新增 `flatten_world3d`：按与 `flatten_world` 相同的确定性遍历顺序（根 id 升序、子节点 id 升序，栈序反转）自父向子累积 mat4；
- `extract_3d` 的 `dr.model` 改为实体 3D 世界矩阵（平级实体 world=local，**零漂移**）；
- 骨骼调色板用关节实体世界矩阵。

### 2.5 已声明但未实现的 schema（与 `plans/12` 的关系）

- `asset.SkeletonData`（bones: `BoneB[112B]`，name_hash/parent/bind TRS/IBM）、`asset.AnimationClip`/`Track`/`Key` 已在 schema 里，但除 2D 属性轨道外无实现。
- `world/anim.yaml` 有完整未来设计（LoopMode/PlayMode/UpdateMode/InterpMode/SkinningMode/Animator/Skeleton/BonePose/...），**只有 `anim.SpineSkeleton` 是已实现的 2D 组件**；`plans/12-animation.md` 是完整模块设计（`mq_anim.h` ABI、`.anim`/`.skel` 文本、状态机、快照），**未实现**，其 §5 排期为 M0 冻结接口→M1 测试→…→M6 集成。
- **本方案的定位**：以迁移为目标，先在 runtime/gfx 内落一个"最小可断言子集"（S1–S3），复用既有 schema/asset 类型，**不新增模块、不冻结 `mq_anim.h`**；等 `plans/12` 真正启动时，把采样/蒙皮数学抽到 `modules/anim`（纯函数部分可平移），避免把完整状态机/快照设计拖进迁移工期。若 Lead 决定先排 `plans/12` 的 M0–M2，则 S2 的采样核心直接落该模块、本方案的 S2 文件清单相应替换（见 §3.5）。

### 2.6 资产统计（主变体）

来源：附录 A 脚本，148 模型。**动画**：26 模型 / 38 片段 / 1319 通道 / 1319 sampler；通道路径 rotation 1001、translation 119、pointer 108、scale 86、weights 5；插值 LINEAR 1311（含缺省）、STEP 5、CUBICSPLINE 3；最长片段 60.0 s（ChronographWatch），次长 34.88 s（BrainStem）。
**skin**：7 模型 / 90 个 skin 实例（84 个在 RecursiveSkeletons）/ 65 个蒙皮 primitive（BrainStem 59、其余每模型 1）；每 skin 关节数：Fox 24、BrainStem 18、CesiumMan 19、RiggedFigure 19、RiggedSimple 2、SimpleSkin 2、RecursiveSkeletons 10×84；**单 skin 最大 24 关节**（决定调色板上限 ≥64 即可，取 256 留余量）。
**morph**：4 模型 / 6 morph primitive；目标数：AnimatedMorphCube 2（NORMAL/POSITION/TANGENT）、MorphPrimitivesTest 2 primitive×1（POSITION）、MorphStressTest 2×8（POSITION/NORMAL）、SimpleMorph 1×2（POSITION）；默认权重：AnimatedMorphCube `[0,0]`、MorphPrimitivesTest `[0.5]`、MorphStressTest `[0,0,0,0,0,0,0,0]`、SimpleMorph `[0.5,0.5]`。
**pointer**：5 模型 / 108 通道 / 104 唯一指针；106 条材质/贴图变换（`baseColorFactor`、`KHR_texture_transform` 的 offset/scale/rotation、扩展材质贴图 transform）+ 2 条 `KHR_node_visibility`（CubeVisibility、LightVisibility）。纯 pointer 的 4 个模型 = AnimationPointerUVs / CubeVisibility / LightVisibility / PotOfCoalsAnimationPointer。
**层级**：动画/骨骼模型最大节点深度 29（RecursiveSkeletons，< runtime `load_entity` 深度上限 64）；节点数 RecursiveSkeletons 924、VirtualCity 234、AnimationPointerUVs 119。

代表性模型（主变体，用于验收抽样）：

| 模型 | 动画片段 | 时长(s) | skin | 关节数 | morph 图元/目标 | pointer | 备注 |
|---|---|---|---|---|---|---|---|
| Fox | 3 | 3.42 | 1 | 24 | — | — | 最典型骨骼动画；当前静态 bind pose |
| BrainStem | 1 | 34.88 | 1 | 18 | — | — | 59 个 prim 共享 1 skin；顶点 34 159 |
| CesiumMan | 1 | 2.00 | 1 | 19 | — | — | 人体行走循环 |
| RiggedFigure | 1 | 1.25 | 1 | 19 | — | — | |
| RiggedSimple | 1 | 2.08 | 1 | 2 | — | — | 最小骨骼（2 关节） |
| SimpleSkin | 1 | 5.50 | 1 | 2 | — | — | 官方最小蒙皮样例（10 顶点） |
| RecursiveSkeletons | 1 | 2.00 | 84 | 10×84 | — | — | 924 节点、840 通道；**极限样例** |
| BoxAnimated | 1 | 3.71 | — | — | — | — | 节点层级旋转（depth 2） |
| InterpolationTest | 9 | 2.00 | — | — | — | — | STEP/LINEAR/CUBICSPLINE 混合（唯一 cubic 来源） |
| VirtualCity | 1 | 30.00 | — | — | — | — | 234 节点/167 prim/73 通道 |
| AnimatedMorphCube | 1 | 4.20 | — | — | 1/2 | — | 权重也由动画驱动 |
| MorphStressTest | 3 | 9.37 | — | — | 2/8 | — | 8 目标压力样例 |
| SimpleMorph | 1 | 4.00 | — | — | 1/2 | — | 默认权重非 0（静态观感依赖） |
| AnimationPointerUVs | 1 | 6.00 | — | — | — | 103 | 纯 pointer（99 个 UV transform） |
| AnimatedColorsCube | 1 | 3.00 | — | — | — | 1 | 2 条节点 TRS + 1 条 baseColorFactor |

### 2.7 压缩扩展对主变体动画模型的影响（回答"主变体里被压缩扩展阻塞"）

- **Draco / KTX2 主变体：0 个阻塞**（148/148 可导入，manifest 全部 `ok`）。26 个动画模型的主变体均无 Draco/KTX2。
- **meshopt**：主变体仅 `MeshoptCubeTest`（1 动画 / 5 通道 / 无 skin/morph）。该模型 `extensionsRequired` 含旧名 `KHR_mesh_quantization`+`KHR_meshopt_compression`，但其 `bufferViews` **没有**压缩扩展，数据在 fallback 未压缩 buffer 里；importer 经 fallback 导入成功，已生成 35 个 `.mesh.json`，bounds 全部 ±0.5（结构核对通过）。因此**不阻塞**，S2 可正常播放它的节点旋转。
- **量化**：主变体 1 模型（同上）；`POSITION` 为 float，不受 `normalized` 问题影响；normal 为 normalized s8 时 shader 会 normalize，方向不变。全变体 10 目录/7 模型中的真正量化变体属可选变体，不迁移。
- **WebP**：主变体 1 模型（SheenWoodLeatherSofa，13 张贴图）已通过 PIL 转 PNG（`scenes/khronos/SheenWoodLeatherSofa/textures/` 13 个 PNG 实测存在），不阻塞。
- 结论：**压缩扩展不会阻塞动画大项**，A 与 B 可完全独立排期。

### 2.8 现有测试/样例基线（验收可挂的位置）

- `tests/gltf_import_e2e.sh`（558 行）：importer e2e；fixtures 在 `tests/data/gltf/{triangle,extensions,vertex_uv1,ext_material_slots,instancing,diffuse_volume,lights_visibility,cube}.gltf`；含确定性（同输入两次逐字节一致）与"输出 = 已入库资产"漂移断言。
- `tests/anim_e2e.sh`（`scenes/anim/fade.anim.json`）：2D 属性轨道动画（Sprite radius 关键帧），`samples/player/CMakeLists.txt` 162 行注册。
- `samples/player/CMakeLists.txt`：`gltf_import_e2e` 135–139、`render3d_scene_e2e` 96–100、`material_*_e2e` 113–125、`anim_e2e` 162。
- 截图基建（AGENTS.md / `--shot`）：`--shot` / `--shot-seq` / `--shot-every --shot-count` / `--frames`，`MAQUE_WINDOW=0x0`、`MAQUE_PRESENT=off`（headless CI 常规用法）；`MAQUE_RENDER3D_INFO=1` 打印 mesh 加载/材质信息（可断言 `mesh=-1` 不出现）。

---

## 3. A · 总体设计决策（先定这些，再拆阶段）

### 3.1 资产格式：复用既有 schema，追加最小字段

**结论：复用 `asset.SkeletonData` / `asset.AnimationClip` + `.mesh.json` 追加 skin/morph 数据，不发明新的二进制格式，不改 `vertex_layout` 0/1 语义。**

```
<M>.mesh.json                # 几何（不变）+ 追加：
  "joints":  [[j0,j1,j2,j3], ...]        # 与 vertices 等长；u16 展开（源 u8 也展开）
  "weights": [[w0,w1,w2,w3], ...]        # 与 vertices 等长；float，归一化（sum≈1）
  "morph":   { "targets":[{"positions":[...],"normals":[...]}, ...],
               "default_weights":[...] } # 仅 morph 模型；targets 顺序 = glTF 顺序
<M>.<node>_skin<k>.skel.json # asset.SkeletonData（+追加 joint_names）
  { "name":"<node>", "joint_names":["...",...],
    "bones":[{"name_hash":..., "parent":-1, "bind_position":[...], "bind_rotation":[x,y,z,w],
              "bind_scale":[...], "inverse_bind":[16 floats 列主序]}, ...],
    "skinning":0, "constraints":[] }
<M>.<clip>.anim.json         # asset.AnimationClip（+追加 node_name / TrackKind::Node、Morph）
  { "name":"<clip>", "length":<max input time>, "loop":0, "interpolation":0, "fps":30,
    "tracks":[{"kind":2, "node_name":"<node>", "field_path":"", "bone_index":0,
               "value_type":3 /*Quat*/|2 /*Vec3*/|0 /*Float*/, "interpolation":1 /*Linear*/|0 /*Step*/,
               "key_count":N, "keys":[{"time":t,"value":[x,y,z,w]}, ...]}, ...] }
<M>.scn                      # 26 个动画/骨骼模型改为"层级实体"
```

字段追加（**全部尾部追加、旧资产零变化**）：
- `asset.Track` 追加 `node_name: string`（`TrackKind::Node`/`Morph` 用；`field_path` 保持 2D 语义不变）。
- `asset.TrackKind` 追加 `Node = 2`、`Morph = 3`（现有 `Property=0, Bone=1`）。
- `asset.SkeletonData` 追加 `joint_names: "string[]"`（实体解析真源；`BoneB` 不动，避免改 112B POD 布局）。
- `render.MeshRenderer` 追加 `skin_path: string`（空 = 非蒙皮；相对场景目录）。
- `scn.AnimationPlayer` 追加 `clips: "string[]"`、`clip_index: int32`（空 = 旧 2D 属性轨道行为；非空 = glTF 多片段）。
- `asset.Key` **不动**（28B）：CUBICSPLINE 暂不扩展切线向量，按 LINEAR 近似（只有 InterpolationTest 3 条通道，明确标注 gap）。

优点：不新增 asset kind；`load_clip` 的 JSON 反序列化/缓存/时间推进可复用；schema-first（改 schema 再由 codegen 生成，不手改生成物）。代价：Track/Key 的 Vec4 容器对 translation/scale 稍浪费（可接受），cubic 精确度有损（记录为已知限制）。

### 3.2 场景层级、命名与"skinned mesh 变换忽略"

- 26 个"有动画/骨骼/morph"的模型改用**层级场景**（glTF node 树 1:1）：每个 node → 实体（`scene.Transform3D` 用 node 本地 TRS），带 mesh 的 node → 子实体（identity local `Transform3D` + `render.MeshRenderer`，一个 primitive 一个）。
- **skinned mesh 节点按 glTF 规范忽略其世界变换**（skinning 矩阵已含全部关节世界信息）：蒙皮网格实体放到场景根下、`Transform3D` 为 identity、`MeshRenderer.skin_path` 指向 `.skel.json`。静态烘焙方案对 BrainStem/CesiumMan/RiggedFigure/RiggedSimple 是错的（这些蒙皮节点有非 identity 父链），S1 会让它们回到规范位置。
- 其余 123 个无动画模型**保持平级 + 世界烘焙输出，逐字节不变**（保护 `gltf_import_e2e` 的 cube 基线漂移断言与批量预览）。
- 节点命名：`name = glTF node.name or "node<idx>"`；重名按遍历顺序追加 `#2/#3`；节点名在**整棵场景内唯一**，作为 `anim` 轨道/`SkeletonData.joint_names` 的引用键（runtime 在场景子树内按名解析后缓存 entity id，失败则告警并回退静态）。
- 动画实体与关节实体数量（最大）：RecursiveSkeletons 924 节点 + 84 蒙皮 mesh 子实体；BrainStem 22 节点 + 59 子实体。`.scn` 体积预计 +0.5–1 MB（派生资产、随迁移提交，见 `docs/31 §0` 的入库约定）。

### 3.3 姿态与调色板语义（关键决策：派生姿态，不写回 `Transform3D`）

问题：`scene.Transform3D.rotation` 是 **Euler XYZ**（`runtime.cpp` 的 `m4_from_trs`，1612–1623 行），glTF 动画是 **quat**，写回组件会经过"quat→Euler"有损转换并可能万向节锁；而关节只用于渲染调色板，无需成为可编辑组件。

**决策：动画采样结果保存在"本帧派生姿态表"，不回写 `Transform3D`。**
- 权威状态 = `scn.AnimationPlayer.time`（进快照，组件字段，确定性回放）；
- 每模拟步：`time += dt*speed`（loop/clamp）→ 对每条 `TrackKind::Node` 轨道采样出 quat/vec3 → 与实体 `Transform3D` 的静态 TRS 合并（有轨道则用采样值，无轨道用组件值）→ 组装本帧局部 TRS；
- `flatten_world3d`：按层级累积得每实体世界矩阵；`extract_3d` 用世界矩阵作为 `draw.model`；
- 蒙皮：`palette[j] = world(关节 j) × inverse_bind[j]`（列主序），仅对上载调色板的 draw 生效；
- bind pose（无动画或 t=0 且片段含全部关节）下 `palette ≈ I`，这是 S1 的核心不变量。
- 快照/确定性：`time` 是纯标量进快照；采样是 `(time, clip)` 的**纯函数**；不引入线程/异步；每步按实体 id 升序、轨道声明序处理 → 可回放。
- 代价/取舍：脚本读 `node_position/rotation` 看不到动画后的值（读到的是静态 bind TRS）。若以后需要"脚本可操控关节"，再引入 transient 的 `anim.JointPose` 组件或把 quat 加到 `Transform3D`（本轮明确不做，见 §8）。

### 3.4 渲染路径

- **skin 顶点用第二个 vertex buffer**（不扩 layout 0/1 的 stride）：`mq_renderer_mesh_gpu` 追加 `skin_vertices`（`const void*`，每顶点 u16×4 JOINTS + f32×4 WEIGHTS = 24 B）、`skin_stride`、`skin_vertex_count`（可直接由 `vertex_bytes/vertex_stride` 推出，追加显式字段更清楚）。原因：layout 0/1 与既有资产业已 byte-stable；第二 buffer 对 (layout0|1)×(skin) 正交组合，shader 属性位置固定为 5/6。
- **关节调色板上限**：`MQ_SKIN_MAX_JOINTS = 256`（本批最大 24）；gfx 校验超限退回静态 + 告警。
- **调色板通道**：group1 **追加 binding 19**（`read-only-storage`，`array<mat4x4f>`，visibility vertex；现在 group1 = 1 uniform + 1 sampler + 17 sampled texture = 19 项，见 `gfx.cpp` 364–385 行，新增后 entryCount 20）。非蒙皮 draw 绑定 1×64B identity dummy buffer → 不改变旧行为。
- **管线键**：`p3dMatPipeline[10][2][3][2][2]` → 追加 `[2]` skin 维（或拆成 `[...,2][2]`）；`shadowPipeline[2]` → `[2][2]`。shader 入口：`vs_skin`/`vs1_skin`/`vs_shadow_skin`/`vs_shadow1_skin`（4 个新入口），fragment 入口全部复用。
- **draw/model**：蒙皮 draw 的 `model` = identity（规范）；`frame3d` 追加 `skins` 数组（`mq_renderer_skin{palette, joint_count}`）+ `draw3d` 追加 `skin` 索引（-1=无）。
- **阴影**：`vs_shadow_skin` 同样做 skin 变换（否则角色影子停在 bind pose）；阴影 pass 与主 pass 共用 group1 布局，binding 19 照绑。
- **ABI 版本**：全部为 `renderer.yaml` 结构尾部追加；`mq_renderer_v1` 版本保持 1（既有 append-only 规则），gfx 需容忍旧调用方传 `skin_vertices=nullptr`、`frame3d.skins==nullptr`。
- **morph（S3）用 CPU 路径**：4 个模型顶点数 3/24/30/1528，GPU morph 无收益；新增 ABI `update_mesh(self, mesh_id, vertices, vertex_bytes)`（gfx VBO 已带 `CopyDst`，见 `r_create_mesh` 1020–1025 行）+ runtime 保留 morph 网格的 CPU 顶点副本。morph + skin 组合在数据上天然兼容（先 CPU 改顶点，再 shader skin），但本批样本没有组合用例。

### 3.5 模块归属（runtime 内建 vs `plans/12` 模块）

| 方案 | 内容 | 优点 | 缺点 |
|---|---|---|---|
| **X（推荐）runtime-first 最小子集** | 采样/蒙皮数学放 `modules/runtime/src/runtime.cpp`（+ gfx/shader），schema 只加组件字段与 host API | 与迁移同批验收；不冻结新模块 ABI；复用现有 scene/flatten/anim_step/snapshot 机制；17–24 人日可交付 | 未来 `plans/12` 落地时采样核心需抽走（纯函数可平移；S1 渲染/ABI 部分本来就要做） |
| Y `plans/12` module-first | 先做 `modules/anim`（`mq_anim.h` ABI/.skel/.anim 文本/状态机/快照），迁移侧接 ABI | 一次到位、接口干净 | `plans/12` §5 是 M0–M6 长计划（状态机/快照/44 单测），会把迁移阻塞数周；且当前迁移只需要"单层骨骼 + 节点 TRS + morph 权重"子集 |

**推荐 X**。若 Lead 选择 Y：S1 的渲染/ABI/资产改动与本方案一致，S2 的 importer 输出改 `plans/12` 的 `.skel/.anim` 文本格式（ADR-0021），runtime 侧改为调用 `mq_anim` vtable；S2 工作量 +2–3 人日（接口对接/格式转换）。

### 3.6 备选方案与不采用理由

| 决策点 | 采用 | 备选 | 不采用理由 |
|---|---|---|---|
| skin 顶点存储 | 第二 vertex buffer（24B/顶点） | 扩 layout2/3 interleaved（48+24 / 56+24） | 会让 layout number 与 color/uv1 组合爆炸（4 种），并与既有 layout 校验/资产语义纠缠；第二 buffer 正交、新增 asset 少 |
| 调色板传递 | group1 binding 19 storage buffer | 塞进 per-draw uniform（Draw 288B→16KB max） | uniform 每 draw 重建（已知 P3），16KB×90 draw/帧浪费带宽；storage 上限 256 关节×64B=16KB 远低于限制 |
| 动画写回 | 派生姿态表（不写组件） | 写回 `Transform3D`（Euler） | quat→Euler 有损/万向锁；写回会与 `flatten_world` 2D 缓存的语义冲突；派生姿态与快照/回放更简单 |
| morph | CPU 重算 + `update_mesh` | GPU morph（targets storage + shader 加法） | 本批最大 1528 顶点；GPU 方案要再加 2 入口/2 binding/权重 uniform，收益为零 |
| 片段格式 | 复用 `asset.AnimationClip`（追加 Node/Morph 种类） | 新 `.anim.json` 自定义 glTF 版式 | 新格式要新解析器/新 asset kind；既有容器已有时间/关键帧/插值/缓存 + 确定性推进 |
| 场景层级 | 只对 26 个动画/骨骼模型启用 | 全部模型统一层级 | 会让 123 个静态模型输出漂移、批量预览/e2e 基线全部重录，收益为零 |

---

## 4. S1 · 骨架 / 蒙皮 / bind pose（里程碑 M-A1）

**目标**：7 个蒙皮模型以正确的 bind pose 渲染（规范语义：蒙皮网格世界变换忽略、`palette≈I`），且**动画未接入时视觉与"无蒙皮路径"等价**。**非目标**：不采样动画、不做 morph。

### 4.1 文件级改动清单

| # | 文件 | 改动 |
|---|---|---|
| 1 | `tools/gltf_import.py` | ① 读 `skins[]`：为每个 `(node with mesh, skin)` 生成一份 `.skel.json`（`asset.SkeletonData` 形状 + `joint_names`）；`inverse_bind` 从 accessor MAT4 读（列主序、6 位小数圆整）；`bones[].bind_*` 从 glTF node 本地 TRS 读（quat 保留 4 分量）。② 读 `JOINTS_0/WEIGHTS_0`（FLOAT / normalized U8/U16 三种，权重归一化；`JOINTS_0` 统一展开 u16）→ 追加到 `.mesh.json` 的 `joints`/`weights`。③ 命名映射：全场景唯一节点名（`node.name` 或 `node<idx>`，重名加 `#k`）；skin 文件和动画都引用该名字。④ `--scene` 增加**层级模式**（仅当模型有 `skins`/`animations`/morph targets 时自动启用，或 `--hierarchy` 显式）：DFS 输出 `children`；带 mesh 的 node 生成子实体（identity local）；蒙皮网格实体置场景根、identity、`skin_path`；蒙皮网格节点自身世界变换忽略并 stderr 说明。⑤ 无 skin/anim/morph 的模型保持旧平级输出（逐字节不变）。 |
| 2 | `modules/core/data/schema/render/render.yaml` | `render.MeshRenderer` 尾部追加 `skin_path: string`（默认 ""，doc：相对场景目录；空=非蒙皮）。 |
| 3 | `modules/core/data/schema/render/renderer.yaml` | ① `record mesh_gpu` 追加 `skin_vertices: "const void*"`、`skin_stride: uint32_t`、`skin_vertex_count: uint32_t`（24B/顶点，u16×4 + f32×4）。② `record draw3d` 追加 `skin: int32_t`（-1 默认；`frame3d.skins` 下标）。③ `record frame3d` 追加 `skins: "const mq_renderer_skin*"`、`skin_count: uint32_t`。④ 新 `record skin{ palette: "const float*", joint_count: uint32_t }`（palette = joint_count×16 列主序 mat4）。 |
| 4 | `modules/render/gfx/src/gfx.cpp` | ① `MeshGpu` 增加 `WGPUBuffer skinVbo`（`Vertex|CopyDst`）+ `bool skinned`；`r_create_mesh` 校验 `skin_stride==24`、创建/上传第二 VBO，旧调用 `skin_vertices=nullptr` 时保持 0。② `get_material_pipeline` 增加 `skinned` 参数：vertex `bufferCount=2`（第二 `WGPUVertexBufferLayout{arrayStride=24, attributes: loc5 Uint16x4@0, loc6 Float32x4@8}`），entry 在 `{vs,vs1} × {skin,no-skin}` 中选；缓存数组加 skin 维。③ `p3dDrawBgl` entryCount 19→20，binding 19 = `read-only-storage`（vertex 可见，`minBindingSize=64`）；`fill_material_bind_entries` 增加第 20 项（palette buffer 或 device 级 1-mat4 dummy）。④ 主 pass：`frame3d.skins[draw.skin]` → 每帧创建 storage buffer + 写入 + 绑 binding 19（与现有 per-draw uniform 同生命周期，释放）；非蒙皮 draw 绑 dummy。⑤ 阴影 pass：`shadowPipeline[2][2]`、`vs_shadow_skin`/`vs_shadow1_skin`、第二 vertex buffer、binding 19 同绑。⑥ 设备创建时若 adapter 限制不足（`maxVertexBuffers<2`/`maxStorageBuffersPerShaderStage<2`）打印原因并禁用 skin 路径（回退静态）。 |
| 5 | `assets/builtin/shaders/pbr/00_types.wgsl` / `40_vertex.wgsl` | ① `@group(1) @binding(19) var<storage, read> skin_palette: array<mat4x4f>;`。② 新增 `vs_skin` / `vs1_skin`：读 loc5/6，`skin = w0*P[j0]+w1*P[j1]+w2*P[j2]+w3*P[j3]`；`pos = (skin*vec4(pos,1)).xyz`；法线用 `mat3x3(skin)`（非均匀缩放下近似，记录为已知限制，本批无缩放关节）。③ 新增 `vs_shadow_skin`/`vs_shadow1_skin`。④ 复用 `vs_common`（只把 model 矩阵传入）。 |
| 6 | `modules/runtime/src/runtime.cpp` | ① `load_mesh` 支持追加字段（`skin_vertices`/`skin_stride`），缓存结构加 `bool skinned`。② 新 `load_skeleton(path)`：解析 `.skel.json`（反射反序列化 `asset.SkeletonData`）→ 缓存 `{joint_names, ibm[16]}`。③ `setup_3d` 阶段扫描 `MeshRenderer.skin_path`：解析 skin、按名字在场景内解析关节实体 id（`node_find_path`/遍历子树；名字唯一）→ 建 `SkinInstance{joint_ids, ibm}`，失败告警并回退静态。④ 新 `flatten_world3d(Engine&)`：确定性遍历（同 `flatten_world` 顺序）把每实体 `Transform3D` 累积成 mat4，存 `Scene::world3d`（`std::vector<float>`，实体数×16）；`setup_3d`/每帧渲染前调用一次。⑤ `extract_3d`：`dr.model` 改用 `world3d`（平级场景 world=local，零漂移）；蒙皮 draw 的 model=identity，`dr.skin=idx`，组 `frame3d.skins`（palette = world(joint)×IBM；S1 阶段即 bind 姿态）。⑥ 新增环境开关 `MAQUE_SKIN=0`（强制不绑 skin VBO/palette，走旧路径）用于 e2e 对照与回滚。 |
| 7 | `tests/gltf_import_e2e.sh` + `tests/data/gltf/` | 新增 fixture `tests/data/gltf/skinned.gltf`（约 10 顶点、2 关节、1 skin，内嵌 buffer）：断言 `.skel.json` 确定性（两次逐字节一致）、`joint_names` 与场景节点一一对应、`bones.len == ibm.len`、weights 每行 sum≈1、蒙皮网格实体 `Transform3D` identity。 |
| 8 | `tests/gltf_skin_e2e.sh`（新） | 流程：importer 把 fixture 导入 `build*` 临时目录 → player `--shot`；断言 ① 退出码 0、日志无 `mesh=-1`；② 图像非背景像素占比 > 阈值；③ `MAQUE_SKIN=0` 与默认路径图像**逐像素一致**（bind pose 等价，证明调色板≈I 且不改变观感）；④ 运行两次同一帧 PNG 哈希一致（确定性）。 |
| 9 | `samples/player/CMakeLists.txt` | 注册 `gltf_skin_e2e`（照 135–139 行式样，`SKIP_RETURN_CODE 77`）。 |
| 10 | `tools/migrate_gltf_models.py` | 7 个蒙皮模型重跑（`--only <Model> --force`）后更新 preview/compare/README/manifest；README 的 gap 从"骨骼蒙皮未应用"改为"S1 已应用 bind pose，动画未播放"（S2 后删除）。 |

### 4.2 不变量（必须写进测试）

1. `palette[j] = world(joint_j) × IBM_j`；bind 姿态下 `max|palette[j] − I| < 1e-4`（浮点容差按 6 位小数资产定）。
2. 蒙皮网格实体世界变换 = identity（忽略 glTF node 变换）。
3. 每顶点权重和 ∈ [0.999, 1.001]；`JOINTS_0` 全部 < joint_count。
4. 旧资产（`skin_vertices` 缺失 / `frame3d.skins` 缺失）行为逐字节不变。

### 4.3 验收（可断言、CI 可跑）

- `ctest -R 'gltf_import_e2e|gltf_skin_e2e'` 全绿；
- 对 7 个模型批量：`--shot` 出图成功、`MAQUE_RENDER3D_INFO` 中每个蒙皮 mesh 均有 `skin=N` 且无 `mesh=-1`；
- bind pose 不变量用 fixture 单测（解析 + 矩阵断言）；
- 与 `MAQUE_SKIN=0` 对照图逐像素一致（fixture；真实模型允许 camera-fit 差异，改为结构断言 + preview 人工复核）。

### 4.4 工作量与风险

| 工作包 | 人日 |
|---|---|
| importer：skin/层级/命名/确定性输出 | 2.0 |
| schema + ABI + gfx 第二 VBO/管线/绑定/阴影 | 2.5 |
| shader 4 个新 entry | 0.5 |
| runtime：load_skeleton/解析骨骼/flatten_world3d/palette/extract | 2.0 |
| 测试（fixture + 2 个 e2e 脚本 + CMake） | 1.5 |
| 7 模型批量重跑 + preview 复核 + 文档 | 0.5 |
| **合计** | **7–10** |

风险：ABI 结构追加导致所有渲染插件需重编（同仓同构建，可接受）；新 shader entry 若 WGSL 校验失败会整批不渲染（用 fixture 先验证 + 失败告警不静默，`gfx.cpp` 现有 `p3dMatWarned` 模式）；Euler/分解精度（骨骼 bind 用 `.skel.json` 的 quat，不经组件，规避）；84 skin 命名/解析（用全场景唯一名 + 失败回退静态）。**回滚**：`MAQUE_SKIN=0` 运行期回退；资产侧 7 个模型可 `--only` 重跑回旧输出。

---

## 5. S2 · 关键帧采样 + 节点/骨骼动画（里程碑 M-A2，依赖 S1）

**目标**：26 个动画模型（除 4 个纯 pointer）按片段播放，固定 dt、可回放；节点 TRS 动画与骨骼动画共用一条采样通路。

### 5.1 文件级改动清单

| # | 文件 | 改动 |
|---|---|---|
| 1 | `tools/gltf_import.py` | ① 读 `animations[]`：每片段生成一份 `.anim.json`（`asset.AnimationClip` 形状）；通道 → `TrackKind::Node`（`node_name` = 唯一节点名；`interpolation`：LINEAR/STEP，CUBICSPLINE 记 `2` 但 runtime 按 LINEAR 近似并 stderr 计数）；quat 通道 `value_type=Quat`，translation/scale `value_type=Vec3/Float`；`Key` 只写 `time/value`（切线不写=0）；② 片段名清洗（非法字符→`_`，重名加后缀）；时长 = 所有 sampler input 的 max；③ pointer 通道**跳过并计数**（`skipped_pointer_channels`），写 stderr 汇总；④ `--scene`：有动画时给根/角色实体挂 `scn.AnimationPlayer`（`clips` = 片段路径数组、`clip_index=0`、`playing=true`、`loop=Loop`、`speed=1`、`time=0`；旧 `clip` 留空）；⑤ 确定性：片段按名字排序、轨道按 (node 名, 路径顺序 translation<rotation<scale<weights, 原通道序) 排序、6 位小数圆整。 |
| 2 | `modules/core/data/schema/asset.yaml` | `TrackKind` 枚举追加 `Node = 2`、`Morph = 3`；`Track` 追加 `node_name: string`（默认 ""）。`Key`/`AnimationClip` 不动。 |
| 3 | `modules/core/data/schema/world/scn.yaml` | `scn.AnimationPlayer` 追加 `clips: "string[]"`、`clip_index: int32`（默认 0）；保留 `clip` 字段兼容 2D。 |
| 4 | `modules/runtime/src/runtime.cpp` | ① 扩展 `anim_step(dt)`：`clips` 非空时走**glTF 通路**（见 ②–⑤），否则走旧属性轨道通路（零改动）。② 片段缓存 `load_clip` 支持 `TrackKind::Node/Morph`（解析 `node_name`；不要求 `field_path`）；③ 每步推进 `time`（固定 dt、loop/clamp，确定性；与现有一致）；④ 采样：`STEP` 取前一关键帧；`LINEAR`：vec3 lerp、quat slerp（先按 dot 符号翻转再 slerp，处理 w<0/相邻符号跳变）；越界 clamp；⑤ 结果写入本帧派生姿态表（`std::vector<LocalPose>`，按实体 id 索引；有轨道用采样值，无轨道用组件 TRS）；⑥ `flatten_world3d`（S1 已建）基于派生姿态计算世界矩阵；`extract_3d` 用世界矩阵；蒙皮 `palette` 用世界矩阵×IBM；⑦ `time` 写回组件（进快照；`duration` transient 写回）；⑧ 采样失败（轨道名解析不到）告警一次并跳过该轨道（不整段失败）。 |
| 5 | `modules/core/data/schema/runtime/host.yaml` | 新增 `anim_*` 脚本函数（照 `spine_*` 约定，167–190 行式样）：`anim_clip_count/anim_clip_name/anim_clip_index/anim_play(name|index,loop)/anim_current_clip/anim_time/anim_set_time/anim_speed/anim_set_speed/anim_playing/anim_set_playing/anim_duration/animation_joint_count/animation_joint_name`。实现放 `runtime.cpp` 现有 spine 函数附近（6720 行起）。 |
| 6 | `modules/core/data/schema/control.yaml`（P2，可选） | `Anim.info`（只读：clip 数/当前 clip/time/playing）——验收不依赖，S2 可暂缓。 |
| 7 | `tests/data/gltf/` + `tests/gltf_anim_e2e.sh`（新） | fixture `animated.gltf`（2 节点：父旋转 + 子平移，1 片段 1 s，LINEAR + 1 条 STEP）+ `skinned.gltf` 扩展一个 2 关节片段；e2e：importer → build 目录 scene + `.js` 脚本（`schedule_once` 设 `anim_set_time`）→ `--shot-seq` 帧 1/30/60；断言 ① 三次出图哈希不同（确实动了）；② 同一帧跑两次哈希一致（确定性）；③ `anim_time` 与预期步数一致；④ 越界/loop 边界不崩。 |
| 8 | `samples/player/CMakeLists.txt` | 注册 `gltf_anim_e2e`。 |
| 9 | `tools/migrate_gltf_models.py` | 22 个可动模型重跑 + preview 对参考图更新；4 个纯 pointer 模型 README 标 gap（`KHR_animation_pointer` 未支持）；`manifest` 增 `animation` 字段（片段数/时长/skin 数）。 |

### 5.2 采样语义（写进实现注释与测试）

- 关键帧定位：`while (k+1 < n && keys[k+1].time <= t) ++k`（与旧 `anim_step` 一致）；`alpha` clamp [0,1]；`STEP` 用 `keys[k]`；`LINEAR` 用 `alpha`。
- quat：`if (dot < 0) q1 = -q1` 后再 slerp；`dot>0.9995` 走 nlerp 兜底（确定性：先归一化）。**不做** cubic（近似为 linear，仅 InterpolationTest 3 通道，README 记 gap）。
- 时间/循环：`loop=Loop` → `while (t >= length) t -= length`；否则 clamp 到 length；`length==0` 不动。
- 顺序：实体 id 升序 → 组件按类型 id 升序（沿用现有遍历顺序）→ 轨道声明序；结果与遍历顺序无关（每轨道独立写派生姿态表，最后再统一算世界矩阵）。
- 快照：只依赖组件字段（`time`/`playing`/`speed`/`clip_index`），采样为纯函数 → 回放一致；无需新快照段。

### 5.3 验收（可断言）

- fixture e2e（上述 4 条）+ 人工抽帧：Fox/BrainStem/CesiumMan 三个模型第 1/30/60 帧像素差 > 阈值（动起来），同帧两次一致。
- 22 个模型批量出图无 `mesh=-1`、无 shader/管线告警；RecursiveSkeletons（84 skin）场景加载成功、draw 数 = 84。
- `ctest -R 'gltf_anim_e2e|gltf_import_e2e|determinism'` 全绿（determinism 用现有 2D 场景，不应受影响）。
- `anim_set_time` 到任意 t 后截图与直接推进到该 t 一致（纯函数性抽查 1 例）。

### 5.4 工作量与风险

| 工作包 | 人日 |
|---|---|
| importer：动画导出 + pointer 跳过 + 场景播放器组件 + 确定性排序 | 2.0 |
| schema（TrackKind/node_name/AnimationPlayer.clips）+ host API 声明/实现 | 1.0 |
| runtime：采样（quat/STEP/LINEAR/loop）、派生姿态、world3d 合并、快照语义 | 2.0 |
| 测试（fixture + e2e + CMake） | 1.5 |
| 22 模型批量重跑 + preview/README + 人工抽帧复核 | 1.0 |
| **合计** | **5–9** |

风险：quat 符号路径/插值漂移（先翻符号再 slerp + 固定顺序，单测覆盖）；层级世界矩阵顺序错（根/子 id 升序与 `flatten_world` 一致）；稀疏/非均匀时间轴（二分定位）；轨道名解析失败静默（告警一次）；CUBICSPLINE 近似需在 README 标注（不宣称支持）。**回滚**：`scn.AnimationPlayer.clips` 清空即回旧属性轨道/静态；资产侧 `--only` 重跑。

---

## 6. S3 · morph 变形目标（里程碑 M-A3，依赖 S2，可与 S2 并行做 importer 部分）

**目标**：4 个 morph 模型应用默认权重 + 播放 weights 动画；可脚本设置权重。

### 6.1 文件级改动清单

| # | 文件 | 改动 |
|---|---|---|
| 1 | `tools/gltf_import.py` | 读 `meshes[].primitives[].targets`：每 target 读 `POSITION`（必需）/`NORMAL`（有则存）增量 → 追加 `.mesh.json` `"morph": {"targets":[{"positions":[...],"normals":[...]}], "default_weights":[...]}`；targets 顺序 = glTF 顺序；TANGENT 增量忽略并 stderr 计数（shader 不用 tangent）。 |
| 2 | `modules/runtime/src/runtime.cpp` | ① `load_mesh` 解析 `morph`，保留 CPU 顶点副本 + targets/default weights（morph 网格缓存结构独立）；② 新 `apply_morph(entity)`：权重 = 播放器轨道（`TrackKind::Morph`，`node_name`+`bone_index`=权重下标）或 mesh 默认权重；重建顶点（`pos + Σ w_i·Δp_i`，`normal + Σ w_i·Δn_i` 后归一化），仅当权重变化时调用 `update_mesh`；③ `anim_step` 采样 Morph 轨道；④ host 函数 `anim_morph_count/anim_morph_default_weight/anim_morph_weight/anim_set_morph_weight`；⑤ 上限：single mesh targets ≤ 8（本批最大 8；超出跳过 + 告警）。 |
| 3 | `modules/core/data/schema/render/renderer.yaml` | 追加 `fn update_mesh`（args `mesh: int32_t, vertices: "const void*", vertex_bytes: uint64_t`）+ `record` 无需新增（复用 mesh_gpu 语义）；版本保持 1。 |
| 4 | `modules/render/gfx/src/gfx.cpp` | `r_update_mesh`：校验 mesh 存在/大小一致 → `wgpuQueueWriteBuffer(vbo)`（VBO 已 `CopyDst`）；索引/stride 不变。 |
| 5 | `modules/core/data/schema/runtime/host.yaml` | 上述 `anim_morph_*` 声明（S2 的 `anim_*` 附近）。 |
| 6 | `tests/gltf_morph_e2e.sh`（新）+ fixture `tests/data/gltf/morph.gltf` | e2e：① 默认权重非 0 的 fixture → 出图与"权重 0"图不同；② 动画驱动权重（`weights` 通道）→ 帧 1/30 哈希不同、同帧两次一致；③ 脚本 `anim_set_morph_weight` 后出图与对应时间点一致。 |
| 7 | `samples/player/CMakeLists.txt` | 注册 `gltf_morph_e2e`。 |
| 8 | `tools/migrate_gltf_models.py` + README | 4 个模型重跑；`MorphPrimitivesTest`/`SimpleMorph` 的默认权重静态观感修正后更新 preview；README 删除"morph 默认权重未应用"。 |

### 6.2 验收

- fixture 3 条 e2e；4 模型批量：`AnimatedMorphCube` 动起来、`SimpleMorph`/`MorphPrimitivesTest` 静态观感与参考更一致、`MorphStressTest` 8 目标无越界/崩溃；
- morph 顶点数据不变量：targets 顶点数 == 基础顶点数；默认权重长度 == target 数；
- 非 morph 模型逐字节不变（`.mesh.json` 无 `morph` 字段）。

### 6.3 工作量与风险

| 工作包 | 人日 |
|---|---|
| importer morph 导出 | 1.0 |
| runtime CPU morph + `update_mesh` + 权重采样 | 1.5 |
| host API + schema + gfx | 0.5 |
| 测试 + 4 模型重跑 | 1.0 |
| **合计** | **3–5** |

风险：每帧顶点上传（1528 顶点×2 prim ≈ 24KB/帧，可忽略，但仅权重变化时上传）；`update_mesh` 与 `create_mesh` 生命周期（mesh 被多实体共享时权重冲突——本批 morph mesh 均唯一，代码里对共享 morph mesh 直接告警并只允许一个实例）；TANGENT 增量忽略（记 gap）。

---

## 7. S4 · 混合 / 状态机（不做，仅设计边界）

- 本轮**不排期**。若做，建议在 S2 采样层之上加"最多 2 片段交叉淡入"（`clip_a/clip_b/weight` + 权重归一化），不引入 `plans/12` 的完整状态机（entry/Any State/trigger 复位）。
- 迁移验收不需要；`plans/12` 的 FSM 设计留在其模块落地时实现（其 §5 阶段 4）。

## 8. A · 总工作量、里程碑拆分与依赖

| 里程碑 | 内容 | 人日 | 依赖 | 交付物 |
|---|---|---|---|---|
| **M-A1** | S1 骨架/蒙皮/bind pose + 3D 层级世界矩阵 + ABI/gfx/shader | 7–10 | 无 | 7 个蒙皮模型规范 bind pose；`gltf_skin_e2e` 绿；ABI v1 追加 |
| **M-A2** | S2 关键帧采样 + 节点/骨骼动画 + 脚本 API | 5–9 | M-A1 | 22/26 模型可播放；`gltf_anim_e2e` 绿；4 个 pointer 模型标 gap |
| **M-A3** | S3 morph 默认权重 + weights 动画 | 3–5 | M-A2（importer 可与 M-A1 并行） | 4 个 morph 模型；`gltf_morph_e2e` 绿 |
| **M-A4（可选）** | S4 交叉淡入；pointer 材质动画；CUBICSPLINE 精确实现 | 5–8 | M-A3 | 按需 |
| 合计 | S1+S2+S3 | **17–24**（乐观 15 / 保守 26） | | |

排期建议：M-A1→M-A2→M-A3 串行（每阶段都批量重跑对应模型并更新 preview/README；S1 的 7 模型重跑会改变 4 个模型的静态观感，需人工复核确认更接近参考图）。批量重跑命令：`python -u tools/migrate_gltf_models.py --only Fox --force`（逐模型），全部完成后 `python -u tools/migrate_gltf_models.py --force` 复核 148 个。

## 9. A · 明确不做（Out of Scope）及原因

| 不做项 | 原因 |
|---|---|
| `plans/12` 完整模块（`mq_anim.h` ABI、状态机、快照插值、44 单测） | 迁移只需子集；完整计划会阻塞数周；S2 采样核心后续可平移进模块 |
| 动画状态机/混合树/过渡条件（S4） | 批量验收不需要；每模型自动播放即可展示；留给 `plans/12` |
| `KHR_animation_pointer`（4 纯 pointer + 1 混合；106 材质 + 2 可见性） | 材质动画需要"每帧材质/贴图变换→uniform 更新"通路；迁移静态截图无收益；P3 后续（结构上 extract 每帧重建材质，可行） |
| 把 `Transform3D.rotation` 改成四元数 / 引入 `anim.JointPose` 组件 | 全局改动风险大（2D/3D 手写场景、脚本语义）；派生姿态已满足渲染 |
| 双四元数蒙皮、IK/约束、动画事件回调、root motion 烘焙、动画压缩 | 样本集无需求；`plans/12` 视为后续能力 |
| GPU morph / 蒙皮网格的 GPU instancing | 本批顶点数小；性能不是瓶颈；GPU instancing 是 `docs/31 §11` 独立 P3 项 |
| CUBICSPLINE 精确切线（quat cubic） | 仅 InterpolationTest 3 通道；LINEAR 近似 + README gap 足够 |
| TANGENT morph 增量 | shader 用屏幕导数构造切线基，不用 TANGENT 属性 |
| 非三角形 primitive（点/线）动画、`KHR_node_visibility` 动画 | 样本集无点线；可见性只有 2 条 pointer 通道 |

---

# B · KTX2/BasisU + meshopt/Draco 解码

## 10. B · 现状

### 10.1 importer 当前如何处理

`tools/gltf_import.py` 的贴图通路（342–407 行）：
- `sniff_image` 按 magic 识别 `png/jpeg/gif/webp/ktx2`（以及其它则 "unsupported"）。
- PNG/JPEG：原样搬运（不解码）。
- GIF/WebP：用 **PIL** 转 PNG（`transcode_to_png`，403 行；PIL 缺失时打印"跳过 … 贴图将缺失"）。WebP 是主变体唯一依赖 PIL 编码转码的场景（SheenWoodLeatherSofa 13 张，实测已生成 13 个 PNG）。
- **KTX2 / BasisU：无解码路径** → 落到 405 行的通用跳过告警（`跳过不支持的贴图 image=… (magic=ktx2, mimeType=…)`），该材质贴图槽为空（`-1` → 运行时绑 1×1 占位）。
- **Draco / meshopt：无解码路径**。`read_buffers` 只拼接普通 buffer；`bufferViews` 的 `extensions.EXT_meshopt_compression` / primitive 的 `extensions.KHR_draco_mesh_compression` 都被忽略——若主变体只有压缩数据则会读不到/读错（当前主变体均无此情况）。
- **量化**：`accessor_norm_scale` 只用于 `COLOR_0`/`TEXCOORD_n`（950–967 行），`POSITION/NORMAL` 不做归一化缩放——主变体唯一量化模型 `POSITION` 是 float，故未暴露；若出现量化 POSITION，当前会读成整数（错误），属潜在 bug（非本轮范围，B 的 P1 可记录）。

### 10.2 运行时贴图能力

- `modules/image/src/image.c` 只支持 Wuffs 的 **PNG/JPEG/GIF/BMP**（其它格式返回 "unsupported image format"）。
- `render.texture_desc`（`renderer.yaml` 94–105 行）`pixel_format`：0=RGBA8、1=RGBA8_SRGB、2=RGBA16F（3=BC7 预留）；`kind` 0=2D/1=cube/2=equirect；`mip_levels` 由数据给出（runtime 对 PBR 贴图做 CPU mip 链）。
- 结论：**运行时不能上传 KTX2/BasisU 容器，也没有压缩纹理格式（BC7/ETC2/ASTC）通路**；即使解码 BasisU，输出也只能是 RGBA8（会失去压缩纹理的显存/带宽收益），因此"引擎内解码"唯一收益是"能加载压缩变体"，而非性能。

### 10.3 统计（主变体 vs 全变体）

| 项目 | 主变体（148 模型） | 全变体（337 文件/149 模型目录） |
|---|---|---|
| 贴图 image 条目 | **705**（PNG 455 / JPEG 237 / WebP 13；不含 ToyCar 为 697 = PNG 447 / JPEG 237 / WebP 13），**KTX2 0、Draco 0** | **1560**（PNG 907 / JPEG 456 / WebP 49 / KTX2 107 / data-uri 41） |
| KTX2 | 0 模型 / 0 贴图 | 7 个变体目录 / 6 个模型（107 条 image 条目）；其中 **5 个目录共 60 个独立 `.ktx2` 文件（45.21 MB）**，另 2 个 GLB 变体把 47 张 KTX2 内嵌在 GLB 中（ABeautifulGame 33 + CarConcept 14）；独立文件为 **24 个 BasisLZ(ETC1S, supercompression=1) + 36 个 UASTC+zstd(scheme=2)**；header `vkFormat` 全为 **0**（BasisU 压缩位流，必须转码） |
| Draco | 0（required 的模型 0） | 18 个变体目录 / 18 模型（`glTF-Draco` 等） |
| meshopt | 1 模型（MeshoptCubeTest，fallback 未压缩，可导入） | 6 个变体目录 / 3 模型；**真正压缩的只有 2 模型**（BrainStem、DragonAttenuation 的 `glTF-Meshopt-EXT`） |
| `KHR_mesh_quantization` | 1 模型（同上） | 10 个变体目录 / 7 模型 |
| WebP | 1 模型（SheenWoodLeatherSofa，PIL 转 PNG） | 5 个变体目录 / 4 模型 |
| 迁移阻塞 | **0**（`manifest.json` 148 条全部 `ok`） | 不参与迁移（只取主变体） |

KTX2 独立文件清单（目录/文件数/体积）：AnisotropyBarnLamp `glTF-KTX-BasisU` 4（4.59 MB）、CarConcept `glTF-KTX-BasisU-Draco` 14（1.35 MB）、ChronographWatch `glTF-KTX-BasisU` 8（1.39 MB）、FlightHelmet `glTF-KTX-BasisU` 15（29.30 MB）、StainedGlassLamp `glTF-KTX-BasisU` 19（8.60 MB）；另有 ABeautifulGame `glTF-Binary-KTX-ETC1S-Draco` 33 张与 CarConcept `GLB` 14 张以 bufferView 内嵌在 GLB 中（无独立文件，但计入 image 条目）。**全部是可选变体，主变体用 PNG/JPEG/WebP。**

### 10.4 为什么主变体不受阻（证据）

- 148 个模型主变体 `extensionsRequired` 中无 `KHR_draco_mesh_compression`；主变体 image 无 KTX2（0 条）。
- `MeshoptCubeTest`：主变体 `extensionsRequired=['KHR_mesh_quantization']`（+旧名 `KHR_meshopt_compression` 但 bufferView 无压缩扩展），importer 读 fallback buffer 成功；实测 35 个 `.mesh.json` 全部有合法顶点/bounds（±0.5），manifest `ok`。其余 meshopt 变体的压缩数据在 `glTF-Meshopt-EXT`，未被选中。
- 因此 **B 不阻塞迁移**（`docs/31 §11` 的"仅当只有压缩变体时 blocked"当前为 0 例）。

---

## 11. B · 方案对比与推荐

### 11.1 候选方案

| 方案 | 做法 | 覆盖范围 |
|---|---|---|
| **C（推荐）跳过 + 用主变体** | 维持现状；主变体 148/148 可导入 → 无 blocked；只补告警/文档 | 迁移 100%（压缩变体不迁移，属可选重复版本） |
| **A 离线转码（importer 侧）** | 在 `gltf_import.py` 调外部工具把 `.ktx2` → PNG（KTX-Software `ktx extract` / basisu `-unpack`）再走现有 PNG 通路；Derived `textures/` 不入库 | 6 个 KTX2 模型的压缩变体（若未来必须迁移） |
| **B 引擎内 vendored 解码器** | 引入 basisu transcoder + zstd（UASTC）+ KTX2 容器解析；meshopt 单头文件；Draco C++；解码后 RGBA8 上传 | 理论全变体；运行时能力 |
| **D 换主变体/跳过并记录** | 与 C 相同的操作，但 manifest 明确记录"存在压缩变体、已选主变体"，并把压缩变体从迁移范围排除 | 同 C |

### 11.2 对比表

| 维度 | C（跳过） | A（离线转码） | B（vendored 解码） |
|---|---|---|---|
| 新增依赖 | 无 | 外部 CLI（Windows 二进制，未验证体积/参数） | basisu + zstd + meshoptimizer + draco（源码 vendoring），构建时间/二进制体积↑ |
| 仓库原则"不新增重依赖" | ✅ | ⚠️ 工具链依赖（仅迁移环境，CI 可不装） | ❌ 违反 |
| Windows clang-cl | ✅ | CLI 无编译问题（未验证） | ⚠️ basisu/draco 的 clang-cl 兼容性未验证；CMake 集成增加维护面 |
| 迁移收益 | 完成（0 blocked） | 仅覆盖 6 模型的可选压缩变体 | 覆盖全变体（但迁移只需要主变体） |
| 运行时收益 | 无 | 无（产物仍是 PNG） | 若解码到 RGBA8：无显存/带宽收益；若走压缩纹理：还需 WebGPU 压缩格式 feature（未验证） |
| 确定性与 CI | 不影响 | 转码产物确定性需验证；CI 不装 CLI 则跳过 | 解码器确定、但增加 CI 构建矩阵 |
| 工作量 | 0.25–0.5（P0 文档/告警） | 1.5–2.5 | 5–10 |
| 风险 | 低 | 中（工具可用性/参数/版本漂移） | 高（依赖+构建+GPU feature） |
| 结论 | **推荐** | 未来可选（若必须压缩变体） | **不推荐** |

### 11.3 推荐与理由

1. **本轮与迁移目标：选 C/D**——主变体 148/148 成功，压缩变体只是同一资产的重复版本；`docs/31 §11` 已写明"当前用 `glTF/` 主变体规避"。为"可选变体的重复资产"引入第三方解码器不符合仓库"不新增重依赖"原则。
2. **若未来必须迁移压缩变体（例如上游只提供 Draco/KTX2）：选 A（importer 侧离线转码）**——解码只发生在离线导入器，运行时/ABI/CI 不变；产物进 derived `textures/`（不入库，符合"派生资产不入库"）；转码失败按现有告警降级（贴图缺失，不中断模型）。
3. **不选 B（运行时解码）**：唯一收益是"能读压缩容器"，但输出仍是 RGBA8/或需要压缩纹理 GPU 通路；对 148 个主变体零收益；draco/basisu 的 Windows clang-cl 构建风险高、维护成本大。meshopt 例外：它只是 bufferView 解压，单头文件成本低，但本批真正需要它的只有 2 个可选模型，且迁移可换主变体，同样不值得。
4. **KHR_mesh_quantization**：不需要专门实现；若未来主变体出现量化 POSITION，修 `accessor_norm_scale` 的适用范围即可（P1，0.5 人日，见 §12）。

### 11.4 若走 A 的落地位置与构建集成（未来，不在本轮）

| 文件 | 改动 |
|---|---|
| `tools/gltf_import.py` | `sniff_image` 的 `ktx2` 分支 → 调 `--ktx-tool`（默认 `ktx` 或 `basisu`，可用环境变量覆盖）；失败回退现有"跳过+告警"；转码输出到 `textures/<name>.png`；stderr 汇总；`--ktx-tool=none` 保持现状（默认值，零行为变化） |
| `tools/gltf_import.py` CLI | 新增 `--ktx-tool <path>` / `--ktx-args`（可选）；文档 `docs/31 §1.2` 前置依赖表补一行"仅压缩变体需要" |
| `tools/migrate_gltf_models.py` | 选源规则保持主变体优先；若主变体是 KTX2-only 才需要工具；`manifest` 记录 `ktx2_transcoded` 数量 |
| 构建/CI | 不引入构建依赖；CI 不装 CLI 时压缩变体测试 skip（`SKIP_RETURN_CODE 77`） |
| 确定性 | 转码工具版本写进 README/日志（产物不入库，只影响 preview）；PNG 输出由工具决定，重复运行同版本应一致（未验证） |
| 工作量/风险 | 1.5–2.5 人日；风险：工具在 Windows 的可用性/参数（尤其 `ktx extract` 对 BasisU 的行为、`basisu -unpack` 输出命名）、版本漂移、45 MB 解压耗时（可接受） |

### 11.5 若走 B 的落地位置与代价（不推荐）

- `third_party/basisu`（transcoder + KTX2 容器 + zstd）、`third_party/meshoptimizer`（单头）、`third_party/draco`（重）；CMake 静态库 + clang-cl 兼容性验证。
- `tools/gltf_import.py` 或 runtime 增加解码：import 期解码可放 Python（需 C++ 桥/子进程，实际只能走 CLI）→ 等价于方案 A；**运行时解码**则要改 `image` 模块/`load_texture` 通路与 `texture_desc`（压缩格式），ABI 扩张、GPU feature 未验证。
- 代价：5–10 人日 + 长期维护 + 构建时间；收益 0（主变体已覆盖）。

## 12. B · 本轮不做 + 可立即执行的 P0/P1

**本轮明确不做 B 的任何实现**（不加解码器、不改 importer 贴图行为、不动 ABI/构建）。以下小改动可立即执行（不依赖动画大项，也不改变主变体 148/148 的成功率）：

| 优先级 | 改动 | 落点 | 验收 | 工时 |
|---|---|---|---|---|
| **P0-1** | KTX2 告警"可操作化"：把 `跳过不支持的贴图 (magic=ktx2 …)` 改为明确说明"主变体已避开 KTX2；如需该压缩变体需离线转码（KTX-Software/basisu）或改用 `glTF/` 主变体；本模型该贴图槽将缺失" | `tools/gltf_import.py` 405 行附近（`sniff_image` 结果分发处） | 构造 KTX2 fixture 跑 importer，stderr 含关键说明；现有 e2e 不受影响 | 0.25 |
| **P0-2** | 迁移范围透明化：`manifest.records[]`/逐模型 README 增加 `compressed_variants: ["glTF-KTX-BasisU", ...]` 与 `note: "主变体已避开压缩扩展"`；4 个纯 pointer 模型与 7 个骨骼模型 README 保持"静态迁移"标注（S2 后更新） | `tools/migrate_gltf_models.py` 的 manifest/README 生成段；`scenes/khronos/<M>/README.md` | 重跑 `--only <6 个 KTX2 模型>` 后 manifest 含字段；README 有明确备注 | 0.25 |
| **P0-3** | `docs/31` 补一段"压缩扩展与主变体"口径（主变体 0 blocked；全变体 KTX2/Draco 仅可选变体；meshopt fallback；量化潜在 bug 记录） | `docs/31-gltf-model-migration.md` §5.3/§11 附近 | 文档审阅 | 0.25 |
| **P1-1**（可选） | importer `--image-report`：模型导入结束时 stderr 汇总"贴图共 N，其中 KTX2 跳过 k1、WebP 转码 k2、缺失 k3"，migrate driver 把 `k1` 写进 manifest（便于审计） | `tools/gltf_import.py` + `tools/migrate_gltf_models.py` | KTX2 fixture 输出汇总行；e2e 断言存在 | 0.5 |
| **P1-2**（可选） | `accessor_norm_scale` 扩展到 `POSITION/NORMAL/TANGENT`（仅当 accessor 有 `normalized` 且 componentType 为整数时缩放）；避免未来量化主变体几何错误 | `tools/gltf_import.py` 950–975 行附近 | 加 1 个量化 fixture 断言顶点值范围正确；现有主变体输出不变 | 0.5 |
| **P1-3**（可选，未来） | 若 Lead 决定支持压缩变体：`tools/ktx2_to_png.py`（封装外部 CLI，`--tool` 参数、失败降级）+ importer `--ktx2` 开关；默认关 → 零行为变化 | 新工具 + importer 分支 + docs | 压缩变体 KTX2 fixture 转码成功；CI 无工具时 skip | 1.5–2.5 |

> 说明：以上 P0 均不改变 148/148 主变体成功率和现有 e2e；P1-2 是"防御未来量化主变体"的独立小修，不建议本轮顺手改（会碰 accessor 逻辑，需要单独验证）。

---

## 13. 给 Lead 的 3 条建议

1. **先把 A 的 S1+S2+S3 作为一条独立大项排期（约 17–24 人日），不要和 B 混排。** 理由：B 当前 0 阻塞（主变体 148/148，KTX2/Draco 只在可选变体；meshopt 走 fallback），而 A 是 26 个模型/38 个片段的用户可见差距（角色 T-pose、morph 默认权重不生效），且 S1 的 3D 层级世界矩阵/蒙皮管线是后续所有动画能力的地基。B 只保留 0.5 人日的 P0 文档/告警收尾。
2. **S1 先做、且先只做 7 个蒙皮模型 + fixture e2e（7–10 人日）；S1 合入并批量复核 bind pose 后再开 S2。** 理由：S1 会引入 ABI 追加、4 个新 shader entry、调色板 storage buffer 与 `flatten_world3d`，风险集中在"管线/ABI 一次性改动"；S2 依赖 S1 的世界矩阵与层级场景，且 S2 的 22 模型批量重跑成本高——先让 S1 的 e2e 证明 `palette≈I`、bind pose 与无蒙皮路径逐像素一致，再铺开动画，可以把"渲染错"和"采样错"两类问题分开定位。S3 的 importer 部分可在 S1 期间并行（顶点数小、无 ABI 风险）。
3. **里程碑拆分按"每阶段都批量重跑 + 更新 preview/README"来验收，而不是只跑单测。** 理由：本项目已有 `migrate_gltf_models.py --only/--force` 与 `manifest.json` 状态机，S1 会改变 BrainStem/CesiumMan/RiggedFigure/RiggedSimple 的静态观感（修掉错误的世界烘焙），必须看 preview 与参考图确认是"更接近"而非回归；S2/S3 的动画/morph 也要用 `--shot-seq` 出多帧人工抽检。验收命令固定为 `ctest -R 'gltf_import_e2e|gltf_skin_e2e|gltf_anim_e2e|gltf_morph_e2e|render3d_scene_e2e|determinism'` + 26 模型批量重跑日志无 `mesh=-1`/shader 告警。

---

## 附录 A · 统计口径与复现脚本（临时脚本，未入库）

> 下面逻辑对应本轮实际运行的三类扫描；只读资产，不写仓库文件。

```python
# 主变体选择（与 tools/migrate_gltf_models.py 一致）
def find_primary(model_dir):
    g = os.path.join(model_dir, "glTF")
    if os.path.isdir(g):
        fs = sorted(f for f in os.listdir(g) if f.endswith(".gltf"))
        if fs:
            base = os.path.basename(model_dir)
            exact = [f for f in fs if os.path.splitext(f)[0] == base]
            return os.path.join(g, (exact or fs)[0])
    b = os.path.join(model_dir, "glTF-Binary")
    if os.path.isdir(b):
        gs = sorted(f for f in os.listdir(b) if f.endswith(".glb"))
        if gs: return os.path.join(b, gs[0])
    return None

# 动画/骨骼/morph/pointer 统计（主变体 148 模型；全变体见下一段）
for m in sorted(os.listdir(MODELS)):
    js = load_gltf(find_primary(os.path.join(MODELS, m)))
    anims = js.get("animations") or []; skins = js.get("skins") or []
    morph = sum(1 for me in (js.get("meshes") or [])
                  for pr in (me.get("primitives") or []) if pr.get("targets"))
    for a in anims:
        for c in a.get("channels") or []:
            t = c.get("target") or {}
            ptr = ((t.get("extensions") or {}).get("KHR_animation_pointer") or {}).get("pointer")
            # ptr 存在 → pointer 通道（注意不是 t["pointer"]）
        for s in a.get("samplers") or []:  # interpolation / input accessor max → duration
            ...

# KTX2：逐文件读 12B magic + 头部（vkFormat@12, levelCount@20, supercompressionScheme@24）
# 全变体：遍历 Models/*/<变体目录>/*.{gltf,glb}，按目录聚合取 max(anim, skin, morph)
# image 编码：images[].mimeType / uri magic；.ktx2 按文件计数并累加 os.path.getsize
```

关键点（避免重复踩坑）：
- `KHR_animation_pointer` 在 `target.extensions` 里，不在 `target` 顶层；
- glTF `extensionsRequired` 含 `KHR_meshopt_compression`（旧名）不等于 bufferView 有压缩数据，要查 `bufferViews[].extensions`；
- 主变体/全变体的计数必须分开报，否则会和 `docs/31 §11` 的历史数字对不上（口径差见 §1.2）。

## 附录 B · 关键文件/行号索引（本轮只读核对）

| 主题 | 位置 |
|---|---|
| 顶点布局/管线/阴影/ABI 消费 | `modules/render/gfx/src/gfx.cpp`：`p3dMatPipeline` 104、`shadowPipeline` 114、group1 BGL 364–385、`get_material_pipeline` 503–570、`r_create_mesh` 1016–1044、`pack_draw_uniform` 1434–1483、`fill_material_bind_entries` 1524、阴影绘制 1811–1846、主 pass 绘制 1922–1979 |
| PBR shader | `assets/builtin/shaders/pbr/00_types.wgsl`（Draw 38 行）、`40_vertex.wgsl`（`vs` 66、`vs1` 79、`vs_shadow` 12、`vs_shadow1` 22、`vs_common` 52） |
| 渲染 ABI / 帧结构 | `modules/core/data/schema/render/renderer.yaml`：`texture_desc` 94–105、`mesh_gpu` 107–116、`draw3d` 196–205、`frame3d` 226–249 |
| 场景/材质组件 | `modules/core/data/schema/render/render.yaml`（`Camera3D` 225、`MeshRenderer` 239、`Light` 252）、`modules/core/data/schema/world/anim.yaml`（BonePose 122、AnimationPlayer 133、Animator 144、Skeleton 154） |
| 动画资产 schema | `modules/core/data/schema/asset.yaml`：`Key` 104、`Track` 111、`AnimationClip` 172、`SkeletonData` 180、`BoneB` 90、`MQ_ASSET_KIND_ANIMATION` 425 |
| 2D 属性轨道实现 | `modules/runtime/src/runtime.cpp`：`load_clip` 2604、`anim_step` 2640、`resolve_field_path` ~2586、每步调用 2292 |
| 场景加载/3D 提取 | `modules/runtime/src/runtime.cpp`：`load_scene_into` 8926、`setup_3d` 1685、`extract_3d` 1717、`m4_from_trs` 1612、`flatten_world` 3652、`render_frame3d` 调用 2428 |
| importer | `tools/gltf_import.py`：image 通路 342–407、`emit_scene` 749–919、`_round_mesh` 922、`import_gltf` 928–1028、CLI 1049+ |
| 测试基建 | `tests/gltf_import_e2e.sh`、`tests/anim_e2e.sh`、`tests/data/gltf/*.gltf`、`samples/player/CMakeLists.txt` 135–139/162 |
| host 脚本函数 | `modules/core/data/schema/runtime/host.yaml` `spine_*` 167–190；实现 `modules/runtime/src/runtime.cpp` 6720 起 |
| 迁移状态与统计 | `scenes/khronos/manifest.json`（148 条全 `ok`）、`docs/31 §5.4` 468 行、`docs/31 §11` 1095 行、`plans/20-pbr-audit.md` 30/38 行 |

---

*（本文件为评估稿；S1–S3 的具体提交按 §4–§6 的清单拆分，每阶段独立 e2e + 批量重跑后再进入下一阶段。）*
