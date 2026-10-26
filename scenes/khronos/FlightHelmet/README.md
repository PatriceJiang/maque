# FlightHelmet —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/FlightHelmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/FlightHelmet)  
**上游概览**：Displayed flight helmet on a wooden stand.
**标签**：`showcase`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Gary Hsu 2018

> 本目录由 `python tools/migrate_gltf_models.py --only FlightHelmet` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `FlightHelmet.gltf` |
| 节点 / 网格 / primitive | 6 / 6 / 6 |
| 顶点 / 三角形 | 55,392 / 94,722 |
| 材质 / 纹理 / 图像 | 6 / 15 / 15 |
| 图像编码 | png×15 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_transmission` |
| alpha 模式分布 | OPAQUE×6 |

**导入产物**：`assets/` 6 个 `.mesh.json`（合计 6.2 MB），`textures/` 15 张；场景 `FlightHelmet.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only FlightHelmet --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/FlightHelmet/glTF/FlightHelmet.gltf --out scenes/khronos/FlightHelmet --name FlightHelmet --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/FlightHelmet/FlightHelmet.scn
./build-win/samples/player/maque_player --shot /tmp/FlightHelmet.png scenes/khronos/FlightHelmet/FlightHelmet.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.jpg`（130×130）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.610 | 0.353 | 差 -0.257 |
| 前景平均 RGB | [171.2, 152.0, 146.0] | [117.4, 84.2, 68.4] | RGB 距离 0.260 |
| 亮度直方图相似度 | — | — | 0.88（16 桶交集，1=完全一致） |
| 前景占比 | 87.1% | 9.2% | 覆盖率比 0.106 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：飞行头盔完整：皮革帽体、护目镜、呼吸管、木底座与铭牌都在，贴图方向和细节正确、无缺件。整体明显偏暗偏橄榄色调，护目镜呈不透明黄绿色（transmission 未实现），与官方明亮工作室光照差距较大，属缺失特性/IBL 限制。
- 差异主因：缺失特性：transmission（镜片）未实现 + 无 HDRI IBL（整体偏暗）
