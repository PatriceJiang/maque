# CompareAnisotropy —— Khronos glTF 示例迁移

**来源**：[KhronosGroup/glTF-Sample-Assets · Models/CompareAnisotropy](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CompareAnisotropy)  
**上游概览**：This model compares without anisotropy vs. with anisotropy.
**标签**：`extension`、`testing`、`pbrtest`  
**许可**：（资产元数据，保留出处）
- Khronos Trademark or Logo（LicenseRef-LegalMark-Khronos）../../LICENSES/LicenseRef-LegalMark-Khronos.txt，Non-copyrightable logo 2017
- Creative Commons Zero v1.0 Universal（CC0-1.0）https://creativecommons.org/publicdomain/zero/1.0/legalcode，Eric Chadwick and DGG 2024

> 本目录由 `python tools/migrate_gltf_models.py --only CompareAnisotropy` 生成；`assets/`、`textures/` 为派生资产（体积大，不入库），需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。

## 模型统计

| 项 | 值 |
|---|---|
| 主 glTF 文件 | `CompareAnisotropy.gltf` |
| 节点 / 网格 / primitive | 6 / 4 / 4 |
| 顶点 / 三角形 | 19,142 / 36,864 |
| 材质 / 纹理 / 图像 | 4 / 5 / 5 |
| 图像编码 | jpeg×2、png×3 |
| 动画 / 骨骼 / morph primitive / 相机 | 0 / 0 / 0 / 0 |
| 使用扩展 | `KHR_texture_transform`、`KHR_materials_anisotropy` |
| alpha 模式分布 | OPAQUE×4 |

**导入产物**：`assets/` 4 个 `.mesh.json`（合计 2.1 MB），`textures/` 5 张；场景 `CompareAnisotropy.scn`。

## 生成 / 运行

```bash
# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）
python tools/migrate_gltf_models.py --only CompareAnisotropy --force
# 或单独导入：
python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/CompareAnisotropy/glTF/CompareAnisotropy.gltf --out scenes/khronos/CompareAnisotropy --name CompareAnisotropy --scene --compact
# 运行 / 截图
./build-win/samples/player/maque_player scenes/khronos/CompareAnisotropy/CompareAnisotropy.scn
./build-win/samples/player/maque_player --shot /tmp/CompareAnisotropy.png scenes/khronos/CompareAnisotropy/CompareAnisotropy.scn
```

## 与官方 screenshot 对比

参考图：`screenshot/screenshot_Large.jpg`（1559×814）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，常带 HDRI studio、软阴影、不同机位/背景；maque 对 README 声明环境的模型注入官方 HDRI（`tools/khronos_env.json`），其余用 analytic studio IBL，无面光/软阴影。

| 指标 | 参考图 | 本预览 | 说明 |
|---|---|---|---|
| 前景平均亮度 | 0.386 | 0.368 | 差 -0.018 |
| 前景平均 RGB | [120.6, 94.7, 71.0] | [128.2, 86.5, 65.1] | RGB 距离 0.029 |
| 亮度直方图相似度 | — | — | 0.53（16 桶交集，1=完全一致） |
| 前景占比 | 70.4% | 17.7% | 覆盖率比 0.252 |

自动初判：**中等差异（多为机位/背景/光照/缺失特性）**

**差异归因**（按本模型实际特征）：
- 官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）
- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_anisotropy：各向异性 GGX（Khronos 规范实现，strength/旋转/贴图方向）+ 环境反射 bent-normal 近似；非真实多次反射
- 引擎光照为方向光硬阴影（无面光/软阴影）；env 为官方 HDRI（有映射的模型）或解析 studio IBL，MSAA 需场景显式开启（高光与间接光观感差异）

## 已知差距（引擎/导入器层面）

- KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似
- KHR_materials_anisotropy：各向异性 GGX（Khronos 规范实现，strength/旋转/贴图方向）+ 环境反射 bent-normal 近似；非真实多次反射

## 踩过的坑 / 注意

- 本模型导入/渲染无告警。

<!-- 手动笔记开始（重生成时保留以下内容） -->

## 人工视觉复核（自动迁移初版）
- 结论：两个铜色球的径向同心环高光可见，glTF logo 与官方位置一致，能看出各向异性方向性；但官方那种强烈的拉丝径向高光与暗环对比在本引擎明显偏弱，属近似实现/IBL 差距。
- 差异主因：缺失特性：anisotropy 为粗糙度近似 + 无 HDRI（拉丝高光弱）
