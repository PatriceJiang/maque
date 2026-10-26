# TrafficCone —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TrafficCone](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TrafficCone)  
**上游概览**：A traffic cone demonstrating the KHR_materials_retroreflection extension, which simulates materials that reflect light back toward its source.
**标签**：`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，hinndia 2024
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Rob Tuytel 2020

> 本目录由 `python tools/migrate_gltf_models.py --only TrafficCone` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TrafficCone.gltf` |
| 节点 / 网格 / primitive | 7 / 4 / 4 |
| 顶点 / 三角形 | 10,539 / 17,224 |
| 材质 / 纹理 / 图像 | 4 / 7 / 5 |
| 图像编码 | jpeg×2、png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| 使用扩展 | `KHR_materials_specular`、`KHR_materials_retroreflection`、`KHR_lights_punctual`、`KHR_materials_unlit` |
| alpha 模式分布 | OPAQUE×4 |

**导入产物**：`assets/` 4 个 `.mesh.json`（合计 1.1 MB），`textures/` 4 张；场景 `TrafficCone.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TrafficCone --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TrafficCone/glTF/TrafficCone.gltf --out scenes/khronos/TrafficCone --name TrafficCone --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TrafficCone/TrafficCone.scn
./build-win/samples/player/maque_player --shot /tmp/TrafficCone.png scenes/khronos/TrafficCone/TrafficCone.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（150×85）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.368 | 0.534 | 差 +0.166 |
| 前景平均 RGB | [158.0, 79.1, 50.4] | [144.5, 135.1, 122.9] | RGB 距离 0.186 |
| 亮度直方图相似度 | — | — | 0.10（16 桶交集，1=完全一致） |
| 前景占比 | 41.6% | 17.7% | 覆盖率比 0.424 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）
- KHR_materials_specular：specularFactor/specularColor 已映射，f90 行为与贴图未实现
- KHR_materials_retroreflection 未实现
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）
- KHR_materials_specular：specularFactor/specularColor 已映射，f90 行为与贴图未实现
- KHR_materials_retroreflection 未实现

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
