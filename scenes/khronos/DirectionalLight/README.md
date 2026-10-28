# DirectionalLight —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/DirectionalLight](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DirectionalLight)  
**上游概览**：Verification of light calculations
**标签**：`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Rickard Sahlin 2020

> 本目录由 `python tools/migrate_gltf_models.py --only DirectionalLight` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `DirectionalLight.gltf` |
| 节点 / 网格 / primitive | 5 / 3 / 3 |
| 顶点 / 三角形 | 16,122 / 31,800 |
| 材质 / 纹理 / 图像 | 3 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| 使用扩展 | `KHR_lights_punctual` |
| 必需扩展 | `KHR_lights_punctual` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 1.7 MB），`textures/` 0 张；场景 `DirectionalLight.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only DirectionalLight --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/DirectionalLight/glTF/DirectionalLight.gltf --out scenes/khronos/DirectionalLight --name DirectionalLight --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/DirectionalLight/DirectionalLight.scn
./build-win/samples/player/maque_player --shot /tmp/DirectionalLight.png scenes/khronos/DirectionalLight/DirectionalLight.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（1919×1110）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.309 | 0.365 | 差 +0.055 |
| 前景平均 RGB | [86.3, 81.7, 29.0] | [104.7, 97.1, 16.3] | RGB 距离 0.061 |
| 亮度直方图相似度 | — | — | 0.50（16 桶交集，1=完全一致） |
| 前景占比 | 14.3% | 14.3% | 覆盖率比 1.0 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）

## 踩过的坑 / 注意

- 手动机位：手动机位：glTF 自带相机 + 官方浅灰背景（0.88）
- 手调环境：手调环境：关闭 analytic IBL（官方为纯方向光测试）
- 手调灯光：光强按 glTF 原值（1.0；README 的能量测试语义，importer 观感归一化 ×2.6 不适用）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
