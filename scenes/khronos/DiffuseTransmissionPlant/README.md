# DiffuseTransmissionPlant —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/DiffuseTransmissionPlant](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DiffuseTransmissionPlant)  
**上游概览**：Potted plant asset using Diffuse Transmission extension.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2024
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Rico Cilliers 2021

> 本目录由 `python tools/migrate_gltf_models.py --only DiffuseTransmissionPlant` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `DiffuseTransmissionPlant.gltf` |
| 节点 / 网格 / primitive | 17 / 9 / 9 |
| 顶点 / 三角形 | 50,369 / 68,409 |
| 材质 / 纹理 / 图像 | 3 / 7 / 7 |
| 图像编码 | jpeg×6、png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 2 |
| 使用扩展 | `KHR_lights_punctual`、`KHR_materials_diffuse_transmission` |
| alpha 模式分布 | BLEND×1、MASK×1、OPAQUE×1 |

**导入产物**：`assets/` 9 个 `.mesh.json`（合计 5.5 MB），`textures/` 6 张；场景 `DiffuseTransmissionPlant.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only DiffuseTransmissionPlant --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/DiffuseTransmissionPlant/glTF/DiffuseTransmissionPlant.gltf --out scenes/khronos/DiffuseTransmissionPlant --name DiffuseTransmissionPlant --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/DiffuseTransmissionPlant/DiffuseTransmissionPlant.scn
./build-win/samples/player/maque_player --shot /tmp/DiffuseTransmissionPlant.png scenes/khronos/DiffuseTransmissionPlant/DiffuseTransmissionPlant.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1028×1104）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.427 | 0.458 | 差 +0.031 |
| 前景平均 RGB | [121.0, 107.1, 89.5] | [136.5, 115.1, 74.6] | RGB 距离 0.050 |
| 亮度直方图相似度 | — | — | 0.59（16 桶交集，1=完全一致） |
| 前景占比 | 79.6% | 8.5% | 覆盖率比 0.107 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）
- KHR_materials_diffuse_transmission 未实现
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）
- KHR_materials_diffuse_transmission 未实现
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
