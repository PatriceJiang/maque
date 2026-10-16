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

    // ---- 6) 网格图元（render_mesh_views）：逐顶点 UV 的四边形 + 索引绘制 + 页纹理 ----
    //        这是骨骼动画（spine）走的路径：任意四边形网格，不能退化成"实例化矩形"。
    {
        // 2x2 纹理：左上红、右上绿、左下蓝、右下黄（行列 = 屏幕顺序，y 向下）
        const uint8_t texels[16] = { 255,0,0,255,   0,255,0,255,
                                     0,0,255,255,   255,255,0,255 };
        const int tex = api->create_texture(self, texels, 2, 2);
        CHECK(tex >= 0);
        // 顶点（渲染目标像素空间，y 向下）：覆盖 (60,60)-(260,180)
        const float verts[8] = { 60.0F,60.0F,  260.0F,60.0F,  60.0F,180.0F,  260.0F,180.0F };
        const float uvs[8]   = { 0.0F,0.0F,    1.0F,0.0F,     0.0F,1.0F,     1.0F,1.0F };
        const float cols[16] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
        const uint32_t idx[6] = { 0,1,2, 2,1,3 };
        mq_renderer_mesh m{};
        m.vertices = verts; m.uvs = uvs; m.colors = cols; m.indices = idx;
        m.vertex_count = 4; m.index_count = 6; m.first_index = 0; m.base_vertex = 0;
        m.texture = tex; m.blend = 0; m.pma = 0; m.layer = 0; m.z = 0;
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 1.0F;
        cv.clear();
        const int rc = api->render_mesh_views(self, 320, 240, &v, 1, nullptr, 0, &m, 1,
                                              bg, cv.px.data(), (uint32_t)cv.px.size());
        CHECK(rc == 0);
        CHECK(cv.at(70, 70, 0) > 180 && cv.at(70, 70, 1) < 80);      // 左上 = 红
        CHECK(cv.at(250, 70, 1) > 180 && cv.at(250, 70, 0) < 80);    // 右上 = 绿
        CHECK(cv.at(70, 170, 2) > 180 && cv.at(70, 170, 0) < 80);    // 左下 = 蓝
        CHECK(cv.at(20, 20, 0) < 40 && cv.at(20, 20, 2) < 60);       // 四边形外 = 背景
        // 图元 + 网格同帧：网格 layer/z 参与统一排序（z 大的后画）
        {
            mq_renderer_item cover[1] = { sprite(160.0F, 120.0F, 40.0F, 1.0F, 1.0F, 1.0F, 0, /*z*/5) };
            cv.clear();
            api->render_mesh_views(self, 320, 240, &v, 1, cover, 1, &m, 1,
                                   bg, cv.px.data(), (uint32_t)cv.px.size());
            CHECK(cv.at(70, 70, 0) > 180);                            // 网格 z=0 先画 → 白块盖不住四角
            CHECK(cv.at(160, 120, 0) > 200 && cv.at(160, 120, 1) > 200);   // 中心被 z=5 的白块覆盖
        }
        api->destroy_texture(self, tex);
    }

    // ---- 6b) 网格 + **预乘 alpha 页纹理**（spine 图集的 pma: true 场景）+ 1024x256 大页 ----
    {
        const int W = 1024, H = 256;
        std::vector<uint8_t> texels((size_t)W * H * 4);
        for (size_t i = 0; i < texels.size(); i += 4) {          // 不透明洋红（预乘后仍是 255,0,255,255）
            texels[i] = 255; texels[i + 1] = 0; texels[i + 2] = 255; texels[i + 3] = 255;
        }
        const int tex = api->create_texture(self, texels.data(), W, H);
        CHECK(tex >= 0);
        const float verts[8] = { 60.0F,60.0F,  260.0F,60.0F,  60.0F,180.0F,  260.0F,180.0F };
        const float uvs[8]   = { 0.0F,0.0F,    1.0F,0.0F,     0.0F,1.0F,     1.0F,1.0F };
        const float cols[16] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
        const uint32_t idx[6] = { 0,1,2, 2,1,3 };
        mq_renderer_mesh m{};
        m.vertices = verts; m.uvs = uvs; m.colors = cols; m.indices = idx;
        m.vertex_count = 4; m.index_count = 6; m.texture = tex;
        m.blend = 0; m.pma = 1; m.layer = 0; m.z = 0;
        mq_renderer_view v{};
        v.vw = 320; v.vh = 240; v.cx = 160; v.cy = 120; v.zoom = 1.0F;
        cv.clear();
        const int rc = api->render_mesh_views(self, 320, 240, &v, 1, nullptr, 0, &m, 1,
                                              bg, cv.px.data(), (uint32_t)cv.px.size());
        CHECK(rc == 0);
        CHECK(cv.at(160, 120, 0) > 200 && cv.at(160, 120, 2) > 200 && cv.at(160, 120, 1) < 60);
        api->destroy_texture(self, tex);
    }

    api->destroy(self);
    mq_host_destroy(host);
    if (g_failures == 0) { std::print("gfx_test: all checks passed\n"); return 0; }
    std::print("gfx_test: {} failure(s)\n", g_failures);
    return 1;
}
