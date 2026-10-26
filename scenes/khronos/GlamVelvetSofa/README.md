# GlamVelvetSofa —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/GlamVelvetSofa](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/GlamVelvetSofa)  
**上游概览**：Sofa using material variants, sheen, and specular.
**标签**：`video`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2021

> 本目录由 `python tools/migrate_gltf_models.py --only GlamVelvetSofa` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `GlamVelvetSofa.gltf` |
| 节点 / 网格 / primitive | 4 / 3 / 3 |
| 顶点 / 三角形 | 3,118 / 4,196 |
| 材质 / 纹理 / 图像 | 7 / 2 / 2 |
| 图像编码 | png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_texture_transform`、`KHR_materials_sheen`、`KHR_materials_specular`、`KHR_materials_variants`、`KHR_lights_punctual` |
| 必需扩展 | `KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×7 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 333.2 KB），`textures/` 2 张；场景 `GlamVelvetSofa.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only GlamVelvetSofa --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/GlamVelvetSofa/glTF/GlamVelvetSofa.gltf --out scenes/khronos/GlamVelvetSofa --name GlamVelvetSofa --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/GlamVelvetSofa/GlamVelvetSofa.scn
./build-win/samples/player/maque_player --shot /tmp/GlamVelvetSofa.png scenes/khronos/GlamVelvetSofa/GlamVelvetSofa.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（989×409）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.234 | 0.168 | 差 -0.065 |
| 前景平均 RGB | [44.9, 60.9, 89.1] | [30.3, 43.6, 73.7] | RGB 距离 0.062 |
| 亮度直方图相似度 | — | — | 0.54（16 桶交集，1=完全一致） |
| 前景占比 | 43.3% | 16.5% | 覆盖率比 0.381 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似
- KHR_materials_specular：factor/color 已映射；specularTexture/specularColorTexture 与 f90 行为未实现
- KHR_materials_variants：只使用默认材质（不做变体切换）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似
- KHR_materials_specular：factor/color 已映射；specularTexture/specularColorTexture 与 f90 行为未实现
- KHR_materials_variants：只使用默认材质（不做变体切换）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
