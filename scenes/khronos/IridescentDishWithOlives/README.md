# IridescentDishWithOlives —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/IridescentDishWithOlives](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/IridescentDishWithOlives)  
**上游概览**：Dish using transmission, volume, IOR, and specular. [Issues: non-Khronos mark]
**标签**：`showcase`、`video`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Eric Chadwick 2020

> 本目录由 `python tools/migrate_gltf_models.py --only IridescentDishWithOlives` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `IridescentDishWithOlives.gltf` |
| 节点 / 网格 / primitive | 6 / 4 / 4 |
| 顶点 / 三角形 | 14,863 / 24,448 |
| 材质 / 纹理 / 图像 | 4 / 11 / 11 |
| 图像编码 | png×11 |
| 动画 / 骨骼 / morph primitive / 相机 | 1 / 0 / 0 / 1 |
| 使用扩展 | `KHR_materials_ior`、`KHR_materials_iridescence`、`KHR_materials_transmission`、`KHR_materials_volume` |
| alpha 模式分布 | MASK×1、OPAQUE×3 |

**导入产物**：`assets/` 4 个 `.mesh.json`（合计 1.9 MB），`textures/` 10 张；场景 `IridescentDishWithOlives.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only IridescentDishWithOlives --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/IridescentDishWithOlives/glTF/IridescentDishWithOlives.gltf --out scenes/khronos/IridescentDishWithOlives --name IridescentDishWithOlives --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/IridescentDishWithOlives/IridescentDishWithOlives.scn
./build-win/samples/player/maque_player --shot /tmp/IridescentDishWithOlives.png scenes/khronos/IridescentDishWithOlives/IridescentDishWithOlives.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1280×1167）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.410 | 0.485 | 差 +0.076 |
| 前景平均 RGB | [110.6, 104.7, 84.5] | [137.9, 122.9, 89.9] | RGB 距离 0.067 |
| 亮度直方图相似度 | — | — | 0.48（16 桶交集，1=完全一致） |
| 前景占比 | 55.9% | 7.4% | 覆盖率比 0.132 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 动画未导入（静态渲染；角色模型为 bind/T-pose）
- 未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）
- KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）
- KHR_materials_volume/dispersion/volume_scatter 未实现
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
