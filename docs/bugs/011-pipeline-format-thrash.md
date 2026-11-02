# 011 · RenderTexture/主目标格式交替导致 3D 管线每 pass 重建

- **类型**：性能（CPU / 管线创建）
- **状态**：fixed
- **影响面**：同一帧里既有 `RenderTexture`（RGBA8Unorm）又有窗口直呈（surface 格式，Windows 常见 BGRA8Unorm）
  的 3D 场景；`scenes/render_texture/render_texture_3d.scn` 是典型
- **发现版本**：`26e73ea` 之后；Tracy 显示 `render_textures` zone 占 ~62% 帧时间

## 症状

`render_texture_3d.scn`（Duck + Fox，两个 128² RT + 1280×720 主目标）：

```text
fps≈33，render≈27–30 ms/frame，CPU≈80–100% of one core
Tracy: render_textures 19.6 ms/frame, frame.3d 8.8 ms/frame
```

RT 只有 128×128、2 个 draw，却比 1280×720 主画面还贵。

## 根因

`ensure_3d_pipelines()` 只有**一个** `p3dFormat` 槽：

- RT pass 目标格式 = `RGBA8Unorm`；
- 主 pass 目标格式 = surface 格式（BGRA8/BGRA8Srgb）；
- 每个 RT pass / 主 pass 进入时发现格式不同 → `release_material_pipelines()` 释放
  `p3dMatPipeline[2][12][2][3][2][4]` 全部管线，再为下一种格式重建。

即：每帧在 RT 与主 pass 之间**全量销毁/重建**材质、wire、sky 管线，造成 27 ms 级 CPU 开销。

## 修复

- 管线缓存增加**格式维度**（2 槽）：
  `p3dMatPipeline[2(格式)][2(msaa)][12][2][3][2][4]`、`p3dWirePipeline[2][2][4]`、
  `skyPipeline[2][2]`；
- `ensure_3d_pipelines()` 按 `p3dFormats[2]` 查找/分配格式槽；RT 与主目标各占一槽，互不驱逐；
- `get_material_pipeline()` 用当前格式槽的 format 建管线；
- 第三种格式才整体重建（正常场景不会发生）；
- `release_material_pipelines()` / `release_sky_pipelines()` 在 device 销毁时释放所有格式槽。

## 回归与证据

- `render_texture_3d.scn`：render **29 → 13 ms/frame**，fps 33 → 60；CPU 80–100% → ~76%
  （Fox 自动播放动画，仍每帧渲染两个 RT + 主画面，属于动画负载）。
- 直接静态 `scenes/khronos-bin/Duck/Duck.scn`：release **0.6–1.4%** of one core。
- 通过：`render_texture_e2e`、`material_models/physical/shadow`、`gltf_anim/skin/morph`、`shot_e2e`。

## 相关

- [009](009-static-scene-redraw-cpu.md)（静态帧跳过；带 Fox 动画的 RT 场景不能跳帧）
- [004](004-render-target-attachment-churn.md)（depth/MSAA 附件缓存）
