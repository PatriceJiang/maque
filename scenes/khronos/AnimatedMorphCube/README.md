# AnimatedMorphCube —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/AnimatedMorphCube](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/AnimatedMorphCube)  
**上游概览**：Demonstrates a simple cube with two simple morph targets and an animation that transitions between them both.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Microsoft 2017

> 本目录由 `python tools/migrate_gltf_models.py --only AnimatedMorphCube` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `AnimatedMorphCube.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 1 |
| 顶点 / 三角形 | 24 / 12 |
| 材质 / 纹理 / 图像 | 1 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 1 / 0 |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 1.9 KB），`textures/` 0 张；场景 `AnimatedMorphCube.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only AnimatedMorphCube --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf --out scenes/khronos/AnimatedMorphCube --name AnimatedMorphCube --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/AnimatedMorphCube/AnimatedMorphCube.scn
./build-win/samples/player/maque_player --shot /tmp/AnimatedMorphCube.png scenes/khronos/AnimatedMorphCube/AnimatedMorphCube.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.gif`（324×342）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.235 | 0.769 | 差 +0.535 |
| 前景平均 RGB | [59.9, 59.9, 60.0] | [195.8, 195.9, 197.8] | RGB 距离 0.536 |
| 亮度直方图相似度 | — | — | 0.01（16 桶交集，1=完全一致） |
| 前景占比 | 54.8% | 26.9% | 覆盖率比 0.491 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- morph targets 未应用（默认权重不生效）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- morph targets 未应用（默认权重不生效）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：立方体几何完整、可辨认，但官方截图是 morph 动画中间帧（带形状/颜色变化），本引擎为静态默认形态、颜色也偏亮（缺 HDRI）。README 已记录 morph targets 未应用，属预期差距。
- 差异主因：缺失特性：morph targets + 动画未应用
