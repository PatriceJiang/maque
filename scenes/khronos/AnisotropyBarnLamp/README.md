# AnisotropyBarnLamp —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/AnisotropyBarnLamp](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/AnisotropyBarnLamp)  
**上游概览**：Wayfair AnisotropyBarnLamp model using anisotropy, clearcoat, emissive_strength, transmission, volume, and basisu extensions.
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2023

> 本目录由 `python tools/migrate_gltf_models.py --only AnisotropyBarnLamp` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `AnisotropyBarnLamp.gltf` |
| 节点 / 网格 / primitive | 3 / 3 / 3 |
| 顶点 / 三角形 | 7,712 / 10,203 |
| 材质 / 纹理 / 图像 | 3 / 4 / 4 |
| 图像编码 | png×4 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_anisotropy`、`KHR_materials_clearcoat`、`KHR_materials_emissive_strength`、`KHR_materials_transmission`、`KHR_materials_volume` |
| alpha 模式分布 | OPAQUE×3 |

**导入产物**：`assets/` 3 个 `.mesh.json`（合计 816.1 KB），`textures/` 4 张；场景 `AnisotropyBarnLamp.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only AnisotropyBarnLamp --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/AnisotropyBarnLamp/glTF/AnisotropyBarnLamp.gltf --out scenes/khronos/AnisotropyBarnLamp --name AnisotropyBarnLamp --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/AnisotropyBarnLamp/AnisotropyBarnLamp.scn
./build-win/samples/player/maque_player --shot /tmp/AnisotropyBarnLamp.png scenes/khronos/AnisotropyBarnLamp/AnisotropyBarnLamp.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1055×1202）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.512 | 0.411 | 差 -0.101 |
| 前景平均 RGB | [148.3, 126.9, 115.3] | [131.5, 98.9, 83.7] | RGB 距离 0.100 |
| 亮度直方图相似度 | — | — | 0.69（16 桶交集，1=完全一致） |
| 前景占比 | 63.7% | 7.3% | 覆盖率比 0.115 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_anisotropy：strength/贴图已导入、只做粗糙度近似（各向异性 BRDF/方向未实现）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_anisotropy：strength/贴图已导入、只做粗糙度近似（各向异性 BRDF/方向未实现）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：铜质谷仓壁灯完整：墙座、支臂、灯罩、灯泡都在，铜材质与清漆高光可信，朝向合理。与官方近景相比缺少强烈的各向异性拉丝反射，且机位更远，但整体可接受、无导入异常。
- 差异主因：缺失特性：anisotropy 拉丝高光偏弱、无 HDRI
