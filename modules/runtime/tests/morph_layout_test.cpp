/* GPU morph 增量打包单测（plans/28 §3.9）：不依赖 wgpu/窗口/Engine。
 * 覆盖：32B/目标/顶点布局（target 主序）、normal 缺失补 0、has_normals 标志、
 * 目标数上限 8、非法输入拒绝、以及"同一份 target 数据两次打包逐字节一致"。 */
#include "runtime_common.h"

#include <cmath>
#include <print>
#include <utility>
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

bool nearly(float a, float b) { return std::fabs(a - b) < 1e-6F; }

}  // namespace

int main() {
    /* 3 顶点 × 2 target：target0 有 pos+normal，target1 只有 pos（normal 段必须全 0）。 */
    const std::vector<float> pos0 = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    const std::vector<float> nrm0 = { 0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F, 0.9F };
    const std::vector<float> pos1 = { -1, -2, -3, -4, -5, -6, -7, -8, -9 };
    const std::pair<const float*, const float*> src[2] = {
        { pos0.data(), nrm0.data() },
        { pos1.data(), nullptr },
    };
    std::vector<float> out;
    const int packed = pack_morph_deltas(3, 2, src, out);
    CHECK(packed == 1);                              /* 含 normal 增量 */
    CHECK(out.size() == 2u * 3u * 8u);
    CHECK(nearly(out[0], 1) && nearly(out[1], 2) && nearly(out[2], 3));
    CHECK(nearly(out[4], 0.1F) && nearly(out[5], 0.2F) && nearly(out[6], 0.3F));
    /* target 主序：第二个 target 从 t*vn*8 = 24 开始（float 下标）。 */
    CHECK(nearly(out[24], -1) && nearly(out[25], -2) && nearly(out[26], -3));
    /* target1 无 normal → [24+4..24+6] 必须是 0（不是上一 target 的残留）。 */
    CHECK(nearly(out[28], 0) && nearly(out[29], 0) && nearly(out[30], 0));
    /* pad 位必须为 0。 */
    CHECK(nearly(out[3], 0) && nearly(out[7], 0) && nearly(out[27], 0) && nearly(out[31], 0));

    /* 确定性：同一输入两次打包逐字节一致。 */
    std::vector<float> out2;
    CHECK(pack_morph_deltas(3, 2, src, out2) == 1);
    CHECK(out == out2);

    /* 全部 target 都没有 normal → 返回 0（shader 跳过法线累加）。 */
    const std::pair<const float*, const float*> pos_only[1] = { { pos0.data(), nullptr } };
    CHECK(pack_morph_deltas(3, 1, pos_only, out) == 0);
    CHECK(out.size() == 24);

    /* 上限 8：9 个 target 拒绝；0 target / 0 顶点 / 空 pointer / 空 pos 都拒绝。 */
    std::vector<std::pair<const float*, const float*>> nine(9, { pos0.data(), nullptr });
    CHECK(pack_morph_deltas(3, 9, nine.data(), out) == -1);
    CHECK(out.empty());
    CHECK(pack_morph_deltas(0, 1, pos_only, out) == -1);
    CHECK(pack_morph_deltas(3, 0, pos_only, out) == -1);
    CHECK(pack_morph_deltas(3, 1, nullptr, out) == -1);
    const std::pair<const float*, const float*> bad[1] = { { nullptr, nullptr } };
    CHECK(pack_morph_deltas(3, 1, bad, out) == -1);

    if (g_fail) {
        std::print("morph_layout_test: {} FAIL\n", g_fail);
        return 1;
    }
    std::print("morph_layout_test: PASS（32B/目标/顶点、target 主序、normal 缺失补 0、上限 8）\n");
    return 0;
}
