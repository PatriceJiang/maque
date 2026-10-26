# CarConcept —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CarConcept](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CarConcept)  
**上游概览**：Concept car with Khronos logos and material variants
**标签**：`showcase`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2024
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2015
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017

> 本目录由 `python tools/migrate_gltf_models.py --only CarConcept` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CarConcept.gltf` |
| 节点 / 网格 / primitive | 101 / 97 / 109 |
| 顶点 / 三角形 | 162,766 / 213,347 |
| 材质 / 纹理 / 图像 | 29 / 15 / 14 |
| 图像编码 | png×14 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_clearcoat`、`KHR_materials_emissive_strength`、`KHR_materials_iridescence`、`KHR_materials_transmission`、`KHR_materials_variants`、`KHR_texture_transform` |
| alpha 模式分布 | OPAQUE×29 |

**导入产物**：`assets/` 109 个 `.mesh.json`（合计 19.7 MB），`textures/` 13 张；场景 `CarConcept.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CarConcept --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CarConcept/glTF/CarConcept.gltf --out scenes/khronos/CarConcept --name CarConcept --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CarConcept/CarConcept.scn
./build-win/samples/player/maque_player --shot /tmp/CarConcept.png scenes/khronos/CarConcept/CarConcept.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1280×953）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.146 | 0.287 | 差 +0.141 |
| 前景平均 RGB | [69.0, 27.8, 36.7] | [131.1, 57.0, 60.6] | RGB 距离 0.151 |
| 亮度直方图相似度 | — | — | 0.30（16 桶交集，1=完全一致） |
| 前景占比 | 58.0% | 13.8% | 覆盖率比 0.237 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- KHR_materials_variants：只使用默认材质（不做变体切换）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_texture_transform 支持 scale/offset；rotation 与逐贴图独立 transform 部分缺失
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- KHR_materials_variants：只使用默认材质（不做变体切换）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：红色概念跑车完整、比例与姿态正确，红色车漆 clearcoat 高光可信，轮胎/轮毂结构正常、无破面。车顶/座舱大面积为白色（官方为深色玻璃+彩色虹彩反射），与 clearcoat 高光在没有 HDRI 时被 analytic 环境拉亮有关，属缺失特性/IBL 限制；源资产的 KTX2/Draco 变体未被迁移，不影响主变体观感。
- 差异主因：缺失特性：transmission/iridescence/无 HDRI → 车顶座舱发白过曝；KTX2 变体未迁移
