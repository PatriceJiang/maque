/* UI 语义与动作（docs/17）：layout/progress/safe area/scroll + tween/schedule/延迟销毁
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* UI 组件语义（role 驱动；引擎不认具体类型名，docs/17）：
 *   · ui_layout：H/V/GRID 自动布局（Layout）
 *   · ui_progress：按 value 调整填充子节点宽度（ProgressBar）
 *   · ui_safe_area：按内缩重排目标节点（SafeArea/Widget）
 *   · ui_scroll：内容偏移（ScrollView 的 scrollOffset） */
void Engine::ui_step() {
    if (!scene || !has_ui_components) return;          // 无 ui.* 组件：跳过全树遍历
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = blob.desc;
            if (!d) continue;

            if (const auto* lay = as_component<ui::Container>(d, blob.data())) {
                const float mode = (float)lay->mode;
                if (std::getenv("MAQUE_TRACE_UI")) std::print("[ui] layout entity={} mode={}\n", id, (int)mode);
                const float spacing = lay->spacing;
                const int columns = lay->columns;
                const float pad_l = lay->padding.x, pad_t = lay->padding.y;
                const std::vector<int> kids = e->children;   // 有序：id/声明序
                float cursor = 0.0F;
                int col = 0;
                for (int k : kids) {
                    SceneNode* ke = scene->at(k);
                    if (!ke) continue;
                    float kw = 32.0F, kh = 32.0F;
                    if (const auto* c = component<ui::Control>(*ke)) {
                        kw = c->offset_max.x; kh = c->offset_max.y;   // 内容尺寸
                    }
                    /* mode: 0=Free 1=Horizontal 2=Vertical 3=Grid */
                    /* 位置经 entity_set_pos 写回 Transform2D 组件（否则被 update_world 覆盖） */
                    if ((int)mode == 1) {
                        host::entity_set_pos(this, (float)k, e->world_x + pad_l + cursor + kw * 0.5F,
                                             e->world_y + pad_t + kh * 0.5F);
                        cursor += kw + spacing;
                    } else if ((int)mode == 2) {
                        host::entity_set_pos(this, (float)k, e->world_x + pad_l + kw * 0.5F,
                                             e->world_y + pad_t + cursor + kh * 0.5F);
                        cursor += kh + spacing;
                    } else if ((int)mode == 3) {
                        const int c = columns > 0 ? columns : 1;
                        const int row = col / c, column = col % c;
                        host::entity_set_pos(this, (float)k,
                                             e->world_x + pad_l + (float)column * (kw + spacing) + kw * 0.5F,
                                             e->world_y + pad_t + (float)row * (kh + spacing) + kh * 0.5F);
                        ++col;
                    }
                    (void)ke;
                }
            }

            if (auto* pb = as_component_mut<ui::ProgressBar>(d, blob.data())) {
                float v = pb->value;
                v = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
                pb->phase = v;
                /* 填充子节点：第一个带 sprite/control 的子节点，宽度 = 父宽 × value */
                for (int k : e->children) {
                    SceneNode* child = scene->at(k);
                    auto* c = child ? component<ui::Control>(*child) : nullptr;
                    if (!c) continue;
                    float full = host::node_width(this, (float)id);
                    if (full <= 0.0F) full = c->offset_max.x > 0.0F ? c->offset_max.x : 100.0F;
                    c->offset_max.x = full * v;
                    break;
                }
            }

            if (auto* sa = as_component_mut<ui::SafeArea>(d, blob.data())) {
                const float il = sa->inset_left, it = sa->inset_top;
                const float ir = sa->inset_right, ib = sa->inset_bottom;
                sa->resolved_inset = math::Vec4{ il, it, ir, ib };
                /* 目标（或首个 Control 子节点）按内缩重排 */
                const std::vector<int>& kids = e->children;
                const int target = kids.empty() ? -1 : kids.front();
                if (target >= 0) {
                    host::entity_set_pos(this, (float)target, e->world_x + il, e->world_y + it);
                    SceneNode* te = scene->at(target);
                    if (auto* c = te ? component<ui::Control>(*te) : nullptr) {
                        c->offset_max.x = (float)W - il - ir;
                        c->offset_max.y = (float)H - it - ib;
                    }
                }
            }

            /* ListView（大文本 P0）：Control 是视口；运行时生成一个 scn.Label 子节点承载
             * 全部行。滚轮在指针位于视口内时改 scroll_pos（归一化），内容按像素偏移移动；
             * 子树裁剪由 compute_ui_clip 对 ui.ListView.clip_content 统一处理（与 Mask 叠加）。 */
            if (auto* lv = as_component_mut<ui::ListView>(d, blob.data())) {
                const auto* ctl = component<ui::Control>(*e);
                if (!ctl) continue;
                const float vw = ctl->offset_max.x, vh = ctl->offset_max.y;
                if (!(vw > 0.0F) || !(vh > 0.0F)) continue;
                SceneNode* ce = lv->content.id > 0 ? scene->at((int)lv->content.id) : nullptr;
                if (!ce || ce->parent != id) {
                    /* 已保存/重载过的场景里，运行时内容子节点可能已经在树中（content 引用是
                     * transient 不落盘）；按稳定名字认领，避免每次载入再生成一份重复文本。 */
                    const std::string content_name = e->name + ".content";
                    for (int k : e->children) {
                        SceneNode* ke = scene->at(k);
                        if (ke && ke->name == content_name && component<scn::Label>(*ke)) {
                            ce = ke; lv->content.id = (uint32_t)k; lv->content.generation = 0; break;
                        }
                    }
                    if (!ce) {
                        const int cid = scene->spawn();
                        scene->attach(cid, id);
                        attach_component(*this, cid, "scn.Label");
                        ce = scene->at(cid);
                        if (ce) ce->name = content_name;
                        lv->content.id = (uint32_t)cid;
                        lv->content.generation = 0;
                    }
                }
                auto* lbl = ce ? component<scn::Label>(*ce) : nullptr;
                if (!lbl) continue;
                if (lbl->text != lv->text) lbl->text = lv->text;
                const float pad_l = lv->padding.x, pad_t = lv->padding.y;
                const float pad_r = lv->padding.z, pad_b = lv->padding.w;
                const float text_w = (vw - pad_l - pad_r > 1.0F) ? (vw - pad_l - pad_r) : vw;
                lbl->x = pad_l;
                lbl->y = 0.0F;
                lbl->z = 0;
                lbl->font = lv->font;
                lbl->font_size = lv->font_size;
                lbl->r = lv->text_color.x; lbl->g = lv->text_color.y;
                lbl->b = lv->text_color.z; lbl->a = lv->text_color.w;
                lbl->wrap = lv->wrap;
                lbl->line_spacing = lv->line_spacing;
                lbl->max_width = text_w;
                lbl->max_height = 0.0F;
                lbl->overflow = ui::TextOverflow::Clip;

                mq_text_style st{};
                st.struct_size = (uint32_t)sizeof st;
                st.font_size = lv->font_size;
                st.device_scale = ss > 0.0F ? ss : 1.0F;
                st.line_spacing = lv->line_spacing;
                st.wrap = (uint32_t)lv->wrap;
                st.align = MQ_TEXT_ALIGN_BEGIN;
                st.max_width = text_w;
                st.r = lv->text_color.x; st.g = lv->text_color.y;
                st.b = lv->text_color.z; st.a = lv->text_color.w;
                mq_text_metrics m{};
                mq_font* f = text_font_for(lv->font);
                if (!lv->text.empty()) (void)mq_text_measure(f, &st, lv->text.c_str(), &m);
                const float content_h = pad_t + m.height + pad_b;
                float max_off = content_h - vh;
                if (max_off < 0.0F) max_off = 0.0F;
                lv->content_height = content_h;
                if (max_off <= 0.0F) lv->scroll_pos = 0.0F;
                /* 滚轮：仅指针在视口内生效。scroll_pos = 0 为顶部；wheel_dy 已按平台边界翻转，
                 * 向下滚为正 → scroll_pos 增加。 */
                if (max_off > 0.0F) {
                    const bool inside = mouse_x >= e->world_x && mouse_x <= e->world_x + vw &&
                                        mouse_y >= e->world_y && mouse_y <= e->world_y + vh;
                    if (inside && wheel_dy != 0.0F)
                        lv->scroll_pos += wheel_dy * lv->wheel_step / max_off;
                }
                lv->scroll_pos = lv->scroll_pos < 0.0F ? 0.0F : (lv->scroll_pos > 1.0F ? 1.0F : lv->scroll_pos);
                lv->scroll_offset = lv->scroll_pos * max_off;
                /* 子节点局部坐标：视口顶 - padding.top + 滚动偏移（世界 y 向上，偏移增大=内容上移）。 */
                const float cy = vh - pad_t + lv->scroll_offset;
                if (auto* tf = component<scn::Transform2D>(*ce)) {
                    tf->x = 0.0F;
                    tf->y = cy;
                } else {
                    ce->local_x = 0.0F;
                    ce->local_y = cy;
                }
            }

            if (auto* sv = as_component_mut<ui::ScrollView>(d, blob.data())) {
                const float v = sv->scroll_v;
                sv->scroll_offset.y = v;
                const std::vector<int>& kids = e->children;
                if (!kids.empty()) {
                    const int k = kids.front();
                    SceneNode* ke = scene->at(k);
                    if (ke) host::entity_set_pos(this, (float)k, ke->world_x, e->world_y - v);
                }
            }
        }
    }
}


/* SystemsRuntime::Tween（tween().to()）：在固定步长内把字段从 from 插值到 to；
 * 缓动 0=linear 1=ease_in 2=ease_out 3=ease_in_out；完成派发 "tween_complete"。 */
void Engine::step_tweens(float dt) {
    if (systems.tweens.empty()) return;
    std::vector<size_t> done;
    for (size_t i = 0; i < systems.tweens.size(); ++i) {
        SystemsRuntime::Tween& tw = systems.tweens[i];
        tw.elapsed += dt;
        float t = tw.duration > 0.0F ? tw.elapsed / tw.duration : 1.0F;
        if (t > 1.0F) t = 1.0F;
        float e = t;
        switch (tw.easing) {
            case 1: e = t * t; break;                       // ease_in
            case 2: e = 1.0F - (1.0F - t) * (1.0F - t); break;   // ease_out
            case 3: e = t < 0.5F ? 2.0F * t * t : 1.0F - 2.0F * (1.0F - t) * (1.0F - t); break;
            default: break;
        }
        const float v = tw.from + (tw.to - tw.from) * e;
        const mq_type_desc* td = nullptr;
        const mq_field_desc* fd = nullptr;
        void* base = nullptr;
        if (resolve_field_path(*this, tw.entity, tw.path, &td, &fd, &base) && fd)
            write_num(td, base, fd->name, (double)v);
        if (t >= 1.0F) {
            host::node_emit_number(this, (float)tw.entity, "tween_complete", tw.to);
            done.push_back(i);
        }
    }
    for (auto it = done.rbegin(); it != done.rend(); ++it) systems.tweens.erase(systems.tweens.begin() + (long)*it);
}

/* 定时器（定时器）：按固定步长累计，到点调用脚本函数（按名字）。 */
void Engine::step_schedules(float dt) {
    if (systems.schedules.empty() || !bus) return;
    std::vector<size_t> drop;
    for (size_t i = 0; i < systems.schedules.size(); ++i) {
        SystemsRuntime::Scheduled& sc = systems.schedules[i];
        sc.remaining -= dt;
        if (sc.remaining > 0.0F) continue;
        mq_script_bus_call(bus, sc.handler.c_str(), (double)dt, 0.0, nullptr);
        if (sc.repeat) sc.remaining += sc.interval > 0.0F ? sc.interval : (1.0F / 60.0F);
        else drop.push_back(i);
    }
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) systems.schedules.erase(systems.schedules.begin() + (long)*it);
}

/* node.destroy()：延迟到帧边界统一销毁（销毁在当前帧末生效）。 */
void Engine::flush_pending_destroy() {
    if (systems.pending_destroy.empty()) return;
    if (std::getenv("MAQUE_TRACE_UI")) std::print("[ui] destroy {} node(s)\n", systems.pending_destroy.size());
    std::vector<int> ids = systems.pending_destroy;
    systems.pending_destroy.clear();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) if (scene->at(id)) scene->remove(id);
}

}  // namespace maque::rt
