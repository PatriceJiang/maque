# PrimitiveModeNormalsTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/PrimitiveModeNormalsTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/PrimitiveModeNormalsTest)  
**上游概览**：Tests rendering of points, lines, and triangles with and without normals.
**标签**：`core`、`issues`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Unknown 2020

> 本目录由 `python tools/migrate_gltf_models.py --only PrimitiveModeNormalsTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `PrimitiveModeNormalsTest.gltf` |
| 节点 / 网格 / primitive | 28 / 25 / 25 |
| 顶点 / 三角形 | 1,050,220 / 352,082 |
| 材质 / 纹理 / 图像 | 2 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | MASK×1、OPAQUE×1 |

**导入产物**：`assets/` 9 个 `.mesh.json`（合计 157.1 KB），`textures/` 1 张；场景 `PrimitiveModeNormalsTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only PrimitiveModeNormalsTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/PrimitiveModeNormalsTest/glTF/PrimitiveModeNormalsTest.gltf --out scenes/khronos/PrimitiveModeNormalsTest --name PrimitiveModeNormalsTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/PrimitiveModeNormalsTest/PrimitiveModeNormalsTest.scn
./build-win/samples/player/maque_player --shot /tmp/PrimitiveModeNormalsTest.png scenes/khronos/PrimitiveModeNormalsTest/PrimitiveModeNormalsTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（720×512）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.730 | 0.893 | 差 +0.163 |
| 前景平均 RGB | [186.6, 186.3, 180.8] | [224.5, 228.6, 229.4] | RGB 距离 0.168 |
| 亮度直方图相似度 | — | — | 0.68（16 桶交集，1=完全一致） |
| 前景占比 | 66.8% | 11.7% | 覆盖率比 0.176 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 非三角形 primitive 被跳过（线/点）
- COLOR_0 顶点色未使用
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 非三角形 primitive 被跳过（线/点）
- COLOR_0 顶点色未使用
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 13 个 primitive 无 NORMAL（导入器按索引重建法线）
- 16 个非三角 primitive 被跳过
- 大模型：1,050,220 顶点（加载/渲染较慢）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
