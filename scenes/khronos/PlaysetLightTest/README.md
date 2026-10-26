# PlaysetLightTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/PlaysetLightTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/PlaysetLightTest)  
**上游概览**：A 3D scene with IKEA assets
**标签**：`test`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution Non Commercial Share Alike 4.0 International（CC-BY-NC-SA-4.0）https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode，Inter IKEA Systems B.V. 2024

> 本目录由 `python tools/migrate_gltf_models.py --only PlaysetLightTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `PlaysetLightTest.gltf` |
| 节点 / 网格 / primitive | 11 / 6 / 6 |
| 顶点 / 三角形 | 89,141 / 129,693 |
| 材质 / 纹理 / 图像 | 6 / 19 / 19 |
| 图像编码 | jpeg×3、png×16 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| 使用扩展 | `KHR_lights_punctual`、`KHR_materials_emissive_strength` |
| 必需扩展 | `KHR_lights_punctual`、`KHR_materials_emissive_strength` |
| alpha 模式分布 | OPAQUE×6 |

**导入产物**：`assets/` 6 个 `.mesh.json`（合计 9.7 MB），`textures/` 19 张；场景 `PlaysetLightTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only PlaysetLightTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/PlaysetLightTest/glTF/PlaysetLightTest.gltf --out scenes/khronos/PlaysetLightTest --name PlaysetLightTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/PlaysetLightTest/PlaysetLightTest.scn
./build-win/samples/player/maque_player --shot /tmp/PlaysetLightTest.png scenes/khronos/PlaysetLightTest/PlaysetLightTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（1922×1112）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.166 | 0.780 | 差 +0.615 |
| 前景平均 RGB | [38.9, 43.1, 45.1] | [174.4, 205.0, 210.4] | RGB 距离 0.605 |
| 亮度直方图相似度 | — | — | 0.04（16 桶交集，1=完全一致） |
| 前景占比 | 99.7% | 17.5% | 覆盖率比 0.176 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_lights_punctual 未导入（统一用自动生成的方向光 + 引擎 analytic studio IBL）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
