# TextureCoordinateTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TextureCoordinateTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TextureCoordinateTest)  
**上游概览**：Shows how XYZ and UV positions relate to displayed geometry.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Ed Mackey 2017

> 本目录由 `python tools/migrate_gltf_models.py --only TextureCoordinateTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TextureCoordinateTest.gltf` |
| 节点 / 网格 / primitive | 5 / 5 / 5 |
| 顶点 / 三角形 | 20 / 10 |
| 材质 / 纹理 / 图像 | 5 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×5 |

**导入产物**：`assets/` 5 个 `.mesh.json`（合计 3.8 KB），`textures/` 1 张；场景 `TextureCoordinateTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TextureCoordinateTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TextureCoordinateTest/glTF/TextureCoordinateTest.gltf --out scenes/khronos/TextureCoordinateTest --name TextureCoordinateTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TextureCoordinateTest/TextureCoordinateTest.scn
./build-win/samples/player/maque_player --shot /tmp/TextureCoordinateTest.png scenes/khronos/TextureCoordinateTest/TextureCoordinateTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（300×300）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.562 | 0.534 | 差 -0.029 |
| 前景平均 RGB | [122.5, 155.7, 83.0] | [123.9, 144.8, 85.1] | RGB 距离 0.019 |
| 亮度直方图相似度 | — | — | 0.22（16 桶交集，1=完全一致） |
| 前景占比 | 81.7% | 27.8% | 覆盖率比 0.339 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
