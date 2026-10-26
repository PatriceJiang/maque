# IridescenceSuzanne —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/IridescenceSuzanne](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/IridescenceSuzanne)  
**上游概览**：Further tests KHR_materials_iridescence.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Mathias Kanzler 2022
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Pascal Schoen 2021

> 本目录由 `python tools/migrate_gltf_models.py --only IridescenceSuzanne` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `IridescenceSuzanne.gltf` |
| 节点 / 网格 / primitive | 4 / 3 / 3 |
| 顶点 / 三角形 | 9,963 / 11,808 |
| 材质 / 纹理 / 图像 | 3 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_lights_punctual`、`KHR_materials_ior`、`KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_iridescence` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 1.0 MB），`textures/` 1 张；场景 `IridescenceSuzanne.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only IridescenceSuzanne --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/IridescenceSuzanne/glTF/IridescenceSuzanne.gltf --out scenes/khronos/IridescenceSuzanne --name IridescenceSuzanne --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/IridescenceSuzanne/IridescenceSuzanne.scn
./build-win/samples/player/maque_player --shot /tmp/IridescenceSuzanne.png scenes/khronos/IridescenceSuzanne/IridescenceSuzanne.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（2000×800）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.506 | 0.634 | 差 +0.128 |
| 前景平均 RGB | [125.6, 129.7, 131.3] | [150.4, 164.9, 162.0] | RGB 距离 0.118 |
| 亮度直方图相似度 | — | — | 0.55（16 桶交集，1=完全一致） |
| 前景占比 | 19.4% | 11.4% | 覆盖率比 0.587 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
