// gfx 冒烟测试：真实 WebGPU 离屏渲染 + readback 校验
#include "render/gfx/mq_gfx.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    using namespace maque::gfx;
    std::string backend;
    Device* dev = create_device(&backend);
    if (!dev) { std::printf("gfx_test: SKIP (no WebGPU device)\n"); return 0; }
    std::printf("gfx_test: backend='%s'\n", backend.c_str());

    SpriteDraw sprites[2] = {
        { 160.0F, 120.0F, 40.0F, 1.0F, 0.4F, 0.2F, 1.0F },   // 橙，居中
        {  60.0F,  60.0F, 20.0F, 0.2F, 1.0F, 0.4F, 1.0F },   // 绿，左上
    };
    const float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };
    std::vector<uint8_t> px;
    render_offscreen(dev, 320, 240, sprites, 2, bg, px);

    CHECK(px.size() == 320u * 240u * 4u);
    auto at = [&](int x, int y, int c) { return (int)px[((size_t)y * 320 + x) * 4 + c]; };
    // 中心是橙色球
    CHECK(at(160, 120, 0) > 200 && at(160, 120, 1) < 150 && at(160, 120, 2) < 120);
    // 左上绿球
    CHECK(at(60, 60, 1) > 200 && at(60, 60, 0) < 120);
    // 角落是背景
    CHECK(at(2, 2, 0) < 40 && at(2, 2, 2) < 60);
    // 球外（中心正上方 120-40-5=75 处）是背景
    CHECK(at(160, 70, 0) < 40);

    destroy_device(dev);

    if (g_failures == 0) { std::printf("gfx_test: all checks passed\n"); return 0; }
    std::printf("gfx_test: %d failure(s)\n", g_failures);
    return 1;
}
