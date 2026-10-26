# TextureTransformTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TextureTransformTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TextureTransformTest)  
**上游概览**：Tests if the KHR_texture_transform extension is supported for BaseColor.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Microsoft 2018

> 本目录由 `python tools/migrate_gltf_models.py --only TextureTransformTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TextureTransformTest.gltf` |
| 节点 / 网格 / primitive | 12 / 9 / 9 |
| 顶点 / 三角形 | 36 / 18 |
| 材质 / 纹理 / 图像 | 9 / 5 / 5 |
| 图像编码 | png×5 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×9 |

**导入产物**：`assets/` 9 个 `.mesh.json`（合计 6.0 KB），`textures/` 5 张；场景 `TextureTransformTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TextureTransformTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TextureTransformTest/glTF/TextureTransformTest.gltf --out scenes/khronos/TextureTransformTest --name TextureTransformTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TextureTransformTest/TextureTransformTest.scn
./build-win/samples/player/maque_player --shot /tmp/TextureTransformTest.png scenes/khronos/TextureTransformTest/TextureTransformTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（300×220）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.689 | 0.644 | 差 -0.045 |
| 前景平均 RGB | [150.2, 181.8, 190.2] | [121.0, 176.9, 161.7] | RGB 距离 0.082 |
| 亮度直方图相似度 | — | — | 0.63（16 桶交集，1=完全一致） |
| 前景占比 | 84.3% | 31.5% | 覆盖率比 0.373 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失

## 踩过的坑 / 注意

- 9 个 primitive 无 NORMAL（导入器按索引重建法线）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
