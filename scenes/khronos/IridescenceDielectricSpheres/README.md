# IridescenceDielectricSpheres —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/IridescenceDielectricSpheres](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/IridescenceDielectricSpheres)  
**上游概览**：Tests KHR_materials_iridescence on a non-metallic material.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Khronos 2019

> 本目录由 `python tools/migrate_gltf_models.py --only IridescenceDielectricSpheres` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `IridescenceDielectricSpheres.gltf` |
| 节点 / 网格 / primitive | 346 / 346 / 346 |
| 顶点 / 三角形 | 329,635 / 617,406 |
| 材质 / 纹理 / 图像 | 344 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_ior`、`KHR_materials_iridescence` |
| alpha 模式分布 | BLEND×1、OPAQUE×343 |

**导入产物**：`assets/` 346 个 `.mesh.json`（合计 32.1 MB），`textures/` 1 张；场景 `IridescenceDielectricSpheres.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only IridescenceDielectricSpheres --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/IridescenceDielectricSpheres/glTF/IridescenceDielectricSpheres.gltf --out scenes/khronos/IridescenceDielectricSpheres --name IridescenceDielectricSpheres --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/IridescenceDielectricSpheres/IridescenceDielectricSpheres.scn
./build-win/samples/player/maque_player --shot /tmp/IridescenceDielectricSpheres.png scenes/khronos/IridescenceDielectricSpheres/IridescenceDielectricSpheres.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（2000×2000）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.615 | 0.822 | 差 +0.208 |
| 前景平均 RGB | [156.9, 156.5, 156.6] | [208.5, 209.6, 213.0] | RGB 距离 0.211 |
| 亮度直方图相似度 | — | — | 0.21（16 桶交集，1=完全一致） |
| 前景占比 | 25.2% | 16.2% | 覆盖率比 0.643 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：6×6 球阵与 Thin Film Thickness / Material IOR 坐标轴标签都在、布局与官方一致（说明场景导入正确），但球面几乎呈白色，官方能看到明显的彩色薄膜干涉光泽；虹彩效果在本引擎中过弱。
- 差异主因：缺失特性：iridescence 近似太弱；机位/标签比例
