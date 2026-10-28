# GlassBrokenWindow —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/GlassBrokenWindow](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/GlassBrokenWindow)  
**上游概览**：This asset demonstrates the combination of two transparency methods in glTF: KHR_materials_transmission for glass and alphaMode:'MASK' for holes in the broken glass.
**标签**：`video`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2023

> 本目录由 `python tools/migrate_gltf_models.py --only GlassBrokenWindow` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `GlassBrokenWindow.gltf` |
| 节点 / 网格 / primitive | 3 / 3 / 3 |
| 顶点 / 三角形 | 984 / 987 |
| 材质 / 纹理 / 图像 | 3 / 5 / 5 |
| 图像编码 | jpeg×3、png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission` |
| alpha 模式分布 | MASK×1、OPAQUE×2 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 96.9 KB），`textures/` 5 张；场景 `GlassBrokenWindow.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only GlassBrokenWindow --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/GlassBrokenWindow/glTF/GlassBrokenWindow.gltf --out scenes/khronos/GlassBrokenWindow --name GlassBrokenWindow --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/GlassBrokenWindow/GlassBrokenWindow.scn
./build-win/samples/player/maque_player --shot /tmp/GlassBrokenWindow.png scenes/khronos/GlassBrokenWindow/GlassBrokenWindow.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（1110×1115）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.491 | 0.759 | 差 +0.268 |
| 前景平均 RGB | [117.1, 127.6, 126.6] | [189.1, 194.5, 197.4] | RGB 距离 0.274 |
| 亮度直方图相似度 | — | — | 0.28（16 桶交集，1=完全一致） |
| 前景占比 | 97.9% | 11.2% | 覆盖率比 0.115 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- HDRI 环境回退 analytic（官方 env: Cannon Exterior；指标 0.245 < 解析 0.278）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
