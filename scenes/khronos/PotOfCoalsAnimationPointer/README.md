# PotOfCoalsAnimationPointer —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/PotOfCoalsAnimationPointer](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/PotOfCoalsAnimationPointer)  
**上游概览**：Burnt copper pot with a clearcoat, containing emissive hot coals, and animated heat refraction.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2024
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2015
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only PotOfCoalsAnimationPointer` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `PotOfCoalsAnimationPointer.gltf` |
| 节点 / 网格 / primitive | 3 / 3 / 3 |
| 顶点 / 三角形 | 54,933 / 18,311 |
| 材质 / 纹理 / 图像 | 3 / 10 / 10 |
| 图像编码 | jpeg×9、png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 动画导入（plans/22） | clip 0 / skin 0 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| 使用扩展 | `KHR_animation_pointer`、`KHR_materials_clearcoat`、`KHR_materials_specular`、`KHR_materials_transmission`、`KHR_materials_volume`、`KHR_texture_transform` |
| 必需扩展 | `KHR_materials_specular`、`KHR_materials_transmission`、`KHR_materials_volume`、`KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 5.0 MB），`textures/` 10 张；场景 `PotOfCoalsAnimationPointer.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only PotOfCoalsAnimationPointer --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/PotOfCoalsAnimationPointer/glTF/PotOfCoalsAnimationPointer.gltf --out scenes/khronos/PotOfCoalsAnimationPointer --name PotOfCoalsAnimationPointer --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/PotOfCoalsAnimationPointer/PotOfCoalsAnimationPointer.scn
./build-win/samples/player/maque_player --shot /tmp/PotOfCoalsAnimationPointer.png scenes/khronos/PotOfCoalsAnimationPointer/PotOfCoalsAnimationPointer.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1119×983）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.338 | 0.333 | 差 -0.006 |
| 前景平均 RGB | [103.7, 82.9, 70.6] | [113.7, 79.1, 56.8] | RGB 距离 0.036 |
| 亮度直方图相似度 | — | — | 0.53（16 桶交集，1=完全一致） |
| 前景占比 | 91.8% | 15.0% | 覆盖率比 0.164 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- KHR_animation_pointer 动画未导入（材质/贴图变换/可见性等指针通道；节点 TRS/骨骼/morph 动画已支持）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- KHR_animation_pointer 动画未导入（材质/贴图变换/可见性等指针通道；节点 TRS/骨骼/morph 动画已支持）

## 踩过的坑 / 注意

- gltf_import: 跳过片段 'animation0'（无有效轨道；pointer 通道已跳过）
- gltf_import: 跳过 KHR_animation_pointer 通道 2 条（片段 0 个）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
