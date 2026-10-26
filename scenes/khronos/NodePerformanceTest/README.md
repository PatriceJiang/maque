# NodePerformanceTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/NodePerformanceTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/NodePerformanceTest)  
**上游概览**：Node, mesh, primitive and texture count performance test
**标签**：`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Jon Aspeheim 2024

> 本目录由 `python tools/migrate_gltf_models.py --only NodePerformanceTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `NodePerformanceTest.gltf` |
| 节点 / 网格 / primitive | 10002 / 10000 / 10000 |
| 顶点 / 三角形 | 731,850 / 268,268 |
| 材质 / 纹理 / 图像 | 10000 / 10000 / 100 |
| 图像编码 | png×100 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| 使用扩展 | `KHR_lights_punctual` |
| 必需扩展 | `KHR_lights_punctual` |
| alpha 模式分布 | OPAQUE×10000 |

**导入产物**：`assets/` 10000 个 `.mesh.json`（合计 70.8 MB），`textures/` 100 张；场景 `NodePerformanceTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only NodePerformanceTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/NodePerformanceTest/glTF/NodePerformanceTest.gltf --out scenes/khronos/NodePerformanceTest --name NodePerformanceTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/NodePerformanceTest/NodePerformanceTest.scn
./build-win/samples/player/maque_player --shot /tmp/NodePerformanceTest.png scenes/khronos/NodePerformanceTest/NodePerformanceTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（1917×1077）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.562 | 0.275 | 差 -0.287 |
| 前景平均 RGB | [146.2, 142.8, 139.5] | [70.0, 70.0, 71.2] | RGB 距离 0.284 |
| 亮度直方图相似度 | — | — | 0.38（16 桶交集，1=完全一致） |
| 前景占比 | 67.8% | 5.6% | 覆盖率比 0.083 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）

## 踩过的坑 / 注意

- 大模型：731,850 顶点（加载/渲染较慢）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
