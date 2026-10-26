# CesiumMilkTruck —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CesiumMilkTruck](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CesiumMilkTruck)  
**上游概览**：Textured. Multiple nodes/meshes. Animations. [Issues: non-Khronos mark]
**标签**：`core`、`issues`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Cesium 2017
- Cesium Trademark or Logo（LicenseRef-LegalMark-Cesium）../../LICENSES/LicenseRef-LegalMark-Cesium.txt，Non-copyrightable logo 2015

> 本目录由 `python tools/migrate_gltf_models.py --only CesiumMilkTruck` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CesiumMilkTruck.gltf` |
| 节点 / 网格 / primitive | 6 / 2 / 4 |
| 顶点 / 三角形 | 3,995 / 2,856 |
| 材质 / 纹理 / 图像 | 4 / 2 / 1 |
| 图像编码 | jpeg×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×4 |

**导入产物**：`assets/` 4 个 `.mesh.json`（合计 337.8 KB），`textures/` 1 张；场景 `CesiumMilkTruck.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CesiumMilkTruck --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CesiumMilkTruck/glTF/CesiumMilkTruck.gltf --out scenes/khronos/CesiumMilkTruck --name CesiumMilkTruck --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CesiumMilkTruck/CesiumMilkTruck.scn
./build-win/samples/player/maque_player --shot /tmp/CesiumMilkTruck.png scenes/khronos/CesiumMilkTruck/CesiumMilkTruck.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.gif`（130×130）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.481 | 0.652 | 差 +0.171 |
| 前景平均 RGB | [120.0, 123.4, 123.7] | [158.4, 168.8, 164.9] | RGB 距离 0.164 |
| 亮度直方图相似度 | — | — | 0.37（16 桶交集，1=完全一致） |
| 前景占比 | 29.9% | 16.0% | 覆盖率比 0.536 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：只看到卡车底盘（车顶朝下、四轮朝上）且右侧被画面裁断，与官方侧视整车的参考图完全不可比。模型几何/贴图都在，但根节点 Yup2Zup 的三轴四元数在导入器 Euler 分解时命中 gimbal lock，按 runtime 的 Rz*Ry*Rx 重组后姿态错误（详见报告与 README 手动笔记）。
- 差异主因：导入问题+机位/背景：节点变换（Euler 分解 gimbal-lock）导致整车翻倒
