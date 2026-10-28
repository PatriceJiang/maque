# TextureSettingsTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TextureSettingsTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TextureSettingsTest)  
**上游概览**：Tests single/double-sided and various texturing modes.
**标签**：`core`、`testing`  
**许可**：（资产元数据，保留出处）
- Creative Commons Attribution 4.0 International（CC-BY-4.0）https://creativecommons.org/licenses/by/4.0/legalcode，Ed Mackey 2017

> 本目录由 `python tools/migrate_gltf_models.py --only TextureSettingsTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TextureSettingsTest.gltf` |
| 节点 / 网格 / primitive | 11 / 10 / 10 |
| 顶点 / 三角形 | 144 / 72 |
| 材质 / 纹理 / 图像 | 10 / 9 / 3 |
| 图像编码 | png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| alpha 模式分布 | OPAQUE×10 |

**导入产物**：`assets/` 10 个 `.mesh.json`（合计 16.1 KB），`textures/` 3 张；场景 `TextureSettingsTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TextureSettingsTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TextureSettingsTest/glTF/TextureSettingsTest.gltf --out scenes/khronos/TextureSettingsTest --name TextureSettingsTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TextureSettingsTest/TextureSettingsTest.scn
./build-win/samples/player/maque_player --shot /tmp/TextureSettingsTest.png scenes/khronos/TextureSettingsTest/TextureSettingsTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot.png`（332×325）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.560 | 0.587 | 差 +0.028 |
| 前景平均 RGB | [114.3, 145.3, 200.2] | [122.9, 152.9, 196.3] | RGB 距离 0.026 |
| 亮度直方图相似度 | — | — | 0.26（16 桶交集，1=完全一致） |
| 前景占比 | 99.4% | 29.8% | 覆盖率比 0.3 |

自动初判：**差异较大（需人工确认：机位/缺失特性/导入问题）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- 无（core glTF + 已支持材质路径）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：测试面板几何/标签/贴图完整，但 Test 列结果与官方不符：官方 8 行全绿（✓ 或绿色方块），maque 的 texture clamp s/t、texture repeat t、texture mirror s/t 行显示红色 X。检查链路：importer 不导出 sampler，runtime 创建贴图写死 wrap_u/wrap_v=0（repeat，runtime.cpp:800/834/895），故 CLAMP_TO_EDGE/MIRRORED_REPEAT 类采样不生效，是明确的引擎侧可修问题。
- 差异主因：引擎限制：glTF sampler（wrap）未导入，runtime 贴图硬编码 REPEAT → clamp/mirror 测试行失败
