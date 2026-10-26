# TextureTransformMultiTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TextureTransformMultiTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TextureTransformMultiTest)  
**上游概览**：Tests if the KHR_texture_transform extension is supported for several inputs.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2020

> 本目录由 `python tools/migrate_gltf_models.py --only TextureTransformMultiTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TextureTransformMultiTest.gltf` |
| 节点 / 网格 / primitive | 29 / 29 / 29 |
| 顶点 / 三角形 | 2,302 / 3,956 |
| 材质 / 纹理 / 图像 | 29 / 4 / 4 |
| 图像编码 | png×4 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_clearcoat`、`KHR_materials_unlit`、`KHR_texture_transform` |
| 必需扩展 | `KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×29 |

**导入产物**：`assets/` 29 个 `.mesh.json`（合计 257.3 KB），`textures/` 4 张；场景 `TextureTransformMultiTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TextureTransformMultiTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TextureTransformMultiTest/glTF/TextureTransformMultiTest.gltf --out scenes/khronos/TextureTransformMultiTest --name TextureTransformMultiTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TextureTransformMultiTest/TextureTransformMultiTest.scn
./build-win/samples/player/maque_player --shot /tmp/TextureTransformMultiTest.png scenes/khronos/TextureTransformMultiTest/TextureTransformMultiTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（930×954）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.423 | 0.490 | 差 +0.067 |
| 前景平均 RGB | [86.5, 109.4, 157.8] | [101.7, 125.8, 186.0] | RGB 距离 0.078 |
| 亮度直方图相似度 | — | — | 0.84（16 桶交集，1=完全一致） |
| 前景占比 | 86.2% | 25.8% | 覆盖率比 0.3 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
