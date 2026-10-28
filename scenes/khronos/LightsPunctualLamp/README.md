# LightsPunctualLamp —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/LightsPunctualLamp](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/LightsPunctualLamp)  
**上游概览**：Lamp using punctual lights.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，DGG 2021

> 本目录由 `python tools/migrate_gltf_models.py --only LightsPunctualLamp` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `LightsPunctualLamp.gltf` |
| 节点 / 网格 / primitive | 8 / 3 / 3 |
| 顶点 / 三角形 | 4,555 / 6,000 |
| 材质 / 纹理 / 图像 | 3 / 7 / 7 |
| 图像编码 | jpeg×4、png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_lights_punctual` |
| alpha 模式分布 | BLEND×1、OPAQUE×2 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 486.5 KB），`textures/` 7 张；场景 `LightsPunctualLamp.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only LightsPunctualLamp --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/LightsPunctualLamp/glTF/LightsPunctualLamp.gltf --out scenes/khronos/LightsPunctualLamp --name LightsPunctualLamp --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/LightsPunctualLamp/LightsPunctualLamp.scn
./build-win/samples/player/maque_player --shot /tmp/LightsPunctualLamp.png scenes/khronos/LightsPunctualLamp/LightsPunctualLamp.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（400×344）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.358 | 0.399 | 差 +0.041 |
| 前景平均 RGB | [101.6, 90.1, 73.2] | [119.9, 98.8, 76.8] | RGB 距离 0.040 |
| 亮度直方图相似度 | — | — | 0.62（16 桶交集，1=完全一致） |
| 前景占比 | 12.5% | 4.8% | 覆盖率比 0.379 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）；预览主体占比过小（<5%）→ 机位过远？**

⚠️ 构图告警：预览主体占比过小（<5%）→ 机位过远？

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射

## 踩过的坑 / 注意

- 构图告警：预览主体占比过小（<5%）→ 机位过远？

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
