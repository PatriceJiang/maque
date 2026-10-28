# RecursiveSkeletons —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/RecursiveSkeletons](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/RecursiveSkeletons)  
**上游概览**：Tests unusual skinning cases with reused meshes and recursive skeletons.
**标签**：`core`、`testing`、`issues`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Cesium 2017

> 本目录由 `python tools/migrate_gltf_models.py --only RecursiveSkeletons` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `RecursiveSkeletons.gltf` |
| 节点 / 网格 / primitive | 924 / 1 / 1 |
| 顶点 / 三角形 | 40 / 76 |
| 材质 / 纹理 / 图像 | 1 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 84 / 0 / 0 |
| 动画导入（plans/22） | clip 1 / skin 84 / morph primitive 0（`assets/anim|skin/`，场景自动播放首个片段） |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 210.5 KB），`textures/` 0 张；场景 `RecursiveSkeletons.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only RecursiveSkeletons --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/RecursiveSkeletons/glTF/RecursiveSkeletons.gltf --out scenes/khronos/RecursiveSkeletons --name RecursiveSkeletons --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/RecursiveSkeletons/RecursiveSkeletons.scn
./build-win/samples/player/maque_player --shot /tmp/RecursiveSkeletons.png scenes/khronos/RecursiveSkeletons/RecursiveSkeletons.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（128×128）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.806 | 0.789 | 差 -0.017 |
| 前景平均 RGB | [205.3, 205.6, 206.0] | [200.4, 201.0, 203.1] | RGB 距离 0.016 |
| 亮度直方图相似度 | — | — | 0.39（16 桶交集，1=完全一致） |
| 前景占比 | 14.5% | 28.0% | 覆盖率比 1.932 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）；预览主体贴边/被裁剪 → 机位过近？**

⚠️ 构图告警：预览主体贴边/被裁剪 → 机位过近？

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- 骨骼蒙皮已应用（plans/22 S1：调色板 = 关节世界矩阵 × inverse_bind，蒙皮网格忽略节点变换；法线按调色板 3×3 线性部分近似）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画已导入（plans/22 S2：关键帧采样，片段按 glTF 声明序自动播放；截图取第 1 帧姿态；CUBICSPLINE 按 LINEAR 近似、无动画状态机/混合）
- 骨骼蒙皮已应用（plans/22 S1：调色板 = 关节世界矩阵 × inverse_bind，蒙皮网格忽略节点变换；法线按调色板 3×3 线性部分近似）

## 踩过的坑 / 注意

- 动画已导入：1 个片段（场景自动播放首个；预览=第 1 帧姿态）
- 骨骼蒙皮已应用：84 个骨架
- 1 个 primitive 无 NORMAL（导入器按索引重建法线）
- 构图告警：预览主体贴边/被裁剪 → 机位过近？

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：官方参考图是多根分叉弯曲的骨骼束（84 个 skin 的递归测试），本引擎只渲染出一根竖直白色柱体：所有蒙皮网格都按未蒙皮的节点世界矩阵烘焙后重叠在一起。几何/贴图本身在，但视觉上完全无法与官方对应，属 skin 未实现的最严重案例。
- 差异主因：缺失特性：skin/骨骼未应用 → 蒙皮网格重叠成一根柱子
