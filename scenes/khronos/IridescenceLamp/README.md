# IridescenceLamp —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/IridescenceLamp](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/IridescenceLamp)  
**上游概览**：Wayfair Lamp model using transmission, volume, and KHR_materials_iridescence.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2022

> 本目录由 `python tools/migrate_gltf_models.py --only IridescenceLamp` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `IridescenceLamp.gltf` |
| 节点 / 网格 / primitive | 3 / 3 / 3 |
| 顶点 / 三角形 | 10,683 / 18,866 |
| 材质 / 纹理 / 图像 | 3 / 3 / 3 |
| 图像编码 | png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_iridescence`、`KHR_materials_ior` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 1.2 MB），`textures/` 3 张；场景 `IridescenceLamp.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only IridescenceLamp --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/IridescenceLamp/glTF/IridescenceLamp.gltf --out scenes/khronos/IridescenceLamp --name IridescenceLamp --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/IridescenceLamp/IridescenceLamp.scn
./build-win/samples/player/maque_player --shot /tmp/IridescenceLamp.png scenes/khronos/IridescenceLamp/IridescenceLamp.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（681×1213）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.284 | 0.261 | 差 -0.023 |
| 前景平均 RGB | [77.5, 70.8, 70.8] | [69.2, 66.8, 54.6] | RGB 距离 0.037 |
| 亮度直方图相似度 | — | — | 0.64（16 桶交集，1=完全一致） |
| 前景占比 | 67.8% | 11.2% | 覆盖率比 0.165 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）

## 踩过的坑 / 注意

- 官方 HDRI 环境：Footprint Court（CC-BY-NC-SA-3.0，env_intensity=1.00；仅 IBL 不开背景）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
