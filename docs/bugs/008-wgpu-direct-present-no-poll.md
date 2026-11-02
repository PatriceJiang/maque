# 008 · wgpu 直呈路径不排空事件队列 → 延迟销毁资源累积

- **类型**：内存（native/GPU 资源延迟释放）
- **状态**：fixed
- **影响面**：`MAQUE_PRESENT=wgpu`（含 `auto` 解析到直呈）的窗口运行；`Fox`、`render_texture_3d`
  等持续逐帧创建临时 GPU 资源的 3D 场景。SDL 呈现路径因每帧 readback（map/poll）不受影响。
- **发现版本**：`d517096` 之后（直呈路径引入）；用进程 WorkingSet/RSS 长跑复现

## 症状

窗口 + `MAQUE_PRESENT=wgpu` 跑 3D 场景，进程内存持续增长，典型 0.4–0.6 MB/s（1280×720、
本机 RTX 2060 SUPER）；用户环境实测 2–4 MB/s，与帧率/窗口尺寸成正比。`[mem]` 的六个域
（script/node/texture/gpu/font/heap）全部平台期，GPU 显存（`nvidia-smi memory.used`）也基本平台期 ——
泄漏在 wgpu-native 的**延迟销毁队列**里，不在引擎账本上。

同一场景用 `MAQUE_PRESENT=sdl` 时平台期：SDL 路径每帧 CPU readback 会 map/poll，
顺带把退役资源排干，掩盖了问题。

## 最小复现

```bash
MAQUE_WINDOW=1280x720 MAQUE_PRESENT=wgpu MAQUE_PACE=0 \
  ./build/samples/player/maque_player --frames 60000 \
  maque-test-tmp/foxgen/Fox.scn      # 或 scenes/render_texture/render_texture_3d.scn
```

每秒采样 `Get-Process maque_player | % WorkingSet64`：

```text
# 修复前（Fox，1280×720）
184.4 MB → 185.2 → 186.0 → 187.2 → 187.5 → 188.3 → 190.4 MB   （10 s，持续上涨）
```

## 根因

`r_present()` 在 `wgpuSurfacePresent` 后 `wgpuTextureRelease(surfaceTex)`；每帧的材质 uniform buffer、
bind group、蒙皮 palette buffer 也在录制后 `wgpu*Release`。但 **wgpu-native 的 Release 只是减引用**，
真正销毁要等 device/instance 的事件队列被处理。

直呈路径没有 readback（`wgpuBufferMap`/map 回调）这趟同步，也从未调用
`wgpuInstanceProcessEvents`；于是每帧退役的纹理/buffer/bind group 一直挂在延迟销毁队列，
RSS 随帧数线性增长。该构建的 `wgpuInstanceWaitAny` 未实现（AGENTS），只能用
`wgpuInstanceProcessEvents` 非阻塞轮询。

## 修复

- `r_present()` 在 `wgpuSurfacePresent` 成功、surface texture 释放后调用一次
  `wgpuInstanceProcessEvents(dv->instance)`（非阻塞），排空本帧的延迟销毁队列；
- 仅直呈路径需要；离屏/headless 没有 surface，资源在 device 销毁时统一回收；
- 与 [004](004-render-target-attachment-churn.md) 配套：004 消除 depth/MSAA 的每帧重建（源头），
  本条负责把仍然逐帧创建的 uniform/bind group/palette 等短命资源及时回收（排水）。

## 回归与证据

- Fox（mesh/skin/clip 生成资产，1280×720，直呈）：修复前 WorkingSet 184.4 → 190.4 MB / 10 s；
  修复后 187,551,744 B **连续 12 s 完全不变**；GPU `memory.used` 1071 MiB 平台期。
- `render_texture_3d.scn`（1280×720，直呈）：修复后 WorkingSet 189–190.5 MB 抖动 < 1.5 MB（20 s），
  不再单调上涨。
- `[mem]` 六域与 `resource_bytes` 保持平台期（本泄漏不在引擎账本内，不能只看 `mem.*`）。
- 相关代码：[gfx.cpp](../../modules/render/gfx/src/gfx.cpp) 的 `r_present`。

## 相关

- [004](004-render-target-attachment-churn.md)（depth/MSAA 每帧重建；同一场景的性能问题）
- [33 · RenderTexture](../33-RenderTexture相机离屏纹理.md)
