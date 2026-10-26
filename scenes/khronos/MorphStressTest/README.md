# MorphStressTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/MorphStressTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/MorphStressTest)  
**上游概览**：Tests up to 8 morph targets.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2021

> 本目录由 `python tools/migrate_gltf_models.py --only MorphStressTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `MorphStressTest.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 2 |
| 顶点 / 三角形 | 1,528 / 2,412 |
| 材质 / 纹理 / 图像 | 2 / 3 / 3 |
| 图像编码 | png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 3 / 0 / 2 / 0 |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 163.1 KB），`textures/` 3 张；场景 `MorphStressTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only MorphStressTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/MorphStressTest/glTF/MorphStressTest.gltf --out scenes/khronos/MorphStressTest --name MorphStressTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/MorphStressTest/MorphStressTest.scn
./build-win/samples/player/maque_player --shot /tmp/MorphStressTest.png scenes/khronos/MorphStressTest/MorphStressTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.png`（640×360）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.534 | 0.608 | 差 +0.073 |
| 前景平均 RGB | [137.9, 136.7, 126.2] | [153.5, 157.1, 137.0] | RGB 距离 0.061 |
| 亮度直方图相似度 | — | — | 0.70（16 桶交集，1=完全一致） |
| 前景占比 | 30.4% | 17.9% | 覆盖率比 0.588 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- morph targets 未应用（默认权重不生效）
- 部分贴图使用 TEXCOORD_1/更高 UV 集（引擎只用 TEXCOORD_0 → 贴图 UV 可能错误）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- morph targets 未应用（默认权重不生效）
- 部分贴图使用 TEXCOORD_1/更高 UV 集（引擎只用 TEXCOORD_0 → 贴图 UV 可能错误）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
