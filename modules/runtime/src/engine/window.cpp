/* 窗口与渲染目标（docs/05/17）：render_scale 解析、目标重建、主视图映射、resize 钩子
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 窗口 resize / 最大化：屏幕、渲染目标与设计坐标分层 --------------------
 *   · Scene.width/height：世界与 UI 的参考坐标，不分配颜色附件；
 *   · drawable：屏幕设备像素，默认就是主颜色附件尺寸；
 *   · render_scale：相对 drawable 的独立输出倍率，只有 !=1 时主目标才与屏幕不同；
 *   · Canvas：把设计坐标映射到目标内的 viewport/scissor，不参与目标分辨率选择。
 * 无窗口的截图/嵌入宿主没有 drawable，以设计尺寸（多视图横向并排）作为虚拟屏幕。 */
void Engine::resolve_render_scale() {
    render_scale_req = scene && scene->desc.render_scale > 0.0F ? scene->desc.render_scale : 1.0F;
    /* 旧场景兼容：Canvas.render_scale 曾是唯一入口；显式 >0 时继续生效。 */
    if (scene && canvas_entity >= 0) {
        if (SceneNode* ce = scene->at(canvas_entity)) {
            for (auto& [tid, blob] : ce->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
                float legacy = 0.0F;
                read_f32(d, blob.data(), "render_scale", &legacy);
                if (legacy > 0.0F) render_scale_req = legacy;
                break;
            }
        }
    }
    if (const char* e = std::getenv("MAQUE_RENDER_SCALE")) {
        const float value = (float)std::atof(e);
        if (value > 0.0F) render_scale_req = value;
    }
    render_scale_req = std::clamp(render_scale_req, 0.25F, 4.0F);
#ifdef MQ_RENDERER_STATIC_NULL
    window_intent = false;
#else
    const char* present_env = std::getenv("MAQUE_PRESENT");
    window_intent = !std::getenv("MAQUE_HEADLESS") && !std::getenv("MAQUE_SHOT") &&
                    !(present_env && (std::strcmp(present_env, "off") == 0 || std::strcmp(present_env, "0") == 0));
#endif
}

void Engine::rebuild_render_target() {
    if (!scene) return;
    int32_t screen_w = std::max(1, W * std::max(1, nviews));
    int32_t screen_h = std::max(1, H);
    if (window_intent) {
        int32_t dw = 0, dh = 0;
        if (platform && window) mq_platform_drawable_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) {
            screen_w = dw;
            screen_h = dh;
        } else if (win_w > 0 && win_h > 0) {
            /* 建窗前只有逻辑尺寸可用；平台创建后会用真实 drawable 再重建一次。 */
            screen_w = win_w;
            screen_h = win_h;
        }
    }
    TW = std::max(1, (int)std::lround((float)screen_w * render_scale_req));
    TH = std::max(1, (int)std::lround((float)screen_h * render_scale_req));
    /* Canvas 尚未解析时给文本/装配期一个保守倍率；帧首 canvas_resolve 会写入精确值。 */
    const float per_view_w = (float)TW / (float)std::max(1, nviews);
    ss = std::min(per_view_w / (float)std::max(1, W), (float)TH / (float)std::max(1, H));
    if (!(ss > 0.0F)) ss = 1.0F;
    views.assign((size_t)std::max(1, nviews), mq_renderer_view{});
}

void Engine::update_main_views() {
    if (!scene || TW <= 0 || TH <= 0) return;
    const int count = std::max(1, nviews);
    if ((int)views.size() != count) views.assign((size_t)count, mq_renderer_view{});
    const float ww = canvas.window_w > 0.0F ? canvas.window_w : (float)TW / render_scale_req;
    const float wh = canvas.window_h > 0.0F ? canvas.window_h : (float)TH / render_scale_req;
    const float rx = (float)TW / std::max(ww, 1.0F);
    const float ry = (float)TH / std::max(wh, 1.0F);
    const float whole_x = canvas.offset_x * rx;
    const float whole_y = canvas.offset_y * ry;
    const float whole_w = (canvas.viewport_w > 0.0F ? canvas.viewport_w : ww) * rx;
    const float whole_h = (canvas.viewport_h > 0.0F ? canvas.viewport_h : wh) * ry;
    const float view_w = whole_w / (float)count;
    const float design_zoom_x = view_w / (float)std::max(1, W);
    const float design_zoom_y = whole_h / (float)std::max(1, H);
    ss = std::min(design_zoom_x, design_zoom_y);
    if (!(ss > 0.0F)) ss = 1.0F;

    for (int i = 0; i < count; ++i) {
        mq_renderer_view& v = views[(size_t)i];
        v = mq_renderer_view{};
        v.vx = whole_x + view_w * (float)i;
        v.vy = whole_y;
        v.vw = view_w;
        v.vh = whole_h;
        v.pan = 0.0F;
        v.eye = (i - (count - 1) * 0.5F) * scene->desc.eye_sep;
        v.cx = W * 0.5F;
        v.cy = H * 0.5F;
        v.zoom = design_zoom_x;
        v.zoom_y = design_zoom_y;
    }
    for (auto& [idx, e] : scene->nodes) {
        if (comp_by_name(*this, idx, "render.RenderTexture", nullptr)) continue;
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_CAMERA)) continue;
            bool active = true;
            if (const mq_field_desc* fa = mq_reflect_field(d, "active")) active = field_get_bool(fa, blob.data());
            if (!active) continue;
            float cx = W * 0.5F, cy = H * 0.5F, zoom = 1.0F, rot = 0.0F;
            read_f32(d, blob.data(), "x", &cx);
            read_f32(d, blob.data(), "y", &cy);
            read_f32(d, blob.data(), "zoom", &zoom);
            read_f32(d, blob.data(), "rotation", &rot);
            for (auto& v : views) {
                v.cx = cx;
                v.cy = cy;
                const float camera_zoom = std::max(zoom, 1.0e-5F);
                v.zoom *= camera_zoom;
                v.zoom_y = (v.zoom_y > 0.0F ? v.zoom_y : v.zoom) * camera_zoom;
                v.rot = rot;
            }
        }
    }
}

/* 窗口事件给逻辑尺寸；实际目标始终从 drawable 设备像素重算。 */
void Engine::sync_window_size(int32_t log_w, int32_t log_h) {
    if (!platform || !window) return;
    if (log_w <= 0 || log_h <= 0) return;
    int32_t surface_w = 0, surface_h = 0;
    mq_platform_drawable_size(platform, &surface_w, &surface_h);
    if (surface_w <= 0 || surface_h <= 0) return;
    if (win_last_w <= 0) {
        win_last_w = log_w; win_last_h = log_h;
        drawable_last_w = surface_w; drawable_last_h = surface_h;
        return;
    }
    const bool logical_changed = log_w != win_last_w || log_h != win_last_h;
    const bool drawable_changed = surface_w != drawable_last_w || surface_h != drawable_last_h;
    if (!logical_changed && !drawable_changed) return;
    const int32_t old_w = win_last_w, old_h = win_last_h;
    const int old_tw = TW, old_th = TH;
    win_last_w = log_w;
    win_last_h = log_h;
    drawable_last_w = surface_w;
    drawable_last_h = surface_h;
    win_w = log_w;
    win_h = log_h;
    rebuild_render_target();

    if (direct_present && rapi && rself && rapi->resize_surface) {
        const int32_t rc = rapi->resize_surface(rself, surface_w, surface_h);
        if (rc != 0) std::print(stderr, "runtime: resize_surface({}x{}) 失败 rc={}\n",
                                surface_w, surface_h, rc);
    }
    if (TW != old_tw || TH != old_th) {
        frame_pixels.clear();
        frame_w = frame_h = 0;
    }
    std::print("runtime: window resized {}x{} → {}x{}: render target {}x{} → {}x{} "
               "(render_scale {:.3f}, views={})\n",
               old_w, old_h, log_w, log_h, old_tw, old_th, TW, TH, render_scale_req, nviews);
}

/* CI/演示钩子：MAQUE_WINDOW_EVENT_AT="<frame>:<op>[;...]" —— 在指定模拟帧对**真实窗口**
 * 下发操作（op = WxH 设尺寸 / max 最大化 / restore 还原），走真实 SDL 窗口事件路径
 * （SDL_SetWindowSize / SDL_MaximizeWindow → SIZE_CHANGED → 平台队列 → sync_window_size），
 * 与用户拖拽标题栏/点最大化按钮**同一条链路**。 */
void Engine::window_event_hooks() {
    if (!platform || !window) return;
    const char* spec = std::getenv("MAQUE_WINDOW_EVENT_AT");
    if (!spec || !*spec) return;
    std::string s(spec), item;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] != ';') { item.push_back(s[i]); continue; }
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const int at = std::atoi(item.substr(0, colon).c_str());
            const std::string op = item.substr(colon + 1);
            if ((int)frame == at) {
                if (op == "max") {
                    const int rc = mq_platform_set_window_maximized(platform, 1);
                    std::print("runtime: window event frame={} maximize (rc={})\n", (long long)frame, rc);
                } else if (op == "restore") {
                    const int rc = mq_platform_set_window_maximized(platform, 0);
                    std::print("runtime: window event frame={} restore (rc={})\n", (long long)frame, rc);
                } else {
                    int w = 0, h = 0;
                    if (std::sscanf(op.c_str(), "%dx%d", &w, &h) == 2) {
                        const int rc = mq_platform_set_window_size(platform, w, h);
                        std::print("runtime: window event frame={} resize {}x{} (rc={})\n", (long long)frame, w, h, rc);
                    } else {
                        std::print(stderr, "runtime: MAQUE_WINDOW_EVENT_AT 帧 {} 操作 '{}' 无法识别\n", at, op);
                    }
                }
            }
        }
        item.clear();
    }
}

}  // namespace maque::rt
