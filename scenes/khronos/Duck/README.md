# Duck —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/Duck](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Duck)  
**上游概览**：The COLLADA duck. One texture.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- SCEA Shared Source License, Version 1.0（SCEA）../../LICENSES/SCEA.txt，Sony 2006

> 本目录由 `python tools/migrate_gltf_models.py --only Duck` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `Duck.gltf` |
| 节点 / 网格 / primitive | 3 / 1 / 1 |
| 顶点 / 三角形 | 2,399 / 4,212 |
| 材质 / 纹理 / 图像 | 1 / 1 / 1 |
| 图像编码 | png×1 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| alpha 模式分布 | OPAQUE×1 |

**导入产物**：`assets/` 1 个 `.mesh.json`（合计 274.5 KB），`textures/` 1 张；场景 `Duck.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only Duck --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/Duck/glTF/Duck.gltf --out scenes/khronos/Duck --name Duck --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/Duck/Duck.scn
./build-win/samples/player/maque_player --shot /tmp/Duck.png scenes/khronos/Duck/Duck.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（128×127）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.472 | 0.679 | 差 +0.208 |
| 前景平均 RGB | [149.3, 123.6, 2.4] | [203.3, 179.8, 19.7] | RGB 距离 0.167 |
| 亮度直方图相似度 | — | — | 0.63（16 桶交集，1=完全一致） |
| 前景占比 | 56.4% | 4.2% | 覆盖率比 0.074 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）；预览主体占比过小（<5%）→ 机位过远？**

⚠️ 构图告警：预览主体占比过小（<5%）→ 机位过远？

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 构图告警：预览主体占比过小（<5%）→ 机位过远？

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：鸭子主体完整（身体/头/嘴可见），黄色本体与橙色嘴的贴图颜色正确，无贴图缺失或破面。当前视角偏后侧，眼睛/正面细节不如官方明显，但可清楚辨认且材质可信。
- 差异主因：机位/背景：自动机位在模型后侧 3/4，官方为正面近景
