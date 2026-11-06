/* 宿主 API · 场景/实例/全局脚本（director_* / scene_* / global_*，docs/32）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

/* ---- Director ---- */
void director_pause(void* ctx) { ((Engine*)ctx)->director_paused = true; }

void director_resume(void* ctx) { ((Engine*)ctx)->director_paused = false; }

float director_paused(void* ctx) { return ((Engine*)ctx)->director_paused ? 1.0F : 0.0F; }

float director_frame(void* ctx) { return (float)((Engine*)ctx)->frame; }

float director_load_scene(void* ctx, const char* path) {
    auto* eng = (Engine*)ctx;
    if (!path || !*path) return 0.0F;
    eng->pending_scene = path;                     // 帧边界生效（docs/10：帧内不加载资源）
    return 1.0F;
}

/* 场景自省（常驻覆盖层/导航脚本用）：当前场景标题/目录/路径。
 * 绑定层立即拷贝返回串（std::string 成员的生命周期由引擎保证）。 */
const char* scene_title(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng->scene ? eng->scene->desc.title.c_str() : "";
}

const char* scene_dir(void* ctx) { return ((Engine*)ctx)->scn_dir.c_str(); }

const char* scene_path(void* ctx) { return ((Engine*)ctx)->scn_full_path.c_str(); }

/* ---- 3D 拾取（docs/19 §4）：主相机屏幕射线 / 显式世界射线 → 最近命中的 MeshRenderer 实体 ---- */
float scene_pick(void* ctx, float x, float y) {
    auto* eng = (Engine*)ctx;
    return (float)eng->pick_screen(x, y).entity;
}

float scene_raycast(void* ctx, float ox, float oy, float oz, float dx, float dy, float dz,
                    float max_distance) {
    auto* eng = (Engine*)ctx;
    const float origin[3] = { ox, oy, oz };
    const float dir[3] = { dx, dy, dz };
    return (float)eng->pick_ray(origin, dir, max_distance).entity;
}

/* ---- 场景实例（docs/32 §7.1）：句柄立即分配，构建/卸载在帧边界（apply_instance_ops） ---- */
float scene_instantiate(void* ctx, float parent, const char* path, const char* params) {
    auto* eng = (Engine*)ctx;
    if (!path || !*path) { eng->scene_last_error = "scene_instantiate: missing path"; return -1.0F; }
    const int attach = (int)parent;
    if (attach >= 0 && (!eng->scene || !eng->scene->at(attach))) {
        eng->scene_last_error = "scene_instantiate: parent entity not found";
        return -1.0F;
    }
    const std::string base = eng->active_instance ? eng->active_instance->base : std::string();
    std::string vfs;
    if (!eng->resolve_instance_path(path, base, vfs)) {
        eng->scene_last_error = "scene_instantiate: invalid path";
        return -1.0F;
    }
    int parent_inst = 0;
    if (attach >= 0) {
        SceneInstanceRuntime* owner = eng->instance_of_entity(attach);
        if (owner) parent_inst = owner->id;
    }
    const int id = eng->create_instance(parent_inst, attach, vfs);
    if (id < 0) return -1.0F;
    /* 参数在**入队时**就写入实例记录：帧边界构建 → 子场景 on_start 一定能读到
     * （docs/32 §16.2；省略第三参数时 params 为空串）。 */
    if (SceneInstanceRuntime* inst = eng->instance_at(id); inst && params) inst->params = params;
    eng->instance_ops.push_back({ 0, id, attach, vfs });
    std::print("runtime: scene_instantiate queued #{} '{}' under entity {}\n", id, vfs, attach);
    return (float)id;
}

float scene_unload(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || inst->id == 0 || inst->state == 2 || inst->state == 4) return 0.0F;
    inst->state = 4;                                // pending unload：立刻停止 update
    eng->instance_ops.push_back({ 1, inst->id, -1, std::string() });
    return 1.0F;
}

float scene_reload(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || inst->id == 0 || inst->state != 1) return 0.0F;
    eng->instance_ops.push_back({ 2, inst->id, -1, std::string() });
    return 1.0F;
}

void scene_set_instance_paused(void* ctx, float instance, float paused) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst) return;
    inst->paused = paused != 0.0F;
    if (inst->attach_entity >= 0) eng->ensure_instance_component(inst->attach_entity, inst->id);
}

void scene_set_instance_active(void* ctx, float instance, float active) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst) return;
    inst->active = active != 0.0F;
    if (inst->attach_entity >= 0) {
        if (SceneNode* e = eng->scene ? eng->scene->at(inst->attach_entity) : nullptr) e->active = inst->active;
        eng->ensure_instance_component(inst->attach_entity, inst->id);
    }
}

float scene_instance_valid(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst && inst->state == 1 ? 1.0F : 0.0F;
}

float scene_instance_root(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst) return -1.0F;
    if (inst->attach_entity >= 0) return (float)inst->attach_entity;
    return inst->roots.empty() ? -1.0F : (float)inst->roots.front();
}

float scene_instance_parent(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst ? (float)inst->parent : -1.0F;
}

const char* scene_instance_source(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst ? inst->source.c_str() : "";
}

float scene_instance_paused(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst && inst->paused ? 1.0F : 0.0F;
}

float scene_instance_count(void* ctx) {
    auto* eng = (Engine*)ctx;
    int n = 0;
    for (auto& [id, up] : eng->instances) {
        (void)id;
        if (up && up->state == 1) ++n;
    }
    return (float)n;
}

float scene_instance_at(void* ctx, float index) {
    auto* eng = (Engine*)ctx;
    int i = (int)index, n = 0;
    for (auto& [id, up] : eng->instances) {
        (void)id;
        if (!up || up->state != 1) continue;
        if (n++ == i) return (float)up->id;
    }
    return -1.0F;
}

float scene_instance_of(void* ctx, float entity) {
    auto* eng = (Engine*)ctx;
    auto it = eng->entity_instance.find((int)entity);
    return it == eng->entity_instance.end() ? -1.0F : (float)it->second;
}

float scene_self(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng->active_instance ? (float)eng->active_instance->id : -1.0F;
}

float scene_self_root(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng->active_instance) return -1.0F;
    return eng->active_instance->attach_entity >= 0
               ? (float)eng->active_instance->attach_entity
               : (eng->active_instance->roots.empty() ? -1.0F : (float)eng->active_instance->roots.front());
}

float scene_emit(void* ctx, float instance, const char* event, float value) {
    auto* eng = (Engine*)ctx;
    if (!event || !*event) return 0.0F;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || inst->state != 1) return 0.0F;
    return (float)eng->call_instance_event(*inst, event, 0.0, (double)value, 0.0, nullptr);
}

const char* scene_last_error(void* ctx) { return ((Engine*)ctx)->scene_last_error.c_str(); }

/* ---- 实例参数（docs/32 §16.2）：父场景 → 子场景的配置 ---- */
const char* scene_self_params(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng->active_instance ? eng->active_instance->params.c_str() : "";
}

const char* scene_instance_params(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst ? inst->params.c_str() : "";
}

float scene_set_instance_params(void* ctx, float instance, const char* params) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || !params) return 0.0F;
    inst->params = params;
    if (inst->attach_entity >= 0) eng->ensure_instance_component(inst->attach_entity, inst->id);
    return 1.0F;
}

/* ---- 全局脚本 / 全局状态（docs/32 §16）---- */
float global_emit(void* ctx, const char* event, float value) {
    auto* eng = (Engine*)ctx;
    if (!event || !*event) return 0.0F;
    return (float)eng->call_global_event(event, 0.0, (double)value, 0.0, nullptr);
}

float global_set(void* ctx, const char* name, float value) {
    auto* eng = (Engine*)ctx;
    if (!name || !*name) return 0.0F;
    eng->global_nums[name] = (double)value;
    return 1.0F;
}

float global_get(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return 0.0F;
    auto it = eng->global_nums.find(name);
    return it == eng->global_nums.end() ? 0.0F : (float)it->second;
}

float global_set_text(void* ctx, const char* name, const char* value) {
    auto* eng = (Engine*)ctx;
    if (!name || !*name) return 0.0F;
    eng->global_texts[name] = value ? value : "";
    return 1.0F;
}

const char* global_get_text(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return "";
    auto it = eng->global_texts.find(name);
    return it == eng->global_texts.end() ? "" : it->second.c_str();
}

float global_has(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return 0.0F;
    return (eng->global_nums.count(name) || eng->global_texts.count(name)) ? 1.0F : 0.0F;
}

float global_remove(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return 0.0F;
    const bool had = eng->global_nums.erase(name) + eng->global_texts.erase(name) > 0;
    return had ? 1.0F : 0.0F;
}

void global_clear(void* ctx) {
    auto* eng = (Engine*)ctx;
    eng->global_nums.clear();
    eng->global_texts.clear();
}

const char* global_keys(void* ctx) {
    auto* eng = (Engine*)ctx;
    std::vector<std::string> keys;
    keys.reserve(eng->global_nums.size() + eng->global_texts.size());
    for (auto& [k, v] : eng->global_nums) keys.push_back(k);
    for (auto& [k, v] : eng->global_texts)
        if (!eng->global_nums.count(k)) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    eng->global_ret.clear();
    eng->global_ret += '[';
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) eng->global_ret += ',';
        write_json_string_pub(eng->global_ret, keys[i]);
    }
    eng->global_ret += ']';
    return eng->global_ret.c_str();
}

}  // namespace host
