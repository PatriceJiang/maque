# 004 · RenderTexture 多尺寸导致 depth/MSAA 每帧重建

- **类型**：性能（GPU 资源 churn；**尚未量化**，不是泄漏）
- **状态**：open
- **影响面**：同一场景里 RenderTexture 目标尺寸 ≠ 主渲染目标尺寸的 3D 场景；
  `scenes/render_texture/render_texture_3d.scn`（2×128² RT + 320×240 主目标）是典型例子
- **发现版本**：`d517096` 之后，调查 001 的 RenderTexture 生命周期时发现

## 症状

`gfx` 的 depth/MSAA 附件是 **Device 级单槽缓存**：

- `ensure_depth(dv, w, h)`：只保留一组 `depthTex/depthView`，宽高不匹配就 release 再 create；
- `ensure_msaa(dv, w, h, fmt, samples)`：同样只有一组 `msaaTex/msaaDepthTex`，键包含 w/h/fmt/samples。

每个 3D pass 进入 `render_3d_only` 时都用 `f->width/height` 调 `ensure_depth`。RenderTexture 场景的时序是：

```text
RT duck 128x128   → ensure_depth(128,128)  [重建]
RT fox  128x128   → 命中 128x128           [复用]
主画面 320x240    → ensure_depth(320,240)  [重建]
下一帧重复 …
```

即**每帧至少两次 depth 纹理 destroy + create**；开启 MSAA 时还叠加 color/depth 两张 4x 附件。
不是内存泄漏（`resource_bytes` / 实际操作系统的 VRAM 采样都是平台期），但属于可避免的
GPU 分配器压力与潜在驱动 stall；多 RenderTexture 或窗口 resize 时会更频繁。

## 最小复现 / 观察方法

目前没有现成的计数指标；建议用以下任一方式确认：

1. 在 `ensure_depth` / `ensure_msaa` 的重建分支加 env 门控计数（`MAQUE_TRACE_GPU_ALLOC=1`），
   跑 `render_texture_3d.scn` 60 帧，预期计数约为 2×帧数；
2. RenderDoc / GPU 调试层看每帧 `CreateTexture`；
3. 对比“RT 尺寸 = 主目标尺寸”的同类场景（如把 RT 改成 320×240）与当前场景的 `render` 耗时。

## 根因

缓存键缺少尺寸维度：一个 `Device` 只有一份 depth/MSAA 槽位，却被**多个不同尺寸的渲染目标**共享。
`ensure_depth` / `ensure_msaa` 的命中条件只看“当前这一组是否等于传入尺寸”，不保存历史尺寸，
所以 128↔320 交替时每帧必然互相驱逐。

## 修复建议（未实现）

- 把 depth/MSAA 附件改成按 `(w, h, format, samples)` 索引的小型缓存（2–4 组，LRU 或固定上限），
  RenderTexture 与主目标各占一组；
- 或在持久 `RenderTexture` 的 `Texture` 里保存自己的 depth（更贴近“每个目标独立附件”的语义），
  主目标保留 Device 级一份；
- 所有新增附件必须在 `destroy_device` 里释放，并保持 `resource_bytes2` 口径不变（帧临时附件不计入 VRAM）；
- 验收：同一场景连续 60 帧的附件 create 次数 ≤ 目标种类数（而不是 ≥ 2×帧数），
  且 `render_texture_e2e` 像素断言与 `vram_bytes` 不回归。

## 回归与证据

- 当前 **open**：没有失败测试；`render_texture_e2e` 只保证像素/RenderTexture 语义正确，不检查分配次数。
- 已确认不是泄漏：`render_texture_3d.scn` 长跑 private bytes / GPU 显存 / handle 数均平台期，
  `resource_bytes` 稳定（6.85 MiB 纹理 + 403.8 KiB 网格）。
- 相关代码：[gfx.cpp:391](../../modules/render/gfx/src/gfx.cpp#L391)（`ensure_depth`）、
  [412](../../modules/render/gfx/src/gfx.cpp#L412)（`ensure_msaa`）、
  [2658](../../modules/render/gfx/src/gfx.cpp#L2658)，调用点。

## 相关

- [33 · RenderTexture](../33-RenderTexture相机离屏纹理.md)（§10 生命周期、§12 性能建议）
- 001（同一场景的生命周期 bug，已修）
