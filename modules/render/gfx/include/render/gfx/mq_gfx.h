/* maque gfx —— WebGPU 渲染器最小切片 (docs/13)。
 * 无窗口/离屏渲染：把 sprite 列表画进 RGBA8 目标，再 readback（供图像输出/AI 预览）。
 * 后端：wgpu-native (webgpu.h)。异步统一用 wgpuInstanceProcessEvents 轮询。 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace maque::gfx {

struct SpriteDraw {
    float x, y;          // 屏幕坐标（像素，y 向下）
    float radius;        // 像素
    float r, g, b, a;    // 0..1
};

struct Device;           // 不透明句柄

/* 创建 WebGPU 设备（instance→adapter→device），out_backend 可选返回后端/设备名。 */
Device* create_device(std::string* out_backend = nullptr);
void    destroy_device(Device* dev);

/* 离屏渲染 N 个 sprite，readback 到 out_rgba（w*h*4，RGBA8）。 */
void render_offscreen(Device* dev, int w, int h,
                      const SpriteDraw* sprites, size_t count,
                      const float bg[4],
                      std::vector<uint8_t>& out_rgba);

} // namespace maque::gfx
