/* 9-patch 纯几何：块数/角尺寸/目标压缩/翻转/退化。 */
#include "render/nine_patch.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace maque::rt;

namespace {
int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

NinePatchParams base() {
    NinePatchParams p{};
    p.dst_w = 80.0F; p.dst_h = 80.0F;
    p.src_x = 0.0F; p.src_y = 0.0F; p.src_w = 24.0F; p.src_h = 24.0F;
    p.tex_w = 24.0F; p.tex_h = 24.0F;
    p.margin_l = p.margin_t = p.margin_r = p.margin_b = 8.0F;
    return p;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-4F; }

void nine_quads() {
    const auto q = nine_patch_quads(base());
    CHECK(q.size() == 9);
    /* 左上角：中心 (-32,-32)，8x8；UV 0..1/3 */
    CHECK(near(q[0].cx, -36.0F) && near(q[0].cy, -36.0F));
    CHECK(near(q[0].hw, 4.0F) && near(q[0].hh, 4.0F));
    CHECK(near(q[0].u0, 0.0F) && near(q[0].u1, 8.0F / 24.0F));
    /* 上边中块：x 中心 0，宽 64；v 仍 0..1/3 */
    CHECK(near(q[1].cx, 0.0F) && near(q[1].hw, 32.0F));
    CHECK(near(q[1].u0, 8.0F / 24.0F) && near(q[1].u1, 16.0F / 24.0F));
    /* 中心块：64x64 */
    CHECK(near(q[4].cx, 0.0F) && near(q[4].cy, 0.0F));
    CHECK(near(q[4].hw, 32.0F) && near(q[4].hh, 32.0F));
    CHECK(near(q[4].u0, 8.0F / 24.0F) && near(q[4].v0, 8.0F / 24.0F));
    /* 右下角 */
    CHECK(near(q[8].cx, 36.0F) && near(q[8].cy, 36.0F));
    CHECK(near(q[8].u1, 1.0F) && near(q[8].v1, 1.0F));
}

void small_target_clamps_dst_margins() {
    NinePatchParams p = base();
    p.dst_w = p.dst_h = 10.0F;
    const auto q = nine_patch_quads(p);
    /* 源角 8px，但目标只有 10 → 两边各 5，中间为 0 → 4 块 */
    CHECK(q.size() == 4);
    for (const auto& v : q) CHECK(near(v.hw, 2.5F));
    CHECK(near(q[0].u1, 8.0F / 24.0F));          /* 源 UV 仍是 8px 角 */
}

void zero_margin_single_quad() {
    NinePatchParams p = base();
    p.margin_l = p.margin_t = p.margin_r = p.margin_b = 0.0F;
    const auto q = nine_patch_quads(p);
    CHECK(q.size() == 1);
    CHECK(near(q[0].cx, 0.0F) && near(q[0].cy, 0.0F));
    CHECK(near(q[0].hw, 40.0F) && near(q[0].hh, 40.0F));
    CHECK(near(q[0].u0, 0.0F) && near(q[0].u1, 1.0F));
}

void flip_mirrors_center_and_uv() {
    NinePatchParams p = base();
    p.flip_h = true;
    const auto q = nine_patch_quads(p);
    CHECK(q.size() == 9);
    CHECK(near(q[0].cx, 36.0F));                 /* 左角 → 右 */
    CHECK(near(q[0].u0, 8.0F / 24.0F) && near(q[0].u1, 0.0F));
    p.flip_h = false; p.flip_v = true;
    const auto r = nine_patch_quads(p);
    CHECK(near(r[0].cy, 36.0F));
    CHECK(near(r[0].v0, 8.0F / 24.0F) && near(r[0].v1, 0.0F));
}

void source_margins_clamp() {
    NinePatchParams p = base();
    p.margin_l = p.margin_r = 20.0F;             /* 40 > 24 → 各 12，中间 0 */
    const auto q = nine_patch_quads(p);
    CHECK(q.size() == 6);                        /* 2 列 × 3 行 */
    CHECK(near(q[0].hw, 6.0F));
}

void degenerate_empty() {
    NinePatchParams p = base();
    p.dst_w = 0.0F;
    CHECK(nine_patch_quads(p).empty());
    p = base();
    p.tex_w = 0.0F;
    CHECK(nine_patch_quads(p).empty());
}

}  // namespace

int main() {
    nine_quads();
    small_target_clamps_dst_margins();
    zero_margin_single_quad();
    flip_mirrors_center_and_uv();
    source_margins_clamp();
    degenerate_empty();
    if (g_fail) {
        std::printf("nine_patch_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("nine_patch_test: ok\n");
    return 0;
}
