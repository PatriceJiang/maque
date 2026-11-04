/* 宿主 API · UI 控件/tween/schedule（按 role 定位组件，不硬编码类型名）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

/* 写回 Label 组件的 text（按 role；字符串字段，docs/19 §8）。 */
void set_label(void* ctx, float id, const char* text) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    if (!e || !text) return;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_LABEL)) continue;
        if (auto* l = as_component_mut<scn::Label>(d, blob.data())) l->text = text;
        else if (auto* tl = as_component_mut<ui::TextLabel>(d, blob.data())) tl->text = text;
        else if (auto* rt = as_component_mut<render::RichText>(d, blob.data())) rt->text = text;
    }
}

/* ---- UI 控件值（按 role 统一读写）---- */
float ui_set_value(void* ctx, float id, float v) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    if (auto* b = comp_by_name(*eng, (int)id, "ProgressBar", &d)) {
        write_num(d, b->data(), "value", v < 0 ? 0 : (v > 1 ? 1 : v));
        return 1.0F;
    }
    if (auto* b = comp_by_name(*eng, (int)id, "Slider", &d)) {
        const float lo = read_num(d, b->data(), "min_value", 0.0F);
        const float hi = read_num(d, b->data(), "max_value", 1.0F);
        const float c = v < lo ? lo : (v > hi ? hi : v);
        write_num(d, b->data(), "value", c);
        write_num(d, b->data(), "ratio", hi > lo ? (c - lo) / (hi - lo) : 0.0F);
        node_emit_number(eng, id, "changed", c);
        return 1.0F;
    }
    return 0.0F;
}

float ui_get_value(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    if (auto* b = comp_by_name(*eng, (int)id, "ProgressBar", &d)) return read_num(d, b->data(), "value", 0.0F);
    if (auto* b = comp_by_name(*eng, (int)id, "Slider", &d)) return read_num(d, b->data(), "value", 0.0F);
    return 0.0F;
}

float ui_set_checked(void* ctx, float id, float on) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*eng, (int)id, "CheckBox", &d);
    if (!b) return 0.0F;
    write_num(d, b->data(), "checked", on != 0.0F ? 1.0 : 0.0);
    node_emit_number(eng, id, "toggled", on != 0.0F ? 1.0F : 0.0F);
    return 1.0F;
}

float ui_get_checked(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*eng, (int)id, "CheckBox", &d);
    if (!b) return 0.0F;
    return read_num(d, b->data(), "checked", 0.0F);
}

/* ---- 文本（Label/TextLabel/RichText 通用：按 role 定位组件）---- */
std::vector<uint8_t>* text_comp(Engine& eng, int id, const mq_type_desc** out_d) {
    std::vector<uint8_t>* blob = comp_by_role(eng, id, MQ_ROLE_UI_LABEL);
    if (!blob) blob = comp_by_role(eng, id, MQ_ROLE_UI_TEXT);
    if (blob && out_d) {
        SceneNode* e = eng.scene->at(id);
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (d && (d->role & (MQ_ROLE_UI_LABEL | MQ_ROLE_UI_TEXT)) && &b == blob) { *out_d = d; break; }
        }
    }
    return blob;
}

float ui_set_text(void* ctx, float id, const char* text) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d || !text) return 0.0F;
    if (auto* l = as_component_mut<scn::Label>(d, blob->data())) { l->text = text; return 1.0F; }
    if (auto* tl = as_component_mut<ui::TextLabel>(d, blob->data())) { tl->text = text; return 1.0F; }
    if (auto* rt = as_component_mut<render::RichText>(d, blob->data())) { rt->text = text; return 1.0F; }
    return 0.0F;
}

const char* ui_get_text(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return "";
    if (const auto* l = as_component<scn::Label>(d, blob->data())) return l->text.c_str();
    if (const auto* tl = as_component<ui::TextLabel>(d, blob->data())) return tl->text.c_str();
    if (const auto* rt = as_component<render::RichText>(d, blob->data())) return rt->text.c_str();
    return "";
}

/* 实测排版尺寸：兼容 text_w/text_h（scn.Label）、text_size（ui.TextLabel）、content_size（RichText） */
float ui_text_width(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return 0.0F;
    if (const auto* l = as_component<scn::Label>(d, blob->data())) return l->text_w;
    if (const auto* tl = as_component<ui::TextLabel>(d, blob->data())) return tl->text_size.x;
    if (const auto* rt = as_component<render::RichText>(d, blob->data())) return rt->content_size.x;
    return 0.0F;
}

float ui_text_height(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return 0.0F;
    if (const auto* l = as_component<scn::Label>(d, blob->data())) return l->text_h;
    if (const auto* tl = as_component<ui::TextLabel>(d, blob->data())) return tl->text_size.y;
    if (const auto* rt = as_component<render::RichText>(d, blob->data())) return rt->content_size.y;
    return 0.0F;
}

float ui_text_lines(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return 0.0F;
    if (const auto* l = as_component<scn::Label>(d, blob->data())) return (float)l->line_count;
    if (const auto* rt = as_component<render::RichText>(d, blob->data())) return (float)rt->line_count;
    return 0.0F;
}

/* ---- Tween ---- */
float tween_start(void* ctx, float id, const char* field, float to, float duration, float easing) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene->at((int)id) || !field) return 0.0F;
    const mq_type_desc* td = nullptr;
    const mq_field_desc* fd = nullptr;
    void* base = nullptr;
    if (!resolve_field_path(*eng, (int)id, field, &td, &fd, &base) || !fd) return 0.0F;
    const float from = read_num(td, base, fd->name, 0.0F);
    eng->tweens.push_back({ (int)id, field, from, to, 0.0F, duration > 0.0F ? duration : (1.0F / 60.0F),
                            (int)easing });
    return 1.0F;
}

float tween_stop(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const size_t before = eng->tweens.size();
    eng->tweens.erase(std::remove_if(eng->tweens.begin(), eng->tweens.end(),
                                     [&](const Engine::Tween& t) { return t.entity == (int)id; }),
                      eng->tweens.end());
    return (float)(before - eng->tweens.size());
}

float tween_count(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    int n = 0;
    for (const auto& t : eng->tweens) if (t.entity == (int)id) ++n;
    return (float)n;
}

float tween_progress(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    for (const auto& t : eng->tweens)
        if (t.entity == (int)id) return t.duration > 0.0F ? (t.elapsed / t.duration) : 1.0F;
    return 1.0F;
}

/* ---- schedule / scheduleOnce ---- */
float schedule_once(void* ctx, const char* handler, float delay) {
    auto* eng = (Engine*)ctx;
    if (!handler || !*handler) return 0.0F;
    eng->schedules.push_back({ handler, 0.0F, delay > 0.0F ? delay : 0.0F, false });
    return 1.0F;
}

float schedule(void* ctx, const char* handler, float interval) {
    auto* eng = (Engine*)ctx;
    if (!handler || !*handler || interval <= 0.0F) return 0.0F;
    eng->schedules.push_back({ handler, interval, interval, true });
    return 1.0F;
}

float unschedule(void* ctx, const char* handler) {
    auto* eng = (Engine*)ctx;
    if (!handler) return 0.0F;
    const std::string h = handler;
    const size_t before = eng->schedules.size();
    eng->schedules.erase(std::remove_if(eng->schedules.begin(), eng->schedules.end(),
                                        [&](const Engine::Scheduled& s) { return s.handler == h; }),
                         eng->schedules.end());
    return (float)(before - eng->schedules.size());
}

float schedule_count(void* ctx) { return (float)((Engine*)ctx)->schedules.size(); }

float ui_hit_at(void* ctx, float x, float y) {
    auto* eng = (Engine*)ctx;
    return (float)ui_hit_entity(*eng, x, y, nullptr);
}

/* UI 命中查询（复用与控制面同一套命中测试 → 行为一致） */
float ui_hit(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng->input) return -1.0F;
    mq_input_event probe{};
    probe.type = MQ_INPUT_POINTER_MOVE;
    probe.x = eng->mouse_x; probe.y = eng->mouse_y;
    mq_input_target t{};
    t.entity = -1;
    runtime_hit_test(eng, &probe, &t);
    return (float)t.entity;
}

float ui_hover(void* ctx) { return (float)((Engine*)ctx)->hover; }

float ui_pressed(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
        if (const mq_field_desc* fp = mq_reflect_field(d, "pressed"))
            return field_get_bool(fp, blob.data()) ? 1.0F : 0.0F;
    }
    return 0.0F;
}

}  // namespace host
