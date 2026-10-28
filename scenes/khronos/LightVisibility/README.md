# LightVisibility —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/LightVisibility](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/LightVisibility)  
**上游概览**：This model demonstrates the `KHR_node_visibility` extension and its interaction with `KHR_animation_pointer`.
**标签**：`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Aaron Franke 2025

> 本目录由 `python tools/migrate_gltf_models.py --only LightVisibility` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `LightVisibility.gltf` |
| 节点 / 网格 / primitive | 7 / 1 / 1 |
| 顶点 / 三角形 | 4 / 2 |
| 材质 / 纹理 / 图像 | 0 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 动画导入（plans/22） | clip 0 / skin 0 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| 使用扩展 | `KHR_animation_pointer`、`KHR_lights_punctual`、`KHR_node_visibility` |
| 必需扩展 | `KHR_lights_punctual`、`KHR_node_visibility` |
| alpha 模式分布 |  |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 416 B），`textures/` 0 张；场景 `LightVisibility.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only LightVisibility --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/LightVisibility/glTF/LightVisibility.gltf --out scenes/khronos/LightVisibility --name LightVisibility --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/LightVisibility/LightVisibility.scn
./build-win/samples/player/maque_player --shot /tmp/LightVisibility.png scenes/khronos/LightVisibility/LightVisibility.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_animated.webp`（1040×680）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.526 | 0.800 | 差 +0.274 |
| 前景平均 RGB | [118.1, 137.8, 142.3] | [192.8, 206.3, 205.1] | RGB 距离 0.269 |
| 亮度直方图相似度 | — | — | 0.10（16 桶交集，1=完全一致） |
| 前景占比 | 66.1% | 42.3% | 覆盖率比 0.64 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入
- KHR_animation_pointer 动画未导入（材质/贴图变换/可见性等指针通道；节点 TRS/骨骼/morph 动画已支持）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入
- KHR_animation_pointer 动画未导入（材质/贴图变换/可见性等指针通道；节点 TRS/骨骼/morph 动画已支持）

## 踩过的坑 / 注意

- gltf_import: 跳过片段 'BlinkingBlueLight'（无有效轨道；pointer 通道已跳过）
- gltf_import: 跳过 KHR_animation_pointer 通道 1 条（片段 0 个）
- gltf_import: KHR_node_visibility=false 跳过 1 个隐藏节点（含整棵子树）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
