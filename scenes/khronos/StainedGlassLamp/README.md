# StainedGlassLamp —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/StainedGlassLamp](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/StainedGlassLamp)  
**上游概览**：A real product, a Tiffany-style stained glass table lamp sold on the Wayfair website.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2021

> 本目录由 `python tools/migrate_gltf_models.py --only StainedGlassLamp` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `StainedGlassLamp.gltf` |
| 节点 / 网格 / primitive | 8 / 8 / 8 |
| 顶点 / 三角形 | 51,150 / 35,132 |
| 材质 / 纹理 / 图像 | 13 / 19 / 19 |
| 图像编码 | png×19 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_clearcoat`、`KHR_materials_variants`、`KHR_materials_ior`、`KHR_materials_volume` |
| alpha 模式分布 | MASK×4、OPAQUE×9 |

**导入产物**：`assets/` 8 个 `.mesh.json`（合计 5.0 MB），`textures/` 19 张；场景 `StainedGlassLamp.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only StainedGlassLamp --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/StainedGlassLamp/glTF/StainedGlassLamp.gltf --out scenes/khronos/StainedGlassLamp --name StainedGlassLamp --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/StainedGlassLamp/StainedGlassLamp.scn
./build-win/samples/player/maque_player --shot /tmp/StainedGlassLamp.png scenes/khronos/StainedGlassLamp/StainedGlassLamp.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（751×1140）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.351 | 0.314 | 差 -0.037 |
| 前景平均 RGB | [102.8, 87.2, 75.0] | [91.0, 77.9, 70.5] | RGB 距离 0.034 |
| 亮度直方图相似度 | — | — | 0.78（16 桶交集，1=完全一致） |
| 前景占比 | 35.6% | 4.6% | 覆盖率比 0.13 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）；预览主体占比过小（<5%）→ 机位过远？**

⚠️ 构图告警：预览主体占比过小（<5%）→ 机位过远？

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_variants：只使用默认材质（不做变体切换）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_variants：只使用默认材质（不做变体切换）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 构图告警：预览主体占比过小（<5%）→ 机位过远？

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：整盏彩绘玻璃灯完整：多层玻璃伞罩彩绘图案、珠串流苏、灯柱与底座都在，贴图正确、无缺件，构图可辨认。玻璃不透光、没有官方那种被内部灯光点亮的效果（transmission/emissive 限制），且自动机位偏远（主体约 6%），属于可接受的已知差距。
- 差异主因：缺失特性：transmission（玻璃罩透光）未实现 + emissive 发光偏弱；机位偏远
