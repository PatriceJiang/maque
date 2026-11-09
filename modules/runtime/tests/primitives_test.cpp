/* 程序化图元几何单测（plans/27 M1-1）：不依赖 wgpu/窗口/Engine。
 * 覆盖：顶点布局/索引范围/法线单位长/包围盒包含/三角绕序朝外/分段夹紧/关键尺寸。 */
#include "runtime_common.h"

#include <cmath>
#include <print>
#include <vector>

using namespace maque::rt;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond);  \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void check_kind(int kind, float sx, float sy, float sz, float seg_x, float seg_y) {
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    const float size[3] = { sx, sy, sz };
    const float seg[3] = { seg_x, seg_y, 8.0F };
    build_geometry(kind, size, seg, verts, idx);
    CHECK(!verts.empty());
    CHECK(verts.size() % 12 == 0);
    CHECK(!idx.empty());
    CHECK(idx.size() % 3 == 0);

    const uint32_t nv = (uint32_t)(verts.size() / 12);
    float bmin[3], bmax[3];
    primitive_bounds(kind, size, bmin, bmax);
    for (uint32_t i = 0; i < nv; ++i) {
        const float* p = &verts[(size_t)i * 12];
        CHECK(finite3(p[0], p[1], p[2]));
        CHECK(finite3(p[3], p[4], p[5]));
        const float nl = std::sqrt(dot3(p + 3, p + 3));
        CHECK(std::fabs(nl - 1.0F) < 1e-3F);
        for (int a = 0; a < 3; ++a) {
            CHECK(p[a] >= bmin[a] - 1e-3F && p[a] <= bmax[a] + 1e-3F);
        }
    }
    for (uint32_t i = 0; i < (uint32_t)idx.size(); ++i) CHECK(idx[i] < nv);

    /* 绕序：三角面法线（右手 CCW）应与三个顶点法线的平均同向（外表面）。 */
    int checked = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const float* a = &verts[(size_t)idx[t] * 12];
        const float* b = &verts[(size_t)idx[t + 1] * 12];
        const float* c = &verts[(size_t)idx[t + 2] * 12];
        const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                             e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0] };
        const float area2 = std::sqrt(dot3(n, n));
        if (area2 < 1e-10F) continue;                    /* 极点退化三角 */
        const float an[3] = { a[3] + b[3] + c[3], a[4] + b[4] + c[4], a[5] + b[5] + c[5] };
        CHECK(dot3(n, an) > 0.0F);
        ++checked;
    }
    CHECK(checked > 0);
}

}  // namespace

int main() {
    for (int kind = 0; kind <= 6; ++kind)
        check_kind(kind, 1.0F, 1.0F, 1.0F, 12.0F, 8.0F);

    /* 缺省 Cube（1.2）与旧内建 cube 尺寸一致；24 顶点 / 36 索引。 */
    {
        std::vector<float> v;
        std::vector<uint32_t> i;
        const float size[3] = { 1.2F, 1.2F, 1.2F };
        const float seg[3] = { 32.0F, 16.0F, 8.0F };
        build_geometry(0, size, seg, v, i);
        CHECK(v.size() / 12 == 24);
        CHECK(i.size() == 36);
        float mn[3], mx[3];
        primitive_bounds(0, size, mn, mx);
        CHECK(std::fabs(mn[0] + 0.6F) < 1e-6F && std::fabs(mx[2] - 0.6F) < 1e-6F);
    }
    /* 球：半径 = size.x，包围盒 ±r。 */
    {
        std::vector<float> v;
        std::vector<uint32_t> i;
        const float size[3] = { 0.5F, 0.0F, 0.0F };
        const float seg[3] = { 8.0F, 6.0F, 1.0F };
        build_geometry(1, size, seg, v, i);
        CHECK(v.size() / 12 == (8 + 1) * (6 + 1));
        float mn[3], mx[3];
        primitive_bounds(1, size, mn, mx);
        CHECK(std::fabs(mn[0] + 0.5F) < 1e-6F && std::fabs(mx[1] - 0.5F) < 1e-6F);
    }
    /* 平面：z 层厚 0；胶囊：y = ±(圆柱段/2 + 半径)。 */
    {
        std::vector<float> v;
        std::vector<uint32_t> i;
        const float psize[3] = { 2.0F, 1.0F, 0.0F };
        const float pseg[3] = { 1.0F, 1.0F, 1.0F };
        build_geometry(2, psize, pseg, v, i);
        CHECK(v.size() / 12 == 4 && i.size() == 6);
        float mn[3], mx[3];
        primitive_bounds(2, psize, mn, mx);
        CHECK(mn[2] == 0.0F && mx[2] == 0.0F);

        const float csize[3] = { 0.3F, 0.8F, 0.0F };
        primitive_bounds(6, csize, mn, mx);
        CHECK(std::fabs(mx[1] - 0.7F) < 1e-6F && std::fabs(mn[1] + 0.7F) < 1e-6F);
    }
    /* 分段夹紧：超大 segments 不能把顶点数炸掉。 */
    {
        std::vector<float> v;
        std::vector<uint32_t> i;
        const float size[3] = { 1.0F, 1.0F, 1.0F };
        const float seg[3] = { 100000.0F, 100000.0F, 1.0F };
        build_geometry(1, size, seg, v, i);
        CHECK(v.size() / 12 <= 257 * 129);
    }

    if (g_fail == 0) std::print("primitives_test: OK\n");
    return g_fail == 0 ? 0 : 1;
}
