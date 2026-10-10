/* maque gfx —— WebGPU 渲染器最小切片 (docs/13)。
 * 无窗口/离屏渲染：把 sprite 列表画进 RGBA8 目标，再 readback（供图像输出/AI 预览）。
 * 后端：wgpu-native (webgpu.h)。异步统一用 wgpuInstanceProcessEvents 轮询。 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace maque::gfx {

struct Texture;          // 不透明纹理句柄

struct SpriteDraw {
    float x, y;          // 屏幕坐标（像素，y 向下）
    float radius;        // 像素
    float r, g, b, a;    // 0..1
    const Texture* tex = nullptr;   // null = 实心圆；否则为纹理四边形
};

struct Device;           // 不透明句柄

/* 创建 WebGPU 设备（instance→adapter→device），out_backend 可选返回后端/设备名。 */
Device* create_device(std::string* out_backend = nullptr);
void    destroy_device(Device* dev);

/* 创建 RGBA8 纹理（用于 sprite 采样）。 */
Texture* create_texture(Device* dev, int w, int h, const uint8_t* rgba8);
void     destroy_texture(Texture* tex);

/* 视图（多屏/VR）：目标视口 + 平移(pan) + 眼距(eye) 偏移。 */
struct View {
    float vx, vy, vw, vh;   // 目标内视口（像素）
    float pan;              // 内容平移（把该视图内容移到自己的视口）
    float eye;              // 眼距/视差偏移（VR 立体）
};

/* 离屏渲染 N 个 sprite，readback 到 out_rgba（w*h*4，RGBA8）。 */
void render_offscreen(Device* dev, int w, int h,
                      const SpriteDraw* sprites, size_t count,
                      const float bg[4],
                      std::vector<uint8_t>& out_rgba);

/* 多视图渲染（VR/多屏）：同一场景到多视口，一次读回（target_w*target_h）。 */
void render_views(Device* dev, int target_w, int target_h,
                  const View* views, size_t nviews,
                  const SpriteDraw* sprites, size_t count,
                  const float bg[4],
                  std::vector<uint8_t>& out_rgba);

} // namespace maque::gfx
