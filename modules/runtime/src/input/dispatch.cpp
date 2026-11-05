/* 输入分发（docs/19 §4）：命中测试（z-order + 树深稳定排序）→ 语义事件 → 脚本回调
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- UI 命中测试（docs/19 §4）：z-order + 树深 稳定排序，命中最上层 ---- */
struct HitBest { int entity = -1; int z = INT32_MIN; int depth = -1; float lx = 0, ly = 0; };

void collect_hits(Engine& eng, int idx, int depth, float px, float py, HitBest& best) {
    SceneNode* e = eng.scene->at(idx);
    if (!e) return;
    /* 有效 z = 自身 z + 祖先 z（层级叠加，稳定） */
    int zsum = 0;
    /* ui.Control 提供盒模型（anchor/offset/size），交互性由 role 决定 */
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = blob.desc;
        if (!d) continue;
        if (d->role & MQ_ROLE_UI_CONTROL) {
            const auto* ctrl = as_component<ui::Control>(d, blob.data());
            if (!ctrl) continue;
            const float w = ctrl->offset_max.x - ctrl->offset_min.x;
            const float h = ctrl->offset_max.y - ctrl->offset_min.y;
            zsum += ctrl->z_index;
            bool interactive = false;
            for (auto& [t2, b2] : e->comps) {
                const mq_type_desc* d2 = b2.desc;
                if (d2 && (d2->role & (MQ_ROLE_UI_BUTTON | MQ_ROLE_UI_SLIDER | MQ_ROLE_UI_CHECK)))
                    interactive = true;
            }
            if (interactive && w > 0 && h > 0) {
                /* 节点位置 = 控件中心（与精灵居中绘制一致）；输入已是世界坐标 → 直接比较 */
                const float cx = e->world_x, cy = e->world_y;
                const float bx = cx - w * 0.5F, by = cy - h * 0.5F;
                if (px >= bx && px <= bx + w && py >= by && py <= by + h) {
                    if (zsum > best.z || (zsum == best.z && depth > best.depth)) {
                        best.entity = idx; best.z = zsum; best.depth = depth;
                        best.lx = px - bx; best.ly = py - by;
                    }
                }
            } else if (e->active) {
                zsum += 0;                                            // 非交互：仅参与 z 叠加
            }
            continue;
        }
        bool ui = (d->role & (MQ_ROLE_UI_BUTTON | MQ_ROLE_UI_LABEL)) != 0;
        if (!ui) continue;
        float x = 0, y = 0, w = 0, h = 0;
        int32_t z = 0;
        if (const auto* btn = as_component<scn::Button>(d, blob.data())) {
            x = btn->x; y = btn->y; w = btn->width; h = btn->height; z = btn->z;
        } else if (const auto* lbl = as_component<scn::Label>(d, blob.data())) {
            x = lbl->x; y = lbl->y; z = lbl->z;
        } else {
            continue;
        }
        zsum += z;
        if (!(d->role & MQ_ROLE_UI_BUTTON)) continue;        // 仅可交互控件参与命中
        /* 本地 (x,y) = 世界坐标下的矩形左下角 → 矩形 = [x, x+w] × [y, y+h]（y 向上） */
        const float bw = w > 0 ? w : 64, bh = h > 0 ? h : 24;
        const float bx = e->world_x + x, by = e->world_y + y;
        if (px >= bx && px <= bx + bw && py >= by && py <= by + bh) {
            const int score = zsum;
            /* 稳定：z 大者优先；同 z 取树深大者（后绘制者在上） */
            if (score > best.z || (score == best.z && depth > best.depth)) {
                best.entity = idx; best.z = score; best.depth = depth;
                best.lx = px - bx; best.ly = py - by;
            }
        }
    }
    for (int k : e->children) collect_hits(eng, k, depth + 1, px, py, best);
}

/* 命中查询（事件与控制面/脚本共用一套）→ 行为一致（docs/19 §4）。 */
int ui_hit_entity(Engine& eng, float px, float py, mq_input_target* out) {
    if (out) { out->entity = -1; out->handle = (uint32_t)-1; out->depth = 0; out->z = 0; }
    if (!eng.scene || !eng.scene->count()) return -1;
    HitBest best;
    for (int k : eng.scene->roots) collect_hits(eng, k, 0, px, py, best);
    if (best.entity < 0) return -1;
    if (out) {
        out->entity = best.entity; out->handle = (uint32_t)best.entity;
        out->depth = best.depth; out->z = best.z;
        out->local_x = best.lx; out->local_y = best.ly;
    }
    return best.entity;
}

int runtime_hit_test(void* user, const mq_input_event* ev, mq_input_target* out) {
    /* 输入事件坐标在进入引擎前已统一为**世界坐标**（设备事件在平台边界映射、
     * 注入/模拟事件按世界坐标给出）→ 这里直接按世界矩形命中（不再二次映射）。 */
    auto* eng = (Engine*)user;
    out->entity = -1; out->handle = (uint32_t)-1;
    if (!eng->scene || !eng->scene->count()) return 0;
    return ui_hit_entity(*eng, ev->x, ev->y, out) >= 0 ? 1 : 0;
}

int runtime_parent(void* user, int entity) {
    SceneNode* e = ((Engine*)user)->scene->at(entity);
    return e ? e->parent : -1;
}

/* 事件 → 脚本回调：映射与参数个数全部由 DSL 声明、codegen 生成（docs/14 §4、docs/19 §8）。
 * 事件 kind → 处理器名（生成的事件表是单一真相）；docs/32 §4.3 的**作用域路由**：
 *   · 生命周期/帧回调：按实例树顺序（on_start 后序、update 前序、on_destroy 后序）；
 *   · 指针/滚轮：root → … → 命中实体的所属实例（祖先链），最后 overlay；未命中/键盘：root + 所有实例；
 *   · overlay 恒在最后（老场景"主脚本之后"的语义不变）。 */
int dispatch_event(Engine& eng, int kind, double a, double b, double c, const char* text) {
    uint32_t n = 0;
    const mq_event_desc* tab = mq_event_table(&n);
    if (!tab || kind < 0 || (uint32_t)kind >= n) return 0;
    const char* name = tab[kind].name;
    MQ_TRACY_ZONE_NAMED(__tz_evt, script, name);
    int hits = 0;

    if (kind == MQ_EVENT_ON_START) {
        hits += eng.dispatch_instance_on_start(0);
        hits += eng.call_global_event(name, a, b, c, text);
        hits += eng.call_overlay_event(name, a, b, c, text);
        return hits;
    }
    if (kind == MQ_EVENT_ON_DESTROY) {
        hits += eng.dispatch_instance_on_destroy(0);
        hits += eng.call_global_event(name, a, b, c, text);
        hits += eng.call_overlay_event(name, a, b, c, text);
        return hits;
    }
    if (kind == MQ_EVENT_UPDATE) {
        std::vector<int> order;
        eng.collect_instance_preorder(0, order, true);       // 前序；paused/active=false 整棵子树跳过
        for (int id : order)
            if (SceneInstanceRuntime* inst = eng.instance_at(id))
                hits += eng.call_instance_event(*inst, name, a, b, c, text);
        hits += eng.call_global_event(name, a, b, c, text);   // 全局服务先于覆盖层（先算状态，再画 HUD）
        hits += eng.call_overlay_event(name, a, b, c, text);
        return hits;
    }

    /* 输入事件：指针类按 target 的实例祖先链做作用域路由；键盘/文本给全部 active 实例。 */
    const bool pointerish = kind >= MQ_EVENT_ON_POINTER_DOWN && kind <= MQ_EVENT_ON_CLICK;
    std::vector<int> order;
    if (pointerish) {
        const int target = (int)a;
        SceneInstanceRuntime* owner = target >= 0 ? eng.instance_of_entity(target) : nullptr;
        std::vector<int> chain;
        for (SceneInstanceRuntime* cur = owner; cur; cur = eng.instance_at(cur->parent))
            chain.push_back(cur->id);
        std::reverse(chain.begin(), chain.end());            // root → … → owner
        if (chain.empty() && eng.instance_at(0)) chain.push_back(0);
        order = chain;
    } else {
        eng.collect_instance_preorder(0, order, false);      // 键盘/文本：全部 active（root 在内）
    }
    for (int id : order) {
        SceneInstanceRuntime* inst = eng.instance_at(id);
        if (!inst || inst->state != 1 || !inst->active) continue;
        hits += eng.call_instance_event(*inst, name, a, b, c, text);
    }
    hits += eng.call_global_event(name, a, b, c, text);
    hits += eng.call_overlay_event(name, a, b, c, text);
    return hits;
}


void call_script_event(Engine& eng, const mq_input_event* ev) {
    const int kind = mq_event_for_input(ev->type);
    if (kind < 0) return;
    const char* text = ev->type == MQ_INPUT_TEXT ? ev->text : nullptr;
    /* 载荷按事件类型选择（生成的回调签名见 callbacks 段）：
     *   指针：on_pointer_*(target, x, y)
     *   **按键：on_key_down/up(target, key)** —— key 走第 2 个参数
     *   文本：on_text(target, text)
     * 此前按键误把指针 x 当作 key 传入（key 恒为 0）→ 键盘驱动的游戏无法工作。 */
    const bool keyish = ev->type == MQ_INPUT_KEY_DOWN || ev->type == MQ_INPUT_KEY_UP;
    dispatch_event(eng, kind, (double)ev->target,
                      keyish ? (double)ev->key : (double)ev->x,
                      keyish ? 0.0 : (double)ev->y, text);
}

/* 窗口像素 → **世界坐标**：等比缩放 + 居中留边（SDL_RenderSetLogicalSize 同规则）+ **y 轴翻转**。
 * 窗口/SDL 事件是 y 向下；引擎世界与脚本是 y 向上（docs/12 §坐标、docs/19 §4）——
 * 漏掉翻转会导致点击位置上下镜像（曾经的真实缺陷）。 */
void window_to_world(const Engine& eng, float* x, float* y) {
    /* 窗口设备像素 → 设计坐标（由画布策略决定缩放/留边；多视图时除以视图数落到单视图） */
    float dx = 0.0F, dy = 0.0F;
    if (mq_canvas_window_to_design(&eng.canvas, *x, *y, &dx, &dy) != 0) return;
    const int views = eng.nviews > 0 ? eng.nviews : 1;
    if (views > 1 && eng.W > 0) {
        const int view = std::clamp((int)std::floor(dx / (float)eng.W), 0, views - 1);
        dx -= (float)(view * eng.W);
    }
    *x = dx;
    *y = (float)eng.H - dy;                         // 设计 y 向下 → 世界 y 向上
}

/* 设备事件：位置映射 + 方向量翻转（指针位移 dy 与滚轮 scroll_y 同属 y 轴方向） */
void device_event_to_world(const Engine& eng, mq_platform_pointer_event* p) {
    float sx = 1.0F, sy = 1.0F;
    if (eng.canvas_use_dpi && eng.platform && eng.window) {
        int32_t lw = 0, lh = 0, dw = 0, dh = 0;
        mq_platform_window_size(eng.platform, &lw, &lh);
        mq_platform_drawable_size(eng.platform, &dw, &dh);
        if (lw > 0 && dw > 0) sx = (float)dw / (float)lw;
        if (lh > 0 && dh > 0) sy = (float)dh / (float)lh;
    }
    p->x *= sx;
    p->y *= sy;
    window_to_world(eng, &p->x, &p->y);
    if (eng.canvas.scale_x > 0.0F) p->dx = p->dx * sx / eng.canvas.scale_x;
    if (eng.canvas.scale_y > 0.0F) p->dy = -p->dy * sy / eng.canvas.scale_y;
    else p->dy = -p->dy;
    p->scroll_y = -p->scroll_y;
}

int runtime_input_handler(void* user, const mq_input_event* ev, mq_input_target* target) {
    auto* eng = (Engine*)user;
    /* 引擎内部一律**世界坐标**：设备事件在平台边界转换（见 step_frame），
     * 注入/模拟事件（mq_input_inject）按世界坐标给出 —— 命中测试、组件语义、脚本回调一致。 */
    eng->ev = ev;
    /* 修饰键状态（shader 图 keys.y = Shift；事件带的是事件发生后的修饰键位）。 */
    if (ev->type == MQ_INPUT_KEY_DOWN || ev->type == MQ_INPUT_KEY_UP)
        eng->shift_down = (ev->modifiers & MQ_PLATFORM_MOD_SHIFT) != 0;
    if (ev->type == MQ_INPUT_POINTER_MOVE || ev->type == MQ_INPUT_POINTER_DOWN ||
        ev->type == MQ_INPUT_POINTER_UP || ev->type == MQ_INPUT_CLICK) {
        eng->mouse_x = ev->x; eng->mouse_y = ev->y;
    }
    if (ev->type == MQ_INPUT_POINTER_MOVE && target->entity != eng->hover) {
        eng->hover = target->entity;                    // 悬停态（脚本经 ui_hover 读）
    }
    if (target->entity >= 0) {
        /* 控件状态写回（按钮按下/悬停；docs/19 §8：组件订阅语义事件） */
        SceneNode* e = eng->scene->at(target->entity);
        if (e && ev->type == MQ_INPUT_POINTER_DOWN)
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = blob.desc;
                if (auto* btn = (d && (d->role & MQ_ROLE_UI_BUTTON))
                                    ? as_component_mut<scn::Button>(d, blob.data()) : nullptr)
                    btn->pressed = true;
            }
        if (e && ev->type == MQ_INPUT_POINTER_UP)
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = blob.desc;
                if (auto* btn = (d && (d->role & MQ_ROLE_UI_BUTTON))
                                    ? as_component_mut<scn::Button>(d, blob.data()) : nullptr)
                    btn->pressed = false;
            }
        /* Slider：按下进入拖拽，拖动中按水平比例改值（滑块语义） */
        if (e) {
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = blob.desc;
                auto* sl = (d && (d->role & MQ_ROLE_UI_SLIDER))
                               ? as_component_mut<ui::Slider>(d, blob.data()) : nullptr;
                if (!sl) continue;
                if (ev->type == MQ_INPUT_POINTER_DOWN) {
                    sl->dragging = true;
                    slider_apply_pointer(*eng, target->entity, d, blob.data(), ev->x);
                } else if (ev->type == MQ_INPUT_POINTER_UP) {
                    sl->dragging = false;
                    slider_apply_pointer(*eng, target->entity, d, blob.data(), ev->x);
                } else if (ev->type == MQ_INPUT_POINTER_MOVE) {
                    if (sl->dragging)
                        slider_apply_pointer(*eng, target->entity, d, blob.data(), ev->x);
                }
            }
            /* CheckBox：点击切换 + toggled 事件（开关语义） */
            if (ev->type == MQ_INPUT_CLICK) {
                for (auto& [tid, blob] : e->comps) {
                    const mq_type_desc* d = blob.desc;
                    auto* cb = (d && (d->role & MQ_ROLE_UI_CHECK))
                                   ? as_component_mut<ui::CheckBox>(d, blob.data()) : nullptr;
                    if (!cb || cb->disabled) continue;
                    const bool now = !cb->checked;
                    cb->checked = now;
                    host::node_emit_number(eng, target->entity, "toggled", now ? 1.0F : 0.0F);
                }
            }
        }
    }
    call_script_event(*eng, ev);          // 分发表由生成代码提供
    if (eng->trace_hits)
        std::print("[input] type={} phase={} target={} pos=({:.1f},{:.1f})\n",
                   (int)ev->type, (int)ev->phase, target->entity, ev->x, ev->y);
    return 0;   // 不消费：脚本/组件各自决定
}

/* cfg：反射读 scn.Scene 标量字段。 */
double cfg_field(Engine* eng, const char* path) {
    const mq_type_desc* t = eng->scene_type;
    void* obj = &eng->scene->desc;
    const char* seg = path;
    const mq_field_desc* f = nullptr; void* parent = obj;
    while (*seg) {
        const char* dot = seg; while (*dot && *dot != '.') ++dot;
        std::string name(seg, (size_t)(dot - seg));
        f = mq_reflect_field(t, name.c_str());
        if (!f) return 0.0;
        if (*dot == '\0') { parent = obj; break; }
        if (f->kind != MQ_TK_STRUCT) return 0.0;
        obj = (uint8_t*)obj + f->offset;
        t = mq_reflect_type_by_id(eng->reflect, f->type_id);
        if (!t) return 0.0;
        seg = dot + 1;
    }
    double v = 0.0; mq_reflect_get_f64(eng->reflect, parent, f, &v); return v;
}

std::vector<uint8_t>* comp_by_role(Engine& eng, int id, uint32_t role) {
    SceneNode* e = eng.scene->at(id);
    if (!e) return nullptr;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = blob.desc;
        if (d && (d->role & role)) return &blob.bytes;
    }
    return nullptr;
}

/* 按类型名找组件（getComponent("ns.Type") 语义）。
 * **只接受全名**（ns.Type）：旧实现会猜 scn./ui./physics. 前缀、再扫全表做后缀匹配 ——
 * 既贵（最坏 254 次字符串构造）又有歧义（scn.Button 与 ui.Button 并存）。
 * 未知/短名按"没有该组件"处理（脚本侧安全），短名提示一次。 */
std::vector<uint8_t>* comp_by_name(Engine& eng, int id, const char* type_name,
                                   const mq_type_desc** out_desc) {
    SceneNode* e = eng.scene->at(id);
    if (!e || !type_name) return nullptr;
    const mq_type_desc* want = mq_reflect_type_by_name(eng.reflect, type_name);
    if (!want) {
        static bool warned = false;                  /* 诊断去重（同仓库其它 warned_* 的先例） */
        if (!warned && type_name[0] && !std::strchr(type_name, '.')) {
            warned = true;
            std::print(stderr, "runtime: 组件类型 '{}' 必须是全名（ns.Type，如 scn.Sprite）\n", type_name);
        }
        return nullptr;
    }
    auto it = e->comps.find(want->type_id);
    if (it == e->comps.end()) return nullptr;
    if (out_desc) *out_desc = want;
    return &it->second.bytes;
}

}  // namespace maque::rt
