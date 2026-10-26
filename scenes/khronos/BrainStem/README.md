# BrainStem —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/BrainStem](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/BrainStem)  
**上游概览**：Animations. Skins.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Poser EULA（LicenseRef-Poser-EULA）../../LICENSES/LicenseRef-Poser-EULA.txt，Keith Hunter 2017

> 本目录由 `python tools/migrate_gltf_models.py --only BrainStem` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `BrainStem.gltf` |
| 节点 / 网格 / primitive | 22 / 1 / 59 |
| 顶点 / 三角形 | 34,159 / 61,666 |
| 材质 / 纹理 / 图像 | 59 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 1 / 0 / 0 |
| alpha 模式分布 | OPAQUE×59 |

**导入产物**：`assets/` 59 个 `.mesh.json`（合计 3.4 MB），`textures/` 0 张；场景 `BrainStem.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only BrainStem --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/BrainStem/glTF/BrainStem.gltf --out scenes/khronos/BrainStem --name BrainStem --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/BrainStem/BrainStem.scn
./build-win/samples/player/maque_player --shot /tmp/BrainStem.png scenes/khronos/BrainStem/BrainStem.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.gif`（130×130）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.323 | 0.597 | 差 +0.274 |
| 前景平均 RGB | [86.6, 82.1, 73.5] | [144.8, 155.5, 143.6] | RGB 距离 0.263 |
| 亮度直方图相似度 | — | — | 0.30（16 桶交集，1=完全一致） |
| 前景占比 | 21.9% | 6.8% | 覆盖率比 0.313 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 骨骼蒙皮未应用（蒙皮网格按节点世界矩阵烘焙，姿势/形变与官方参考不同）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 骨骼蒙皮未应用（蒙皮网格按节点世界矩阵烘焙，姿势/形变与官方参考不同）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：机器人模型完整（躯干/四肢/头部/武器都在），贴图存在，但呈双臂平举的绑定 pose，官方为动画姿态（手臂下垂持物）；且缺少 HDRI，整体比官方亮、脏旧质感弱。属动画/骨骼未实现的预期差异。
- 差异主因：缺失特性：动画+骨骼未应用 → T/A-pose；整体偏亮偏平
