# 004 · RenderTexture 多尺寸导致 depth/MSAA 每帧重建

- **类型**：性能（GPU 资源 churn）；直呈路径的进程内存增长是独立问题，见 [008](008-wgpu-direct-present-no-poll.md)
- **状态**：fixed
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
`resource_bytes` / `mem.gpu` 是平台期（临时附件不计入 VRAM 口径）。它是**分配器压力/churn**
（每帧 2 次 depth destroy+create），本身不会让引擎内存账本增长；但直呈路径若不同时排空
wgpu 事件队列，这些退役附件会堆在延迟销毁队列里（见 [008](008-wgpu-direct-present-no-poll.md)）。

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

`wgpuTextureRelease` 只是减引用；已提交命令仍在用的纹理要等 device poll 才真正销毁。
SDL 呈现路径每帧 readback（map/poll）会顺带排干；直呈路径的排水由 008 的每帧
`wgpuInstanceProcessEvents` 负责。本条只修“每帧重建”这个浪费源头。

## 修复

- `DepthAttachment` / `MsaaAttachment` 按 `(w,h[,fmt,samples])` 做 **4 槽 LRU 缓存**
  （[gfx.cpp](../../modules/render/gfx/src/gfx.cpp) 的 `depthCache` / `msaaCache`）：
  RenderTexture 128² 与主目标 320×240 各占一组，命中后不再 destroy/create；
- `destroy_device` 统一释放所有缓存项；`depthTex/depthView/...` 仅作“当前选中项”的别名，不拥有资源；
- `resource_bytes2` 口径不变（帧临时附件仍不计入 VRAM）；
- 诊断开关 `MAQUE_TRACE_GPU_ALLOC=1`：每次附件创建打印一行，退出时打印
  `gfx: gpu attachment creates depth=N msaa=M`。

## 回归与证据

- `ctest -R render_texture_e2e` 新增附件门：同一 3D 场景跑 60 帧，
  `gfx: gpu attachment create depth` 次数必须 **≤ 8**（实测修复后 **2**；修复前约 **120**）。
- 直呈路径的 RSS 平台期证据与修复见 [008](008-wgpu-direct-present-no-poll.md)
  （附件缓存本身不负责延迟销毁队列）。
- `resource_bytes` 稳定（6.85 MiB 纹理 + 403.8 KiB 网格），像素断言不回归。
- 相关代码：`ensure_depth` / `ensure_msaa`（gfx.cpp）、`destroy_device`。

## 相关

- [33 · RenderTexture](../33-RenderTexture相机离屏纹理.md)（§10 生命周期、§12 性能建议）
- 001（同一场景的生命周期 bug，已修）
