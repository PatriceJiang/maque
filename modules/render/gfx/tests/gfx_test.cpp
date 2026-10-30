// gfx 冒烟测试：经模块宿主 dlopen 渲染插件，走生成的 C ABI vtable（docs/13）。
// 覆盖：相机（中心/缩放/旋转）、分层 z 序、批处理正确性。
#include "maque_ifaces.h"
#include "core/module/mq_module.h"

#include <print>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstddef>

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
    auto* api = (const mq_renderer_v2*)mq_host_query(host, MQ_RENDERER_API_NAME, MQ_RENDERER_API_VERSION);
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

    // ---- 7) 3D/PBR 资源层（plans/18-pbr-3d.md M0）：caps / 纹理 v2 / 网格上传 ----
    //     这些方法只在表尾追加；老插件的 struct_size 更小 → 调用前用 struct_size 守卫。
    const bool has_3d_api = api->header.struct_size >=
        offsetof(mq_renderer_v2, present) + sizeof(api->present);
    CHECK(has_3d_api);
    if (has_3d_api) {
        mq_renderer_caps caps{};
        CHECK(api->caps(self, &caps) == 0);
        CHECK(caps.max_texture_size >= 2048);
        CHECK(caps.max_lights >= 1);
        CHECK(caps.target_format == 0);            // 未 attach_window → 离屏 RGBA8
        CHECK(caps.direct_present == 0);           // 无 surface
        std::print("gfx_test: caps tex={} lights={} fmt={} direct_present={}\n",
                   caps.max_texture_size, caps.max_lights, caps.target_format, caps.direct_present);

        // 纹理 v2：RGBA8 + 2 级 mip（2x2 → 1x1），带 sampler
        const uint8_t mip0[16] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255 };
        const uint8_t mip1[4]  = { 128,128,128,255 };
        uint8_t mips[20];
        std::memcpy(mips, mip0, sizeof mip0);
        std::memcpy(mips + sizeof mip0, mip1, sizeof mip1);
        mq_renderer_texture_desc td{};
        td.width = 2; td.height = 2; td.pixel_format = 0; td.kind = 0;
        td.mip_levels = 2; td.filter = 1; td.wrap_u = 1; td.wrap_v = 1;
        td.data = mips; td.data_size = sizeof mips;
        int32_t tex2 = -1;
        CHECK(api->create_texture2(self, &td, &tex2) == 0);
        CHECK(tex2 >= 0 && api->has_texture(self, tex2) == 1);
        api->destroy_texture(self, tex2);
        CHECK(api->has_texture(self, tex2) == 0);

        // RenderTexture：pass A 画红色目标，pass B 把同一句柄当普通 Texture 显示（首帧即生效）。
        const bool has_rt_api = api->header.struct_size >=
            offsetof(mq_renderer_v2, read_texture) + sizeof(api->read_texture);
        CHECK(has_rt_api);
        if (has_rt_api) {
            mq_renderer_render_texture_desc rd{};
            rd.struct_size = sizeof rd;
            rd.width = 64; rd.height = 64; rd.pixel_format = 0;
            rd.filter = 1; rd.wrap_u = rd.wrap_v = 1;
            rd.clear_color[0] = 0.0F; rd.clear_color[1] = 0.0F;
            rd.clear_color[2] = 0.0F; rd.clear_color[3] = 0.0F;
            int32_t rt = -1;
            const uint64_t bytes0 = api->resource_bytes ? api->resource_bytes(self) : 0;
            CHECK(api->create_render_texture(self, &rd, &rt) == 0);
            CHECK(rt >= 0 && api->has_texture(self, rt) == 1);
            mq_renderer_item red = sprite(32.0F, 32.0F, 30.0F, 1.0F, 0.0F, 0.0F);
            mq_renderer_view rv{};
            rv.vw = 64; rv.vh = 64; rv.cx = 32; rv.cy = 32; rv.zoom = 1.0F;
            mq_renderer_frame3d rf{};
            rf.width = 64; rf.height = 64;
            rf.views2d = &rv; rf.view2d_count = 1;
            rf.items = &red; rf.item_count = 1;
            rf.bg[3] = 1.0F;
            CHECK(api->render_to_texture(self, rt, &rf) == 0);
            std::vector<uint8_t> rtpx(64u * 64u * 4u);
            CHECK(api->read_texture(self, rt, rtpx.data(), (uint32_t)rtpx.size()) == 0);
            CHECK(rtpx[(32u * 64u + 32u) * 4u] > 220);

            mq_renderer_item display = sprite(160.0F, 120.0F, 50.0F, 1.0F, 1.0F, 1.0F);
            display.texture = rt;
            display.half_w = 50.0F; display.half_h = 50.0F;
            mq_renderer_view mainv{};
            mainv.vw = 320; mainv.vh = 240; mainv.cx = 160; mainv.cy = 120; mainv.zoom = 1.0F;
            cv.clear();
            CHECK(api->render_views(self, 320, 240, &mainv, 1, &display, 1, bg,
                                    cv.px.data(), (uint32_t)cv.px.size()) == 0);
            CHECK(cv.at(160, 120, 0) > 220 && cv.at(160, 120, 1) < 40 && cv.at(160, 120, 2) < 40);

            // 目标直接采样自身：后端必须剔除该批次，只留下蓝色 clear，不能触发 WebGPU validation。
            mq_renderer_item self_sample = display;
            self_sample.x = 32; self_sample.y = 32; self_sample.half_w = 30; self_sample.half_h = 30;
            rf.items = &self_sample; rf.item_count = 1;
            rf.bg[0] = 0.0F; rf.bg[1] = 0.0F; rf.bg[2] = 1.0F; rf.bg[3] = 1.0F;
            CHECK(api->render_to_texture(self, rt, &rf) == 0);
            CHECK(api->read_texture(self, rt, rtpx.data(), (uint32_t)rtpx.size()) == 0);
            const size_t center = (32u * 64u + 32u) * 4u;
            CHECK(rtpx[center] < 30 && rtpx[center + 1] < 30 && rtpx[center + 2] > 220);

            rd.width = 32; rd.height = 16;
            CHECK(api->configure_render_texture(self, rt, &rd) == 0);
            CHECK(api->has_texture(self, rt) == 1);                    // 原位替换，句柄不变
            rf.width = 32; rf.height = 16;
            rv.vw = 32; rv.vh = 16; rv.cx = 16; rv.cy = 8;
            red.x = 16; red.y = 8; red.size = 7;
            rf.items = &red; rf.item_count = 1;
            CHECK(api->render_to_texture(self, rt, &rf) == 0);
            api->destroy_texture(self, rt);
            CHECK(api->has_texture(self, rt) == 0);
            if (api->resource_bytes) CHECK(api->resource_bytes(self) == bytes0);
        }

        // 网格上传：单三角形，规范布局 pos3/normal3/tangent4/uv2 = 48B
        const float verts48[36] = {
            // pos3              normal3          tangent4           uv2
             0.0F,  0.5F, 0.0F,  0.0F, 0.0F, 1.0F,  1.0F, 0.0F, 0.0F, 1.0F,  0.5F, 1.0F,
            -0.5F, -0.5F, 0.0F,  0.0F, 0.0F, 1.0F,  1.0F, 0.0F, 0.0F, 1.0F,  0.0F, 0.0F,
             0.5F, -0.5F, 0.0F,  0.0F, 0.0F, 1.0F,  1.0F, 0.0F, 0.0F, 1.0F,  1.0F, 0.0F,
        };
        const uint32_t idx[3] = { 0, 1, 2 };
        mq_renderer_mesh_gpu md{};
        md.vertices = verts48; md.vertex_bytes = sizeof verts48; md.vertex_stride = 48; md.vertex_layout = 0;
        md.indices = idx; md.index_count = 3;
        md.bounds_min[0] = -0.5F; md.bounds_min[1] = -0.5F; md.bounds_min[2] = 0.0F;
        md.bounds_max[0] =  0.5F; md.bounds_max[1] =  0.5F; md.bounds_max[2] = 0.0F;
        int32_t mesh = -1;
        CHECK(api->create_mesh(self, &md, &mesh) == 0);
        CHECK(mesh >= 0);
        api->destroy_mesh(self, mesh);
        // 非法布局/数据：明确失败（不静默）
        mq_renderer_mesh_gpu bad = md;
        bad.vertex_stride = 32;
        int32_t dummy = -1;
        CHECK(api->create_mesh(self, &bad, &dummy) != 0);
        bad = md; bad.vertices = nullptr;
        CHECK(api->create_mesh(self, &bad, &dummy) != 0);

        // GPU 资源按类拆分（docs/29 §10.4）：texture + buffer 必须等于旧 resource_bytes 总量。
        const bool has_res2 = api->header.struct_size >=
            offsetof(mq_renderer_v2, resource_bytes2) + sizeof(api->resource_bytes2);
        CHECK(has_res2);
        if (has_res2) {
            mq_renderer_resource_usage usage{};
            usage.struct_size = (uint32_t)sizeof usage;
            CHECK(api->resource_bytes2(self, &usage) == 0);
            CHECK(usage.other_bytes == 0);
            CHECK(usage.texture_bytes + usage.buffer_bytes == api->resource_bytes(self));
        }

        // kind=0 是显式 detach：无 surface 时也应幂等成功，runtime 用它从直呈回退离屏。
        mq_renderer_window win{};
        CHECK(api->attach_window(self, &win) == 0);
    }

    api->destroy(self);
    mq_host_destroy(host);
    if (g_failures == 0) { std::print("gfx_test: all checks passed\n"); return 0; }
    std::print("gfx_test: {} failure(s)\n", g_failures);
    return 1;
}
