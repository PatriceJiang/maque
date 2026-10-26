# GlassVaseFlowers —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/GlassVaseFlowers](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/GlassVaseFlowers)  
**上游概览**：This model compares transparency methods for representing glass in glTF: alphaMode:'BLEND' (left) versus the extensions KHR_materials_transmission and KHR_materials_volume (right).
**标签**：`video`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Eric Chadwick 2023
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Rico Cilliers 2023

> 本目录由 `python tools/migrate_gltf_models.py --only GlassVaseFlowers` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `GlassVaseFlowers.gltf` |
| 节点 / 网格 / primitive | 4 / 3 / 3 |
| 顶点 / 三角形 | 5,989 / 10,218 |
| 材质 / 纹理 / 图像 | 3 / 4 / 4 |
| 图像编码 | jpeg×2、png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume` |
| alpha 模式分布 | BLEND×1、MASK×1、OPAQUE×1 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 660.0 KB），`textures/` 3 张；场景 `GlassVaseFlowers.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only GlassVaseFlowers --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/GlassVaseFlowers/glTF/GlassVaseFlowers.gltf --out scenes/khronos/GlassVaseFlowers --name GlassVaseFlowers --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/GlassVaseFlowers/GlassVaseFlowers.scn
./build-win/samples/player/maque_player --shot /tmp/GlassVaseFlowers.png scenes/khronos/GlassVaseFlowers/GlassVaseFlowers.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（1186×1144）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.400 | 0.377 | 差 -0.024 |
| 前景平均 RGB | [103.4, 103.0, 90.0] | [93.4, 98.0, 83.5] | RGB 距离 0.028 |
| 亮度直方图相似度 | — | — | 0.66（16 桶交集，1=完全一致） |
| 前景占比 | 94.7% | 10.6% | 覆盖率比 0.112 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
