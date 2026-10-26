# BoxAnimated —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/BoxAnimated](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/BoxAnimated)  
**上游概览**：Rotation and Translation Animations. Start with this to test animations.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Cesium 2017

> 本目录由 `python tools/migrate_gltf_models.py --only BoxAnimated` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `BoxAnimated.gltf` |
| 节点 / 网格 / primitive | 4 / 2 / 2 |
| 顶点 / 三角形 | 320 / 254 |
| 材质 / 纹理 / 图像 | 2 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 24.8 KB），`textures/` 0 张；场景 `BoxAnimated.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only BoxAnimated --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/BoxAnimated/glTF/BoxAnimated.gltf --out scenes/khronos/BoxAnimated --name BoxAnimated --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/BoxAnimated/BoxAnimated.scn
./build-win/samples/player/maque_player --shot /tmp/BoxAnimated.png scenes/khronos/BoxAnimated/BoxAnimated.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.gif`（130×130）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.180 | 0.722 | 差 +0.542 |
| 前景平均 RGB | [34.6, 46.9, 70.4] | [154.8, 190.0, 211.3] | RGB 距离 0.528 |
| 亮度直方图相似度 | — | — | 0.01（16 桶交集，1=完全一致） |
| 前景占比 | 8.2% | 26.8% | 覆盖率比 3.251 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
