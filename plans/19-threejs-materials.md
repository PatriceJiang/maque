# 19 · three.js 材质迁移（多着色模型）

> 上游：`plans/18-pbr-3d.md`（PBR/3D 渲染基础设施：3D pass、`asset.PbrMaterial`、贴图、光照、阴影、透明）。
> 参考：three.js 文档 Materials 组（<https://threejs.org/docs/>）与 `dev` 分支源码
> `src/materials/*.js`、`src/materials/nodes/*.js`（Node 变体）。
> 本文把 three.js 的**材质种类**按语义迁移到 maque 原生实现（schema 组件 + C ABI + WGSL + runtime + glTF 导入），
> 不是移植 JS/TSL 代码，也不实现节点图编译器。

## 1. 目标与非目标

**目标**：一个 3D 表面材质组件支持 10 种着色模型，参数语义/观感对齐 three.js；glTF 材质扩展（clearcoat/sheen/
iridescence/specular/ior/anisotropy/unlit/emissive_strength）可在离线导入时落成对应字段；每种模型有 e2e 鉴别力断言。

**非目标（本期明确不做，避免范围膨胀）**：
- TSL 节点图（`NodeMaterial` 的 `colorNode/positionNode/...`）、`ShaderMaterial/RawShaderMaterial` 自定义 shader；
- transmission/refraction（需要 backbuffer 折射 pass）、IBL/env cubemap（引擎无 IBL，环境用常量近似）；
- `wireframe`、`vertexColors`、`fog`、`lightMap`、bumpMap、displacementMap、alphaMap、clipping/stencil；
- Points/Line/Sprite 图元材质（需要渲染器 topology/billboard 支持，另立计划）；
- `MeshDistanceMaterial`（无点光阴影）、`MeshSSSNodeMaterial`、`VolumeNodeMaterial`、`Line2NodeMaterial`。

## 2. 映射表

| three.js（经典 / Node） | `shading_model` | 实现入口 | 备注 |
|---|---|---|---|
| MeshStandardMaterial / MeshStandardNodeMaterial | 0 Standard | `fs_standard` | 既有 PBR 路径（GGX + ACES + 8 灯 + 阴影 + clearcoat） |
| MeshPhysicalMaterial / MeshPhysicalNodeMaterial | 1 Physical | `fs_physical` | + clearcoat/sheen/iridescence/specular+IOR/anisotropy；transmission 不做 |
| MeshBasicMaterial / MeshBasicNodeMaterial | 2 Basic | `fs_basic` | 无光照，base_color × albedo |
| MeshLambertMaterial / MeshLambertNodeMaterial | 3 Lambert | `fs_lambert` | 漫反射 + 环境 + emissive |
| MeshPhongMaterial / MeshPhongNodeMaterial | 4 Phong | `fs_phong` | Blinn-Phong：specular / shininess |
| MeshToonMaterial / MeshToonNodeMaterial | 5 Toon | `fs_toon` | NdotL 分档；gradient_texture 或 toon_steps |
| MeshNormalMaterial / MeshNormalNodeMaterial | 6 Normal | `fs_normal` | 视空间法线 → RGB，不做 tonemap |
| MeshMatcapMaterial / MeshMatcapNodeMaterial | 7 Matcap | `fs_matcap` | view-space 法线查 matcap × base_color |
| MeshDepthMaterial | 8 Depth | `fs_depth` | BasicDepthPacking 灰度 / RGBA 打包 |
| ShadowMaterial / ShadowNodeMaterial | 9 Shadow | `fs_shadow` | 只显示方向光阴影的透明材质；不投射阴影 |

Node 变体在 maque 中与对应基础模型等价（Node 是 three.js 的构造方式，不是另一种视觉模型）。

**Material 基类属性取舍**：支持 `opacity`、`transparent`（→ `alpha_mode=Blend`）、`alphaTest`
（→ `alpha_cutoff`+`alpha_mode=Mask`）、`side`、`flatShading`、`emissiveIntensity`、`envMapIntensity`（缩放环境近似项）、
`toneMapped`（Normal/Depth/Shadow 不做 ACES）。其余见 §1 非目标。

## 3. 设计决策

1. **组件**：沿用 `asset.PbrMaterial`（`roles: [pbr_material]`），尾部追加 `shading_model` 与各模型参数；
   缺省 `Standard`，旧场景/旧资产零变化。不改组件名/C ABI 记录名（renderer v1 只允许表尾追加；
   改名留作未来独立 ABI 版本迁移）。
2. **shader**：`assets/builtin/shaders/pbr/` 拆成按文件名排序的多个 `*.wgsl`（类型→工具→表面→光照→顶点→入口），
   一个模块 10 个 fragment 入口；gfx 按 `shading_model` 选 entry point。
3. **管线**：gfx 按 `(shading_model, blend, side)` 懒创建管线缓存（≤10×2×3）；`side` 决定 cullMode
   （Front 剔背面 / Back 剔正面 / Double 不剔）。缺省 `side=Double` 与引擎此前 `cullMode=None` 一致。
4. **材质 uniform**：`Draw` 扩到 64 floats / 256B（新增 model_params/phys0/phys1/phys2/misc/specular/sheen_color），
   gfx 用单一 `pack_draw_uniform()` 打包（主 pass 与阴影 pass 共用，防字段漂移）。
5. **贴图槽**：`group1` 扩到 10 项：uniform + 8 贴图 + sampler。新增
   `matcap_texture`（sRGB）/`gradient_texture`/`clearcoat_normal_texture`；缺省法线贴图绑 1×1 中性法线
   `(0.5,0.5,1)`（修复旧版绑纯白导致法线扰动的隐患）。
6. **camera uniform**：720→784B，尾部追加 `view` 矩阵（matcap 需要 view-space 法线）。
7. **贴图路径**：沿用稳定 VFS 路径字符串（`builtin:/...` 或资产相对路径）；内置程序化纹理
   `builtin:/textures/toon_ramp`（256×1 四档灰阶）、`builtin:/textures/matcap_studio`（256×256 冷色相影棚球）。
8. **glTF 导入**：新增 KHR 扩展映射（见 §5），只写非默认字段 → 旧 `.mesh.json` 逐字节不变。

## 4. 字段表（`asset.PbrMaterial` 追加；顺序即 ABI 追加顺序）

| 字段 | 类型 | 默认 | 语义 |
|---|---|---|---|
| `shading_model` | MaterialShadingModel | Standard | 0..9 见 §2 |
| `opacity` | float | 1.0 | × base_color.a × albedo.a |
| `side` | MaterialSide | Double | Front/Back/Double → cullMode |
| `flat_shading` | bool | false | 屏幕导数求面法线 |
| `emissive_intensity` | float | 1.0 | × emissive × emissive_texture |
| `env_intensity` | float | 1.0 | 缩放常量环境项 |
| `normal_scale_y` | float | 1.0 | normalScale.y（x 复用既有 `normal_scale`） |
| `specular` | math.Vec3 | (1,1,1) | Phong specular / Physical specularColor |
| `shininess` | float | 30 | Phong |
| `specular_intensity` | float | 1.0 | Physical（KHR_materials_specular） |
| `ior` | float | 1.5 | Physical（KHR_materials_ior） |
| `sheen` / `sheen_roughness` / `sheen_color` | float/float/Vec3 | 0/0.5/(0,0,0) | Physical 边缘光 |
| `iridescence` / `iridescence_ior` / `iridescence_thickness_min` / `max` | float×4 | 0/1.3/100/400 | Physical 薄膜干涉 |
| `anisotropy` / `anisotropy_rotation` | float | 0 | Physical（观感近似） |
| `clearcoat_normal_scale` | float | 1.0 | 清漆法线强度 |
| `toon_steps` | int32_t | 3 | 无 gradient_texture 时的分档数 |
| `depth_packing` | DepthPacking | Basic | Depth：Basic 灰度 / RGBA 打包 |
| `matcap_texture` | string | "" | Matcap 贴图路径 |
| `gradient_texture` | string | "" | Toon 渐变图路径 |
| `clearcoat_normal_texture` | string | "" | 清漆法线贴图路径 |

renderer 记录 `pbr_material` 同步追加扁平字段 + `textures2[3]`（matcap/gradient/clearcoat-normal GPU 句柄，-1=无）
+ `flags2`（bit0 flat_shading / bit1 has_matcap / bit2 has_gradient / bit3 has_clearcoat_normal）。

## 5. glTF 扩展导入（`tools/gltf_import.py`）

| 扩展 | 映射 |
|---|---|
| `KHR_materials_unlit` | `shading_model=2`（Basic） |
| `KHR_materials_clearcoat` | `clearcoat` / `clearcoat_roughness` / `clearcoat_normal_texture` / `clearcoat_normal_scale` |
| `KHR_materials_sheen` | `sheen=1` / `sheen_color` / `sheen_roughness` |
| `KHR_materials_iridescence` | `iridescence` / `iridescence_ior` / `iridescence_thickness_min/max` |
| `KHR_materials_specular` | `specular_intensity` / `specular` |
| `KHR_materials_ior` | `ior` |
| `KHR_materials_anisotropy` | `anisotropy` / `anisotropy_rotation` |
| `KHR_materials_emissive_strength` | `emissive_intensity` |

有任一 physical 扩展 → `shading_model=1`，否则 0；未给扩展的材质不写新字段（旧资产零漂移）。

## 6. 里程碑与状态

| # | 内容 | 状态 |
|---|---|---|
| M0 | 计划冻结 + 基线（81/81 ctest、materials/ToyCar 基线截图哈希） | ✅ |
| M1 | schema/ABI 字段 + camera 784B + 材质 uniform 256B + 管线缓存 + runtime 提取 + shader 拆分/10 入口 + 内置纹理 | ✅ |
| M2 | 非 PBR 模型场景/断言：Basic/Lambert/Phong/Toon/Normal/Matcap/Depth/Shadow + `material_models.scn` | ✅ |
| M3 | Physical 扩展 + glTF 扩展导入 + `material_physical.scn` / `material_shadow.scn` 断言 | ✅ |
| M4 | Points/Line/Sprite 图元材质 | ⏸ 未批准（需渲染器 topology/billboard 支持，另立计划） |
| M5 | 文档/AGENTS/画廊预览/全量回归 | ✅（`scenes/pbr/material_models.png` / `material_physical.png` + ToyCar `preview.png` 已按最终构建重生成） |

### 6.1 附带修复（M1 期间发现并修复）

1. **法线贴图缺省值错误**：旧 gfx 在所有贴图槽缺省时绑 1×1 白纹理；法线贴图槽用白纹理解码为
   `(1,1,1)` 而不是中性法线 `(0,0,1)`，导致**没有 normal_map 的材质法线被扰动**。
   修复：新增 1×1 中性法线 `(0.5,0.5,1)` 作为法线/清漆法线槽缺省；`render3d_scene_e2e` ③ 与基线随之更新。
2. **阴影两处数学 bug**（M5c 遗留；由材质 e2e 的 Phong 断言暴露）：
   - `mat4_look_at` 视空间 z 为负，正交投影传参把近/远平面写反 → 深度反向，几乎整个 shadow 视锥被误判为遮挡；
   - `shadow_factor` 采样 uv.y 未翻（WebGPU NDC +y = 纹理 v=0）→ 阴影镜像。
   修复后 `material_shadow_e2e` 增加**空间一致性断言**（投影单连通、质心/展布靠近物体），
   `render3d_scene_e2e` ⑨ 改为双边界（有真实投影 ≤ 25% 像素），防止"整视锥误遮挡"回归。
3. 上述两项会小幅改变旧场景观感（materials 6.9%、ToyCar 7.1% 像素，均已定位为上述修复）；
   M1 终态基线见 `build-win/baseline-m1/`（构建产物，不入库）。

## 7. 验收（DoD）

- `codegen_drift` / `schema_imports` / `builtin_shaders` 全绿；generated 入库；
- 全量 ctest（81 基线 + 新增材质 e2e）全绿；2D/SDL/文字/ImGui 零回归；
- 既有场景截图（Standard 路径）变化仅来自 §6.1 两项修复（materials 6.9% / ToyCar 7.1% 像素）；
- `material_models_e2e`：9 模型逐个有鉴别力断言（实测 Basic std=0.0 / Lambert std=26.6 /
  Phong max=254 vs Lambert 210 / Toon bins=5 / Normal Δhue=179° / Matcap b-r=+52.8 / Depth gray=1.00）；
- `material_shadow_e2e`：MAQUE_SHADOW 开/关变暗 2850 px、反向 0、投影单连通块占比 1.00 + 质心/展布约束；
- `material_physical_e2e`：Physical 两角度 hueΔ=31° vs Standard 0°；
- `gltf_import_e2e`：扩展夹具字段断言 + 逐字节确定性 + 旧无扩展资产零漂移（ToyCar 资产按扩展重生成）；
- 性能：`MAQUE_TRACE_STATS` 对比 M0 无明显劣化。

## 8. 风险

| 风险 | 对策 |
|---|---|
| 多入口 shader 校验失败（wgpu 管线创建期才报错） | 一次一个入口小步验证；gfx 对失败管线打印一次并跳过绘制，不崩 |
| 材质 uniform/贴图绑定错位 | 单一打包函数 + `static_assert` 尺寸 + camera/draw BGL minBindingSize 同步 |
| 法线贴图缺省值错误 | 缺省绑 1×1 中性法线（(0.5,0.5,1)），旧白纹理问题一并修复 |
| 透明/阴影材质互相影响 | ShadowMaterial 不参与 shadow pass（只接收）；blend 排序沿用 M5a |
| e2e 像素断言跨 GPU 不稳 | 结构性/对照断言，容差宽松，固定相机/帧号；场景对象不重叠按区域聚类 |
| ABI 追加破坏旧插件 | 保持 renderer v1 只追加；旧字段偏移不变；生成物重生成 |

## 9. Owner

- **角色**：`agent-render3d`（与 `agent-renderer` 协作：gfx 管线/ABI 归 renderer，提取/资产归 runtime）。
- **职责边界**：不改 2D 组件语义与脚本可见 API（新参数通过已有组件反射读写，无需新脚本 API）。

## 10. 用法示例（`.scn` 片段）

材质是**节点组件**（不是独立资产文件）；贴图路径相对场景目录（运行时 VFS 根 = 场景目录）；
枚举写 JSON 数字。完整画廊见 `scenes/pbr/material_models.scn`（9 个球，Standard..Depth）。

```json
// Standard（默认；不写 shading_model）
{ "type": "asset.PbrMaterial",
  "base_color": { "r": 0.88, "g": 0.63, "b": 0.25, "a": 1.0 },
  "metallic": 0.15, "roughness": 0.5,
  "base_color_texture": "builtin:/textures/checker" }

// Physical：clearcoat + sheen + iridescence
{ "type": "asset.PbrMaterial", "shading_model": 1,
  "base_color": { "r": 0.5, "g": 0.5, "b": 0.5, "a": 1.0 }, "roughness": 0.4,
  "clearcoat": 1.0, "clearcoat_roughness": 0.03,
  "sheen": 0.8, "sheen_color": { "x": 0.2, "y": 0.35, "z": 1.0 },
  "iridescence": 1.0, "iridescence_ior": 1.3 }

// Toon：4 档渐变（gradient_texture 可省，改用 toon_steps 量化）
{ "type": "asset.PbrMaterial", "shading_model": 5,
  "base_color": { "r": 0.88, "g": 0.63, "b": 0.25, "a": 1.0 },
  "toon_steps": 4, "gradient_texture": "builtin:/textures/toon_ramp" }

// Matcap
{ "type": "asset.PbrMaterial", "shading_model": 7,
  "base_color": { "r": 0.88, "g": 0.92, "b": 1.0, "a": 1.0 },
  "matcap_texture": "builtin:/textures/matcap_studio" }

// Shadow catcher（透明地面只显示阴影）
{ "type": "asset.PbrMaterial", "shading_model": 9,
  "base_color": { "r": 0.0, "g": 0.0, "b": 0.0, "a": 1.0 }, "alpha_mode": 2 }
```

测试：`ctest --test-dir build-win -R 'material_.*_e2e' --output-on-failure`（三套：模型画廊/阴影 catcher/
Physical vs Standard + iridescence 视角偏色）；场景缺失时脚本自身 SKIP 77。
