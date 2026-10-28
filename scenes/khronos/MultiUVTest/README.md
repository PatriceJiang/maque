# MultiUVTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/MultiUVTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/MultiUVTest)  
**上游概览**：This model has two uv texture coordinates and tests a second set of texture coordinates.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Hilo 3D 2017
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only MultiUVTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `MultiUVTest.gltf` |
| 节点 / 网格 / primitive | 3 / 1 / 1 |
| 顶点 / 三角形 | 24 / 12 |
| 材质 / 纹理 / 图像 | 1 / 2 / 2 |
| 图像编码 | png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 2.3 KB），`textures/` 2 张；场景 `MultiUVTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only MultiUVTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/MultiUVTest/glTF/MultiUVTest.gltf --out scenes/khronos/MultiUVTest --name MultiUVTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/MultiUVTest/MultiUVTest.scn
./build-win/samples/player/maque_player --shot /tmp/MultiUVTest.png scenes/khronos/MultiUVTest/MultiUVTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（320×320）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.446 | 0.740 | 差 +0.295 |
| 前景平均 RGB | [104.2, 117.9, 99.3] | [187.8, 195.1, 129.4] | RGB 距离 0.249 |
| 亮度直方图相似度 | — | — | 0.16（16 桶交集，1=完全一致） |
| 前景占比 | 61.6% | 26.9% | 覆盖率比 0.436 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：立方体几何与 UV0 贴图完整，但立方体表面显示的是“Multiple UVs not supported in this viewer”文字贴图，官方参考图为经 TEXCOORD_1 采样的 glTF logo 图案。与 README 已知差距（引擎只用 TEXCOORD_0）完全对应，属预期行为。
- 差异主因：缺失特性/引擎限制：TEXCOORD_1 未使用 → 显示“Multiple UVs not supported”兜底贴图
