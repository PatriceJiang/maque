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

            /* ListView：Control 是视口；运行时创建 content_root，把直接子节点（Button/Sprite/
             * Label/Image 等）收拢到其下，滚动只移动 content_root。滚轮/拖拽在指针位于视口内时
             * 改 scroll_pos（归一化）；子树裁剪由 compute_ui_clip 对 clip_content 统一处理。 */
            if (auto* lv = as_component_mut<ui::ListView>(d, blob.data())) {
                const auto* ctl = component<ui::Control>(*e);
                if (!ctl) continue;
                const float vw = ctl->offset_max.x, vh = ctl->offset_max.y;
                if (!(vw > 0.0F) || !(vh > 0.0F)) continue;
                /* 内容根：所有直接子节点（按钮/精灵/标签/手写内容）都挂到它下面；
                 * 只移动内容根，子节点局部坐标保持作者声明值。这样按钮/精灵/9-patch 等
                 * 任意子树都能随 ListView 一起滚动并继承视口裁剪。 */
                const std::string root_name = e->name + ".content_root";
                SceneNode* root = lv->content_root.id > 0 ? scene->at((int)lv->content_root.id) : nullptr;
                if (!root || root->parent != id) {
                    for (int k : e->children) {
                        SceneNode* ke = scene->at(k);
                        if (ke && ke->name == root_name) { root = ke; break; }
                    }
                    if (!root) {
                        const int rid = scene->spawn();
                        scene->attach(rid, id);
                        root = scene->at(rid);
                        if (root) root->name = root_name;
                    }
                }
                if (!root) continue;
                /* 找 root 的稳定 id：SceneNode 不存 id，从父 children 反查 */
                int root_id = -1;
                for (int k : e->children) if (scene->at(k) == root) { root_id = k; break; }
                if (root_id < 0) continue;
                lv->content_root.id = (uint32_t)root_id;
                lv->content_root.generation = 0;
                /* 百分比 label 是 ListView 自身子节点，不参与内容滚动（不能收进 content_root）。 */
                SceneNode* status = lv->scroll_label.id > 0 ? scene->at((int)lv->scroll_label.id) : nullptr;
                int status_id = -1;
                if (status && status->parent == id) {
                    for (int k : e->children) if (scene->at(k) == status) { status_id = k; break; }
                }
                if (status_id < 0) {
                    const std::string status_name = e->name + ".scroll_label";
                    for (int k : e->children) {
                        SceneNode* ke = scene->at(k);
                        if (ke && ke->name == status_name && component<scn::Label>(*ke)) {
                            status = ke; status_id = k; lv->scroll_label.id = (uint32_t)k;
                            break;
                        }
                    }
                }
                /* 把 ListView 的其余直接子节点（包括旧版直接挂的 Label）移到内容根下。 */
                const std::vector<int> kids = e->children;
                for (int k : kids) if (k != root_id && k != status_id) scene->attach(k, root_id);

                const float pad_l = lv->padding.x, pad_t = lv->padding.y;
                const float pad_r = lv->padding.z, pad_b = lv->padding.w;
                const float text_w = (vw - pad_l - pad_r > 1.0F) ? (vw - pad_l - pad_r) : vw;
                SceneNode* ce = nullptr;
                scn::Label* lbl = nullptr;
                if (!lv->text.empty()) {
                    if (lv->content.id > 0) ce = scene->at((int)lv->content.id);
                    if (!ce || ce->parent != root_id) {
                        const std::string content_name = e->name + ".content";
                        for (int k : root->children) {
                            SceneNode* ke = scene->at(k);
                            if (ke && ke->name == content_name && component<scn::Label>(*ke)) {
                                ce = ke; break;
                            }
                        }
                        if (!ce) {
                            const int cid = scene->spawn();
                            scene->attach(cid, root_id);
                            attach_component(*this, cid, "scn.Label");
                            ce = scene->at(cid);
                            if (ce) ce->name = content_name;
                        }
                    }
                    if (ce) {
                        int cid = -1;
                        for (int k : root->children) if (scene->at(k) == ce) { cid = k; break; }
                        if (cid < 0) ce = nullptr;
                        else { lv->content.id = (uint32_t)cid; lv->content.generation = 0; }
                    }
                    lbl = ce ? component<scn::Label>(*ce) : nullptr;
                }
                if (lbl) {
                    if (lbl->text != lv->text) lbl->text = lv->text;
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
                }

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
                /* 内容高度：文本度量优先；纯子节点模式用 item_count/item_extent 定义虚拟长度，
                 * 这样 Button/Sprite 列表不需要依赖文本排版。 */
                float inner_h = m.height;
                if (lv->item_count > 0 && lv->item_extent > 0.0F) {
                    const float item_h = (float)lv->item_count * (lv->item_extent + lv->spacing);
                    if (item_h > inner_h) inner_h = item_h;
                } else if (lv->text.empty()) {
                    /* 没有 item_count 时，按直接子节点的粗略范围兜底（负 y = 向下排列）。 */
                    float max_down = 0.0F;
                    for (int k : root->children) {
                        SceneNode* ke = scene->at(k);
                        if (!ke || ke == ce) continue;
                        float top = ke->local_y, h = 0.0F;
                        if (const auto* b = component<scn::Button>(*ke)) { h = b->height; top += b->y + h; }
                        else if (const auto* sp = component<scn::Sprite>(*ke)) { h = sp->height; top += h * 0.5F; }
                        else if (const auto* c = component<ui::Control>(*ke)) { h = c->offset_max.y; top += h; }
                        else if (const auto* lb = component<scn::Label>(*ke)) {
                            h = lb->text_h > 0.0F ? lb->text_h : lb->font_size;
                            top += h;
                        }
                        if (h <= 0.0F) continue;
                        const float down = -(top - h);
                        if (down > max_down) max_down = down;
                    }
                    if (max_down > inner_h) inner_h = max_down;
                }
                const float content_h = pad_t + inner_h + pad_b;
                float max_off = content_h - vh;
                if (max_off < 0.0F) max_off = 0.0F;
                lv->content_height = content_h;
                if (max_off <= 0.0F) { lv->scroll_pos = 0.0F; lv->scroll_velocity = 0.0F; }
                const bool inside = mouse_x >= e->world_x && mouse_x <= e->world_x + vw &&
                                    mouse_y >= e->world_y && mouse_y <= e->world_y + vh;
                constexpr float kDt = 1.0F / 60.0F;
                ListBarGeometry bar = list_scrollbar_geometry(*lv, e->world_x, e->world_y, vw, vh);
                /* 滚轮：只在指针位于视口内生效；滚轮后不额外加速，保持可预测。 */
                if (max_off > 0.0F && inside && wheel_dy != 0.0F) {
                    lv->scroll_pos += wheel_dy * lv->wheel_step / max_off;
                    lv->scroll_velocity = 0.0F;
                }
                const bool down = input && mq_input_pointer_down(input, 0) != 0;
                const bool in_bar = bar.visible && mouse_x >= bar.x0 && mouse_x <= bar.x1 &&
                                    mouse_y >= bar.y0 && mouse_y <= bar.y1;
                if (!down) {
                    const bool was_drag = lv->dragging || lv->scrollbar_dragging;
                    lv->dragging = false;
                    lv->scrollbar_dragging = false;
                    if (was_drag && !lv->inertia) lv->scroll_velocity = 0.0F;
                } else if (lv->scrollbar_dragging) {
                    /* thumb 往下拖（世界 y 减小）= 看后面的内容 → scroll_pos 增加。 */
                    const float denom = std::max(1.0F, bar.view_h - bar.thumb_h);
                    lv->scroll_pos = lv->drag_start_scroll - (mouse_y - lv->drag_start_y) / denom;
                    lv->scroll_velocity = -(mouse_y - lv->drag_last_y) / kDt / denom;
                } else if (lv->dragging) {
                    lv->scroll_pos = lv->drag_start_scroll + (mouse_y - lv->drag_start_y) / std::max(1.0F, max_off);
                    lv->scroll_velocity = (mouse_y - lv->drag_last_y) / kDt / std::max(1.0F, max_off);
                } else if (inside && max_off > 0.0F) {
                    if (in_bar) {
                        /* 滚动条可触摸：按在 thumb 上直接拖；按在轨道上先跳转再拖。 */
                        lv->scrollbar_dragging = true;
                        lv->dragging = false;
                        lv->drag_last_y = mouse_y;
                        lv->scroll_velocity = 0.0F;
                        if (mouse_y >= bar.thumb_y0 && mouse_y <= bar.thumb_y1) {
                            lv->drag_start_y = mouse_y;
                            lv->drag_start_scroll = lv->scroll_pos;
                        } else {
                            const float denom = std::max(1.0F, bar.view_h - bar.thumb_h);
                            lv->scroll_pos = (bar.y1 - mouse_y - bar.thumb_h * 0.5F) / denom;
                            lv->drag_start_y = mouse_y;
                            lv->drag_start_scroll = lv->scroll_pos;
                        }
                    } else {
                        lv->dragging = true;
                        lv->scrollbar_dragging = false;
                        lv->drag_start_y = mouse_y;
                        lv->drag_start_scroll = lv->scroll_pos;
                        lv->drag_last_y = mouse_y;
                        lv->scroll_velocity = 0.0F;
                    }
                }
                if (down && (lv->dragging || lv->scrollbar_dragging))
                    lv->drag_last_y = mouse_y;                 // 速度按**相邻两步**计算，不累积
                /* 惯性：拖拽释放后按速度继续滑动，指数摩擦；固定步长 → 可回放。 */
                if (!down && lv->inertia && max_off > 0.0F && std::abs(lv->scroll_velocity) > 0.0005F) {
                    lv->scroll_pos += lv->scroll_velocity * kDt;
                    lv->scroll_velocity *= std::exp(-std::max(0.0F, lv->friction) * kDt);
                    if (std::abs(lv->scroll_velocity) < 0.0005F) lv->scroll_velocity = 0.0F;
                }
                lv->scroll_pos = lv->scroll_pos < 0.0F ? 0.0F : (lv->scroll_pos > 1.0F ? 1.0F : lv->scroll_pos);
                lv->scroll_offset = lv->scroll_pos * max_off;
                lv->scroll_percent = (float)std::lround(lv->scroll_pos * 100.0F);

                /* 百分比指示：ListView 自身子节点（不滚），显示在右上、滚动条左侧。 */
                if (!status || status->parent != id) {
                    const std::string status_name = e->name + ".scroll_label";
                    for (int k : e->children) {
                        SceneNode* ke = scene->at(k);
                        if (ke && ke->name == status_name && component<scn::Label>(*ke)) {
                            status = ke; lv->scroll_label.id = (uint32_t)k; break;
                        }
                    }
                    if (!status) {
                        const int sid = scene->spawn();
                        scene->attach(sid, id);
                        attach_component(*this, sid, "scn.Label");
                        status = scene->at(sid);
                        if (status) status->name = status_name;
                        lv->scroll_label.id = (uint32_t)sid;
                    }
                }
                if (status) {
                    int sid = -1;
                    for (int k : e->children) if (scene->at(k) == status) { sid = k; break; }
                    auto* sl = sid >= 0 ? component<scn::Label>(*status) : nullptr;
                    if (sl) {
                        lv->scroll_label.id = (uint32_t)sid;
                        const bool show = lv->show_scroll_percent && bar.visible;
                        const std::string ptxt = show
                            ? (std::to_string((int)lv->scroll_percent) + "%") : std::string();
                        if (sl->text != ptxt) sl->text = ptxt;
                        sl->x = std::max(0.0F, vw - 78.0F);
                        sl->y = 20.0F;                       /* 视口底部靠右，文字向上留 10px */
                        sl->z = 900;
                        sl->font = lv->font;
                        sl->font_size = 10.0F;
                        sl->r = lv->scrollbar_color.x; sl->g = lv->scrollbar_color.y;
                        sl->b = lv->scrollbar_color.z; sl->a = 0.95F;
                        sl->wrap = ui::TextWrap::None;
                        sl->max_width = 0.0F;
                    }
                }
                /* 内容根局部坐标：内容顶部 = 视口顶 - padding.top + 滚动偏移。
                 * 子节点局部坐标保持作者声明（向下排列用负 y），随内容根整体移动。 */
                const float cy = vh - pad_t + lv->scroll_offset;
                if (auto* tf = component<scn::Transform2D>(*root)) {
                    tf->x = 0.0F;
                    tf->y = cy;
                } else {
                    root->local_x = 0.0F;
                    root->local_y = cy;
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
