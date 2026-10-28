# 20 · PBR 实现审计（Khronos glTF-Sample-Assets 批量迁移前置）

> 触发：用户要求"逐个迁移 Models 目录的每个模型"，但**先审查 PBR 实现**。
> 本文是审查报告 + 修复记录 + 迁移就绪度评估。
> 审计快照：`f464468`（2026-10-26 17:29 +0800，工作区干净）之后、修复前。
> 审计/实证的完整原始证据在 `build-win/audit/`（构建产物，不入库）：
> - `shader/findings.md`（WGSL 数学与材质语义，22 条）
> - `renderer/findings.md`（gfx 管线/UBO/贴图/阴影/排序，19 条 + 数值复现脚本）
> - `asset/findings.md`（importer/runtime/schema/场景生成，60+ 条 + 迁移阻塞分级）
> - `verify/findings.md`（实证 probe 与 ToyCar 参考图对比）
> - `asset/scan_models.py` 输出（149 个模型的 feature 统计）

## 0. 结论摘要

**总体判断**：PBR 的骨架（uniform 布局、GGX/Fresnel/Karis 公式、sRGB 槽选择、阴影 y 翻转/near-far、
透明排序、MASK 走 opaque 管线等）经三路独立核对**基本正确**（详见 §5"已验证正确"）；
但存在 **9 类会直接破坏 Khronos 模型观感/可加载性的 P1 级缺陷**，以及若干 P2/P3。
修复前不适合开始批量迁移（会把系统性错误复制到 149 个场景里）。

| # | 严重度 | 缺陷 | 影响范围 | 状态 |
|---|---|---|---|---|
| 1 | **P1** | 法线贴图切线基两处缺陷：①未补偿 WebGPU `dpdy` 反号 → normal map 在切线空间旋转 **180°**（凹凸/缝隙方向反）；②T/B 用共同 epsilon `1e-8` 归一化 → 小尺度场景 T/B 长度被错误缩放，法线贴图效果被削弱 ~85%（ToyCar 1e-4 尺度即命中） | 所有带 `normalTexture` 的模型（statistics：419 个 texture 引用） | ✅ 已修（shader；实测 oracle 从 nv=(0.14,0,0.98) → (0.79,0,0.60)） |
| 2 | **P1** | glTF `doubleSided` 语义完全未实现：不剔除背面、背面法线不反转；importer 只写 `double_sided`，runtime/gfx 只读 `side` | 全部 `doubleSided=false` 模型（20529 处材质）不剔除背面；双面材质背面光照错 | ✅ 已修（shader+importer+资产） |
| 3 | **P1** | 材质 8 张贴图共用全局 ClampToEdge sampler，per-texture sampler 从未绑定 → glTF REPEAT/filter 全丢 | KHR_texture_transform 平铺（39 个模型使用该扩展）、地面/墙面、TextureSettings 类 | ✅ 已修（gfx） |
| 4 | **P1** | 阴影视锥是"原点为中心、半径=平移量启发式"，不用 mesh bounds → 小尺度资产阴影≈4 px、大/偏移资产无阴影 | ToyCar（已入库，实测阴影贴合 4.4×4.3 贴图像素）、所有大/偏移/小尺度模型 | ✅ 已修（gfx，按 bounds 拟合） |
| 5 | **P1** | 阴影 depth pass 无 fragment → alphaMode=MASK 镂空几何投**实心方块**阴影 | 树叶/栅栏/发卡/贴花类模型（MASK 材质 92 处） | ✅ 已修（shader+gfx） |
| 6 | **P1** | JPEG 贴图被 importer 整张跳过（引擎 image 模块其实支持 JPEG） | **482 张 JPEG**（DamagedHelmet/FlightHelmet/Sponza 等大量资产），GLB 迁移会大面积丢贴图 | ✅ 已修（importer） |
| 7 | **P1** | KHR 材质扩展的**贴图槽**全部缺失（clearcoat/sheen/iridescence/specular/anisotropy 的 texture），ToyCar 车漆 clearcoat 贴图被丢 | harness 扩展材质：clearcoat 31 模型、sheen 19、iridescence 23、specular 20、anisotropy 15 | ⏳ 待做（需 ABI/schema 追加贴图槽） |
| 8 | **P1** | 顶点/材质属性缺口：`TEXCOORD_1`（209 个 texture 引用）、`COLOR_0`（78 个 primitive）、`TANGENT` 未用（屏幕空间 TBN 可接受） | 多 UV 集资产、顶点色资产 | ⏳ 待做（顶点布局 48B 需扩展，牵涉 ABI） |
| 9 | **P1** | 不支持 glTF feature 无法加载：Draco 18 模型（required）、KTX2/BasisU 7、EXT_meshopt 2、WebP 5、KHR_mesh_quantization 10 | 这些模型的 `glTF-Draco`/`glTF-KTX2` 变体无法导入（主 `glTF` 变体通常仍可） | ⏳ 待做（解码器/解压依赖）；**KTX2 runtime 直读已支持**（plans/24 P4），离线 importer 仍跳过 |
| 10 | **P2** | occlusion 公式 `ao×strength` 而非规范 `mix(1,ao,strength)`；`occlusionTexture.strength` 未导入 | strength≠1 的资产 | ✅ 已修（shader+importer） |
| 11 | **P2** | 顶点法线用 model 矩阵变换（非均匀缩放下错误）；负行列式（镜像）未处理 | 非均匀缩放/镜像节点（ToyCar 为 1e-4 均匀缩放，未暴露） | ✅ 已修（shader 逆转置 + gfx frontFace） |
| 12 | **P2** | sRGB 贴图的 CPU mip 在编码值上做 box filter（远处偏暗，实测黑白棋盘 -57% 亮度） | 所有 sRGB albedo/emissive 的 minified 区域 | ✅ 已修（runtime） |
| 13 | **P2** | glTF `normalTexture.scale` 未导入（法线强度丢失） | 使用 scale≠1 的资产 | ✅ 已修（importer） |
| 14 | **P2** | 阴影灯与 `lights[0]` 约定不一致（矩阵取"第一盏方向光"，shader 固定乘 `li==0`；灯序=实体 id 序） | 多灯场景 / 先建点光后建方向光的场景 | ✅ 已修（gfx 灯序重排） |
| 15 | **P2** | KHR_materials_specular 的 f90 未实现（specularFactor=0 仍有掠射高光）；iridescence 忽略 ior 且只调 F0、量级偏亮；clearcoat 无底层能量衰减 | specular 20 / iridescence 23 / clearcoat 31 模型 | ⏳ 待做（物理公式完善） |
| 16 | **P2** | ACES 曲线是 Narkowicz 近似，与 three.js `ACESFilmicToneMapping`（RRT/ODT + /0.6）中灰差 ~14 sRGB 码值 | 与 three.js/参考图做像素对比时的系统性偏差 | ⏳ 待决策（切换曲线会整体改变观感基线） |
| 17 | **P2** | 骨骼/变形/动画不导入（51 模型有动画、24 有 skin、15 有 morph）：角色静态 T-pose；morph 默认权重未应用 | 角色/动画类模型与参考图姿势不同 | ⏳ 待规划（迁移可先静态渲染并标注） |
| 18 | **P2** | 每帧每 draw 新建 uniform buffer + bind group（主/阴影 pass 各一遍），违反 plans/18 "无逐帧资源重建" | 149 模型批量渲染性能 | ⏳ 待做（uniform arena） |
| 19 | **P3** | 无 MSAA；点光 range=0 会熄灯（three.js 语义=无限远）；glTF sampler 的 wrap/filter 不导出；无 deviceLost 回调；文档-代码漂移若干 | 观感/健壮性 | 部分已修（range=0、文档） |

## 1. 审计方法

1. **三路独立静态审计**（不同 agent、不同视角，互不通气）：
   - WGSL shader：逐行对照 glTF 2.0 材质规范、KHR_materials_* 规范、three.js 实现；
   - gfx 渲染器：uniform 布局逐字段数值验证（Python 复刻 WGSL 对齐规则）、管线状态、贴图上传、阴影数学（用 ToyCar 真实 bounds 数值复现）；
   - 资产管线：importer 逐字段对照 glTF 语义，runtime 提取默认值/SRGB 槽/场景生成，全部结论给 `file:line` 证据。
2. **交叉验证**：三份报告独立发现了同一批问题（如 doubleSided 的 shader/runtime/importer 三方链、阴影灯序、sRGB mip），互相印证；不一致处以代码/数值为准。
3. **实证 probe**（`build-win/audit/verify/`）：35 个最小场景（平面/立方体 + 手写 `.mesh.json` + 生成 PNG）在引擎内渲染，
   与 Python/NumPy 独立计算的期望值对比；ToyCar 与 Khronos 官方参考图做区域统计对比。
4. **资产面统计**（`asset/scan_models.py`）：对 `.deps/gltf-assets/.../Models` 149 个模型的 `glTF` 变体做 feature 扫描（见 §4）。

## 2. 分领域结论

### 2.1 WGSL shader（完整报告：`build-win/audit/shader/findings.md`）

**正确项（抽样）**：GGX D（a=rough² 幂次链）、Schlick Fresnel（V·H）、直接光能量 `(kd·base/π+spec)·L·NdotL`、
Lambert、Karis env BRDF、sRGB OETF、MR 通道（B=metallic/G=roughness）、alpha MASK discard、
阴影 uv.y 翻转/near-far 方向、`k=(r+1)²/8` 的 Schlick-GGX、四种入口的点光衰减公式一致性。

**缺陷**（编号与详细报告对应）：

| ID | 级 | 结论 | 依据 |
|---|---|---|---|
| S1 | P1 | `cotangent_frame` 未补偿 WebGPU `dpdy`（framebuffer y 向下）与 GLSL `dFdy` 反号 → T、B 同时取反，normal map 旋转 180° | 公式推导 + PlayCanvas WebGPU 同类修复 PR#9099；修复后 T=+∂P/∂u、B=+∂P/∂v |
| S1b | P1 | `cotangent_frame` 用 `inverseSqrt(max(max(|t|²,|b|²), 1e-8))` 共同归一化 T/B。`|t|²` 量纲是 (世界/像素 × UV/像素)²，小尺度场景（ToyCar scale=1e-4）下 <1e-8 → T/B 被缩到 ~0.07，normal map 效果被削 ~85%（实测 oracle 输出 nv=(0.14,0,0.98)，应 (0.8,0,0.6)） | verify 的 Normal-oracle probe 实测（静态审计未发现）；修法：T/B 各自 `inverseSqrt(max(|t|²,1e-30))` |
| S2 | P1 | doubleSided 未实现：`dr.alpha.z` 全 shader 零读取、无 `front_facing` 法线/TBN 翻转 | glTF 规范 `doubleSided` normative "back-face MUST have normals reversed"；three.js `faceDirection` |
| S3 | P2 | occlusion = strength×tex，规范 = 1+strength×(tex−1) | glTF 2.0 / three.js `aomap_fragment` |
| S4 | P2 | KHR_materials_specular 缺 f90=specular 层权重；specularFactor=0 掠射仍 F→1 | KHR_materials_specular 规范 |
| S5 | P2 | iridescence 忽略 `iridescenceIor`，用 NdotV 插值厚度、只混 F0 且量级 0.43–0.58（介质应≈0.04） | KHR_materials_iridescence / three.js |
| S6 | P2 | clearcoat 无底层/emissive 能量衰减、Fcc 用 V·H 而非 V·Nc、NdotL 用基底法线、无 clearcoat 贴图 | KHR_materials_clearcoat normative 层混合公式 |
| S7 | P2 | ACES 用 Narkowicz 近似，与 three.js RRT/ODT 曲线中灰差 ~14 码值 | three.js `tonemapping_pars_fragment` 数值对比 |
| S8 | P2/P3 | 点光 `range=0` 熄灯（应为无限远）；`d²+1` 近距偏差最多 5× | three.js/KHR_lights_punctual 语义 |
| S9 | P2 | 阴影固定乘 `li==0`，与 C++ 取"第一盏方向光"不是同一套规则 | 两侧代码交叉 |
| S10 | P2 | 顶点法线用 model 矩阵（非均匀缩放错误）；负行列式未处理 | 逆转置矩阵定义 |
| S11 | P2 | 只保留 base+normal 两组 UV transform；MR/AO/emissive 被强制共用 base 组；texCoord 集不支持 | glTF 每个 textureInfo 可有独立 transform/texCoord |
| S12 | P2 | importer 未映射 normalTexture.scale（shader 侧 x/y 语义本身正确） | glTF 规范 / three.js normalScale |
| S12B | P2 | 阴影 pass 无 fragment → MASK 材质投影实心（与 renderer R3 互证） | 代码 |
| S13–S21 | P3 | analytic studio IBL 的非物理近似、sheen rim 近似、anisotropy 只压 roughness、Normal/Depth toneMapped 语义、Depth 灰度反向、roughness 下限 0.02 vs three.js 0.045、死字段等 | 详细报告 |

### 2.2 渲染器 gfx（完整报告：`build-win/audit/renderer/findings.md`）

**正确项（抽样，数值验证过）**：Camera 816B / Draw 288B 与 WGSL 逐字段偏移一致；顶点布局 48B；
阴影 y 翻转、near/far 方向、`depthSampleCompare` 用法、ShadowMaterial 排除；transparent 排序/深度写；
MASK 走 opaque；cull/side 映射；sRGB 槽选择；`create_texture2` 逐 mip 上传与 256B 对齐结论（writeTexture 无该约束）；
fallback 白纹理/中性法线；离屏回读/直呈格式；资源释放。

**缺陷**：

| ID | 级 | 结论 |
|---|---|---|
| R1 | P1 | 材质 8 贴图共用全局 ClampToEdge sampler；`create_texture2` 的 per-texture sampler 从未绑定（与 shader/资产审计互证） |
| R2 | P1 | 阴影视锥固定原点 + `radius=max(4,|t|₁+2.5)≤12` 启发式，mesh bounds 上传后从未使用。数值：ToyCar 阴影 u/v 跨度 4.4×4.3 贴图像素、bias 世界深度 0.0144（≈车长 38%）；±10 模型 0/8 角在视锥；t=60 无影 |
| R3 | P1 | 阴影 depth pass 无 fragment/alpha；BLEND 也投实心影；`draw3d.flags`/`light.shadow_slot` 字段未读 |
| R4 | P2 | 阴影灯与 lights[0] 不一致；>8 灯静默截断；caps.max_lights=64 与 shader 8 矛盾 |
| R5 | P2 | 导入的 glTF 材质全部退化为 Double-sided（importer/runtime/gfx 三方链） |
| R6 | P2 | 每帧每 draw 新建 288B UB + 10 项 bind group ×2 pass |
| R7 | P2 | 直呈换链瞬时失败→runtime 永久关 3D；无 `resize_surface` 接线 |
| R8 | P2 | sRGB mip 在编码值上平均 |
| R9 | P2 | 负行列式（镜像）未补偿 frontFace |
| R10–R19 | P3 | depthCompare Less vs LessEqual、无 MSAA、多视图 3D 未实现、caps/文档漂移、布局无 static_assert 守卫、destroy_texture 漏 sampler 等 |

### 2.3 资产管线（完整报告：`build-win/audit/asset/findings.md`）

**正确项（数值复核）**：`_decompose`（Rz·Ry·Rx）↔ runtime `m4_from_trs` 往返误差 5e-14；相机 `_look_rotation` ↔ `m4_view`；
灯光方向约定；AABB 8 角点；贴图 V 方向；sRGB/线性槽选择；四元数→矩阵；输出确定性。

**缺陷**：M1 doubleSided→side（已修）；M2 normalTexture.scale（已修）；M3 occlusionTexture.strength（已修）；
M4 KHR 扩展贴图槽全缺；M5 texCoord 集；M6 texture_transform.rotation；M7 逐贴图 UV transform 只保留两组；
M8 transmission/volume/diffuse_transmission/pbrSpecularGlossiness/variants 无报告；I1 JPEG（已修）；
I2/I3 KTX2（runtime 直读已支持；离线 importer 跳过）/WebP 无解码器；I4 贴图命名冲突；I5 sampler 状态丢弃；I6 sRGB mip（已修）；G1 缺 NORMAL 索引网格法线错（已修）；
G2 COLOR_0；G3 TEXCOORD_1；G4 morph 默认权重；G5 skin；G6 动画；G7 sparse accessor；G8 Draco/meshopt/quantization；
G11 负缩放（`_decompose` 丢失）；G12 剪切无法用 TRS 表达；S1 `--scene` 无条件覆盖；S2 忽略 glTF 自带相机；
S3 自动取景公式；S4 无 env_*；S5 固定方向光、忽略 punctual lights；S6 节点树烘焙为平级；S10 深色背景 vs 参考棚拍；
R1 无材质默认值不一致；R2 每帧懒加载贴图；R3 缓存跨场景；R4 mesh 失败回退 cube；R7 SRGB/线性槽正确。
X 组：大模型文本 JSON 体积、`--compact` 数值差异、无 extensionsRequired 报告、坏资产 traceback。

## 3. 本次已实施的修复（P1/P2 的子集）

> 原则：不改 schema/ABI；保持旧资产/旧场景的确定性；每项修复可独立回滚。修改文件见 `git diff`。

### 3.1 shader（Lead）

1. **S1** `10_utils.wgsl::cotangent_frame`：`dp2 = -dpdy(p); duv2 = -dpdy(uv)` → T/B 恢复 glTF/three.js 约定。
   **S1b** 同函数：T/B 改为各自归一化（`inverseSqrt(max(|t|²,1e-30))`），修小尺度场景 normal map 被削 85% 的缺陷
   （ToyCar/oracle probe 实测）。
2. **S2** `20_surface.wgsl`：新增 `surface_geo_normal(raw, front_facing)`（`dr.alpha.z>0.5 && !ff` 时法线取反）；
   `surface_common(in, front_facing)`；10 个 fragment 入口增加 `@builtin(front_facing)`；clearcoat 法线同样处理。
3. **S3** `20_surface.wgsl`：`ao = clamp(1 + strength*(ao_tex-1))`（规范 mix 语义）。
4. **S10** `40_vertex.wgsl`：`model_normal_matrix()` 用伴随矩阵实现 inverse-transpose（含 det<0 的 sign 补偿）。
5. **S8** `30_lighting.wgsl`：`range<=0` 视为无限远（4 处）。
6. **S12B/R3** `40_vertex.wgsl`+`67_fs_shadow.wgsl`：`vs_shadow` 输出 `ShadowVOut{pos,uv}`；新增 `fs_shadow_alpha`
   （只做 MASK alpha discard、无颜色输出）。
7. **C3** `10_utils.wgsl::shadow_factor`：改用 `textureSampleCompareLevel`（显式 LOD0，不要求 uniform control flow）。

### 3.2 gfx（renderer auditor，Lead 已审阅 diff）

R1 per-texture sampler 绑定（`material_sampler()`，优先 albedo 贴图）；R2 按 draw mesh bounds×model 在光源视空间拟合
正交盒（新 `mat4_ortho_box`、bias 按纹素世界尺寸/深度范围派生，旧启发式仅作 fallback）；R3 阴影管线 + `fs_shadow_alpha`
（pos@0+uv@40、零 color target、真实材质 bind group）；R4 灯序重排（方向光优先、截断 8 盏告警）+ `caps.max_lights=8`；
R9 pipeline 缓存加 mirrored 维 + `frontFace=CW`；R19 阴影资源分离创建/失败关闭阴影（不绑错 sampler 类型）；
`destroy_texture` 释放 per-texture sampler；`maxAnisotropy=4`（linear 过滤时）。

### 3.3 runtime / importer / 资产（asset auditor）

I1 JPEG 搬运（`image_encoding()`，PNG 零漂移）；M2 normalTexture.scale；M3 occlusionTexture.strength；
M1 每个材质写 `side`（true→2/false→0）；G1 compute_normals 用真实 indices；R8 sRGB mip 线性化；
ToyCar/scene/assets 重新生成（仅新增 `side`，贴图逐字节不变）；`tests/gltf_import_e2e.sh` 期望同步。

### 3.4 修复后验证（本机实测）

构建：`cmake -S . -B build-win`（重新生成内置 shader 包）+ `ninja -C build-win`（本机 CMake 的再生成步骤在工具
会话里会卡住，直接 configure + ninja 正常；`-DCMAKE_SUPPRESS_REGENERATION=ON` 仅用于规避卡死）。

**全量测试：84/84 PASS**（`ctest --test-dir build-win --timeout 180`，355s；2 个 hot_reload 按环境 SKIP），
含 `gltf_import_e2e` / `render3d_scene_e2e` / `material_models_e2e` / `material_physical_e2e` /
`material_shadow_e2e` / `builtin_shaders` / `codegen_drift` 等全部与 PBR 相关用例。

**probe 复测**（`build-win/audit/verify/` 的 41 个 probe，修复前后同一套；数值为最终构建）：

| 缺陷 | 修复前 | 修复后 | 结论 |
|---|---|---|---|
| S1/S1b 法线基（Normal oracle，法线贴图=(0.8,0,0.6)） | nv=(−0.14,0,0.98)，效果削 85% | **nv=(0.79,0,0.60)**（o2 的 +Y 贴图 → nv=(0,−0.80,0.60)，与 mesh UV 约定一致） | ✅ 方向+强度正确 |
| R1 wrap repeat（uv_scale=4 + checker16） | 8 行采样 transitions=42、unique colors=82（clamp 拉伸） | **transitions=175、unique colors=171** | ✅ REPEAT 生效 |
| R2 ToyCar 阴影（shadow on/off 差图） | 125 px（0.0136%） | **39,172 px（4.25%）**，投影贴合车底/布料 | ✅ 阴影可见 |
| R3 MASK 镂空投影 | 无 alpha test（应为实心矩形） | 棋盘影（diff 2,141 px，bbox 129×51，fill 0.325） | ✅ 镂空生效 |
| S3 occlusion strength（黑 AO） | strength 0/0.5/1 全为 107 | **207 / 179 / 106**（白 AO 三档均 207） | ✅ mix(1,ao,strength) |
| R8 sRGB mip（棋盘最远采样） | 154（编码域平均） | **206**（线性域平均正确值） | ✅ |
| R9 镜像缩放（scale.x=−1, side=0） | 全黑（正面被剔） | 与对照组一致（156,240 非背景像素） | ✅ |
| S2 doubleSided | 输入不参与剔除；法线不翻 | side 由 importer 生成（2/0）；shader 背面翻转（`front_facing`）。**仍需手写 scn 显式写 side** | ✅（导入路径） |
| ToyCar 参考图 | — | 全图亮度差 +34.5、前景 +37.7、直方图 KL 1.43（红布亮约 2 倍） | 剩余差异主要来自缺失特性：HDRI/PMREM、transmission（Glass）、clearcoat 贴图、MSAA/软阴影；阴影/法线/AO/mip 缺陷已消除 |

`build-win/audit/after/` 保存了修复后截图与日志；`build-win/audit/probes/out_prefix/` 是修复前同一套 probe 输出，
可用于并排对比。ToyCar `preview.png`、`scenes/pbr/material_models.png`、`material_physical.png` 已按最终构建重生成。

**注意**：`build-win/audit/verify/` 的原始实证报告是在**旧 exe**（17:09 构建、与 HEAD 源码不同步）上跑的，
个别"绝对像素"结论已标注需重建复测；上表是最终构建的复测值。

## 4. 迁移就绪度（Models 目录，149 个模型）

### 4.1 资产面统计（`asset/scan_models.py`，扫每个模型的 `glTF` 变体）

- **149 个模型目录，149 个都有 `screenshot/` 参考图**；`glTF` 变体 336 个文件，1 个 GLB-only。
- 贴图编码：PNG ~499、**JPEG 482**、KTX2 81、WebP 26。
- 扩展使用（模型数，按变体计）：`KHR_materials_transmission` 77、`KHR_materials_volume` 55、
  `KHR_texture_transform` 39、`KHR_materials_ior` 33、`KHR_materials_clearcoat` 31、
  `KHR_lights_punctual` 24、`KHR_materials_iridescence` 23、`KHR_materials_variants` 22、
  `KHR_materials_specular` 20、`KHR_materials_sheen` 19、`KHR_draco_mesh_compression` 18、
  `KHR_materials_anisotropy` 15、`KHR_materials_emissive_strength` 15、`KHR_materials_unlit` 12、
  `KHR_mesho_quantization` 10、`KHR_animation_pointer` 10、`KHR_texture_basisu` 7、`EXT_texture_webp` 5。
- 几何/动画：有动画 69 个变体、skin 24、morph target 15、`COLOR_0` 78 个 primitive、`TEXCOORD_1` 691 处属性引用
  （209 处 textureInfo 用 texCoord=1）、非三角 primitive 28、缺 NORMAL 的 primitive 68。
- alpha：BLEND 30、MASK 92（相对 23597 个 OPAQUE 材质）。

### 4.2 分层迁移策略

| 层 | 条件 | 处理 | 规模估计 |
|---|---|---|---|
| **A 可直接迁移** | core metallic-roughness、PNG/JPEG、texCoord=0、无/仅已支持扩展、无动画/骨骼 | `gltf_import.py --scene` → 渲染 → README/preview | 大部分简单资产（Box/Duck/BoomBox/Avocado/Helmet 等） |
| **B 小修可迁移** | 使用 KHR_texture_transform/clearcoat/unlit/emissive_strength 等已支持扩展；单面/双面、MASK | 修复后 importer 已覆盖；`--scene` 生成后按参考图手调相机/灯光/env | 扩展 demo 类 |
| **C 需新特性（先记录 gap）** | transmission/volume（77/55）、iridescence 精确、specular/clearcoat 物理、punctual lights、动画/骨骼/morph、COLOR_0、TEXCOORD_1 | 先迁移静态近似版本，README 标注"参考图依赖 X 特性，当前不支持"；后续按优先级补 | 约 1/3 模型 |
| **D 无法加载** | 仅有 Draco（18 required）/KTX2（7 required）/meshopt（2）/WebP（5 required）/quantization（10）的变体 | 主 `glTF` 变体通常存在 → 用主变体；若仅压缩变体则记录 blocked | 少量 |

### 4.3 参考图对比策略（重要）

Khronos 的 `screenshot/` 多为资产作者在离线渲染器（常带 HDRI studio、软阴影、部分带地面/背景板）里出的图；
maque 当前**没有 HDRI/PMREM IBL、没有 transmission 折射、没有软阴影/面光、无 MSAA**，因此：

- **不以像素相似度作为 pass/fail**。验收用：模型可见、无 NaN/破面、材质主色/贴图存在、方向性特征（金属/粗糙度/
  法线细节/阴影存在）、与参考图的区域统计量级一致；
- 每模型 README 记录"与参考图的差异归因"（缺失特性 vs 实现缺陷）；
- 相机/灯光/env 的匹配是**场景资产**问题（`--scene` 默认机位与参考无关），迁移流程需允许逐模型手调 `.scn`。

### 4.4 建议的批量迁移流程（下一步实现）

1. 用 `node .deps/download-gltf-assets.js` 下载并解包（本机已执行：`.deps/gltf-assets/glTF-Sample-Assets-main/`，
   149 模型 / 1.34GB；本环境 shell 的 schannel 出网被阻断，但 Node/Python 的 OpenSSL TLS 可用）。
2. `python tools/gltf_import.py <model>/glTF/<model>.gltf --out scenes/khronos/<model> --name <model> --scene --compact`
   （JPEG 修复后可覆盖 482 张 jpg）。
3. 渲染 `--shot` 1280×720 → 与 `screenshot/screenshot.png` 并排；用脚本产出结构指标（非背景占比/色彩分布/
   高光/阴影）与差异摘要。
4. 逐模型修 `.scn` 相机/灯光/env（参考图机位），重拍 `preview.png`，写 README（来源/许可/生成命令/差异归因）。
5. 批量 driver 脚本 + 汇总索引（`scenes/khronos/README.md`）；对 C/D 类模型记录 blocked 原因。
6. 回归：`ctest -R 'gltf_import_e2e|render3d_scene_e2e|material_.*_e2e'` + 抽查截图确定性（同场景两次逐字节一致）。

## 5. 已验证正确（避免过度修复）

- WGSL：GGX D、Schlick Fresnel(V·H)、直接光能量/单位、Lambert、Karis env BRDF、精确 sRGB OETF、
  MR 通道映射、MASK discard、阴影采样 uv/near-far 约定、`k=(r+1)²/8`、四入口衰减一致性。
- gfx：Camera 816B / Draw 288B 布局逐字段、顶点 48B、blend 公式、MASK 走 opaque、透明排序确定性、
  阴影 y 翻转/深度方向、sRGB 槽选择、create_texture2 mip 上传、回读路径、资源释放、直呈格式选择。
- 资产数学：Euler 分解 ↔ m4_from_trs（5e-14 往返）、相机/灯光旋转约定、AABB 8 角点、贴图 V 方向、
  四元数→矩阵、输出确定性（排序键+固定缩进）。
- runtime：纹理/网格缓存 key、mesh_path 相对场景目录、sRGB 槽选择（albedo/emissive/matcap 为 sRGB；
  MR/AO/normal 为线性）。

## 6. 决策点 / 待办

1. **是否切换 ACES 曲线到 three.js RRT/ODT**（S7）：会整体改变现有场景/预览观感与基线，需用户拍板。
2. **IBL/HDRI 是否排期**：这是与 Khronos 参考图观感差距的最大单一因素（chrome/车漆/玻璃/布料）。
3. **动画/骨骼/morph 是否在迁移范围**（51/24/15 模型）：若只做静态展示，需在 README 标注。
4. **transmission/volume**（77/55 模型）：需要 backbuffer 折射 pass，属新渲染特性。
5. **扩展贴图槽**（clearcoat/sheen/iridescence/specular/anisotropy）：需要 renderer ABI 尾部追加贴图槽 +
   schema 字段（生成物重生成），建议与迁移并行规划。
6. **环境说明**：本会话后段已具备完整权限，Git Bash/`ctest` 可正常运行（84/84 通过）；此前沙箱会阻止
   MSYS 命名对象，导致 e2e 无法启动。网络：shell 的 schannel/wolfSSL 出网不稳定，但 **Node/Python 的
   OpenSSL TLS 可用**——Khronos 资产已用 `node .deps/download-gltf-assets.js` 下载到
   `.deps/gltf-assets/`（149 模型 / 1.34GB），迁移无需再联网。
7. **后续回归**：建议把 `build-win/audit/verify/findings.md` §11 的 8 条断言落成 `tests/pbr_regression_e2e.sh`
   （normal oracle 方向+强度、AO mix、wrap repeat、sRGB mip、双面法线、镜像缩放、ToyCar 阴影像素数），
   现有 `material_*` 测试只做结构性断言，抓不到本次这批缺陷。

## 7. 附录：证据索引

| 证据 | 路径 |
|---|---|
| shader 审计全文（22 findings + 覆盖清单 + 测试缺口） | `build-win/audit/shader/findings.md` |
| shader 数值/资产分析脚本 | `build-win/audit/shader/_calc.py`、`_toycar_analysis.py` |
| renderer 审计全文（19 findings + 20 条正确项 + 文档漂移表） | `build-win/audit/renderer/findings.md` |
| renderer 布局/阴影视锥数值复现 | `build-win/audit/renderer/verify_layout.py`、`verify_layout.out.txt` |
| asset 审计全文（60+ findings + 阻塞分级 + T1–T14） | `build-win/audit/asset/findings.md` |
| asset 数学复核 | `build-win/audit/asset/verify_math.py`、`verify_math.out.txt` |
| 149 模型 feature 统计脚本 | `build-win/audit/asset/scan_models.py` |
| 实证 probe（35 场景/贴图/输出） | `build-win/audit/probes/`、`build-win/audit/verify/` |
| ToyCar 参考图/源资产 | `build-win/ref/toycar_ref.jpg`、`build-win/ref/toycar-src/` |
| Khronos 全量资产（已下载解包，不入库） | `.deps/gltf-assets/glTF-Sample-Assets-main/` |
