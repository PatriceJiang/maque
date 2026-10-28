# UnlitTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/UnlitTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/UnlitTest)  
**上游概览**：Tests if the KHR_materials_unlit extension is supported properly.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2019

> 本目录由 `python tools/migrate_gltf_models.py --only UnlitTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `UnlitTest.gltf` |
| 节点 / 网格 / primitive | 2 / 2 / 2 |
| 顶点 / 三角形 | 192 / 88 |
| 材质 / 纹理 / 图像 | 2 / 0 / 0 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_materials_unlit` |
| 必需扩展 | `KHR_materials_unlit` |
| alpha 模式分布 | OPAQUE×2 |

**导入产物**：`assets/` 2 个 `.mesh.json`（合计 14.4 KB），`textures/` 0 张；场景 `UnlitTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only UnlitTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/UnlitTest/glTF/UnlitTest.gltf --out scenes/khronos/UnlitTest --name UnlitTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/UnlitTest/UnlitTest.scn
./build-win/samples/player/maque_player --shot /tmp/UnlitTest.png scenes/khronos/UnlitTest/UnlitTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.jpg`（737×435）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.498 | 0.519 | 差 +0.021 |
| 前景平均 RGB | [127.6, 126.9, 126.3] | [160.3, 127.9, 94.7] | RGB 距离 0.086 |
| 亮度直方图相似度 | — | — | 0.98（16 桶交集，1=完全一致） |
| 前景占比 | 40.0% | 28.4% | 覆盖率比 0.709 |

自动初判：**主体亮度/色调分布较接近（机位/背景可能不同）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：两个 unlit 八边形完整、大小/位置关系正确，KHR_materials_unlit 已生效（不受光照影响、无明暗面）。但颜色与官方不符：橙色偏黄、蓝色偏青（官方为纯橙/纯蓝）。根因是 pbr shader 对 Basic（unlit）也做 ACES tonemap + exposure（assets/builtin/shaders/pbr/10_utils.wgsl:99-101），属色彩管线可修问题。
- 差异主因：引擎限制：unlit 材质也走 ACES 色调映射 → 颜色偏移
