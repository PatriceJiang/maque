/* 宿主 API · 系统/画布/输入/相机（window/canvas/screen/input/camera/load_texture/env…）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

float window_w(void* ctx) { return (float)((Engine*)ctx)->canvas.window_w; }

float window_h(void* ctx) { return (float)((Engine*)ctx)->canvas.window_h; }

float canvas_scale(void* ctx) { return ((Engine*)ctx)->canvas.scale; }

float canvas_scale_x(void* ctx) { return ((Engine*)ctx)->canvas.scale_x; }

float canvas_scale_y(void* ctx) { return ((Engine*)ctx)->canvas.scale_y; }

float canvas_offset_x(void* ctx) { return ((Engine*)ctx)->canvas.offset_x; }

float canvas_offset_y(void* ctx) { return ((Engine*)ctx)->canvas.offset_y; }

float canvas_device_scale(void* ctx) { return ((Engine*)ctx)->canvas.device_scale; }

float canvas_safe_area(void* ctx, float edge) {
    auto* eng = (Engine*)ctx;
    const int e = (int)edge;
    if (e < 0 || e > 3) return 0.0F;
    return eng->canvas_safe[e];
}

float screen_w(void* ctx) { return (float)((Engine*)ctx)->W; }

float screen_h(void* ctx) { return (float)((Engine*)ctx)->H; }

float rng(void* ctx) { return (float)((Engine*)ctx)->next_rng() / 4294967296.0f; }

void  quit(void* ctx) {
    auto* eng = (Engine*)ctx;
    /* MAQUE_KEEP_OPEN=1：演示/人工查看时忽略脚本的 quit()（关闭窗口仍退出） */
    if (keep_open) { std::print("runtime: script requested quit (ignored: MAQUE_KEEP_OPEN)\n"); return; }
    eng->quit = true;
}

void  clear(void* ctx, float r, float g, float b) {
    auto* e = (Engine*)ctx; e->bg[0] = r; e->bg[1] = g; e->bg[2] = b;
}

void  log(void*, const char* msg) { std::print("[game] {}\n", msg ? msg : ""); }

float cfg(void* ctx, const char* path) { return (float)cfg_field((Engine*)ctx, path); }

float load_texture(void* ctx, const char* path) {
    auto* e = (Engine*)ctx;
    std::string p = path ? path : "";
    static constexpr const char* kRt = "rendertexture://";
    if (p.rfind(kRt, 0) == 0) {
        const std::string tail = p.substr(std::strlen(kRt));
        if (tail.find('/') == std::string::npos) {
            const int owner = e->active_instance ? e->active_instance->id : 0;
            p = std::string(kRt) + std::to_string(owner) + "/" + tail;
        }
        return (float)e->load_texture(p);
    }
    if (p.empty() || p[0] != '/') p.insert(p.begin(), '/');
    return (float)e->load_texture(p);
}

const char* camera_texture(void* ctx, float camera) {
    auto* e = (Engine*)ctx;
    const int id = (int)camera;
    if (const Engine::RenderTextureState* rt = e->render_texture_for(id)) {
        e->script_ret = rt->uri;
        return e->script_ret.c_str();
    }
    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*e, id, "render.RenderTexture", &d);
    SceneNode* ent = e->scene ? e->scene->at(id) : nullptr;
    if (!b || !d || !ent) { e->script_ret.clear(); return e->script_ret.c_str(); }
    const std::string* configured = read_str(d, b->data(), "name");
    const std::string name = configured && !configured->empty() ? *configured
                                                                 : (!ent->name.empty() ? ent->name : "camera-" + std::to_string(id));
    const int owner = e->entity_instance.count(id) ? e->entity_instance[id] : 0;
    e->script_ret = "rendertexture://" + std::to_string(owner) + "/" + name;
    return e->script_ret.c_str();
}

float camera_update(void* ctx, float camera) {
    auto* e = (Engine*)ctx;
    const int id = (int)camera;
    if (Engine::RenderTextureState* rt = e->render_texture_for(id)) { rt->dirty = true; return 1.0F; }
    return comp_by_name(*e, id, "render.RenderTexture", nullptr) ? 1.0F : 0.0F;
}

float camera_capture(void* ctx, float camera, const char* path) {
    auto* e = (Engine*)ctx;
    const int id = (int)camera;
    if (!path || !*path || !comp_by_name(*e, id, "render.RenderTexture", nullptr)) return 0.0F;
    if (Engine::RenderTextureState* rt = e->render_texture_for(id)) rt->dirty = true;
    e->camera_captures.push_back({ id, path });
    return 1.0F;
}

/* 运行期配置：读宿主环境变量（不存在 → ""）。返回 std::string 的 c_str，绑定侧立即拷贝。 */
const char* env(void*, const char* name) {
    static std::string scratch;
    scratch.clear();
    if (name && *name) {
        if (const char* v = std::getenv(name)) scratch = v;
    }
    return scratch.c_str();
}

/* 输入状态（事件发生时；docs/19 §9：不读设备，回放一致） */
float input_x(void* ctx) { return ((Engine*)ctx)->mouse_x; }

float input_y(void* ctx) { return ((Engine*)ctx)->mouse_y; }

float input_target(void* ctx) {
    auto* e = (Engine*)ctx;
    return e->ev ? (float)e->ev->target : -1.0F;
}

float input_down(void* ctx) {
    auto* e = (Engine*)ctx;
    return e->input && mq_input_pointer_down(e->input, 0) ? 1.0F : 0.0F;
}

float input_key(void* ctx, float code) {
    auto* e = (Engine*)ctx;
    return e->input && mq_input_key_down(e->input, (uint32_t)code) ? 1.0F : 0.0F;
}

float input_focus(void* ctx) { return (float)mq_input_focus(((Engine*)ctx)->input); }

void  input_set_focus(void* ctx, float id) { mq_input_set_focus(((Engine*)ctx)->input, (int)id); }

}  // namespace host
