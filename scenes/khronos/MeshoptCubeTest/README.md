# MeshoptCubeTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/MeshoptCubeTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/MeshoptCubeTest)  
**上游概览**：This model tests the KHR_meshopt_compression extension.
**标签**：`extension`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Arseny Kapoulkine 2025

> 本目录由 `python tools/migrate_gltf_models.py --only MeshoptCubeTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `MeshoptCubeTest.gltf` |
| 节点 / 网格 / primitive | 35 / 35 / 35 |
| 顶点 / 三角形 | 640 / 320 |
| 材质 / 纹理 / 图像 | 10 / 10 / 10 |
| 图像编码 | png×10 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| 使用扩展 | `KHR_mesh_quantization`、`KHR_meshopt_compression` |
| 必需扩展 | `KHR_mesh_quantization` |
| alpha 模式分布 | MASK×10 |

**导入产物**：`assets/` 35 个 `.mesh.json`（合计 50.9 KB），`textures/` 10 张；场景 `MeshoptCubeTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only MeshoptCubeTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/MeshoptCubeTest/glTF/MeshoptCubeTest.gltf --out scenes/khronos/MeshoptCubeTest --name MeshoptCubeTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/MeshoptCubeTest/MeshoptCubeTest.scn
./build-win/samples/player/maque_player --shot /tmp/MeshoptCubeTest.png scenes/khronos/MeshoptCubeTest/MeshoptCubeTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（1324×1471）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.750 | 0.828 | 差 +0.078 |
| 前景平均 RGB | [193.0, 190.3, 194.9] | [210.0, 210.9, 217.0] | RGB 距离 0.078 |
| 亮度直方图相似度 | — | — | 0.48（16 桶交集，1=完全一致） |
| 前景占比 | 16.1% | 6.8% | 覆盖率比 0.425 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- KHR_mesh_quantization（量化顶点）导入未特殊处理（数值/法线精度可能异常）
- meshopt 压缩未实现
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- KHR_mesh_quantization（量化顶点）导入未特殊处理（数值/法线精度可能异常）
- meshopt 压缩未实现
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：压缩/未压缩各变体的立方体阵列全部成功渲染（说明 meshopt/量化数据可用）、标签可读、布局正确；但立方体全为白色，官方为顶点色染色的彩色立方体。差异可归因于 COLOR_0 未使用（8 个含顶点色模型之一）。
- 差异主因：缺失特性：COLOR_0 顶点色未使用（meshopt 压缩几何本身加载成功）
