# AntiqueCamera —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/AntiqueCamera](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/AntiqueCamera)  
**上游概览**：Static model of old camera on a tripod. [Issues: non-Khronos mark]
**标签**：`core`、`testing`、`issues`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Maximillan Kamps 2018
- UX3D Trademark or Logo（LicenseRef-LegalMark-UX3D）../../LICENSES/LicenseRef-LegalMark-UX3D.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only AntiqueCamera` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `AntiqueCamera.gltf` |
| 节点 / 网格 / primitive | 2 / 2 / 2 |
| 顶点 / 三角形 | 21,178 / 20,066 |
| 材质 / 纹理 / 图像 | 2 / 6 / 6 |
| 图像编码 | png×6 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 2.1 MB），`textures/` 6 张；场景 `AntiqueCamera.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only AntiqueCamera --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/AntiqueCamera/glTF/AntiqueCamera.gltf --out scenes/khronos/AntiqueCamera --name AntiqueCamera --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/AntiqueCamera/AntiqueCamera.scn
./build-win/samples/player/maque_player --shot /tmp/AntiqueCamera.png scenes/khronos/AntiqueCamera/AntiqueCamera.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（520×1000）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.326 | 0.261 | 差 -0.065 |
| 前景平均 RGB | [90.7, 80.9, 80.3] | [75.0, 64.5, 61.0] | RGB 距离 0.067 |
| 亮度直方图相似度 | — | — | 0.60（16 桶交集，1=完全一致） |
| 前景占比 | 43.5% | 4.3% | 覆盖率比 0.098 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）；预览主体占比过小（<5%）→ 机位过远？**

⚠️ 构图告警：预览主体占比过小（<5%）→ 机位过远？

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 构图告警：预览主体占比过小（<5%）→ 机位过远？

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：三脚架相机完整、木质/金属材质与贴图正确、朝向合理，能清楚辨认主体；与官方近景相比整机偏小（自动机位拟合偏保守）。除取景过远外无异常，属唯一“主体 <5%”告警模型。
- 差异主因：机位/背景：自动机位过远（主体占比 4.3%）
