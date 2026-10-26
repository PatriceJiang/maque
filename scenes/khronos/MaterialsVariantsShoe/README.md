# MaterialsVariantsShoe —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/MaterialsVariantsShoe](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/MaterialsVariantsShoe)  
**上游概览**：A model of a shoe that illustrates use of the KHR_materials_variants extension with three materials included in the asset.
**标签**：`extension`、`showcase`、`written`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Shopify 2021

> 本目录由 `python tools/migrate_gltf_models.py --only MaterialsVariantsShoe` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `MaterialsVariantsShoe.gltf` |
| 节点 / 网格 / primitive | 3 / 1 / 1 |
| 顶点 / 三角形 | 13,540 / 22,700 |
| 材质 / 纹理 / 图像 | 3 / 5 / 5 |
| 图像编码 | jpeg×5 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_variants` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 1.5 MB），`textures/` 3 张；场景 `MaterialsVariantsShoe.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only MaterialsVariantsShoe --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/MaterialsVariantsShoe/glTF/MaterialsVariantsShoe.gltf --out scenes/khronos/MaterialsVariantsShoe --name MaterialsVariantsShoe --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/MaterialsVariantsShoe/MaterialsVariantsShoe.scn
./build-win/samples/player/maque_player --shot /tmp/MaterialsVariantsShoe.png scenes/khronos/MaterialsVariantsShoe/MaterialsVariantsShoe.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot-large.png`（1958×808）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.551 | 0.454 | 差 -0.097 |
| 前景平均 RGB | [144.3, 139.1, 141.4] | [79.7, 122.9, 150.9] | RGB 距离 0.118 |
| 亮度直方图相似度 | — | — | 0.53（16 桶交集，1=完全一致） |
| 前景占比 | 27.0% | 15.6% | 覆盖率比 0.577 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_variants：只使用默认材质（不做变体切换）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_variants：只使用默认材质（不做变体切换）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
