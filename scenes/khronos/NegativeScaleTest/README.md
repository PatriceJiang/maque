# NegativeScaleTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/NegativeScaleTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/NegativeScaleTest)  
**上游概览**：This model tests what happens when a glTF node has negative scaling.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2023

> 本目录由 `python tools/migrate_gltf_models.py --only NegativeScaleTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `NegativeScaleTest.gltf` |
| 节点 / 网格 / primitive | 14 / 8 / 8 |
| 顶点 / 三角形 | 2,032 / 3,884 |
| 材质 / 纹理 / 图像 | 6 / 2 / 2 |
| 图像编码 | png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×6 |

**导入产物**：`assets/` 8 个 `.mesh.json`（合计 207.8 KB），`textures/` 2 张；场景 `NegativeScaleTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only NegativeScaleTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/NegativeScaleTest/glTF/NegativeScaleTest.gltf --out scenes/khronos/NegativeScaleTest --name NegativeScaleTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/NegativeScaleTest/NegativeScaleTest.scn
./build-win/samples/player/maque_player --shot /tmp/NegativeScaleTest.png scenes/khronos/NegativeScaleTest/NegativeScaleTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（294×256）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.262 | 0.542 | 差 +0.280 |
| 前景平均 RGB | [37.7, 69.2, 127.7] | [110.8, 140.1, 198.2] | RGB 距离 0.281 |
| 亮度直方图相似度 | — | — | 0.13（16 桶交集，1=完全一致） |
| 前景占比 | 93.2% | 29.4% | 覆盖率比 0.316 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：蓝色测试面板完整：scale 1.0/-1.0 行、front/back/sample 列的绿色 ✓ 与官方一致，白色/深蓝/金属三种球在 ±1 缩放下均正常、镜像未出现破面或剔除错误。金属球缺环境反射属 IBL 限制。
- 差异主因：缺失特性：无 HDRI（金属球反射弱）；机位/背景
