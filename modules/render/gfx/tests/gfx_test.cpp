// gfx 冒烟测试：经模块宿主 dlopen 渲染插件，走生成的 C ABI vtable（docs/13）。
// 覆盖：相机（中心/缩放/旋转）、分层 z 序、批处理正确性。
#include "maque_ifaces.h"
#include "core/module/mq_module.h"

#include <print>
#include <vector>
#include <cmath>
#include <algorithm>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

struct Canvas {
    int W = 320, H = 240;
    std::vector<uint8_t> px = std::vector<uint8_t>(static_cast<size_t>(W) * static_cast<size_t>(H) * 4u);
    int at(int x, int y, int c) const { return (int)px[((size_t)y * W + x) * 4 + c]; }
    void clear() { std::fill(px.begin(), px.end(), (uint8_t)0); }
};

mq_renderer_item sprite(float x, float y, float size, float r, float g, float b,
                        int layer = 0, int z = 0, float rot = 0.0F) {
    mq_renderer_item it{};
    it.x = x; it.y = y; it.size = size;
    it.r = r; it.g = g; it.b = b; it.a = 1.0F;
    it.texture = -1; it.rotation = rot; it.layer = layer; it.z = z;
    return it;
}

} // namespace

int main() {
    mq_host* host = mq_host_create();
    mq_loaded_module* mod = nullptr;
    if (mq_host_load(host, MQ_GFX_PATH, &mod) != 0) { std::print("gfx_test: SKIP (no plugin)\n"); return 0; }
    auto* api = (const mq_renderer_v1*)mq_host_query(host, MQ_RENDERER_API_NAME, 1);
    CHECK(api != nullptr);
    if (!api) return 1;

    const char* backend = nullptr;
    void* self = api->create(&backend);
    CHECK(self != nullptr);
    std::print("gfx_test: plugin backend='{}'\n", backend ? backend : "?");

    float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };
    Canvas cv;

    // ---- 1) 默认相机（中心 = 视口中心，1:1）：世界坐标 = 像素坐标 ----
    {
        mq_renderer_item items[2] = {
            sprite(160.0F, 120.0F, 40.0F, 1.0F, 0.4F, 0.2F),     // 红球在中心
            sprite(60.0F, 60.0F, 20.0F, 0.2F, 1.0F, 0.4F),       // 绿球在左上
        };
        mq_renderer_view v{};
        v.vx = 0; v.vy = 0; v.vw = 320; v.vh = 240;
        v.cx = 160; v.cy = 120; v.zoom = 1.0F;                   // 相机中心 = 视口中心
        cv.clear();
        int rc = api->render_views(self, 320, 240, &v, 1, items, 2, bg, cv.px.data(), (uint32_t)cv.px.size());
        CHECK(rc == 0);
        CHECK(cv.at(160, 120, 0) > 200 && cv.at(160, 120, 1) < 150);
        CHECK(cv.at(60, 60, 1) > 200);
        CHECK(cv.at(2, 2, 0) < 40);
        CHECK(cv.at(160, 120, 2) > 40 && cv.at(160, 120, 2) < 70);   // b=0.2 → ~51（预乘 alpha 合成）
    }

    // ---- 2) 相机平移：中心右移 40 → 图元在屏幕上左移 40 ----
    {
        mq_renderer_item items[1] = { sprite(160.0F, 120.0F, 30.0F, 1.0F, 1.0F, 1.0F) };
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 200; v.cy = 120; v.zoom = 1.0F;
        cv.clear();
        api->render_views(self, 320, 240, &v, 1, items, 1, bg, cv.px.data(), (uint32_t)cv.px.size());
        CHECK(cv.at(120, 120, 0) > 200);                          // 160 - 40
        CHECK(cv.at(160, 120, 0) < 40);                           // 原位置已空
    }

    // ---- 3) 缩放 2×：半径 30 的图元覆盖 ±60 像素 ----
    {
        mq_renderer_item items[1] = { sprite(160.0F, 120.0F, 30.0F, 1.0F, 1.0F, 1.0F) };
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 2.0F;
        cv.clear();
        api->render_views(self, 320, 240, &v, 1, items, 1, bg, cv.px.data(), (uint32_t)cv.px.size());
        CHECK(cv.at(160, 120, 0) > 200);
        CHECK(cv.at(160 + 50, 120, 0) > 200);                     // 2× 后仍在圆内
        CHECK(cv.at(160 + 70, 120, 0) < 40);                      // 2× 后（>60px）已在圆外
    }

    // ---- 4) 分层：layer 大者后绘制（覆盖小者）----
    {
        mq_renderer_item items[2] = {
            sprite(160.0F, 120.0F, 30.0F, 1.0F, 0.0F, 0.0F, /*layer*/5),   // 红在下
            sprite(160.0F, 120.0F, 30.0F, 0.0F, 0.0F, 1.0F, /*layer*/1),   // 蓝在上（layer 小 → 先画）
        };
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 1.0F;
        cv.clear();
        api->render_views(self, 320, 240, &v, 1, items, 2, bg, cv.px.data(), (uint32_t)cv.px.size());
        // 先 layer=1（蓝）后 layer=5（红）→ 中心为红
        CHECK(cv.at(160, 120, 0) > 200 && cv.at(160, 120, 2) < 150);
    }

    // ---- 4b) alpha 合成：半透明图元按预乘 alpha 与背景混合（不再直接替换）----
    {
        mq_renderer_item items[1] = { sprite(160.0F, 120.0F, 30.0F, 1.0F, 1.0F, 1.0F) };
        items[0].a = 0.5F;
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 1.0F;
        cv.clear();
        api->render_views(self, 320, 240, &v, 1, items, 1, bg, cv.px.data(), (uint32_t)cv.px.size());
        const int r = cv.at(160, 120, 0);
        CHECK(r > 120 && r < 150);                                // 0.5*255 + 0.5*15 ≈ 135
    }

    // ---- 4c) 相机旋转 90°：偏离中心的图元绕相机中心旋转到新位置 ----
    {
        mq_renderer_item items[1] = { sprite(160.0F + 60.0F, 120.0F, 12.0F, 1.0F, 1.0F, 1.0F) };
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 1.0F;
        v.rot = 1.5707963F;                                   // 90°
        cv.clear();
        api->render_views(self, 320, 240, &v, 1, items, 1, bg, cv.px.data(), (uint32_t)cv.px.size());
        // 逆旋转 90°：世界 (+60, 0) 的相对位置 → 屏幕 (0, -60) 方向 → (160, 60)
        CHECK(cv.at(160, 60, 0) > 200);
        CHECK(cv.at(220, 120, 0) < 40);
    }

    // ---- 5) 旋转：把方形贴图旋转 90° 仍是同一图元（位置不变），仅检查不崩溃且中心着色 ----
    {
        mq_renderer_item items[1] = { sprite(160.0F, 120.0F, 25.0F, 0.3F, 0.9F, 0.9F, 0, 0, 1.5707963F) };
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 1.0F;
        cv.clear();
        api->render_views(self, 320, 240, &v, 1, items, 1, bg, cv.px.data(), (uint32_t)cv.px.size());
        CHECK(cv.at(160, 120, 1) > 200);
    }

    api->destroy(self);
    mq_host_destroy(host);
    if (g_failures == 0) { std::print("gfx_test: all checks passed\n"); return 0; }
    std::print("gfx_test: {} failure(s)\n", g_failures);
    return 1;
}
