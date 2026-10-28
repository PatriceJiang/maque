# DamagedHelmet —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/DamagedHelmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet)  
**上游概览**：Flight helmet with damage
**标签**：`core`、`showcase`、`testing`、`video`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，ctxwing 2018
- Creative Commons Attribution Non Commercial 4.0 International（CC-BY-NC-4.0）https://creativecommons.org/licenses/by-nc/4.0/legalcode，theblueturtle_ 2016

> 本目录由 `python tools/migrate_gltf_models.py --only DamagedHelmet` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `DamagedHelmet.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 1 |
| 顶点 / 三角形 | 14,556 / 15,452 |
| 材质 / 纹理 / 图像 | 1 / 5 / 5 |
| 图像编码 | jpeg×5 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 1.5 MB），`textures/` 5 张；场景 `DamagedHelmet.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only DamagedHelmet --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/DamagedHelmet/glTF/DamagedHelmet.gltf --out scenes/khronos/DamagedHelmet --name DamagedHelmet --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/DamagedHelmet/DamagedHelmet.scn
./build-win/samples/player/maque_player --shot /tmp/DamagedHelmet.png scenes/khronos/DamagedHelmet/DamagedHelmet.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（130×126，含透明背景）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.319 | 0.261 | 差 -0.058 |
| 前景平均 RGB | [82.9, 82.0, 68.6] | [60.1, 67.8, 71.8] | RGB 距离 0.052 |
| 亮度直方图相似度 | — | — | 0.82（16 桶交集，1=完全一致） |
| 前景占比 | 60.7% | 38.9% | 覆盖率比 0.641 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）；预览主体贴边/被裁剪 → 机位过近？**

⚠️ 构图告警：预览主体贴边/被裁剪 → 机位过近？

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 手动机位：手动机位：官方 3/4 前视特写（方位 45°/仰角 8°、距离系数 0.95、浅灰底）
- 手调环境：手调环境：analytic 亮度 1.6→0.8（金属高光不过曝，对齐官方亮度/直方图）
- 参考图说明：官方 screenshot.png 只有 130×126、带 alpha（旧 SampleViewer 截图），整图 hist/rgb 指标只作趋势参考
- 构图告警：预览主体贴边/被裁剪 → 机位过近？

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：头盔完整（面罩、护甲、发光片都在），BaseColor/MR/normal/emissive 贴图正确，面罩上发光元素可见，无破面或法线反向。与官方差距主要是整体偏暗、金属高光与环境反射弱（无 HDRI），属引擎光照能力差距而非导入问题。
- 差异主因：缺失特性：无 HDRI IBL/面光 → 金属反射与整体亮度偏弱
