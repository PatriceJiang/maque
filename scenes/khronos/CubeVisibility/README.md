# CubeVisibility —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CubeVisibility](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CubeVisibility)  
**上游概览**：This model demonstrates the `KHR_node_visibility` extension and its interaction with `KHR_animation_pointer`.
**标签**：`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Aaron Franke 2025

> 本目录由 `python tools/migrate_gltf_models.py --only CubeVisibility` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CubeVisibility.gltf` |
| 节点 / 网格 / primitive | 6 / 3 / 3 |
| 顶点 / 三角形 | 72 / 36 |
| 材质 / 纹理 / 图像 | 3 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 使用扩展 | `KHR_animation_pointer`、`KHR_node_visibility` |
| 必需扩展 | `KHR_node_visibility` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 5.4 KB），`textures/` 0 张；场景 `CubeVisibility.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CubeVisibility --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CubeVisibility/glTF/CubeVisibility.gltf --out scenes/khronos/CubeVisibility --name CubeVisibility --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CubeVisibility/CubeVisibility.scn
./build-win/samples/player/maque_player --shot /tmp/CubeVisibility.png scenes/khronos/CubeVisibility/CubeVisibility.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_animated.webp`（1040×682）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.454 | 0.440 | 差 -0.014 |
| 前景平均 RGB | [98.3, 119.8, 130.5] | [22.5, 140.3, 96.8] | RGB 距离 0.170 |
| 亮度直方图相似度 | — | — | 0.30（16 桶交集，1=完全一致） |
| 前景占比 | 36.3% | 34.1% | 覆盖率比 0.938 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入
- KHR_animation_pointer 动画未导入
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入
- KHR_animation_pointer 动画未导入

## 踩过的坑 / 注意

- gltf_import: KHR_node_visibility=false 跳过 1 个隐藏节点（含整棵子树）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
