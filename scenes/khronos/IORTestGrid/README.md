# IORTestGrid —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/IORTestGrid](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/IORTestGrid)  
**上游概览**：Grid of spheres over a checkered backdrop to test IOR rendering.
**标签**：`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Eric Chadwick 2023

> 本目录由 `python tools/migrate_gltf_models.py --only IORTestGrid` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `IORTestGrid.gltf` |
| 节点 / 网格 / primitive | 23 / 23 / 23 |
| 顶点 / 三角形 | 92,835 / 30,945 |
| 材质 / 纹理 / 图像 | 23 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_specular`、`KHR_materials_ior` |
| alpha 模式分布 | OPAQUE×23 |

**导入产物**：`assets/` 23 个 `.mesh.json`（合计 7.4 MB），`textures/` 1 张；场景 `IORTestGrid.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only IORTestGrid --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/IORTestGrid/glTF/IORTestGrid.gltf --out scenes/khronos/IORTestGrid --name IORTestGrid --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/IORTestGrid/IORTestGrid.scn
./build-win/samples/player/maque_player --shot /tmp/IORTestGrid.png scenes/khronos/IORTestGrid/IORTestGrid.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（991×1080）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.480 | 0.350 | 差 -0.130 |
| 前景平均 RGB | [132.9, 120.2, 112.7] | [91.7, 88.7, 86.2] | RGB 距离 0.130 |
| 亮度直方图相似度 | — | — | 0.60（16 桶交集，1=完全一致） |
| 前景占比 | 93.4% | 16.4% | 覆盖率比 0.176 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_specular：factor/color/specularTexture/specularColorTexture 已映射（贴图仅作用于电介质 F0；f90 行为未实现）

## 踩过的坑 / 注意

- 官方 HDRI 环境：Colorful Studio（CC-BY-4.0，env_intensity=1.64；仅 IBL 不开背景）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
