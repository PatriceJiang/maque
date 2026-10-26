# CompareSheen —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CompareSheen](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CompareSheen)  
**上游概览**：This model compares sheen methods.
**标签**：`extension`、`testing`、`pbrtest`  
**许可**：（资产元数据，保留出处）
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Eric Chadwick and DGG 2024

> 本目录由 `python tools/migrate_gltf_models.py --only CompareSheen` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CompareSheen.gltf` |
| 节点 / 网格 / primitive | 2 / 2 / 2 |
| 顶点 / 三角形 | 1,346 / 2,560 |
| 材质 / 纹理 / 图像 | 2 / 3 / 3 |
| 图像编码 | jpeg×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_sheen` |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 147.2 KB），`textures/` 3 张；场景 `CompareSheen.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CompareSheen --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CompareSheen/glTF/CompareSheen.gltf --out scenes/khronos/CompareSheen --name CompareSheen --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CompareSheen/CompareSheen.scn
./build-win/samples/player/maque_player --shot /tmp/CompareSheen.png scenes/khronos/CompareSheen/CompareSheen.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（150×74）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.359 | 0.263 | 差 -0.096 |
| 前景平均 RGB | [154.9, 76.2, 55.4] | [129.1, 52.0, 34.1] | RGB 距离 0.093 |
| 亮度直方图相似度 | — | — | 0.63（16 桶交集，1=完全一致） |
| 前景占比 | 70.5% | 19.8% | 覆盖率比 0.281 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_sheen：sheen 因子/颜色为观感近似，sheen 贴图未实现
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_sheen：sheen 因子/颜色为观感近似，sheen 贴图未实现

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：两颗红色 sheen 球完整，glTF logo 与边缘柔和的绒面反光可见，与官方两球的观感接近（右侧球偏暗属光照）。未发现贴图错误或异常。
- 差异主因：缺失特性：无 HDRI IBL（高光/环境偏弱）
