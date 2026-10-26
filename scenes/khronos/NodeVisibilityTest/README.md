# NodeVisibilityTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/NodeVisibilityTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/NodeVisibilityTest)  
**上游概览**：A test for the KHR_node_visibility extension
**标签**：`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Marco Hutter (https://github.com/javagl/) 2025

> 本目录由 `python tools/migrate_gltf_models.py --only NodeVisibilityTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `NodeVisibilityTest.gltf` |
| 节点 / 网格 / primitive | 15 / 8 / 8 |
| 顶点 / 三角形 | 32 / 16 |
| 材质 / 纹理 / 图像 | 8 / 8 / 8 |
| 图像编码 | png×8 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_node_visibility` |
| alpha 模式分布 | OPAQUE×8 |

**导入产物**：`assets/` 8 个 `.mesh.json`（合计 5.0 KB），`textures/` 8 张；场景 `NodeVisibilityTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only NodeVisibilityTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/NodeVisibilityTest/glTF/NodeVisibilityTest.gltf --out scenes/khronos/NodeVisibilityTest --name NodeVisibilityTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/NodeVisibilityTest/NodeVisibilityTest.scn
./build-win/samples/player/maque_player --shot /tmp/NodeVisibilityTest.png scenes/khronos/NodeVisibilityTest/NodeVisibilityTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（574×120）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.559 | 0.829 | 差 +0.270 |
| 前景平均 RGB | [147.8, 141.6, 136.7] | [211.0, 211.4, 212.3] | RGB 距离 0.273 |
| 亮度直方图相似度 | — | — | 0.15（16 桶交集，1=完全一致） |
| 前景占比 | 78.6% | 12.1% | 覆盖率比 0.154 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入

## 踩过的坑 / 注意

- gltf_import: KHR_node_visibility=false 跳过 5 个隐藏节点（含整棵子树）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
