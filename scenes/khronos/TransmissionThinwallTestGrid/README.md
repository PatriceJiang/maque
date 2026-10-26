# TransmissionThinwallTestGrid —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TransmissionThinwallTestGrid](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TransmissionThinwallTestGrid)  
**上游概览**：Grid of spheres over a checkered backdrop to test Thinfilm and IOR rendering.
**标签**：`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Eric Chadwick 2023

> 本目录由 `python tools/migrate_gltf_models.py --only TransmissionThinwallTestGrid` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TransmissionThinwallTestGrid.gltf` |
| 节点 / 网格 / primitive | 14 / 14 / 14 |
| 顶点 / 三角形 | 41,946 / 13,982 |
| 材质 / 纹理 / 图像 | 13 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_ior` |
| alpha 模式分布 | OPAQUE×13 |

**导入产物**：`assets/` 14 个 `.mesh.json`（合计 3.4 MB），`textures/` 1 张；场景 `TransmissionThinwallTestGrid.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TransmissionThinwallTestGrid --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TransmissionThinwallTestGrid/glTF/TransmissionThinwallTestGrid.gltf --out scenes/khronos/TransmissionThinwallTestGrid --name TransmissionThinwallTestGrid --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TransmissionThinwallTestGrid/TransmissionThinwallTestGrid.scn
./build-win/samples/player/maque_player --shot /tmp/TransmissionThinwallTestGrid.png scenes/khronos/TransmissionThinwallTestGrid/TransmissionThinwallTestGrid.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1280×600）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.310 | 0.693 | 差 +0.383 |
| 前景平均 RGB | [87.6, 77.0, 73.5] | [176.4, 176.7, 178.8] | RGB 距离 0.384 |
| 亮度直方图相似度 | — | — | 0.08（16 桶交集，1=完全一致） |
| 前景占比 | 56.3% | 16.3% | 覆盖率比 0.289 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：测试板标签（Index of Refraction 1.0/1.33/1.5/1.76/2.42、Thin Wall / Volume Transmission 两行）和 10 个球位布局正确，说明场景结构导入无误；但所有球均为不透明白球，官方为可折射棋盘格的玻璃球，属 transmission 已知差距。
- 差异主因：缺失特性：transmission/volume（薄壁与体积传输）未实现
