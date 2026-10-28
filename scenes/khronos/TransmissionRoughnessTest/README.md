# TransmissionRoughnessTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TransmissionRoughnessTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TransmissionRoughnessTest)  
**上游概览**：Tests the interaction between roughness and IOR.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2021

> 本目录由 `python tools/migrate_gltf_models.py --only TransmissionRoughnessTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TransmissionRoughnessTest.gltf` |
| 节点 / 网格 / primitive | 10 / 9 / 9 |
| 顶点 / 三角形 | 47,260 / 77,792 |
| 材质 / 纹理 / 图像 | 9 / 9 / 5 |
| 图像编码 | png×5 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_ior`、`KHR_materials_volume` |
| alpha 模式分布 | OPAQUE×9 |

**导入产物**：`assets/` 9 个 `.mesh.json`（合计 5.0 MB），`textures/` 5 张；场景 `TransmissionRoughnessTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TransmissionRoughnessTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TransmissionRoughnessTest/glTF/TransmissionRoughnessTest.gltf --out scenes/khronos/TransmissionRoughnessTest --name TransmissionRoughnessTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TransmissionRoughnessTest/TransmissionRoughnessTest.scn
./build-win/samples/player/maque_player --shot /tmp/TransmissionRoughnessTest.png scenes/khronos/TransmissionRoughnessTest/TransmissionRoughnessTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot-large.png`（1673×831）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.538 | 0.603 | 差 +0.065 |
| 前景平均 RGB | [137.5, 135.2, 155.2] | [156.1, 152.8, 155.7] | RGB 距离 0.048 |
| 亮度直方图相似度 | — | — | 0.66（16 桶交集，1=完全一致） |
| 前景占比 | 92.1% | 32.6% | 覆盖率比 0.354 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）

## 踩过的坑 / 注意

- 官方 HDRI 环境：Footprint Court（CC-BY-NC-SA-3.0，env_intensity=0.87；仅 IBL 不开背景）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
