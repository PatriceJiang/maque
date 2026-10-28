# TransmissionTest —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/TransmissionTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/TransmissionTest)  
**上游概览**：Tests if the KHR_materials_transmission extension is supported properly.
**标签**：`testing`、`extension`  
**许可**：（资产元数据，保留出处）
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Adobe 2020

> 本目录由 `python tools/migrate_gltf_models.py --only TransmissionTest` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `TransmissionTest.gltf` |
| 节点 / 网格 / primitive | 25 / 22 / 22 |
| 顶点 / 三角形 | 66,902 / 128,775 |
| 材质 / 纹理 / 图像 | 14 / 9 / 9 |
| 图像编码 | png×9 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 1 |
| 使用扩展 | `KHR_materials_transmission`、`KHR_xmp` |
| alpha 模式分布 | MASK×6、OPAQUE×8 |

**导入产物**：`assets/` 22 个 `.mesh.json`（合计 7.4 MB），`textures/` 9 张；场景 `TransmissionTest.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only TransmissionTest --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/TransmissionTest/glTF/TransmissionTest.gltf --out scenes/khronos/TransmissionTest --name TransmissionTest --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/TransmissionTest/TransmissionTest.scn
./build-win/samples/player/maque_player --shot /tmp/TransmissionTest.png scenes/khronos/TransmissionTest/TransmissionTest.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_large.png`（1105×718）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.553 | 0.714 | 差 +0.162 |
| 前景平均 RGB | [141.1, 141.7, 133.7] | [180.4, 183.4, 173.6] | RGB 距离 0.158 |
| 亮度直方图相似度 | — | — | 0.51（16 桶交集，1=完全一致） |
| 前景占比 | 87.9% | 65.3% | 覆盖率比 0.744 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_materials_transmission：屏幕空间折射近似（opaque pass 颜色拷贝 + 逐像素折射采样、transmissionTexture 掩码、Fresnel 反射）；无真实体积吸收/多次折射
- alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

（可在此手写本模型的额外注意事项）
