# AnimationPointerUVs —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/AnimationPointerUVs](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/AnimationPointerUVs)  
**上游概览**：This model tests UV transform animations with KHR_animation_pointer.
**标签**：`extension`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2024

> 本目录由 `python tools/migrate_gltf_models.py --only AnimationPointerUVs` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `AnimationPointerUVs.gltf` |
| 节点 / 网格 / primitive | 119 / 106 / 132 |
| 顶点 / 三角形 | 178,821 / 59,607 |
| 材质 / 纹理 / 图像 | 82 / 61 / 13 |
| 图像编码 | png×13 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 11 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_specular`、`KHR_materials_sheen`、`KHR_materials_clearcoat`、`KHR_texture_transform`、`KHR_animation_pointer`、`KHR_materials_anisotropy`、`KHR_materials_iridescence`、`KHR_materials_diffuse_transmission`、`KHR_materials_unlit`、`KHR_lights_punctual` |
| 必需扩展 | `KHR_materials_unlit`、`KHR_lights_punctual` |
| alpha 模式分布 | BLEND×3、MASK×9、OPAQUE×70 |

**导入产物**：`assets/` 132 个 `.mesh.json`（合计 11.7 MB），`textures/` 11 张；场景 `AnimationPointerUVs.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only AnimationPointerUVs --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/AnimationPointerUVs/glTF/AnimationPointerUVs.gltf --out scenes/khronos/AnimationPointerUVs --name AnimationPointerUVs --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/AnimationPointerUVs/AnimationPointerUVs.scn
./build-win/samples/player/maque_player --shot /tmp/AnimationPointerUVs.png scenes/khronos/AnimationPointerUVs/AnimationPointerUVs.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（2322×1237）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.471 | 0.456 | 差 -0.015 |
| 前景平均 RGB | [110.0, 124.2, 111.8] | [102.5, 120.0, 119.5] | RGB 距离 0.025 |
| 亮度直方图相似度 | — | — | 0.65（16 桶交集，1=完全一致） |
| 前景占比 | 40.7% | 13.1% | 覆盖率比 0.323 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- KHR_materials_diffuse_transmission 近似为 alpha BLEND（乳浊半透观感，无散射）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- KHR_materials_anisotropy：strength/贴图已导入、只做粗糙度近似（各向异性 BRDF/方向未实现）
- KHR_animation_pointer 动画未导入
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- KHR_materials_diffuse_transmission 近似为 alpha BLEND（乳浊半透观感，无散射）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- KHR_materials_anisotropy：strength/贴图已导入、只做粗糙度近似（各向异性 BRDF/方向未实现）
- KHR_animation_pointer 动画未导入
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
