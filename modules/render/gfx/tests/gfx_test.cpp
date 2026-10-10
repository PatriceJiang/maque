// gfx 冒烟测试：经后端无关的 Renderer 接口离屏渲染 + readback 校验
#include "render/renderer/mq_renderer.h"

#include <print>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    using namespace maque::render;
    std::string backend;
    Renderer* r = create_renderer(&backend);
    if (!r) { std::print("gfx_test: SKIP (no WebGPU device)\n"); return 0; }
    std::print("gfx_test: backend='{}'\n", backend);

    DrawItem items[2] = {
        { 160.0F, 120.0F, 40.0F, 1.0F, 0.4F, 0.2F, 1.0F, -1 },   // 橙，居中
        {  60.0F,  60.0F, 20.0F, 0.2F, 1.0F, 0.4F, 1.0F, -1 },   // 绿，左上
    };
    const float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };
    std::vector<uint8_t> px;
    View v{ 0, 0, 320, 240, 0, 0 };
    r->render_views(320, 240, &v, 1, items, 2, bg, px);

    CHECK(px.size() == 320u * 240u * 4u);
    auto at = [&](int x, int y, int c) { return (int)px[((size_t)y * 320 + x) * 4 + c]; };
    CHECK(at(160, 120, 0) > 200 && at(160, 120, 1) < 150 && at(160, 120, 2) < 120);
    CHECK(at(60, 60, 1) > 200 && at(60, 60, 0) < 120);
    CHECK(at(2, 2, 0) < 40 && at(2, 2, 2) < 60);
    CHECK(at(160, 70, 0) < 40);

    delete r;
    if (g_failures == 0) { std::print("gfx_test: all checks passed\n"); return 0; }
    std::print("gfx_test: {} failure(s)\n", g_failures);
    return 1;
}
