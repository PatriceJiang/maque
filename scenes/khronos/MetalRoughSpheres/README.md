# MetalRoughSpheres —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/MetalRoughSpheres](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/MetalRoughSpheres)  
**上游概览**：Tests various metal and roughness values (texture mapped).
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2017

> 本目录由 `python tools/migrate_gltf_models.py --only MetalRoughSpheres` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `MetalRoughSpheres.gltf` |
| 节点 / 网格 / primitive | 6 / 5 / 5 |
| 顶点 / 三角形 | 255,914 / 501,776 |
| 材质 / 纹理 / 图像 | 1 / 2 / 2 |
| 图像编码 | png×2 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 5 个 `.mesh.json`（合计 30.6 MB），`textures/` 2 张；场景 `MetalRoughSpheres.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only MetalRoughSpheres --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/MetalRoughSpheres/glTF/MetalRoughSpheres.gltf --out scenes/khronos/MetalRoughSpheres --name MetalRoughSpheres --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/MetalRoughSpheres/MetalRoughSpheres.scn
./build-win/samples/player/maque_player --shot /tmp/MetalRoughSpheres.png scenes/khronos/MetalRoughSpheres/MetalRoughSpheres.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（338×329）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.475 | 0.674 | 差 +0.199 |
| 前景平均 RGB | [110.6, 122.9, 133.6] | [176.5, 173.2, 146.0] | RGB 距离 0.168 |
| 亮度直方图相似度 | — | — | 0.72（16 桶交集，1=完全一致） |
| 前景占比 | 46.5% | 13.2% | 覆盖率比 0.283 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：金属/非金属两排、光滑/粗糙两列的球阵与标签全部正确呈现，金属球呈黄铜色、非金属球偏白，粗糙度趋势可辨（粗糙球高光变宽），说明 MR 贴图生效。金属球缺少官方那种暗镜面环境反射（无 HDRI），整体偏亮偏灰，属引擎 IBL 限制。
- 差异主因：缺失特性：无 HDRI IBL → 金属反射弱、环境缺失
