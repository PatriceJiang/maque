/* 宿主 API · 节点与组件（node_* / entity_*；树语义规范面 + 扁平别名，docs/18）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

float entity_spawn(void* ctx) {
    auto* eng = (Engine*)ctx;
    const int id = eng->scene->spawn();
    if (id >= 0) eng->entity_instance[id] = eng->active_instance ? eng->active_instance->id : 0;
    return (float)id;
}

void  entity_set_pos(void* ctx, float id, float x, float y) {
    auto* e = (Engine*)ctx;
    if (SceneNode* ent = e->scene->at((int)id)) {
        /* 无 Transform2D 组件（纯 UI/空节点）：位置写在节点的局部兜底字段上（父空间）。
         * world_x/y 同步赋值只为"本帧立即读回"；下一次 update_world 会按父世界重算。 */
        bool has_tf = false;
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            if (d && (d->role & MQ_ROLE_TRANSFORM2D)) { has_tf = true; break; }
        }
        if (!has_tf) { ent->local_x = x; ent->local_y = y; ent->world_x = x; ent->world_y = y; return; }
        // 写回 Transform2D 组件（直接访问生成的 struct；兼容三种声明形态）
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            void* p = blob.data();
            if (auto* t = as_component_mut<scn::Transform2D>(d, p)) { t->x = x; t->y = y; continue; }
            if (auto* t = as_component_mut<scene::Transform2D>(d, p)) {
                t->local.position = math::Vec2{ x, y }; continue;
            }
            if (auto* t = as_component_mut<math::Transform2D>(d, p)) {
                t->position = math::Vec2{ x, y }; continue;
            }
        }
        ent->world_x = x; ent->world_y = y;
    }
}

float entity_x(void* ctx, float id) { SceneNode* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->world_x : 0.0f; }

float entity_y(void* ctx, float id) { SceneNode* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->world_y : 0.0f; }

float entity_count(void* ctx) { return (float)((Engine*)ctx)->scene->count(); }

void  entity_destroy(void* ctx, float id) { ((Engine*)ctx)->scene->remove((int)id); }

/* 实体查询（脚本按名字找节点，docs/12/19） */
const char* entity_name(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->name.c_str() : "";
}

float entity_find(void* ctx, const char* name) {
    if (!name) return -1.0F;
    auto* eng = (Engine*)ctx;
    int best = -1;
    /* 重名取**最小 id**（旧语义；树序不保证全局 id 升序，重挂后尤其如此）。 */
    eng->scene->visit([&](int id, SceneNode& e, int) {
        if (e.name == name && (best < 0 || id < best)) best = id;
    });
    return (float)best;
}

/* ============================================================================
 * 节点/组件 API（对照 参考实现：Node / Component / Director）
 * 全部经反射按 role 处理，不硬编码组件类型名（docs/06/17/18）。
 * ==========================================================================*/

/* 按 role 找组件 blob（找不到返回 nullptr）。 */


/* ---------------- 宿主 API 实现（声明来自生成的 maque_api.h） ---------------- */
/* ---- 语义辅助（全局：host 函数与引擎内部共用）---- */

/* ---- 层级 ---- */
float node_set_parent(void* ctx, float id, float parent) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    const int p = (int)parent;
    if (p == (int)id) return 0.0F;                       // 禁止自环
    for (int a = p, guard = 0; a >= 0 && guard < 4096; ++guard) {   // 禁止成环（沿祖先链上溯）
        if (a == (int)id) return 0.0F;
        SceneNode* pe = eng->scene->at(a);
        a = pe ? pe->parent : -1;
    }
    eng->scene->attach((int)id, p);                      // children 与 parent 同步维护
    return 1.0F;
}

float node_parent(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (float)e->parent : -1.0F;
}

float node_child_count(void* ctx, float id) {
    return (float)((Engine*)ctx)->scene->child_count((int)id);
}

float node_child_at(void* ctx, float id, float index) {
    return (float)((Engine*)ctx)->scene->child_at((int)id, (int)index);
}

float node_find_child(void* ctx, float id, const char* name) {
    auto* eng = (Engine*)ctx;
    const std::string want = name ? name : "";
    auto match = [&](const std::vector<int>& ids) -> int {
        for (int k : ids) {
            SceneNode* e = eng->scene->at(k);
            if (e && e->name == want) return k;
        }
        return -1;
    };
    if ((int)id < 0) return (float)match(eng->scene->roots);     // -1 = 场景根集合
    SceneNode* p = eng->scene->at((int)id);
    return p ? (float)match(p->children) : -1.0F;
}

void node_set_name(void* ctx, float id, const char* name) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    if (e && name) e->name = name;
}

/* ---- 节点属性 ---- */
void node_set_active(void* ctx, float id, float on) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->active = on != 0.0F;
}

float node_active(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (e->active ? 1.0F : 0.0F) : 0.0F;
}

void node_set_scale(void* ctx, float id, float v) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->scale = v;
}

float node_scale(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->scale : 1.0F;
}

void node_set_angle(void* ctx, float id, float deg) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->angle = deg;
}

float node_angle(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->angle : 0.0F;
}

void node_set_opacity(void* ctx, float id, float a) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->opacity = a < 0.0F ? 0.0F : (a > 1.0F ? 1.0F : a);
}

float node_opacity(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->opacity : 1.0F;
}

void node_set_z_index(void* ctx, float id, float z) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->z_index = (int)z;
}

float node_z_index(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (float)e->z_index : 0.0F;
}

/* 位置（node.position / setPosition）：写回 Transform2D 组件（非仅缓存字段）。 */
void node_set_position(void* ctx, float id, float x, float y) { entity_set_pos(ctx, id, x, y); }

float node_position_x(void* ctx, float id) { return entity_x(ctx, id); }

float node_position_y(void* ctx, float id) { return entity_y(ctx, id); }

/* 内容尺寸/锚点：由 ui.Control 承载（UITransform）。 */
float node_width(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    const auto* ctrl = (blob && d) ? as_component<ui::Control>(d, blob->data()) : nullptr;
    return ctrl ? ctrl->offset_max.x : 0.0F;
}

float node_height(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    const auto* ctrl = (blob && d) ? as_component<ui::Control>(d, blob->data()) : nullptr;
    return ctrl ? ctrl->offset_max.y : 0.0F;
}

void node_set_content_size(void* ctx, float id, float w, float h) {
    auto* eng = (Engine*)ctx;
    if (!comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL))
        attach_component(*eng, (int)id, "ui.Control");        // 控件盒模型恒在（按需补挂）
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    auto* ctrl = (blob && d) ? as_component_mut<ui::Control>(d, blob->data()) : nullptr;
    if (ctrl) ctrl->offset_max = math::Vec2{ w, h };
}

void node_set_anchor(void* ctx, float id, float ax, float ay) {
    auto* eng = (Engine*)ctx;
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    auto* ctrl = (blob && d) ? as_component_mut<ui::Control>(d, blob->data()) : nullptr;
    if (!ctrl) return;
    const math::Vec2 v{ ax, ay };
    ctrl->anchor_min = v; ctrl->anchor_max = v; ctrl->pivot = v;
}

/* ---- 组件 ---- */
float node_has_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    return comp_by_name(*eng, (int)id, type, nullptr) ? 1.0F : 0.0F;
}

float node_add_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    const char* tn = type ? type : "";
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, tn);
    if (!d) {
        char fq[128];
        for (const char* ns : { "scn", "ui", "physics", "anim", "render", "input" }) {
            std::snprintf(fq, sizeof fq, "%s.%s", ns, tn);
            d = mq_reflect_type_by_name(eng->reflect, fq);
            if (d) break;
        }
    }
    if (!d || !eng->scene->at((int)id)) return 0.0F;
    attach_component(*eng, (int)id, d->name);
    return 1.0F;
}

float node_remove_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, type, &d);
    if (!e || !blob || !d) return 0.0F;
    destroy_blob(*eng, d->type_id, *blob);
    e->comps.erase(d->type_id);
    return 1.0F;
}

float node_component_count(void* ctx, float id) {
    SceneNode* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (float)e->comps.size() : 0.0F;
}

const char* node_component_at(void* ctx, float id, float index) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    if (!e) return "";
    std::vector<uint32_t> tids;
    for (auto& [t, b] : e->comps) tids.push_back(t);
    std::sort(tids.begin(), tids.end());                 // 稳定顺序（type_id 升序）
    const int i = (int)index;
    if (i < 0 || i >= (int)tids.size()) return "";
    const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tids[(size_t)i]);
    return d ? d->name : "";
}

float node_child_with_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    auto match = [&](const std::vector<int>& ids) -> int {
        for (int k : ids) if (comp_by_name(*eng, k, type, nullptr)) return k;
        return -1;
    };
    if ((int)id < 0) return (float)match(eng->scene->roots);     // -1 = 场景根集合
    SceneNode* p = eng->scene->at((int)id);
    return p ? (float)match(p->children) : -1.0F;
}

/* ---- 组件字段（反射读写）---- */
float node_get_number(void* ctx, float id, const char* comp, const char* field) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    return read_num(d, blob->data(), field ? field : "", 0.0F);
}

float node_set_number(void* ctx, float id, const char* comp, const char* field, float value) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    return write_num(d, blob->data(), field ? field : "", (double)value) ? 1.0F : 0.0F;
}

float node_get_bool(void* ctx, float id, const char* comp, const char* field) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, field ? field : "");
    if (!f || f->kind != MQ_TK_BOOL) return 0.0F;
    return field_get_bool(f, blob->data()) ? 1.0F : 0.0F;
}

float node_set_bool(void* ctx, float id, const char* comp, const char* field, float value) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, field ? field : "");
    if (!f || f->kind != MQ_TK_BOOL) return 0.0F;
    field_set_bool(f, blob->data(), value != 0.0F);
    return 1.0F;
}

const char* node_get_string(void* ctx, float id, const char* comp, const char* field) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return "";
    if (const std::string* sp = read_str(d, blob->data(), field ? field : "")) return sp->c_str();
    return "";
}

float node_set_string(void* ctx, float id, const char* comp, const char* field, const char* value) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, field ? field : "");
    if (!f || f->kind != MQ_TK_STRING) return 0.0F;
    std::string assigned = value ? value : "";
    (void)field_set_str(f, blob->data(), assigned.c_str());
    /* 改了资源引用就**当场解析并缓存**。
     * 为什么必须在这里做：渲染提取（Scene::extract）是**只读**的（docs/13 §8 / docs/15 §1：
     * 不得触发加载），它只查 material_cache / tex_cache。装配期会预解析场景自带实体的资源，
     * 但**脚本运行时生成的**实体没有那次机会 —— 于是贴图缺失、图元退化成纯色方块
     *（实测：动态生成的水管全是 sprite 默认蓝色，而不是贴图的绿色）。
     * 所以"赋值即解析"是这条路径上唯一能补齐的时机。 */
    if (d->role & MQ_ROLE_SPRITE) {
        const std::string& ref = assigned;
        if (!ref.empty()) {
            /* 直接比字段名：这里原实现又调用两次 mq_reflect_field 来"认"字段，
             * 每个 set_string 白付 ~50ns；字段描述符名就是唯一真相。 */
            if (f->name && std::strcmp(f->name, "material") == 0) {
                scn::SpriteMaterial m{};
                if (eng->load_material(ref, m)) {
                    eng->material_cache[ref] = m;
                    if (!m.texture.empty()) eng->load_texture_ref(eng->scoped_texture_ref((int)id, m.texture));
                }
            } else if (f->name && std::strcmp(f->name, "texture") == 0) {
                eng->load_texture_ref(eng->scoped_texture_ref((int)id, ref));
            }
        }
    }
    return 1.0F;
}

/* ---- 事件（node.on/off/emit；emit 沿父链冒泡，docs/32 §4.2/§4.3）---- */
float node_on(void* ctx, float id, const char* event, const char* handler) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene || !eng->scene->at((int)id) || !event || !handler) return 0.0F;
    /* 归属：全局脚本 = -3、常驻覆盖层 = -2（两者都可监听任意节点，保持旧能力）；
     * 否则 = 当前脚本所属实例。
     * 子实例脚本只能注册自己子树内的节点（docs/32 §4.2：越界显式拒绝，不静默生效）。 */
    const int owner = eng->in_global ? -3
                      : eng->in_overlay ? -2
                      : (eng->active_instance ? eng->active_instance->id : 0);
    if (owner > 0 && !eng->entity_in_instance_subtree(owner, (int)id)) {
        std::print(stderr, "runtime: node_on rejected — entity {} is outside scene instance {} scope\n",
                   (int)id, owner);
        return 0.0F;
    }
    Engine::NodeHandler rec{ event, handler, owner, eng->script_slot_for_handler(handler) };
    rec.script_inst = eng->active_script_instance;
    eng->node_events[(int)id].push_back(rec);
    return 1.0F;
}

float node_off(void* ctx, float id, const char* event) {
    auto* eng = (Engine*)ctx;
    auto it = eng->node_events.find((int)id);
    if (it == eng->node_events.end()) return 0.0F;
    const std::string ev = event ? event : "";
    auto& v = it->second;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Engine::NodeHandler& h) { return h.event == ev; }), v.end());
    return (float)(before - v.size());
}

float node_emit(void* ctx, float id, const char* event) { return node_emit_number(ctx, id, event, 0.0F); }

float node_emit_number(void* ctx, float id, const char* event, float value) {
    auto* eng = (Engine*)ctx;
    if ((!eng->bus && !eng->overlay_bus && !eng->global_bus && eng->instances.empty()) || !event) return 0.0F;
    const std::string ev = event;
    int hits = 0;
    /* 目标 → 祖先（冒泡，跨实例边界不截断）；处理器只在其注册者所属实例的 bus 里调用。
     * 每层按注册顺序派发（确定性）。 */
    for (int cur = (int)id; cur >= 0; ) {
        auto it = eng->node_events.find(cur);
        if (it != eng->node_events.end()) {
            for (const Engine::NodeHandler& h : it->second) {
                if (h.event != ev) continue;
                if (h.script_inst) {
                    const double args[3] = { (double)cur, (double)value, 0.0 };
                    mq_script_instance* prev = eng->active_script_instance;
                    eng->active_script_instance = h.script_inst;
                    const int rc = mq_script_instance_call(h.script_inst, h.handler.c_str(), 3, args, nullptr, nullptr);
                    eng->active_script_instance = prev;
                    if (rc == 2) ++eng->module_errors;
                    ++hits;
                    continue;
                }
                if (h.instance == -3) {
                    eng->call_global_event(h.handler.c_str(), (double)cur, (double)value, 0.0, nullptr);
                } else if (h.instance == -2) {
                    eng->call_overlay_event(h.handler.c_str(), (double)cur, (double)value, 0.0, nullptr);
                } else if (SceneInstanceRuntime* ci = eng->instance_at(h.instance)) {
                    eng->call_instance_event(*ci, h.handler.c_str(), (double)cur, (double)value, 0.0, nullptr);
                }
                ++hits;
            }
        }
        SceneNode* e = eng->scene->at(cur);
        cur = e ? e->parent : -1;
    }
    return (float)hits;
}

/* ---- destroy / find(path) ---- */
float node_destroy(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene->at((int)id)) return 0.0F;
    eng->systems.pending_destroy.push_back((int)id);
    return 1.0F;
}

/* isValid(node)：实体仍存在且未处于待销毁队列。 */
/* isValid(node)：节点在**帧末真正销毁前**仍然有效（destroy 只是排队）。 */
float node_valid(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    return eng->scene->at((int)id) ? 1.0F : 0.0F;
}

float node_find_path(void* ctx, float root, const char* path) {
    auto* eng = (Engine*)ctx;
    if (!path || !*path) return -1.0F;
    int cur = (int)root;
    std::string seg;
    for (const char* p = path;; ++p) {
        if (*p == '/' || *p == '\0') {
            if (!seg.empty() && seg != ".") {
                int next = -1;
                auto match = [&](const std::vector<int>& ids) {
                    for (int k : ids) {
                        SceneNode* e = eng->scene->at(k);
                        if (e && e->name == seg) { next = k; break; }
                    }
                };
                if (cur < 0) match(eng->scene->roots);          // root=-1：从场景根集合找
                else if (SceneNode* p = eng->scene->at(cur)) match(p->children);
                cur = next;
                if (cur < 0) return -1.0F;
            }
            seg.clear();
            if (*p == '\0') break;
        } else {
            seg.push_back(*p);
        }
    }
    return (float)cur;
}

float entity_valid(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    return eng->scene->at((int)id) ? 1.0F : 0.0F;
}

float entity_z(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    /* 快路径：内置按钮的 z 直接在编译期判型后读（schema 里只有 scn.Button 带该角色）。 */
    if (auto* b = component_of<MQ_TID_SCN_BUTTON>(*e)) return (float)b->z;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
        if (const mq_field_desc* fz = mq_reflect_field(d, "z"))
            return (float)field_get_i32(fz, blob.data());
    }
    return 0.0F;
}

void entity_set_z(void* ctx, float id, float z) {
    auto* eng = (Engine*)ctx;
    SceneNode* e = eng->scene->at((int)id);
    if (!e) return;
    if (auto* b = component_of<MQ_TID_SCN_BUTTON>(*e)) { b->z = (int32_t)z; return; }
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
        if (const mq_field_desc* fz = mq_reflect_field(d, "z")) field_set_i32(fz, blob.data(), (int32_t)z);
    }
}

}  // namespace host
