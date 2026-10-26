# IridescenceAbalone —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/IridescenceAbalone](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/IridescenceAbalone)  
**上游概览**：A model of an abalone shell with highly iridescent interior.
**标签**：`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Abby Crawford 2015
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2023

> 本目录由 `python tools/migrate_gltf_models.py --only IridescenceAbalone` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `IridescenceAbalone.gltf` |
| 节点 / 网格 / primitive | 1 / 1 / 1 |
| 顶点 / 三角形 | 3,159 / 6,036 |
| 材质 / 纹理 / 图像 | 1 / 4 / 4 |
| 图像编码 | jpeg×3、png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_iridescence` |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 367.7 KB），`textures/` 4 张；场景 `IridescenceAbalone.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only IridescenceAbalone --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/IridescenceAbalone/glTF/IridescenceAbalone.gltf --out scenes/khronos/IridescenceAbalone --name IridescenceAbalone --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/IridescenceAbalone/IridescenceAbalone.scn
./build-win/samples/player/maque_player --shot /tmp/IridescenceAbalone.png scenes/khronos/IridescenceAbalone/IridescenceAbalone.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1625×985）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.567 | 0.655 | 差 +0.088 |
| 前景平均 RGB | [141.5, 147.6, 123.7] | [178.5, 162.9, 174.7] | RGB 距离 0.135 |
| 亮度直方图相似度 | — | — | 0.51（16 桶交集，1=完全一致） |
| 前景占比 | 68.9% | 17.0% | 覆盖率比 0.246 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
