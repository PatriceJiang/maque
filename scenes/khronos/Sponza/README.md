# Sponza —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/Sponza](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza)  
**上游概览**：Building interior, often used to test lighting.
**标签**：`core`  
**许可**：（资产元数据，保留出处）
- Cryengine Limited License Agreement（LicenseRef-CRYENGINE-Agreement）../../LICENSES/LicenseRef-CRYENGINE-Agreement.txt，Crytek 2016

> 本目录由 `python tools/migrate_gltf_models.py --only Sponza` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `Sponza.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 103 |
| 顶点 / 三角形 | 192,496 / 262,267 |
| 材质 / 纹理 / 图像 | 25 / 69 / 69 |
| 图像编码 | jpeg×65、png×4 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | MASK×3、OPAQUE×22 |

**导入产物**：`assets/` 103 个 `.mesh.json`（合计 20.3 MB），`textures/` 69 张；场景 `Sponza.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only Sponza --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/Sponza/glTF/Sponza.gltf --out scenes/khronos/Sponza --name Sponza --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/Sponza/Sponza.scn
./build-win/samples/player/maque_player --shot /tmp/Sponza.png scenes/khronos/Sponza/Sponza.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/large.jpg`（2491×1259）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.348 | 0.457 | 差 +0.109 |
| 前景平均 RGB | [98.8, 87.0, 77.8] | [122.5, 116.0, 104.2] | RGB 距离 0.103 |
| 亮度直方图相似度 | — | — | 0.62（16 桶交集，1=完全一致） |
| 前景占比 | 79.9% | 30.4% | 覆盖率比 0.381 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：预览显示整个 Sponza 建筑的外壳（石墙/瓦顶贴图正常、几何完整、无破面），但官方参考图是内部中庭视角（柱廊、旗帜、雕塑），当前机位看不到室内细节。若需要核验室内材质/构图需手动机位；作为自动迁移初版可接受。
- 差异主因：机位/背景：自动机位在包围盒外，只能看到外墙
