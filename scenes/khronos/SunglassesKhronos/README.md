# SunglassesKhronos —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/SunglassesKhronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/SunglassesKhronos)  
**上游概览**：Sunglasses with Khronos logos and iridescent lenses
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2024
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2015
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only SunglassesKhronos` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `SunglassesKhronos.gltf` |
| 节点 / 网格 / primitive | 16 / 8 / 8 |
| 顶点 / 三角形 | 7,420 / 13,396 |
| 材质 / 纹理 / 图像 | 5 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_ior`、`KHR_materials_iridescence` |
| alpha 模式分布 | OPAQUE×5 |

**导入产物**：`assets/` 8 个 `.mesh.json`（合计 762.2 KB），`textures/` 1 张；场景 `SunglassesKhronos.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only SunglassesKhronos --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/SunglassesKhronos/glTF/SunglassesKhronos.gltf --out scenes/khronos/SunglassesKhronos --name SunglassesKhronos --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/SunglassesKhronos/SunglassesKhronos.scn
./build-win/samples/player/maque_player --shot /tmp/SunglassesKhronos.png scenes/khronos/SunglassesKhronos/SunglassesKhronos.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1474×869）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.511 | 0.354 | 差 -0.157 |
| 前景平均 RGB | [136.4, 124.8, 167.5] | [80.5, 92.5, 97.4] | RGB 距离 0.207 |
| 亮度直方图相似度 | — | — | 0.52（16 桶交集，1=完全一致） |
| 前景占比 | 53.5% | 6.4% | 覆盖率比 0.119 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- KHR_materials_iridescence：薄膜干涉为观感近似，厚度/虹彩贴图未实现
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- KHR_materials_iridescence：薄膜干涉为观感近似，厚度/虹彩贴图未实现

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
