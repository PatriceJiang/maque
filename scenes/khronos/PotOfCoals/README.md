# PotOfCoals —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/PotOfCoals](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/PotOfCoals)  
**上游概览**：Burnt copper pot with a clearcoat, containing emissive hot coals.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2023
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2015
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only PotOfCoals` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `PotOfCoals.gltf` |
| 节点 / 网格 / primitive | 2 / 2 / 2 |
| 顶点 / 三角形 | 54,669 / 18,223 |
| 材质 / 纹理 / 图像 | 2 / 8 / 8 |
| 图像编码 | jpeg×7、png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_clearcoat` |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 5.0 MB），`textures/` 8 张；场景 `PotOfCoals.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only PotOfCoals --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/PotOfCoals/glTF/PotOfCoals.gltf --out scenes/khronos/PotOfCoals --name PotOfCoals --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/PotOfCoals/PotOfCoals.scn
./build-win/samples/player/maque_player --shot /tmp/PotOfCoals.png scenes/khronos/PotOfCoals/PotOfCoals.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1315×1183）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.426 | 0.315 | 差 -0.111 |
| 前景平均 RGB | [147.5, 100.8, 74.0] | [108.6, 74.8, 53.2] | RGB 距离 0.112 |
| 亮度直方图相似度 | — | — | 0.48（16 桶交集，1=完全一致） |
| 前景占比 | 84.7% | 15.2% | 覆盖率比 0.179 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似

## 踩过的坑 / 注意

- HDRI 环境回退 analytic（官方 env: Dining Room；指标 0.406 < 解析 0.484）

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
