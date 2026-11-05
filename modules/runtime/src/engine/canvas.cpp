/* 画布与分辨率策略（docs/17）：设计坐标 → 屏幕设备像素
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 画布与分辨率策略（docs/17）----
 * 设计分辨率只定义世界/UI 参考坐标；Canvas 解析到屏幕设备像素，再由 render_scale 映射进
 * 实际颜色附件。没有 ui.Canvas 实体时按 ShowAll + 居中兜底。 */
void Engine::canvas_setup() {
    canvas_entity = -1;
    canvas_design_warned = false;
    if (!scene) return;
    for (auto& [idx, e] : scene->nodes) {
        auto* cv = component<ui::Canvas>(e);
        if (!cv) continue;
        canvas_entity = idx;                       /* 升序首个画布 */
        canvas_active = true;
        std::print("runtime: canvas on entity {} (design {}x{})\n", idx,
                   cv->design_width, cv->design_height);
        break;
    }
}

void Engine::canvas_resolve() {
    mq_canvas_desc desc{};
    desc.struct_size = (uint32_t)sizeof desc;
    /* 每个 view 使用一份设计宽度；多视图在屏幕中横向并排。 */
    const int view_count = std::max(1, nviews);
    desc.design_w = (float)W * (float)view_count;
    desc.design_h = (float)H;
    desc.fit = MQ_CANVAS_FIT_SHOW_ALL;
    desc.align = MQ_CANVAS_ALIGN_CENTER;
    desc.dpi_scale = 1.0F;
    canvas_use_dpi = true;
    desc.safe_left = canvas_safe[0]; desc.safe_top = canvas_safe[1];
    desc.safe_right = canvas_safe[2]; desc.safe_bottom = canvas_safe[3];
    if (canvas_entity >= 0) {
        SceneNode* e = scene->at(canvas_entity);
        const auto* cv = e ? component<ui::Canvas>(*e) : nullptr;
        if (cv) {
            const float dw = cv->design_width;
            const float dh = cv->design_height;
            if (dw > 0.0F && dh > 0.0F &&
                (std::fabs(dw - (float)W) > 1.0e-4F || std::fabs(dh - (float)H) > 1.0e-4F) &&
                !canvas_design_warned) {
                canvas_design_warned = true;
                std::print(stderr, "runtime: ui.Canvas.design {}x{} 与 Scene 设计坐标 {}x{} 不一致，"
                                   "已忽略 Canvas.design_*（Scene.width/height 是唯一坐标基准）\n",
                           dw, dh, W, H);
            }
            desc.fit = (int32_t)cv->fit_mode;
            desc.align = (int32_t)cv->align;
            desc.min_scale = cv->min_scale;
            desc.max_scale = cv->max_scale;
            const bool dpi = cv->dpi_scale;
            canvas_use_dpi = dpi;
            desc.dpi_scale = (dpi && platform && window) ? mq_platform_content_scale(platform) : 1.0F;
        }
    } else if (platform && window) {
        desc.dpi_scale = mq_platform_content_scale(platform);
    }
    /* Canvas 绘制区按 dpi_scale 选择 drawable 设备像素或逻辑窗口像素；无窗口用虚拟设计屏幕。 */
    desc.window_w = (float)W * (float)view_count;
    desc.window_h = (float)H;
    if (platform && window) {
        int32_t dw = 0, dh = 0;
        if (canvas_use_dpi) mq_platform_drawable_size(platform, &dw, &dh);
        else mq_platform_window_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) { desc.window_w = (float)dw; desc.window_h = (float)dh; }
    }
    const int rc = mq_canvas_resolve(&desc, &canvas);
    if (rc != 0 && std::getenv("MAQUE_TRACE_CANVAS"))
        std::print(stderr, "runtime: canvas resolve fallback (rc={})\n", rc);
    update_main_views();
    /* 回写 transient 字段 */
    if (canvas_entity >= 0) {
        SceneNode* e = scene->at(canvas_entity);
        if (auto* cv = e ? component<ui::Canvas>(*e) : nullptr) {
            cv->scale = canvas.scale;
            cv->scale_x = canvas.scale_x;
            cv->scale_y = canvas.scale_y;
            cv->offset_x = canvas.offset_x;
            cv->offset_y = canvas.offset_y;
            cv->viewport_w = canvas.viewport_w;
            cv->viewport_h = canvas.viewport_h;
            cv->window_w = canvas.window_w;
            cv->window_h = canvas.window_h;
            cv->device_scale = canvas.device_scale;
        }
    }
    if (std::getenv("MAQUE_TRACE_CANVAS"))
        std::print("[canvas] design={}x{} win={}x{} fit={} scale=({:.3f},{:.3f}) off=({:.1f},{:.1f}) dpi={:.2f}\n",
                   desc.design_w, desc.design_h, desc.window_w, desc.window_h, desc.fit,
                   canvas.scale_x, canvas.scale_y, canvas.offset_x, canvas.offset_y, canvas.device_scale);
}

}  // namespace maque::rt
