// 两色着色（Tint Black / Two-color tinting）像素级测试 —— **有鉴别力**的那种。
//
// 背景（必读，否则容易写出"看着对、其实什么都没验证"的测试）：
//   引擎原先的 mesh 片元只做 `tex * light`，没有 dark 一路。要证明"两色着色真的做对了"，
//   必须让**开与关的输出确实不同**。为此本测试**不用**官方 raptor/batching 素材 —— 实测
//   `.deps/spine42ex/.../examples/raptor/export/raptor-pro.json` 全文 0 个 `"dark"` 键、
//   `BatchingExample.cpp` 载入的是 `spineboy-pro.json`（同样 0 个）且它的
//   `setTwoColorTint(true)` 在上游源码里就是注释掉的 → 这两个 demo 开关两色着色**像素完全一样**，
//   拿它们做断言等于没测（spineboy 裁剪遮罩上已经踩过这个坑）。
//   官方素材里唯一"dark 非黑"的是 `alien-pro.json` 的 `eyeball` 槽（light=dfa921ff / dark=c57c1e），
//   本文件直接采用这对官方颜色做断言（也覆盖"dark=黑 ⇒ 零差异"的退化情形）。
//
// 公式出处（**照抄，不是推导**）：
//   spine-runtimes 4.2 `spine-glfw/src/spine-glfw.cpp`（官方 GLFW 前端内嵌 GLSL）
//     顶点：`layout (location = 3) in vec4 aDarkColor;`
//     片元：vec4 texColor = texture(uTexture, texCoord);
//           float alpha = texColor.a * lightColor.a;
//           fragColor.rgb = ((texColor.a - 1.0) * darkColor.a + 1.0 - texColor.rgb)
//                           * darkColor.rgb + texColor.rgb * lightColor.rgb;
//   （取回命令见 assets/builtin/shaders/mesh/shader.wgsl 顶部注释；本仓库存档了一份在
//     `.deps/spine42ex/_upstream/spine-glfw.cpp`。）
//   dark 的**取值**语义来自 `third_party/spine-cpp/src/spine/SkeletonRenderer.cpp:217`
//   （`uint32_t darkColor = 0xff000000;` = 未启用 TintBlack → rgba(0,0,0,1)）。
//
// 五组断言（每组都能被证伪）：
//   ① **退化 / 不回归**：dark 不给（nullptr）与显式 dark=(0,0,0,1) 输出**逐字节相同**，
//      且等于上传的纹理字节本身（light=白）。
//   ② **有鉴别力**：用官方 alien eyeball 的 light/dark 对，开与关的输出**明显不同**（给像素数值）。
//   ③ **公式正确**：逐通道等于官方公式的期望值（±1，吸收 unorm↔float 的舍入）。
//   ④ **dark.a 项**：`(texColor.a - 1.0) * darkColor.a` 只对**半透明纹素**起作用 ——
//      用 a=128 的纹素 + dark.a ∈ {0, 0.5, 1} 得到 128/96/64 三个不同值，钉住这一项
//      （丢掉 dark.a 的实现三项都会是 128）。
//   ⑤ **light 未丢失**：light=(0.5,1,0.25) 且无 dark 时仍按通道相乘。
#include "maque_ifaces.h"
#include "core/module/mq_module.h"

#include <print>
#include <vector>
#include <cmath>
#include <algorithm>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print(stderr, "FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr int W = 320, H = 240;
constexpr int NTEX = 8;

struct Canvas {
    std::vector<uint8_t> px = std::vector<uint8_t>((size_t)W * H * 4);
    int at(int x, int y, int c) const { return (int)px[((size_t)y * W + x) * 4 + c]; }
};

/* 8 个竖条，第 i 条的采样中心像素 (block_cx(i), 120)。
 * 每条 = 一个四边形，**四个顶点共用同一个 UV**（= 第 i 个纹素的中心 (i+0.5)/8）。
 * 为什么这样摆：UV 在四边形内恒定 → 无论线性还是最近邻采样，采到的都是**那一个纹素本身**，
 * 不存在插值/寻址的模糊地带，期望值可以精确算出来。纹理 mipLevelCount=1（见 gfx create_texture），
 * 所以也不会有 mip 混色。对 1x1 纹理（ClampToEdge）则恒采样唯一纹素。 */
int block_x0(int i) { return 8 + i * 38; }
int block_cx(int i) { return block_x0(i) + 16; }
constexpr int BLOCK_Y0 = 96, BLOCK_H = 48;

uint8_t texel_value(int i) { return (uint8_t)std::lround((double)i * 255.0 / (double)(NTEX - 1)); }

/* 期望像素（完全按官方片元公式 + 本引擎的混合设置推导）。
 *   片元：s_rgb = ((tex_a - 1)*dark_a + 1 - tex_rgb) * dark_rgb + tex_rgb * light_rgb
 *         s_a   = tex_a * light_a
 *   混合：pma=0 / blend=normal → src 因子 = SrcAlpha = s_a，dst 因子 = 1 - s_a；
 *         背景全黑 → 目标 = s_rgb * s_a。
 *   目标格式 RGBA8Unorm → byte = round(clamp(v,0,1) * 255)。
 * 注：GPU 的 float→unorm 在 x.5 上按"就近偶数"，与 lround 可能差 1 —— 调用方按 ±1 断言。 */
int expect_byte(double tex_a, double tex_rgb, double light_rgb, double light_a,
                double dark_rgb, double dark_a) {
    double s = ((tex_a - 1.0) * dark_a + 1.0 - tex_rgb) * dark_rgb + tex_rgb * light_rgb;
    double out = s * (tex_a * light_a);
    if (out < 0.0) out = 0.0;
    if (out > 1.0) out = 1.0;
    return (int)std::lround(out * 255.0);
}

struct RenderArgs {
    bool explicit_dark = false;              // false → dark_colors = nullptr（gfx 用默认 0,0,0,1）
    float lr = 1.0F, lg = 1.0F, lb = 1.0F, la = 1.0F;
    float dr = 0.0F, dg = 0.0F, db = 0.0F, da = 1.0F;
};

bool render_stripes(const mq_renderer_v1* api, void* self, int tex, const RenderArgs& a,
                    const float bg[4], Canvas& cv) {
    std::vector<float> verts((size_t)NTEX * 4 * 2), uvs((size_t)NTEX * 4 * 2);
    std::vector<float> cols((size_t)NTEX * 4 * 4), darks;
    if (a.explicit_dark) darks.assign((size_t)NTEX * 4 * 4, 0.0F);
    std::vector<uint32_t> idx((size_t)NTEX * 6);

    for (int i = 0; i < NTEX; ++i) {
        const float x0 = (float)block_x0(i), x1 = x0 + 32.0F;
        const float y0 = (float)BLOCK_Y0, y1 = y0 + (float)BLOCK_H;
        const float u = ((float)i + 0.5F) / (float)NTEX, v = 0.5F;
        const float xs[4] = { x0, x1, x0, x1 };
        const float ys[4] = { y0, y0, y1, y1 };
        for (int k = 0; k < 4; ++k) {
            const size_t vi = (size_t)(i * 4 + k);
            verts[vi * 2] = xs[k]; verts[vi * 2 + 1] = ys[k];
            uvs[vi * 2] = u; uvs[vi * 2 + 1] = v;
            cols[vi * 4] = a.lr; cols[vi * 4 + 1] = a.lg;
            cols[vi * 4 + 2] = a.lb; cols[vi * 4 + 3] = a.la;
            if (a.explicit_dark) {
                darks[vi * 4] = a.dr; darks[vi * 4 + 1] = a.dg;
                darks[vi * 4 + 2] = a.db; darks[vi * 4 + 3] = a.da;
            }
        }
        static const uint32_t kQuad[6] = { 0, 1, 2, 2, 1, 3 };
        for (int k = 0; k < 6; ++k) idx[(size_t)i * 6 + k] = (uint32_t)(i * 4) + kQuad[k];
    }

    mq_renderer_mesh m{};
    m.vertices = verts.data(); m.uvs = uvs.data(); m.colors = cols.data();
    m.dark_colors = a.explicit_dark ? darks.data() : nullptr;
    m.indices = idx.data();
    m.vertex_count = (uint32_t)(NTEX * 4); m.index_count = (uint32_t)(NTEX * 6);
    m.texture = tex; m.blend = 0; m.pma = 0;

    mq_renderer_view view{};
    view.vw = W; view.vh = H; view.cx = W / 2; view.cy = H / 2; view.zoom = 1.0F;
    cv.px.assign((size_t)W * H * 4, 0);
    return api->render_mesh_views(self, W, H, &view, 1, nullptr, 0, &m, 1,
                                  bg, cv.px.data(), (uint32_t)cv.px.size()) == 0;
}

} // namespace

int main() {
    mq_host* host = mq_host_create();
    mq_loaded_module* mod = nullptr;
    if (mq_host_load(host, MQ_GFX_PATH, &mod) != 0) { std::print("twocolor_test: SKIP (no plugin)\n"); return 0; }
    auto* api = (const mq_renderer_v1*)mq_host_query(host, MQ_RENDERER_API_NAME, 1);
    CHECK(api != nullptr);
    if (!api) return 1;
    const char* backend = nullptr;
    void* self = api->create(&backend);
    CHECK(self != nullptr);
    if (!self) return 1;
    std::print("twocolor_test: backend='{}'\n", backend ? backend : "?");

    const float bg[4] = { 0.0F, 0.0F, 0.0F, 1.0F };   // 全黑：合成结果 = 源色 × alpha

    /* 纹理 A：8x1 灰度斜坡（alpha 全 255）——测 light/dark 的 rgb 插值 */
    uint8_t texels[NTEX * 4];
    for (int i = 0; i < NTEX; ++i) {
        const uint8_t v = texel_value(i);
        texels[i * 4] = v; texels[i * 4 + 1] = v; texels[i * 4 + 2] = v; texels[i * 4 + 3] = 255;
    }
    const int texA = api->create_texture(self, texels, NTEX, 1);
    CHECK(texA >= 0);
    std::print("  纹理A {0}x1 灰度斜坡 (alpha=255):", NTEX);
    for (int i = 0; i < NTEX; ++i) std::print(" {}", (int)texel_value(i));
    std::print("\n");

    /* 纹理 B：1x1 白 + alpha=128 —— 测 dark.a 那一项（只对半透明纹素生效） */
    const uint8_t tmpl[4] = { 255, 255, 255, 128 };
    const int texB = api->create_texture(self, tmpl, 1, 1);
    CHECK(texB >= 0);

    /* 官方 alien-pro.json 的 eyeball 槽：light=dfa921ff / dark=c57c1e */
    const float AL[3] = { 0xdf / 255.0F, 0xa9 / 255.0F, 0x21 / 255.0F };
    const float AD[3] = { 0xc5 / 255.0F, 0x7c / 255.0F, 0x1e / 255.0F };

    Canvas cv_off, cv_black, cv_on, cv_lightish;

    // ---------- ① dark=nullptr（light=白）：像素**就是纹理字节** ----------
    {
        RenderArgs a;
        CHECK(render_stripes(api, self, texA, a, bg, cv_off));
        int bad = 0;
        std::print("  ① light=白 dark=nullptr        :");
        for (int i = 0; i < NTEX; ++i) {
            const int got = cv_off.at(block_cx(i), 120, 0);
            std::print(" {}", got);
            if (got != (int)texel_value(i)) ++bad;
            for (int c = 1; c < 3; ++c) if (cv_off.at(block_cx(i), 120, c) != got) ++bad;
            if (cv_off.at(block_cx(i), 120, 3) != 255) ++bad;
        }
        std::print("\n");
        CHECK(bad == 0);
    }

    // ---------- ①b 显式 dark=(0,0,0,1)：必须与①**整幅逐字节相同** ----------
    {
        RenderArgs a; a.explicit_dark = true;                 // dr=dg=db=0, da=1
        CHECK(render_stripes(api, self, texA, a, bg, cv_black));
        const bool same = (cv_black.px == cv_off.px);
        std::print("  ①b dark=(0,0,0,1) 与①整幅逐字节相同: {}\n", same ? "是" : "否");
        CHECK(same);
    }

    // ---------- ②③ 官方 alien eyeball 的 light/dark：开 vs 关 + 公式对比 ----------
    Canvas cv_off_alien;
    {
        RenderArgs off; off.lr = AL[0]; off.lg = AL[1]; off.lb = AL[2];   // dark=nullptr
        CHECK(render_stripes(api, self, texA, off, bg, cv_off_alien));

        RenderArgs on; on.explicit_dark = true;
        on.lr = AL[0]; on.lg = AL[1]; on.lb = AL[2];
        on.dr = AD[0]; on.dg = AD[1]; on.db = AD[2]; on.da = 1.0F;
        CHECK(render_stripes(api, self, texA, on, bg, cv_on));

        int maxd[3] = { 0, 0, 0 };
        std::print("  ② 官方 alien eyeball light=dfa921ff / dark=c57c1e\n");
        std::print("     关(无 dark)  R:");
        for (int i = 0; i < NTEX; ++i) std::print(" {}", cv_off_alien.at(block_cx(i), 120, 0));
        std::print("\n     开(dark=c57c1e) R:");
        for (int i = 0; i < NTEX; ++i) std::print(" {}", cv_on.at(block_cx(i), 120, 0));
        for (int i = 0; i < NTEX; ++i)
            for (int c = 0; c < 3; ++c)
                maxd[c] = std::max(maxd[c], std::abs(cv_on.at(block_cx(i), 120, c) -
                                                     cv_off_alien.at(block_cx(i), 120, c)));
        std::print("\n     开/关最大逐通道差 (R,G,B) = ({}, {}, {})\n", maxd[0], maxd[1], maxd[2]);
        CHECK(cv_on.px != cv_off_alien.px);
        /* 解析：diff(t) = (1-t)·dark.rgb，在 t=0（最暗纹素）取最大 = dark.rgb×255。
         * 关的那一路在那个纹素上是 0（纹理是黑的），所以差值就是 dark 的字节值本身：
         * c5=197 / 7c=124 / 1e=30。**精确**断言，不留"猜一个阈值"的余地。 */
        for (int c = 0; c < 3; ++c) {
            const int exp_delta = (int)std::lround((double)AD[c] * 255.0);
            CHECK(maxd[c] == exp_delta);
        }

        int worst = 0;
        std::print("  ③ 官方公式对比 (纹素 v | 通道: 实测 vs 期望)\n");
        for (int i = 0; i < NTEX; ++i) {
            const double t = (double)texel_value(i) / 255.0;
            std::print("     v={0:3d}  ", (int)texel_value(i));
            for (int c = 0; c < 3; ++c) {
                const double exp = expect_byte(1.0, t, (double)AL[c], 1.0, (double)AD[c], 1.0);
                const int got = cv_on.at(block_cx(i), 120, c);
                std::print("{}: {:3d}/{:<3.0f}  ", "RGB"[c], got, exp);
                worst = std::max(worst, std::abs(got - (int)exp));
            }
            std::print("\n");
        }
        std::print("     最大偏差 = {} (容差 1)\n", worst);
        CHECK(worst <= 1);

        int worst_off = 0;   // 关的那一路也要对（= 旧的 `tex * light` 行为）
        for (int i = 0; i < NTEX; ++i) {
            const double t = (double)texel_value(i) / 255.0;
            for (int c = 0; c < 3; ++c)
                worst_off = std::max(worst_off, std::abs(cv_off_alien.at(block_cx(i), 120, c) -
                                                        expect_byte(1.0, t, (double)AL[c], 1.0, 0.0, 1.0)));
        }
        std::print("     关(无 dark) 最大偏差 = {} (容差 1)\n", worst_off);
        CHECK(worst_off <= 1);
    }

    // ---------- ④ dark.a 那一项：只对**半透明纹素**生效 ----------
    //   纹理 B：rgb=255, a=128 → tex_a = 128/255；light=白、dark=(1,1,1,da)
    //     s_rgb = ((tex_a-1)*da + 1 - 1)*1 + 1*1 = (tex_a-1)*da + 1
    //     s_a   = tex_a ；目标 = s_rgb * tex_a
    //   da=0   → s_rgb = 1        → 0.50196*255 ≈ 128
    //   da=0.5 → s_rgb = 0.75098  → 0.37696*255 ≈  96
    //   da=1   → s_rgb = 0.50196  → 0.25196*255 ≈  64
    {
        const float das[3] = { 0.0F, 0.5F, 1.0F };
        const int expects[3] = { 128, 96, 64 };
        std::print("  ④ dark.a 项（纹素 rgb=255 a=128, light=白, dark=(1,1,1,da)）\n");
        for (int k = 0; k < 3; ++k) {
            RenderArgs a; a.explicit_dark = true;
            a.dr = a.dg = a.db = 1.0F; a.da = das[k];
            Canvas cv;
            CHECK(render_stripes(api, self, texB, a, bg, cv));
            const int got = cv.at(block_cx(0), 120, 0);
            const int exp = expect_byte(128.0 / 255.0, 1.0, 1.0, 1.0, 1.0, (double)das[k]);
            std::print("     da={:.1f}  实测 {}  期望 {} (解析值 {})\n", (double)das[k], got, exp, expects[k]);
            CHECK(std::abs(got - exp) <= 1);
            CHECK(std::abs(got - expects[k]) <= 1);
        }
    }

    // ---------- ⑤ light 仍然照常逐通道相乘（两色着色不能把 light 弄丢）----------
    {
        RenderArgs a; a.lr = 0.5F; a.lg = 1.0F; a.lb = 0.25F;    // dark=nullptr
        CHECK(render_stripes(api, self, texA, a, bg, cv_lightish));
        const double ls[3] = { 0.5, 1.0, 0.25 };
        int worst = 0;
        std::print("  ⑤ light=(0.5,1,0.25) dark=nullptr  R:");
        for (int i = 0; i < NTEX; ++i) std::print(" {}", cv_lightish.at(block_cx(i), 120, 0));
        std::print("\n");
        for (int i = 0; i < NTEX; ++i) {
            const double t = (double)texel_value(i) / 255.0;
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(cv_lightish.at(block_cx(i), 120, c) -
                                                 expect_byte(1.0, t, ls[c], 1.0, 0.0, 1.0)));
        }
        std::print("     最大偏差 = {} (容差 1)\n", worst);
        CHECK(worst <= 1);
    }

    api->destroy_texture(self, texA);
    api->destroy_texture(self, texB);
    api->destroy(self);
    mq_host_destroy(host);
    if (g_failures == 0) { std::print("twocolor_test: all checks passed\n"); return 0; }
    std::print(stderr, "twocolor_test: {} failure(s)\n", g_failures);
    return 1;
}
