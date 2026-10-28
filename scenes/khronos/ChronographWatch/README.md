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
| 动画导入（plans/22） | clip 1 / skin 0 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_variants`、`KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×29 |

**导入产物**：`assets/` 19 个 `.mesh.json`（合计 10.8 MB），`textures/` 8 张；场景 `ChronographWatch.scn`。

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

参考图：`screenshot/screenshot_Large.jpg`（1104×1176）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.420 | 0.416 | 差 -0.004 |
| 前景平均 RGB | [112.9, 107.1, 90.2] | [114.9, 105.7, 82.4] | RGB 距离 0.015 |
| 亮度直方图相似度 | — | — | 0.73（16 桶交集，1=完全一致） |
| 前景占比 | 94.2% | 13.3% | 覆盖率比 0.141 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_variants：只使用默认材质（不做变体切换）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_variants：只使用默认材质（不做变体切换）

## 踩过的坑 / 注意

- 动画已导入：1 个片段（场景自动播放首个；预览=第 1 帧姿态）
- 官方 HDRI 环境：Cannon Exterior（CC0-1.0，env_intensity=1.00；仅 IBL 不开背景）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
