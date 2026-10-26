# ChronographWatch —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/ChronographWatch](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ChronographWatch)  
**上游概览**：Wrist watch with Khronos logos and material variants
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2025
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2015
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017
- DGG Trademark or Logo（LicenseRef-LegalMark-DGG）../../LICENSES/LicenseRef-LegalMark-DGG.txt，Copyrightable logo 2020

> 本目录由 `python tools/migrate_gltf_models.py --only ChronographWatch` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `ChronographWatch.gltf` |
| 节点 / 网格 / primitive | 14 / 13 / 19 |
| 顶点 / 三角形 | 104,765 / 100,002 |
| 材质 / 纹理 / 图像 | 29 / 8 / 8 |
| 图像编码 | png×8 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_variants`、`KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×29 |

**导入产物**：`assets/` 19 个 `.mesh.json`（合计 10.7 MB），`textures/` 8 张；场景 `ChronographWatch.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only ChronographWatch --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/ChronographWatch/glTF/ChronographWatch.gltf --out scenes/khronos/ChronographWatch --name ChronographWatch --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/ChronographWatch/ChronographWatch.scn
./build-win/samples/player/maque_player --shot /tmp/ChronographWatch.png scenes/khronos/ChronographWatch/ChronographWatch.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1104×1176）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.420 | 0.530 | 差 +0.111 |
| 前景平均 RGB | [112.9, 107.1, 90.2] | [143.8, 134.7, 115.8] | RGB 距离 0.110 |
| 亮度直方图相似度 | — | — | 0.74（16 桶交集，1=完全一致） |
| 前景占比 | 94.2% | 14.1% | 覆盖率比 0.15 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 部分贴图使用 TEXCOORD_1/更高 UV 集（引擎只用 TEXCOORD_0 → 贴图 UV 可能错误）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_transmission 未实现（玻璃/透明材质会渲染为不透明；ToyCar 用 alpha BLEND 近似）
- KHR_materials_variants：只使用默认材质（不做变体切换）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 部分贴图使用 TEXCOORD_1/更高 UV 集（引擎只用 TEXCOORD_0 → 贴图 UV 可能错误）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_transmission 未实现（玻璃/透明材质会渲染为不透明；ToyCar 用 alpha BLEND 近似）
- KHR_materials_variants：只使用默认材质（不做变体切换）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
