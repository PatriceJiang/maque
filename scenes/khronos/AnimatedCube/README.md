# AnimatedCube —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/AnimatedCube](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/AnimatedCube)  
**上游概览**：Same as 'Cube', but having a linear rotation animation.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Norbert Nopper 2017

> 本目录由 `python tools/migrate_gltf_models.py --only AnimatedCube` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `AnimatedCube.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 1 |
| 顶点 / 三角形 | 36 / 12 |
| 材质 / 纹理 / 图像 | 1 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 动画导入（plans/22） | clip 1 / skin 0 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 2.7 KB），`textures/` 1 张；场景 `AnimatedCube.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only AnimatedCube --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/AnimatedCube/glTF/AnimatedCube.gltf --out scenes/khronos/AnimatedCube --name AnimatedCube --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/AnimatedCube/AnimatedCube.scn
./build-win/samples/player/maque_player --shot /tmp/AnimatedCube.png scenes/khronos/AnimatedCube/AnimatedCube.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.gif`（174×130）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.572 | 0.715 | 差 +0.142 |
| 前景平均 RGB | [158.2, 143.2, 136.5] | [189.9, 181.3, 167.4] | RGB 距离 0.132 |
| 亮度直方图相似度 | — | — | 0.74（16 桶交集，1=完全一致） |
| 前景占比 | 93.4% | 27.2% | 覆盖率比 0.291 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）

## 踩过的坑 / 注意

- 动画已导入：1 个片段（场景自动播放首个；预览=第 1 帧姿态）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
