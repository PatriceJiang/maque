# Box With Spaces —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/Box With Spaces](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Box With Spaces)  
**上游概览**：Box with URI-encoded spaces in the texture names used by a simple PBR material.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Ed Mackey 2017
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only Box With Spaces` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `Box With Spaces.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 1 |
| 顶点 / 三角形 | 24 / 12 |
| 材质 / 纹理 / 图像 | 1 / 3 / 3 |
| 图像编码 | png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 1.9 KB），`textures/` 3 张；场景 `Box With Spaces.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only Box With Spaces --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/Box With Spaces/glTF/Box With Spaces.gltf --out scenes/khronos/Box With Spaces --name Box With Spaces --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/Box With Spaces/Box With Spaces.scn
./build-win/samples/player/maque_player --shot /tmp/Box With Spaces.png scenes/khronos/Box With Spaces/Box With Spaces.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（128×128）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.210 | 0.322 | 差 +0.112 |
| 前景平均 RGB | [50.8, 54.8, 48.4] | [59.1, 93.7, 34.1] | RGB 距离 0.081 |
| 亮度直方图相似度 | — | — | 0.24（16 桶交集，1=完全一致） |
| 前景占比 | 60.2% | 5.1% | 覆盖率比 0.085 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（该模型尚未完成迁移；可在此记录阻塞原因与后续处理）

## 人工视觉复核（自动迁移初版）
- 结论：带空格路径的 glTF logo 立方体加载成功，绿色 logo 贴图位置/方向正确，几何完整。pass=1 曾因文件名 URL 编码报 FileNotFoundError，pass=3 已修复为 ok（148/148）；视觉上与官方一致，仅机位/光照差异。
- 差异主因：机位/背景：官方视角更近、更正面
