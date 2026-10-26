# ChairDamaskPurplegold —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/ChairDamaskPurplegold](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ChairDamaskPurplegold)  
**上游概览**：Use of sheen to create a high-end small seat (damask).
**标签**：`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2021

> 本目录由 `python tools/migrate_gltf_models.py --only ChairDamaskPurplegold` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `ChairDamaskPurplegold.gltf` |
| 节点 / 网格 / primitive | 11 / 11 / 11 |
| 顶点 / 三角形 | 6,275 / 9,984 |
| 材质 / 纹理 / 图像 | 4 / 9 / 9 |
| 图像编码 | jpeg×9 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_texture_transform`、`KHR_materials_sheen`、`KHR_materials_specular` |
| alpha 模式分布 | OPAQUE×4 |

**导入产物**：`assets/` 11 个 `.mesh.json`（合计 686.0 KB），`textures/` 9 张；场景 `ChairDamaskPurplegold.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only ChairDamaskPurplegold --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/ChairDamaskPurplegold/glTF/ChairDamaskPurplegold.gltf --out scenes/khronos/ChairDamaskPurplegold --name ChairDamaskPurplegold --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/ChairDamaskPurplegold/ChairDamaskPurplegold.scn
./build-win/samples/player/maque_player --shot /tmp/ChairDamaskPurplegold.png scenes/khronos/ChairDamaskPurplegold/ChairDamaskPurplegold.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1280×970）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.335 | 0.179 | 差 -0.156 |
| 前景平均 RGB | [106.5, 75.4, 122.3] | [68.9, 35.5, 77.2] | RGB 距离 0.160 |
| 亮度直方图相似度 | — | — | 0.68（16 桶交集，1=完全一致） |
| 前景占比 | 48.9% | 11.2% | 覆盖率比 0.228 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 部分贴图使用 TEXCOORD_1/更高 UV 集（引擎只用 TEXCOORD_0 → 贴图 UV 可能错误）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_sheen：sheen 因子/颜色为观感近似，sheen 贴图未实现
- KHR_materials_specular：specularFactor/specularColor 已映射，f90 行为与贴图未实现
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 部分贴图使用 TEXCOORD_1/更高 UV 集（引擎只用 TEXCOORD_0 → 贴图 UV 可能错误）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_sheen：sheen 因子/颜色为观感近似，sheen 贴图未实现
- KHR_materials_specular：specularFactor/specularColor 已映射，f90 行为与贴图未实现

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
