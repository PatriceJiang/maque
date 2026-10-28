# DiffuseTransmissionTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/DiffuseTransmissionTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DiffuseTransmissionTest)  
**上游概览**：Test model for KHR_materials_diffuse_transmission extension.
**标签**：`video`、`written`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2025
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2015
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only DiffuseTransmissionTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `DiffuseTransmissionTest.gltf` |
| 节点 / 网格 / primitive | 34 / 33 / 33 |
| 顶点 / 三角形 | 5,530 / 4,980 |
| 材质 / 纹理 / 图像 | 29 / 3 / 3 |
| 图像编码 | png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_diffuse_transmission`、`KHR_materials_unlit`、`KHR_lights_punctual` |
| 必需扩展 | `KHR_materials_unlit`、`KHR_lights_punctual` |
| alpha 模式分布 | BLEND×1、OPAQUE×28 |

**导入产物**：`assets/` 33 个 `.mesh.json`（合计 404.8 KB），`textures/` 3 张；场景 `DiffuseTransmissionTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only DiffuseTransmissionTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/DiffuseTransmissionTest/glTF/DiffuseTransmissionTest.gltf --out scenes/khronos/DiffuseTransmissionTest --name DiffuseTransmissionTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/DiffuseTransmissionTest/DiffuseTransmissionTest.scn
./build-win/samples/player/maque_player --shot /tmp/DiffuseTransmissionTest.png scenes/khronos/DiffuseTransmissionTest/DiffuseTransmissionTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1280×828）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.426 | 0.410 | 差 -0.016 |
| 前景平均 RGB | [108.9, 109.9, 98.8] | [70.0, 114.8, 103.6] | RGB 距离 0.064 |
| 亮度直方图相似度 | — | — | 0.82（16 桶交集，1=完全一致） |
| 前景占比 | 95.9% | 20.1% | 覆盖率比 0.209 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_materials_diffuse_transmission：不透明材质 + 背光透射（factor/colorFactor + 透射率/透射色贴图 + volume 吸收）；无真实散射/厚度（能量拆分按可加近似）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_materials_diffuse_transmission：不透明材质 + 背光透射（factor/colorFactor + 透射率/透射色贴图 + volume 吸收）；无真实散射/厚度（能量拆分按可加近似）

## 踩过的坑 / 注意

- 官方 HDRI 环境：Cannon Exterior（CC0-1.0，env_intensity=1.07；仅 IBL 不开背景）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
