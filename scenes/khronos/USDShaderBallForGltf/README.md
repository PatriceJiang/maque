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

**导入产物**：`assets/` 4 个 `.mesh.json`（合计 1.9 MB），`textures/` 4 张；场景 `USDShaderBallForGltf.scn`。

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

参考图：`screenshot/screenshot_large.jpg`（1422×808）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.477 | 0.536 | 差 +0.058 |
| 前景平均 RGB | [119.7, 122.5, 121.1] | [132.4, 138.1, 134.5] | RGB 距离 0.055 |
| 亮度直方图相似度 | — | — | 0.71（16 桶交集，1=完全一致） |
| 前景占比 | 56.6% | 33.6% | 覆盖率比 0.593 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- KHR_materials_volume：thicknessFactor/thicknessTexture.G + attenuationColor/Distance 的 Beer-Lambert 吸收（第 24 轮）；dispersion 用 RGB 折射偏移近似；volume_scatter 的 multiscatterColor 以各向同性 in-scatter 近似（无真实多次散射/相位函数）

## 踩过的坑 / 注意

- 手动机位：手动机位：官方特写构图（球占 ~90% 画面高、正视开口、浅灰平背景）
- 手调环境：手调环境：Studio Neutral HDRI 仅 IBL（不画背景；官方为平背景）
- 参考图说明：官方 screenshot_large 是 three.js 编辑器两联图（左 pathtraced / 右 rasterized），整图 hist/rgb 指标只作趋势参考，观感以右侧 rasterized 对照

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
