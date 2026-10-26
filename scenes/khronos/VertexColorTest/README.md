# VertexColorTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/VertexColorTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/VertexColorTest)  
**上游概览**：Tests if vertex colors are supported.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2018

> 本目录由 `python tools/migrate_gltf_models.py --only VertexColorTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `VertexColorTest.gltf` |
| 节点 / 网格 / primitive | 2 / 2 / 2 |
| 顶点 / 三角形 | 72 / 36 |
| 材质 / 纹理 / 图像 | 2 / 2 / 2 |
| 图像编码 | png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 6.5 KB），`textures/` 2 张；场景 `VertexColorTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only VertexColorTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/VertexColorTest/glTF/VertexColorTest.gltf --out scenes/khronos/VertexColorTest --name VertexColorTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/VertexColorTest/VertexColorTest.scn
./build-win/samples/player/maque_player --shot /tmp/VertexColorTest.png scenes/khronos/VertexColorTest/VertexColorTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（256×256）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.089 | 0.190 | 差 +0.102 |
| 前景平均 RGB | [22.2, 22.1, 28.5] | [38.4, 38.1, 181.2] | RGB 距离 0.242 |
| 亮度直方图相似度 | — | — | 0.16（16 桶交集，1=完全一致） |
| 前景占比 | 47.5% | 23.1% | 覆盖率比 0.486 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- COLOR_0 顶点色未使用
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- COLOR_0 顶点色未使用

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：测试面板/文字标签/红绿蓝行布局完整，但 Test 列本应全为彩色 ✓（官方如此），maque 显示异常配色的 X 形；Sample pass 列正常。与 manifest 记录的 COLOR_0 未使用一致，属顶点色通道缺失导致的可视差异。
- 差异主因：缺失特性：COLOR_0 顶点色未使用
