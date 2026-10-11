// 分辨率策略单测（docs/17）：适配模式、对齐、限幅、安全区、退化输入、坐标双向映射。
#include "canvas/mq_canvas.h"

#include <print>
#include <cmath>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double _a = (a), _b = (b); if (std::fabs(_a - _b) > (eps)) { \
    std::print("FAIL {}:{}  {} ≈ {}（实际 {:.4f} vs {:.4f}）\n", __FILE__, __LINE__, #a, #b, _a, _b); ++g_failures; } } while (0)

static mq_canvas_desc desc(float dw, float dh, int fit, float ww, float wh) {
    mq_canvas_desc d{};
    d.struct_size = (uint32_t)sizeof d;
    d.design_w = dw; d.design_h = dh;
    d.fit = fit; d.align = MQ_CANVAS_ALIGN_CENTER;
    d.window_w = ww; d.window_h = wh;
    d.dpi_scale = 1.0F;
    return d;
}

int main() {
    mq_canvas_metrics m{};

    // ---- 1) ShowAll（等比完整显示 + 留边）：4:3 窗口显示 16:9 设计 → 左右留边 ----
    {
        mq_canvas_desc d = desc(480.0F, 270.0F, MQ_CANVAS_FIT_SHOW_ALL, 800.0F, 600.0F);
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 800.0F / 480.0F, 1e-4);        // 1.6667（宽度先满）
        CHECK_NEAR(m.viewport_w, 800.0F, 1e-3);
        CHECK_NEAR(m.viewport_h, 450.0F, 1e-3);            // 600 - 450 = 150 留边
        CHECK_NEAR(m.offset_x, 0.0F, 1e-3);
        CHECK_NEAR(m.offset_y, 75.0F, 1e-3);               // 居中
        // 对齐：顶部对齐 → offset_y = 0
        d.align = MQ_CANVAS_ALIGN_BEGIN;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.offset_y, 0.0F, 1e-3);
        // 底部对齐 → offset_y = 150
        d.align = MQ_CANVAS_ALIGN_END;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.offset_y, 150.0F, 1e-3);
    }

    // ---- 2) NoBorder（铺满 + 裁剪）：16:9 设计在 4:3 窗口 → 高度溢出（viewport 大于窗口）----
    {
        mq_canvas_desc d = desc(480.0F, 270.0F, MQ_CANVAS_FIT_NO_BORDER, 800.0F, 600.0F);
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 600.0F / 270.0F, 1e-4);        // 2.2222（高度先满）
        CHECK_NEAR(m.viewport_w, 1066.67F, 0.05F);
        CHECK(m.viewport_h >= 600.0F);
        CHECK_NEAR(m.offset_x, (800.0F - 1066.67F) * 0.5F, 0.05F);   // 负偏移 = 两侧裁剪
        CHECK(m.offset_x < 0.0F);
    }

    // ---- 3) FitWidth / FitHeight / Exact / None ----
    {
        mq_canvas_desc d = desc(320.0F, 240.0F, MQ_CANVAS_FIT_WIDTH, 640.0F, 200.0F);
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale_x, 2.0F, 1e-4); CHECK_NEAR(m.scale_y, 2.0F, 1e-4);
        CHECK_NEAR(m.viewport_h, 480.0F, 1e-3);            // 高度溢出（由调用方裁剪）
        d.fit = MQ_CANVAS_FIT_HEIGHT; d.window_h = 480.0F; d.window_w = 1000.0F;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 2.0F, 1e-4);
        CHECK_NEAR(m.viewport_w, 640.0F, 1e-3);
        d.fit = MQ_CANVAS_FIT_EXACT; d.window_w = 640.0F; d.window_h = 480.0F;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale_x, 2.0F, 1e-4); CHECK_NEAR(m.scale_y, 2.0F, 1e-4);
        d.window_w = 800.0F;                               // 非等比
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale_x, 2.5F, 1e-4); CHECK_NEAR(m.scale_y, 2.0F, 1e-4);
        CHECK_NEAR(m.scale, 2.0F, 1e-4);                   // 等比基准取 min（文字用）
        d.fit = MQ_CANVAS_FIT_NONE;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 1.0F, 1e-4);
    }

    // ---- 4) 限幅（极端 DPI 下不无限放大/缩小）----
    {
        mq_canvas_desc d = desc(320.0F, 240.0F, MQ_CANVAS_FIT_SHOW_ALL, 4000.0F, 3000.0F);
        d.max_scale = 4.0F;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 4.0F, 1e-4);
        CHECK_NEAR(m.viewport_w, 1280.0F, 1e-3);           // 限幅后居中留边
        d.min_scale = 0.5F; d.window_w = 100.0F; d.window_h = 100.0F; d.max_scale = 0.0F;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 0.5F, 1e-4);
    }

    // ---- 5) 安全区（刘海）：可用区 = 绘制区 - 安全区；偏移从安全区原点起算 ----
    {
        mq_canvas_desc d = desc(400.0F, 200.0F, MQ_CANVAS_FIT_SHOW_ALL, 800.0F, 400.0F);
        d.safe_left = 40.0F; d.safe_right = 40.0F; d.safe_top = 20.0F; d.safe_bottom = 20.0F;
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK_NEAR(m.scale, 1.8F, 1e-4);                   // min(720/400, 360/200) = 1.8
        CHECK_NEAR(m.offset_x, 40.0F, 1e-3);               // 宽度刚好填满可用区
        CHECK_NEAR(m.offset_y, 20.0F, 1e-3);
        CHECK_NEAR(m.viewport_w, 720.0F, 1e-3);
        CHECK_NEAR(m.viewport_h, 360.0F, 1e-3);
    }

    // ---- 6) 退化输入：设计/窗口 <=0 → 1:1 兜底 + 非 0 返回 ----
    {
        mq_canvas_desc d = desc(0.0F, 0.0F, MQ_CANVAS_FIT_SHOW_ALL, 800.0F, 600.0F);
        CHECK(mq_canvas_resolve(&d, &m) != 0);
        CHECK_NEAR(m.scale, 1.0F, 1e-6);
        d = desc(320.0F, 240.0F, MQ_CANVAS_FIT_SHOW_ALL, 0.0F, 0.0F);
        CHECK(mq_canvas_resolve(&d, &m) != 0);
        CHECK(mq_canvas_resolve(nullptr, &m) != 0);
        CHECK(mq_canvas_resolve(&d, nullptr) != 0);
        /* 安全区吃掉整个绘制区 */
        d = desc(320.0F, 240.0F, MQ_CANVAS_FIT_SHOW_ALL, 100.0F, 100.0F);
        d.safe_left = 60.0F; d.safe_right = 60.0F;
        CHECK(mq_canvas_resolve(&d, &m) != 0);
    }

    // ---- 7) 坐标双向映射（输入命中测试 / 反向布局）----
    {
        mq_canvas_desc d = desc(480.0F, 270.0F, MQ_CANVAS_FIT_SHOW_ALL, 800.0F, 600.0F);
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        float dx = 0, dy = 0, wx = 0, wy = 0;
        /* 窗口中心（400,300）→ 设计中心（240,135） */
        CHECK(mq_canvas_window_to_design(&m, 400.0F, 300.0F, &dx, &dy) == 0);
        CHECK_NEAR(dx, 240.0F, 1e-3); CHECK_NEAR(dy, 135.0F, 1e-3);
        /* 设计左上角 → 窗口 (0,75)（留边偏移） */
        CHECK(mq_canvas_design_to_window(&m, 0.0F, 0.0F, &wx, &wy) == 0);
        CHECK_NEAR(wx, 0.0F, 1e-3); CHECK_NEAR(wy, 75.0F, 1e-3);
        /* 往返一致（随机抽查若干点） */
        for (float t = 0.0F; t <= 1.0F; t += 0.25F) {
            const float ix = t * 480.0F, iy = t * 270.0F;
            CHECK(mq_canvas_design_to_window(&m, ix, iy, &wx, &wy) == 0);
            CHECK(mq_canvas_window_to_design(&m, wx, wy, &dx, &dy) == 0);
            CHECK_NEAR(dx, ix, 1e-2); CHECK_NEAR(dy, iy, 1e-2);
        }
        /* NoBorder 下窗口坐标可落在设计视口之外（负值/超界）→ 调用方据此裁剪输入 */
        d = desc(480.0F, 270.0F, MQ_CANVAS_FIT_NO_BORDER, 800.0F, 600.0F);
        CHECK(mq_canvas_resolve(&d, &m) == 0);
        CHECK(mq_canvas_window_to_design(&m, 0.0F, 0.0F, &dx, &dy) == 0);
        CHECK(dx > 0.0F);                                  // 左边缘在设计坐标内偏右（被裁掉的是两侧）
        CHECK(mq_canvas_window_to_design(nullptr, 0, 0, &dx, &dy) != 0);
    }

    if (g_failures == 0) { std::print("canvas_test: all checks passed\n"); return 0; }
    std::print("canvas_test: {} failure(s)\n", g_failures);
    return 1;
}
