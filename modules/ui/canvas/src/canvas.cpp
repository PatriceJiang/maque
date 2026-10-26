/* 分辨率策略：把"设计分辨率"适配到任意绘制区（设备像素）。
 *
 * 设计要点（docs/17「画布与分辨率策略」）：
 *   · **纯函数**：同样的输入 → 同样的输出，不依赖窗口系统/帧号（可单测、可确定性回放）；
 *   · 留边（ShowAll）与裁剪（NoBorder）由同一套公式表达：viewport = design × scale，
 *     offset 由 align 决定；NoBorder 时 viewport 大于绘制区（由调用方裁剪）；
 *   · 安全区（刘海/圆角）以绘制区内缩方式参与可用区计算：可用区 = 绘制区 - 安全区；
 *   · min/max_scale 用于限制极端 DPI 下的缩放（<=0 表示不限）；
 *   · 非法/退化输入（设计分辨率 <=0、绘制区 <=0）回退 1:1 并返回非 0，调用方可据此记录告警。 */
#include "canvas/mq_canvas.h"
#include "core/tracy/mq_tracy.h"

#include <algorithm>
#include <cmath>

namespace {

float usable_w(const mq_canvas_desc& d) {
    const float w = d.window_w - (d.safe_left + d.safe_right);
    return w > 0.0F ? w : 0.0F;
}
float usable_h(const mq_canvas_desc& d) {
    const float h = d.window_h - (d.safe_top + d.safe_bottom);
    return h > 0.0F ? h : 0.0F;
}

float clamp_scale(float s, float lo, float hi) {
    if (lo > 0.0F && s < lo) s = lo;
    if (hi > 0.0F && s > hi) s = hi;
    return s;
}

void fallback(const mq_canvas_desc& d, mq_canvas_metrics* m) {
    m->scale = 1.0F; m->scale_x = 1.0F; m->scale_y = 1.0F;
    m->offset_x = 0.0F; m->offset_y = 0.0F;
    m->viewport_w = d.design_w > 0.0F ? d.design_w : 0.0F;
    m->viewport_h = d.design_h > 0.0F ? d.design_h : 0.0F;
    m->window_w = d.window_w > 0.0F ? d.window_w : 0.0F;
    m->window_h = d.window_h > 0.0F ? d.window_h : 0.0F;
    m->device_scale = d.dpi_scale > 0.0F ? d.dpi_scale : 1.0F;
}

} // namespace

extern "C" int mq_canvas_resolve(const mq_canvas_desc* desc, mq_canvas_metrics* out) {
    MQ_TRACY_ZONE_N(ui, "resolve");
    if (!desc || !out) return 1;
    *out = mq_canvas_metrics{};
    const mq_canvas_desc& d = *desc;
    if (d.design_w <= 0.0F || d.design_h <= 0.0F || d.window_w <= 0.0F || d.window_h <= 0.0F) {
        fallback(d, out);
        return 2;                                        /* 退化输入：1:1 兜底 */
    }

    const float aw = usable_w(d), ah = usable_h(d);
    if (aw <= 0.0F || ah <= 0.0F) { fallback(d, out); return 3; }

    float sx = 1.0F, sy = 1.0F;
    switch (d.fit) {
        case MQ_CANVAS_FIT_NONE:
            sx = sy = 1.0F; break;
        case MQ_CANVAS_FIT_WIDTH:
            sx = sy = aw / d.design_w; break;
        case MQ_CANVAS_FIT_HEIGHT:
            sx = sy = ah / d.design_h; break;
        case MQ_CANVAS_FIT_NO_BORDER:
            sx = sy = std::max(aw / d.design_w, ah / d.design_h); break;
        case MQ_CANVAS_FIT_EXACT:
            sx = aw / d.design_w; sy = ah / d.design_h; break;
        case MQ_CANVAS_FIT_SHOW_ALL:
        default:
            sx = sy = std::min(aw / d.design_w, ah / d.design_h); break;
    }
    /* 限幅：等比策略统一限幅；Exact 分别限幅（保持各轴意图） */
    if (d.fit == MQ_CANVAS_FIT_EXACT) {
        sx = clamp_scale(sx, d.min_scale, d.max_scale);
        sy = clamp_scale(sy, d.min_scale, d.max_scale);
    } else {
        const float s = clamp_scale(sx, d.min_scale, d.max_scale);
        sx = sy = s;
    }
    if (!(sx > 0.0F) || !(sy > 0.0F)) { fallback(d, out); return 4; }

    const float vw = d.design_w * sx, vh = d.design_h * sy;
    const float free_x = aw - vw, free_y = ah - vh;      /* NoBorder 时为负（溢出 → 裁剪） */
    float ox = d.safe_left, oy = d.safe_top;
    if (d.align == MQ_CANVAS_ALIGN_CENTER) { ox += free_x * 0.5F; oy += free_y * 0.5F; }
    else if (d.align == MQ_CANVAS_ALIGN_END) { ox += free_x; oy += free_y; }

    out->scale = std::min(sx, sy);
    out->scale_x = sx; out->scale_y = sy;
    out->offset_x = ox; out->offset_y = oy;
    out->viewport_w = vw; out->viewport_h = vh;
    out->window_w = d.window_w; out->window_h = d.window_h;
    out->device_scale = d.dpi_scale > 0.0F ? d.dpi_scale : 1.0F;
    return 0;
}

extern "C" int mq_canvas_window_to_design(const mq_canvas_metrics* m, float wx, float wy,
                                          float* out_dx, float* out_dy) {
    if (!m || !out_dx || !out_dy) return 1;
    if (!(m->scale_x > 0.0F) || !(m->scale_y > 0.0F)) return 2;
    *out_dx = (wx - m->offset_x) / m->scale_x;
    *out_dy = (wy - m->offset_y) / m->scale_y;
    return 0;
}

extern "C" int mq_canvas_design_to_window(const mq_canvas_metrics* m, float dx, float dy,
                                          float* out_wx, float* out_wy) {
    if (!m || !out_wx || !out_wy) return 1;
    if (!(m->scale_x > 0.0F) || !(m->scale_y > 0.0F)) return 2;
    *out_wx = dx * m->scale_x + m->offset_x;
    *out_wy = dy * m->scale_y + m->offset_y;
    return 0;
}
