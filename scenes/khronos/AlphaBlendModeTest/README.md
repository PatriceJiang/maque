# AlphaBlendModeTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/AlphaBlendModeTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/AlphaBlendModeTest)  
**上游概览**：Tests alpha modes and settings.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2018

> 本目录由 `python tools/migrate_gltf_models.py --only AlphaBlendModeTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `AlphaBlendModeTest.gltf` |
| 节点 / 网格 / primitive | 9 / 9 / 9 |
| 顶点 / 三角形 | 137 / 61 |
| 材质 / 纹理 / 图像 | 6 / 4 / 4 |
| 图像编码 | jpeg×3、png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | BLEND×1、MASK×3、OPAQUE×2 |

**导入产物**：`assets/` 9 个 `.mesh.json`（合计 14.2 KB），`textures/` 4 张；场景 `AlphaBlendModeTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only AlphaBlendModeTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/AlphaBlendModeTest/glTF/AlphaBlendModeTest.gltf --out scenes/khronos/AlphaBlendModeTest --name AlphaBlendModeTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/AlphaBlendModeTest/AlphaBlendModeTest.scn
./build-win/samples/player/maque_player --shot /tmp/AlphaBlendModeTest.png scenes/khronos/AlphaBlendModeTest/AlphaBlendModeTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（332×154）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.659 | 0.745 | 差 +0.086 |
| 前景平均 RGB | [161.2, 171.2, 157.2] | [185.2, 192.3, 177.0] | RGB 距离 0.085 |
| 亮度直方图相似度 | — | — | 0.46（16 桶交集，1=完全一致） |
| 前景占比 | 49.0% | 28.5% | 覆盖率比 0.582 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：5 块 OPAQUE/BLEND/MASK 测试面板与底部标签（Opaque/Blend/Mask 0.25/0.5 等）都在，棋盘条纹透过 alpha 面板的观感与官方基本一致，未发现明显排序错误。差异主要是机位（官方正对、本引擎侧向 3/4）与光照。
- 差异主因：机位/背景：透视角度不同；光照差异
