/* maque::imgui 单测：验证 **绘制数据导出** 的正确性（不需要 GPU/窗口）。
 *
 * 这一层能抓到的问题很具体：
 *   · begin/end 时序与返回码；
 *   · 字体图集是否真的产出 RGBA32（宿主据此上传纹理）；
 *   · 导出命令的索引是否**都在顶点范围内**（越界会导致渲染读野内存）；
 *   · 显示→目标 的坐标映射是否按给定 scale/offset 生效（否则面板位置会错）；
 *   · 没有任何控件时不应产出几何（避免空提交）。
 */
#include "ui/mq_imgui.h"

#include "imgui.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        auto _a = (a);                                                           \
        auto _b = (b);                                                           \
        if (!(_a == _b)) {                                                       \
            ++g_fail;                                                            \
            std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,       \
                        __LINE__, #a, #b, (long long)_a, (long long)_b);         \
        }                                                                        \
    } while (0)

/* 跑一帧：show=显示尺寸，target=渲染目标，scale/off=目标→显示映射 */
static int run_frame(mq_imgui_frame *out, float show_w, float show_h,
                     float target_w, float target_h, float scale, float off_x, float off_y) {
    mq_imgui_begin_frame(show_w, show_h, target_w, target_h, scale, off_x, off_y, 1.0F / 60.0F, scale);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(220, 120), ImGuiCond_Always);
    ImGui::Begin("controls", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("spine demos: 12 skeletons");
    static float speed = 1.0F;
    ImGui::SliderFloat("speed", &speed, 0.0F, 3.0F);
    static bool paused = false;
    ImGui::Checkbox("pause", &paused);
    if (ImGui::Button("reset")) speed = 1.0F;
    ImGui::End();
    return mq_imgui_end_frame(out);
}

int main() {
    /* 未初始化时应安全失败，而不是崩 */
    CHECK_EQ(mq_imgui_ready(), 0);
    CHECK_EQ(mq_imgui_begin_frame(100, 100, 100, 100, 1.0F, 0, 0, 0.016F, 1.0F), 1);

    /* ① 初始化 + 字体图集（RGBA32） */
    CHECK_EQ(mq_imgui_init(0.0F, 1.0F), 0);
    CHECK_EQ(mq_imgui_ready(), 1);
    int fw = 0, fh = 0;
    const uint8_t *fp = mq_imgui_font_pixels(&fw, &fh);
    CHECK(fp != nullptr);
    CHECK(fw > 0 && fh > 0);
    CHECK(mq_imgui_font_generation() >= 1);
    CHECK(mq_imgui_font_texture() == 0);          /* 宿主还没上传 → 句柄为 0 */
    /* 图集里应有非零像素（否则字形没栅格化出来，面板会是空白） */
    int nonzero = 0;
    for (int i = 0; i < fw * fh; i += 7)
        if (fp[(size_t)i * 4 + 3] != 0) ++nonzero;
    CHECK(nonzero > 0);

    /* ② 一帧：应产出几何，且索引全在顶点范围内 */
    mq_imgui_frame f{};
    CHECK_EQ(run_frame(&f, 800.0F, 600.0F, 800.0F, 600.0F, 1.0F, 0.0F, 0.0F), 0);
    CHECK(f.cmd_count > 0);
    CHECK(f.cmds != nullptr);

    uint64_t total_idx = 0, total_vtx = 0;
    bool idx_ok = true, finite_ok = true;
    /* 索引是**全局**的（每条命令的局部索引已加了自己的 base）→ 断言上界用总顶点数。 */
    for (uint32_t ci = 0; ci < f.cmd_count; ++ci) {
        const mq_imgui_cmd &c = f.cmds[ci];
        CHECK(c.vertex_count > 0);
        CHECK(c.index_count > 0);
        total_idx += c.index_count;
        total_vtx += c.vertex_count;
        for (uint32_t i = 0; i < c.index_count; ++i)
            if (c.indices[i] >= total_vtx) idx_ok = false;
        for (uint32_t i = 0; i < c.vertex_count * 2; ++i)
            if (!std::isfinite(c.positions[i]) || !std::isfinite(c.uvs[i])) finite_ok = false;
        for (uint32_t i = 0; i < c.vertex_count * 4; ++i) {
            const float v = c.colors[i];
            if (!(v >= 0.0F && v <= 1.0F)) finite_ok = false;
        }
    }
    CHECK(idx_ok);
    CHECK(finite_ok);
    CHECK(total_idx > 0 && total_vtx > 0);
    /* ③ 坐标映射（用**缩放帧**验证）：显示 800x600、目标 400x300、scale=0.5 无偏移时，
     * 面板在显示空间 (10,10)-(230,130) 应映射到目标空间 (20,20)-(460,260)。 */
    mq_imgui_frame s{};
    CHECK_EQ(run_frame(&s, 800.0F, 600.0F, 400.0F, 300.0F, 0.5F, 0.0F, 0.0F), 0);
    float sx0 = 1e9F, sy0 = 1e9F, sx1 = -1e9F, sy1 = -1e9F;
    for (uint32_t ci = 0; ci < s.cmd_count; ++ci) {
        const mq_imgui_cmd &c = s.cmds[ci];
        for (uint32_t i = 0; i < c.vertex_count; ++i) {
            const float x = c.positions[i * 2], y = c.positions[i * 2 + 1];
            if (x < sx0) sx0 = x;
            if (y < sy0) sy0 = y;
            if (x > sx1) sx1 = x;
            if (y > sy1) sy1 = y;
        }
    }
    /* 允许几像素误差（ImGui 的窗口边框/内边距会略微外扩） */
    CHECK(std::fabs(sx0 - 20.0F) < 6.0F);
    CHECK(std::fabs(sy0 - 20.0F) < 6.0F);
    CHECK(sx1 > 400.0F && sy1 > 200.0F);

    /* 同理验证放大帧（scale=2, off=(10,20)）：显示 200x150 → 目标 (d-10)/2, (d-20)/2 */
    mq_imgui_frame z{};
    CHECK_EQ(run_frame(&z, 400.0F, 300.0F, 200.0F, 150.0F, 2.0F, 10.0F, 20.0F), 0);
    float zx0 = 1e9F, zy0 = 1e9F;
    for (uint32_t ci = 0; ci < z.cmd_count; ++ci) {
        const mq_imgui_cmd &c = z.cmds[ci];
        for (uint32_t i = 0; i < c.vertex_count; ++i) {
            if (c.positions[i * 2] < zx0) zx0 = c.positions[i * 2];
            if (c.positions[i * 2 + 1] < zy0) zy0 = c.positions[i * 2 + 1];
        }
    }
    CHECK(std::fabs(zx0 - 0.0F) < 6.0F);      /* (10-10)/2 = 0 */
    CHECK(std::fabs(zy0 + 5.0F) < 6.0F);      /* (10-20)/2 = -5 */

    /* ④ 空帧：没有控件 → 不应产出几何（避免空提交给渲染器） */
    mq_imgui_begin_frame(800.0F, 600.0F, 800.0F, 600.0F, 1.0F, 0, 0, 1.0F / 60.0F, 1.0F);
    mq_imgui_frame empty{};
    CHECK_EQ(mq_imgui_end_frame(&empty), 0);
    CHECK_EQ(empty.cmd_count, 0);

    /* ⑤ 非法参数应报错而不是崩 */
    CHECK(mq_imgui_begin_frame(0.0F, 600.0F, 800.0F, 600.0F, 1.0F, 0, 0, 0.016F, 1.0F) != 0);
    CHECK_EQ(mq_imgui_end_frame(nullptr), 1);

    /* ⑥ want_mouse / want_keyboard 在无输入时是安全的（返回 0/1 均可，但不能崩） */
    (void)mq_imgui_want_mouse();
    (void)mq_imgui_want_keyboard();
    (void)mq_imgui_hovered();
    mq_imgui_input_focus(1);

    mq_imgui_shutdown();
    CHECK_EQ(mq_imgui_ready(), 0);

    std::printf("imgui: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
