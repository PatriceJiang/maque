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
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 3.8 KB），`textures/` 0 张；场景 `RecursiveSkeletons.scn`。

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

参考图：`screenshot/screenshot.jpg`（128×128）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.806 | 0.788 | 差 -0.019 |
| 前景平均 RGB | [205.3, 205.6, 206.0] | [200.3, 200.9, 203.1] | RGB 距离 0.017 |
| 亮度直方图相似度 | — | — | 0.31（16 桶交集，1=完全一致） |
| 前景占比 | 14.5% | 6.2% | 覆盖率比 0.432 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 骨骼蒙皮未应用（蒙皮网格按节点世界矩阵烘焙，姿势/形变与官方参考不同）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 骨骼蒙皮未应用（蒙皮网格按节点世界矩阵烘焙，姿势/形变与官方参考不同）

## 踩过的坑 / 注意

- 1 个 primitive 无 NORMAL（导入器按索引重建法线）

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：官方参考图是多根分叉弯曲的骨骼束（84 个 skin 的递归测试），本引擎只渲染出一根竖直白色柱体：所有蒙皮网格都按未蒙皮的节点世界矩阵烘焙后重叠在一起。几何/贴图本身在，但视觉上完全无法与官方对应，属 skin 未实现的最严重案例。
- 差异主因：缺失特性：skin/骨骼未应用 → 蒙皮网格重叠成一根柱子
