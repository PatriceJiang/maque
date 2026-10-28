# 24 · 运行时 glTF 能力矩阵与进度（C++ 直读）

> 状态：**P1–P5 已落地；P6 已覆盖 instancing / sparse / node_visibility（静态）/ materials_variants /
> animation_pointer（材质数值子集）**；另接入 **KTX2 / Basis Universal 解码**（P4 贴图链路）。
> 上游：[plans/23](23-runtime-gltf-loading.md)（静态网格 + 核心 PBR）、[plans/22](22-gltf-animation-implementation.md)
> （离线 importer 的蒙皮/动画/morph 管线）。本文记录 **runtime 直接加载 `.gltf`/`.glb`** 的能力覆盖、进度与测试证据；每次推进后更新。

## 0. 图例

| 标记 | 含义 |
|---|---|
| ✅ | 已实现 + 自动化测试覆盖 |
| 🟡 | 部分实现（有明确缺口，见备注） |
| ⬜ | 未实现（排在后续） |
| ⛔ | 不做 / 受第三方依赖阻塞（记录原因） |

## 1. 能力矩阵（runtime 直读）

| 类别 | 能力 | 状态 | 说明 / 测试 |
|---|---|---|---|
| 容器 | `.gltf` JSON + 外部 `.bin` | ✅ | `gltf_runtime_e2e` ① |
| 容器 | `data:` URI（buffer/image 内嵌） | ✅ | `gltf_runtime_e2e` ⑤ |
| 容器 | `.glb`（JSON + BIN chunk） | ✅ | `gltf_runtime_e2e` ② |
| 容器 | 外部图片（相对模型文件目录，百分号解码） | ✅ | `gltf_runtime_e2e` ① |
| 几何 | 三角 primitive / indices（u8/u16/u32） | ✅ | `gltf_runtime_e2e` |
| 几何 | POSITION / NORMAL（缺省自动算法线） | ✅ | `gltf_runtime_e2e` |
| 几何 | TEXCOORD_0 / TEXCOORD_1 / COLOR_0 | ✅ | 顶点布局 48B/56B；`TANGENT` 不读 |
| 几何 | 节点 matrix / TRS 层级 | ✅ | 逐 draw 应用节点世界矩阵；动画通道逐帧覆盖（一个实体 = 整体模型） |
| 几何 | `KHR_mesh_quantization`（归一化整型属性） | 🟡 | accessor normalized 已支持；POSITION 量化未专门测试 |
| 几何 | sparse accessor | ✅ | P6：indices/values 覆盖（支持无 bufferView 零基）；`gltf_runtime_sparse_e2e` 差分 |
| 几何 | EXT_mesh_gpu_instancing | ✅ | P6：TRANSLATION/ROTATION/SCALE 展开为合成子节点（`gltf_runtime_instancing_e2e`） |
| 蒙皮 | `skins` + `JOINTS_0/WEIGHTS_0` + IBM | ✅ | P1：多关节、bind pose palette、`MAQUE_SKIN=0` 对照（`gltf_runtime_anim_e2e` A） |
| 动画 | translation / rotation / scale 通道 | ✅ | P2：STEP / LINEAR / CUBICSPLINE；自动播放首个 clip（`gltf_runtime_anim_e2e` B） |
| 动画 | `weights` 通道（morph 权重） | ✅ | P3：与 morph targets 联动（`gltf_runtime_anim_e2e` C） |
| 动画 | 组件控制（clip/playing/loop/speed/time） | ✅ | P2：`render.MeshRenderer` 新增字段，`time/duration` 运行期回写 |
| Morph | targets（POSITION/NORMAL 增量）、default weights | ✅ | P3：CPU morph + `update_mesh`；`MAQUE_MORPH=0` 对照 |
| 物料 | 核心 PBR（baseColor/MR/emissive/alpha/doubleSided） | ✅ | `gltf_runtime_e2e` |
| 物料 | KHR_materials_unlit / emissive_strength | ✅ | 映射到 Basic / emissive_intensity |
| 物料 | KHR_texture_transform（offset/scale/rotation） | 🟡 | base 组 + normal 组完整；扩展贴图槽共用基础 UV（近似） |
| 物料 | clearcoat / sheen / iridescence / specular / ior / anisotropy | ✅ | P4：因子 + 7 扩展贴图槽（`gltf_runtime_material_e2e` A/B） |
| 物料 | transmission / volume / diffuse_transmission / dispersion | ✅ | P4：近似参数 + 贴图槽（`gltf_runtime_material_e2e` C） |
| 贴图 | PNG / JPEG 解码、sRGB/线性、mip 链、sampler | ✅ | `gltf_runtime_e2e` |
| 贴图 | KTX2 / BasisU（ETC1S、UASTC+Zstd、未压缩 RGBA/RGB/BGRA） | ✅ | image 模块集成 `third_party/basisu`；glTF `KHR_texture_basisu`（`gltf_runtime_ktx2_e2e`） |
| 贴图 | WebP / AVIF | ⛔ | image 模块无解码器（KTX2 已单独接入） |
| 贴图 | KTX2 BC/ASTC/ETC 等块格式、cubemap/array | ⬜ | 目前只解 BasisU + 未压缩 2D 到 RGBA8；块格式需 GPU 原生格式上传 |
| 压缩 | Draco / meshopt | ⛔ | 无解码依赖；需新增第三方库 |
| 灯光 | KHR_lights_punctual | ✅ | P5：方向/点/聚光 → frame3d（强度按模型尺度归一化、方向光优先、≤8；`gltf_runtime_light_e2e` A） |
| 相机 | glTF 相机 | ✅ | P5：场景无 `render.Camera3D` 时用首个 perspective 相机；ortho 回退自动机位（`gltf_runtime_light_e2e` B） |
| 扩展 | KHR_node_visibility | ✅ | P6 静态：visible=false 隐藏整棵子树（mesh/灯/相机）；动画可见性未做 |
| 扩展 | KHR_materials_variants | ✅ | P6：`render.MeshRenderer.variant` 按变体名替换 primitive 材质（`gltf_runtime_variants_e2e`） |
| 扩展 | KHR_animation_pointer | 🟡 | 材质数值属性（baseColor/metallic/roughness/emissive/alphaCutoff + KHR 因子）逐帧更新；节点/贴图/可见性指针跳过（`gltf_runtime_pointer_e2e`） |
| 诊断 | `MAQUE_ANIM/SKIN/MORPH` 开关、`MAQUE_RENDER3D_INFO=1` 日志 | ✅ | 模型/skin/clip/morph/首帧动画状态日志；A/B 对照开关 |

## 2. 分阶段进度

| 阶段 | 内容 | 状态 | 验收 |
|---|---|---|---|
| P1 | 节点层级 + 蒙皮（skins/IBM/palette/`MAQUE_SKIN=0`） | ✅ | `skinned.gltf` 默认与 `MAQUE_SKIN=0` 逐字节一致 |
| P2 | 动画采样（T/R/S、STEP/LINEAR/CUBICSPLINE）+ 组件控制 | ✅ | 1/20/40 帧互不相同、同帧确定；`MAQUE_ANIM=0` 各帧一致 |
| P3 | morph targets + default/动画权重 + `MAQUE_MORPH=0` | ✅ | 默认 vs 关闭 morph 截图不同；动画帧不同、同帧确定 |
| P4 | 材质 KHR 扩展映射（clearcoat/sheen/iridescence/specular/transmission/volume…） | ✅ | 扩展夹具日志/材质字段断言（`gltf_runtime_material_e2e`） |
| P5 | KHR_lights_punctual / glTF 相机 | ✅ | 灯光数/相机参数日志 + 截图（`gltf_runtime_light_e2e`） |
| P6 | instancing ✅ / sparse ✅ / node_visibility（静态）✅ / materials_variants ✅ / animation_pointer（材质数值子集）🟡 | 🟡 | 逐项 e2e（instancing/sparse/visibility/variants/pointer 已有） |

## 3. 关键设计（P1–P3）

- **顶点保留 mesh 空间**（不再把节点世界变换烘焙进顶点）：非蒙皮 primitive 的 `draw.model = 实体世界矩阵 × 节点世界矩阵`；
  蒙皮 primitive 的 `draw.model = 实体世界矩阵`，调色板 `palette[j] = world(joint_j) × inverse_bind_j`（与 shader
  `wp = model × (skin × pos)` 一致）。
- 节点局部 TRS 每帧由动画通道覆盖后按**父先子**顺序累积世界矩阵；无动画时用 glTF 静态 TRS。
- 动画状态按**实体**（不是模型）维护：同一模型多实例可各自播放不同时间/片段。
- morph 每 primitive 保存 CPU 基础顶点 + target 增量；权重变化才 `update_mesh`（与既有 `.mesh.json` morph 同策略）。
- 蒙皮/morph 的共享网格：首实体驱动，多实体不一致时告警（首版；后续可每实体独立上传）。
- 组件：`render.MeshRenderer` 追加 `clip`（string，空=首个）、`playing`、`loop`、`speed`、`time`、`duration`
  （`time`/`duration` 运行期回写，脚本可读写/跳转）。
- 诊断：`MAQUE_ANIM=0`（不采样）、`MAQUE_SKIN=0`（identity palette）、`MAQUE_MORPH=0`（不应用 morph）。
- 确定性：动画时间按固定 `dt` 推进、无墙钟/异步；同帧重复截图逐字节一致。
- **P4 材质扩展**：`gltf::Material` 解析 KHR_materials_{clearcoat,sheen,iridescence,specular,ior,anisotropy,
  transmission,volume,volume_scatter,dispersion,diffuse_transmission,emissive_strength} 的因子与贴图槽；
  runtime 映射到 `mq_renderer_pbr_material`（`textures3[7]/textures4[2]/textures5[1]/textures6[2]/textures7[1]`
  + `flags2` presence 位），`physical` 材质 → `shading_model=1`，unlit → 2；transmission 走既有 alpha BLEND 近似
  （含 MASK 镂空保留 flags2 bit19），diffuse_transmission 走 flags2 bit24。
- **P5 灯光/相机**：`KHR_lights_punctual` 的灯定义 + 节点引用 → 每实例一条 frame3d 灯光；强度观感归一化
  （directional `2.6×`，point/spot 按 `2.6×(dref²+1)`，`dref=max(0.5×模型半径,1e-3)`，上限 20/2000），
  方向光优先并截断到 8 盏。相机：场景无 `render.Camera3D` 时用模型首个 perspective 相机（节点世界矩阵 +
  yfov/znear/zfar）；orthographic 忽略、回退自动机位。
- **P6 instancing**：`EXT_mesh_gpu_instancing` 的 TRANSLATION/ROTATION/SCALE accessor → N 个合成子节点
  （parent = 原节点），该节点 primitive 展开为 N 份（世界 = 原节点世界 × 实例 TRS）；>4096 截断。
- **实现位置**：解析（skins/animations/morph/nodes/materials/lights/cameras/instancing）在
  `modules/runtime/src/gltf_model.{h,cpp}`；实体级状态/采样/调色板/CPU morph/材质映射/灯光相机在
  `modules/runtime/src/runtime.cpp`（`gltf_state_ensure` / `gltf_anim_step` / `apply_gltf_morph` /
  `load_gltf_model` + `extract_3d` glTF 分支）。

## 4. 测试

- `tests/gltf_runtime_anim_e2e.sh`（P1–P3）：夹具 `tests/data/gltf/runtime_anim/`（`skinned/animated/morph.gltf`
  与 `tests/data/gltf/` 同名文件 SHA256 一致，配 3 个场景）。实测（Debug+ASan，2026-10-28）：
  - A 蒙皮：`skins=1`/`joints=2`；非背景 0.108/center 15773；默认 vs `MAQUE_SKIN=0` **逐字节一致**；
  - B 动画：`anims=1`/`clip 'spin'`；第 1/20/40 帧 nonbg 0.059/0.030/0.029，两两像素差 20505/13730/20203；
    同帧重跑逐字节一致；`MAQUE_ANIM=0` 三帧一致；
  - C morph：`morph_prims=1`/`targets=2`；默认 vs `MAQUE_MORPH=0` 差 1953 px；权重动画 f1/f20/f40 两两差
    1739/1877/606 px；同帧重跑逐字节一致。
- `tests/gltf_runtime_material_e2e.sh`（P4）：夹具 `tests/data/gltf/runtime_material/`（`extensions` /
  `ext_material_slots` / `diffuse_volume` 的逐字节副本）。断言：physical/unlit shading 值、clearcoat=0.8 /
  sheen=1 / iridescence=0.6 / specular=0.7 / ior=1.7 / anisotropy=0.45、`tex3=true`/`tex4=3/2`、
  `diffuse_transmission=0.500`/`tex6=0/1`、`transmission=1.000`；三个场景 PPM nonbg=0.166/center=12840。
- `tests/gltf_runtime_light_e2e.sh`（P5 + visibility）：夹具 `tests/data/gltf/runtime_light/`。
  A：隐藏父节点子树后 `lights=9 cameras=0`、灯光实例 ≥9、无 `node=12`、`frame0 draws=1 ... lights=8 mats=1`、
  PPM nonbg=0.063/center=10353；B：无场景相机 → `cams=0`、`camera[0] perspective=1 yfov=0.600`、
  `fov=34.38`、`frame0 draws=1 lights=1`、PPM nonbg=0.029/center=5135。
- `tests/gltf_runtime_instancing_e2e.sh`（P6）：`instancing.gltf`（5 实例）；断言 `primitives=5 nodes=6`、
  `frame0 draws=5 mats=5`、PPM nonbg=0.047/center=3701。
- `tests/gltf_runtime_sparse_e2e.sh`（P6）：`runtime_sparse/sparse.gltf`（POSITION sparse 把顶点 y 0.5→1.5）
  与 `base.gltf`（无 sparse）差分；两帧均非空，PPM 差异 28380 字节（>1000），证明覆盖生效。
- `tests/gltf_runtime_variants_e2e.sh`（P6）：`variants.gltf`（红默认/绿变体 unlit 三角形）；
  `render.MeshRenderer.variant="green"` 后中心色 `(243,108,89)` → `(89,243,124)`，PPM 差 70296 字节。
- `tests/gltf_runtime_pointer_e2e.sh`（P6）：`pointer.gltf` 的 pointer 通道动画
  `/materials/0/pbrMetallicRoughness/baseColorFactor`（红 → 绿）；f1 中心 `(240,116,90)`、f40 `(160,213,114)`，
  PPM 差 70296；日志 `gltf anim 'color_anim' channels=1`。
- `tests/gltf_runtime_ktx2_e2e.sh`（KTX2）：`runtime_ktx2/` 的 ETC1S / UASTC+Zstd / 未压缩 RGBA
  三套 `KHR_texture_basisu` glTF；断言 `textures=1`、纹理 id≥0、PPM 同时有红/绿主导像素；
- 既有 `gltf_runtime_e2e`（静态/容器）保持通过；离线管线 `gltf_skin_e2e`/`gltf_anim_e2e`/`gltf_morph_e2e`
  与 `render3d_scene_e2e` 不回归。

## 5. 后续 / 风险

- **P6 剩余 / 已知边界**：`KHR_animation_pointer` 只做**材质数值属性**；贴图槽、节点/可见性指针跳过。
  `KHR_node_visibility` 只支持静态 `visible=false`，动画驱动可见性未做。
  `KHR_materials_variants` 通过组件 `variant` 选择，无脚本/控制面切换 API。
- **⛔ 明确不做**：WebP / AVIF 贴图、Draco / meshopt 压缩（需要额外解码依赖）；KTX2 的 BC/ASTC/ETC
  块格式与 cubemap/array 暂不做（当前统一转 RGBA8，2D + BasisU/未压缩）。
- 蒙皮 + 动态 `scene_instantiate` / 热重载场景重建：目前只在装配期 `setup_gltf_assets` 建实体级状态；
  动态创建/重载的 glTF 实体在首次 extract 时惰性 `gltf_state_ensure`（bind pose），动画下一帧接上。
- 共享网格的 morph 首实体驱动（多实体不同权重会告警并只画首实体权重）；后续可每实体独立上传。
- 蒙皮网格的 shadow bounds 仍是 bind pose 包围盒（与离线管线一致）；大幅动画时阴影拟合可能偏紧。
- 材质扩展的逐槽 UV/纹理 transform 仍近似（扩展槽共用基础 UV 组）；`anisotropy_rotation` 与 transmission
  近似共用同一槽（两者同时存在时 transmission 覆盖）。
- glTF 相机只在**场景没有** `render.Camera3D` 时生效；多模型各有相机时取实体 id 序第一个；
  `KHR_lights_punctual` 强度归一化是**观感近似**（未按物理 candela/lux 精确换算）。
