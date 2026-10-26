# SheenWoodLeatherSofa —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/SheenWoodLeatherSofa](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/SheenWoodLeatherSofa)  
**上游概览**：Sofa asset using sheen, specular, and high-quality tiling textures.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2024
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Fran Calvente 2021

> 本目录由 `python tools/migrate_gltf_models.py --only SheenWoodLeatherSofa` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `SheenWoodLeatherSofa.gltf` |
| 节点 / 网格 / primitive | 6 / 6 / 6 |
| 顶点 / 三角形 | 139,476 / 46,492 |
| 材质 / 纹理 / 图像 | 6 / 15 / 13 |
| 图像编码 | webp×13 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_specular`、`KHR_materials_sheen`、`KHR_texture_transform`、`EXT_texture_webp` |
| 必需扩展 | `KHR_texture_transform`、`EXT_texture_webp` |
| alpha 模式分布 | BLEND×1、OPAQUE×5 |

**导入产物**：`assets/` 6 个 `.mesh.json`（合计 15.5 MB），`textures/` 13 张；场景 `SheenWoodLeatherSofa.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only SheenWoodLeatherSofa --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/SheenWoodLeatherSofa/glTF/SheenWoodLeatherSofa.gltf --out scenes/khronos/SheenWoodLeatherSofa --name SheenWoodLeatherSofa --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/SheenWoodLeatherSofa/SheenWoodLeatherSofa.scn
./build-win/samples/player/maque_player --shot /tmp/SheenWoodLeatherSofa.png scenes/khronos/SheenWoodLeatherSofa/SheenWoodLeatherSofa.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1610×720）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.286 | 0.269 | 差 -0.017 |
| 前景平均 RGB | [93.2, 69.4, 50.5] | [94.0, 63.7, 42.1] | RGB 距离 0.019 |
| 亮度直方图相似度 | — | — | 0.67（16 桶交集，1=完全一致） |
| 前景占比 | 64.1% | 25.3% | 覆盖率比 0.396 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_sheen：sheen 因子/颜色为观感近似，sheen 贴图未实现
- KHR_materials_specular：specularFactor/specularColor 已映射，f90 行为与贴图未实现
- WebP 贴图：导入期用 PIL 转 PNG（需 Pillow；无 Pillow 时该贴图缺失）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_sheen：sheen 因子/颜色为观感近似，sheen 贴图未实现
- KHR_materials_specular：specularFactor/specularColor 已映射，f90 行为与贴图未实现
- WebP 贴图：导入期用 PIL 转 PNG（需 Pillow；无 Pillow 时该贴图缺失）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：沙发几何完整（软垫/靠背/木框形状正确），但所有部件呈纯白无贴图，官方为深色木质+金棕色织物。README 已记录“WebP 贴图无解码器”，本模型是 148 个里唯一使用 EXT_texture_webp 的模型，属明确可定位的贴图缺失问题（补 WebP 解码即可恢复）。
- 差异主因：导入问题：EXT_texture_webp 贴图无解码器 → 全部贴图缺失（白模）
