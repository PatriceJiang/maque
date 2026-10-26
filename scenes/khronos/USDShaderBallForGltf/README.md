# USDShaderBallForGltf —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/USDShaderBallForGltf](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/USDShaderBallForGltf)  
**上游概览**：USD Shader Ball converted for glTF
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Geometry and textures: Chris Rydalch; original specification and validation: André Mazzone; original scene, inspiration and consultation: Thomas Anagnostou; glTF conversion: Eric Chadwick 2026

> 本目录由 `python tools/migrate_gltf_models.py --only USDShaderBallForGltf` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `USDShaderBallForGltf.gltf` |
| 节点 / 网格 / primitive | 4 / 4 / 4 |
| 顶点 / 三角形 | 17,442 / 32,996 |
| 材质 / 纹理 / 图像 | 3 / 4 / 4 |
| 图像编码 | png×4 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 4 个 `.mesh.json`（合计 1.9 MB），`textures/` 6 张；场景 `USDShaderBallForGltf.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only USDShaderBallForGltf --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/USDShaderBallForGltf/glTF/USDShaderBallForGltf.gltf --out scenes/khronos/USDShaderBallForGltf --name USDShaderBallForGltf --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/USDShaderBallForGltf/USDShaderBallForGltf.scn
./build-win/samples/player/maque_player --shot /tmp/USDShaderBallForGltf.png scenes/khronos/USDShaderBallForGltf/USDShaderBallForGltf.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（150×85）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.491 | 0.854 | 差 +0.363 |
| 前景平均 RGB | [123.1, 125.9, 124.5] | [217.3, 217.6, 219.1] | RGB 距离 0.367 |
| 亮度直方图相似度 | — | — | 0.36（16 桶交集，1=完全一致） |
| 前景占比 | 58.5% | 13.7% | 覆盖率比 0.234 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission 未实现（玻璃/透明材质会渲染为不透明；ToyCar 用 alpha BLEND 近似）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission 未实现（玻璃/透明材质会渲染为不透明；ToyCar 用 alpha BLEND 近似）
- KHR_materials_volume/dispersion/volume_scatter 未实现

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
