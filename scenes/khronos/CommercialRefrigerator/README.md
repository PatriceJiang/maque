# CommercialRefrigerator —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CommercialRefrigerator](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CommercialRefrigerator)  
**上游概览**：Commercial refrigerator with glass condensation, champagne bottles, and door animation.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2025
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Sean Thomas 2025

> 本目录由 `python tools/migrate_gltf_models.py --only CommercialRefrigerator` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CommercialRefrigerator.gltf` |
| 节点 / 网格 / primitive | 6 / 6 / 8 |
| 顶点 / 三角形 | 116,820 / 208,679 |
| 材质 / 纹理 / 图像 | 6 / 10 / 10 |
| 图像编码 | jpeg×10 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 动画导入（plans/22） | clip 1 / skin 0 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| 使用扩展 | `KHR_materials_clearcoat`、`KHR_materials_transmission` |
| 必需扩展 | `KHR_materials_transmission` |
| alpha 模式分布 | OPAQUE×6 |

**导入产物**：`assets/` 8 个 `.mesh.json`（合计 13.7 MB），`textures/` 10 张；场景 `CommercialRefrigerator.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CommercialRefrigerator --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CommercialRefrigerator/glTF/CommercialRefrigerator.gltf --out scenes/khronos/CommercialRefrigerator --name CommercialRefrigerator --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CommercialRefrigerator/CommercialRefrigerator.scn
./build-win/samples/player/maque_player --shot /tmp/CommercialRefrigerator.png scenes/khronos/CommercialRefrigerator/CommercialRefrigerator.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（639×1244）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.363 | 0.357 | 差 -0.006 |
| 前景平均 RGB | [100.7, 91.3, 83.9] | [90.7, 91.4, 89.2] | RGB 距离 0.020 |
| 亮度直方图相似度 | — | — | 0.66（16 桶交集，1=完全一致） |
| 前景占比 | 97.0% | 15.2% | 覆盖率比 0.157 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似

## 踩过的坑 / 注意

- 动画已导入：1 个片段（场景自动播放首个；预览=第 1 帧姿态）
- 官方 HDRI 环境：Colorful Studio（CC-BY-4.0，env_intensity=1.00；仅 IBL 不开背景）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
