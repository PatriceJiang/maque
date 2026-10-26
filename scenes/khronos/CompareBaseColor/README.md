# CompareBaseColor —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CompareBaseColor](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CompareBaseColor)  
**上游概览**：This model compares base color methods.
**标签**：`testing`、`pbrtest`  
**许可**：（资产元数据，保留出处）
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Eric Chadwick and DGG 2024

> 本目录由 `python tools/migrate_gltf_models.py --only CompareBaseColor` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CompareBaseColor.gltf` |
| 节点 / 网格 / primitive | 3 / 3 / 3 |
| 顶点 / 三角形 | 27,648 / 9,216 |
| 材质 / 纹理 / 图像 | 2 / 2 / 2 |
| 图像编码 | png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 2.7 MB），`textures/` 2 张；场景 `CompareBaseColor.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CompareBaseColor --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CompareBaseColor/glTF/CompareBaseColor.gltf --out scenes/khronos/CompareBaseColor --name CompareBaseColor --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CompareBaseColor/CompareBaseColor.scn
./build-win/samples/player/maque_player --shot /tmp/CompareBaseColor.png scenes/khronos/CompareBaseColor/CompareBaseColor.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1822×600）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.388 | 0.453 | 差 +0.066 |
| 前景平均 RGB | [129.0, 93.7, 62.1] | [146.7, 110.4, 75.6] | RGB 距离 0.063 |
| 亮度直方图相似度 | — | — | 0.58（16 桶交集，1=完全一致） |
| 前景占比 | 63.9% | 22.9% | 覆盖率比 0.358 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
