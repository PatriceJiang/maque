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
| 动画导入（plans/22） | clip 0 / skin 0 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_specular`、`KHR_materials_sheen`、`KHR_materials_clearcoat`、`KHR_texture_transform`、`KHR_animation_pointer`、`KHR_materials_anisotropy`、`KHR_materials_iridescence`、`KHR_materials_diffuse_transmission`、`KHR_materials_unlit`、`KHR_lights_punctual` |
| 必需扩展 | `KHR_materials_unlit`、`KHR_lights_punctual` |
| alpha 模式分布 | BLEND×3、MASK×9、OPAQUE×70 |

**导入产物**：`assets/` 132 个 `.mesh.json`（合计 11.7 MB），`textures/` 13 张；场景 `AnimationPointerUVs.scn`。

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

参考图：`screenshot/screenshot_Large.jpg`（2322×1237）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.471 | 0.424 | 差 -0.048 |
| 前景平均 RGB | [110.0, 124.2, 111.8] | [95.0, 111.6, 111.1] | RGB 距离 0.037 |
| 亮度直方图相似度 | — | — | 0.63（16 桶交集，1=完全一致） |
| 前景占比 | 40.7% | 14.9% | 覆盖率比 0.366 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_diffuse_transmission：不透明材质 + 背光透射（factor/colorFactor + 透射率/透射色贴图 + volume 吸收）；无真实散射/厚度（能量拆分按可加近似）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- KHR_materials_anisotropy：各向异性 GGX（Khronos 规范实现，strength/旋转/贴图方向）+ 环境反射 bent-normal 近似；非真实多次反射
- KHR_animation_pointer 动画未导入（材质/贴图变换/可见性等指针通道；节点 TRS/骨骼/morph 动画已支持）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_diffuse_transmission：不透明材质 + 背光透射（factor/colorFactor + 透射率/透射色贴图 + volume 吸收）；无真实散射/厚度（能量拆分按可加近似）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- KHR_materials_anisotropy：各向异性 GGX（Khronos 规范实现，strength/旋转/贴图方向）+ 环境反射 bent-normal 近似；非真实多次反射
- KHR_animation_pointer 动画未导入（材质/贴图变换/可见性等指针通道；节点 TRS/骨骼/morph 动画已支持）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- gltf_import: 跳过片段 'animation0'（无有效轨道；pointer 通道已跳过）
- gltf_import: 跳过 KHR_animation_pointer 通道 103 条（片段 0 个）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
