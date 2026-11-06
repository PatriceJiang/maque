/* 场景实例（docs/32）：独立 VM 组/生命周期/事件作用域、动态实例化队列、实例快照表
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* =====================================================================================
 * 场景实例（docs/32）
 * ===================================================================================== */

SceneInstanceRuntime* Engine::instance_at(int id) {
    auto it = instances.find(id);
    return it == instances.end() ? nullptr : it->second.get();
}

SceneInstanceRuntime* Engine::instance_of_entity(int entity) {
    auto it = entity_instance.find(entity);
    return it == entity_instance.end() ? nullptr : instance_at(it->second);
}

SceneInstanceRuntime* Engine::instance_by_attach(int entity) {
    for (auto& [id, up] : instances) {
        (void)id;
        if (up && up->attach_entity == entity && up->state != 2) return up.get();
    }
    return nullptr;
}

bool Engine::entity_in_instance_subtree(int instance, int entity) const {
    if (instance <= 0) return true;                 // 根实例：整棵树可见
    int cur = entity;
    for (int guard = 0; cur >= 0 && guard < 100000; ++guard) {
        auto it = entity_instance.find(cur);
        if (it != entity_instance.end() && it->second == instance) return true;
        auto* e = scene ? scene->at(cur) : nullptr;
        cur = e ? e->parent : -1;
    }
    return false;
}

int Engine::create_instance(int parent, int attach, const std::string& vfs_source, int forced_id) {
    /* 预算按**存活**实例数：已卸载的记录已经从表里移除，不占名额（否则长局游戏会被历史实例耗尽）。 */
    if ((int)instances.size() >= std::max(1, instance_limit)) {
        scene_last_error = "instance limit reached";
        std::print(stderr, "runtime: scene instance limit reached ({}) — '{}' skipped\n",
                   instance_limit, vfs_source);
        mq_obs_event(obs, "runtime", 3, "scene_instance_limit");
        return -1;
    }
    auto up = std::make_unique<SceneInstanceRuntime>();
    const int id = forced_id >= 0 ? forced_id : next_instance_id++;
    if (id >= next_instance_id) next_instance_id = id + 1;   // 水位只增不减（陈旧句柄绝不复用）
    up->id = id;
    up->parent = parent;
    up->attach_entity = attach;
    up->source = vfs_source;
    up->base = vfs_dirname(vfs_source);
    up->state = 0;
    const auto inserted = instances.emplace(id, std::move(up)).second;
    if (!inserted) {                                        // id 冲突（快照/重复创建）：不覆盖已有实例
        scene_last_error = "duplicate instance id";
        return -1;
    }
    SceneInstanceRuntime* parent_inst = instance_at(parent);
    if (parent_inst) parent_inst->children.push_back(id);
    return id;
}

bool Engine::resolve_instance_path(const std::string& rel, const std::string& base,
                                   std::string& out_vfs) const {
    out_vfs = vfs_join(base, rel);
    return !out_vfs.empty() && out_vfs.size() > 1;
}

int Engine::call_instance_event(SceneInstanceRuntime& inst, const char* name,
                                double a, double b, double c, const char* text) {
    if (!name) return 0;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = &inst;
    in_overlay = false;
    in_global = false;
    int hits = 0;
    if (!inst.module_instances.empty()) {
        for (auto* ci : inst.module_instances) hits += call_module_event(ci, name, a, b, c, text);
    } else if (inst.bus) {
        hits = mq_script_bus_call3(inst.bus, name, a, b, c, text);
    }
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    return hits;
}

int Engine::call_overlay_event(const char* name, double a, double b, double c, const char* text) {
    if (!name) return 0;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = nullptr;
    in_overlay = true;
    in_global = false;
    int hits = 0;
    if (overlay_script_instance) hits = call_module_event(overlay_script_instance, name, a, b, c, text);
    else if (overlay_bus) hits = mq_script_bus_call3(overlay_bus, name, a, b, c, text);
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    return hits;
}

int Engine::call_global_event(const char* name, double a, double b, double c, const char* text) {
    if (!name) return 0;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = nullptr;
    in_overlay = false;
    in_global = true;
    int hits = 0;
    if (global_script_instance) hits = call_module_event(global_script_instance, name, a, b, c, text);
    else if (global_bus) hits = mq_script_bus_call3(global_bus, name, a, b, c, text);
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    return hits;
}

void Engine::collect_instance_preorder(int id, std::vector<int>& out, bool skip_paused) const {
    auto it = instances.find(id);
    if (it == instances.end() || !it->second) return;
    const SceneInstanceRuntime* inst = it->second.get();
    if (inst->state != 1) return;
    if (!inst->active) return;
    if (skip_paused && inst->paused) return;
    out.push_back(id);
    for (int c : inst->children) collect_instance_preorder(c, out, skip_paused);
}

void Engine::collect_instance_postorder(int id, std::vector<int>& out) const {
    auto it = instances.find(id);
    if (it == instances.end() || !it->second) return;
    const SceneInstanceRuntime* inst = it->second.get();
    if (inst->state != 1) return;
    for (int c : inst->children) collect_instance_postorder(c, out);
    out.push_back(id);
}

bool Engine::load_instance_scripts(SceneInstanceRuntime& inst, std::vector<std::string> rels, bool is_root) {
    /* docs/34 P0：有包清单 + 全部 JS → 单 VM + 模块 + 实例；否则走旧的每脚本 VM 路径。 */
    if (module_pkg.active && !rels.empty()) {
        bool all_js = true;
        for (const std::string& rel : rels)
            if (std::strcmp(mq_script_lang_for_path(rel.c_str()), "js") != 0) all_js = false;
        if (all_js) return load_module_scripts(inst, rels, is_root);
    }
    mq_script_bus* bus = mq_script_bus_create();
    if (!bus) {
        inst.error = "script bus create failed";
        scene_last_error = inst.error;
        return false;
    }
    inst.bus = bus;
    inst.scripts = rels;
    inst.script_hashes.clear();
    if (is_root) { this->bus = bus; }
    for (const std::string& rel : rels) {
        const char* want = mq_script_lang_for_path(rel.c_str());
        const bool is_native = std::strcmp(want, "native") == 0;
        const std::string vfs_path = vfs_join(inst.base, rel);
        mq_script_vm* one = make_script_vm(want, vfs_path);
        if (!one) {
            std::print(stderr, "runtime: script backend '{}' unavailable for '{}' (instance {})\n",
                       want, rel, inst.id);
            inst.error = "script backend unavailable";
            scene_last_error = inst.error;
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        std::print("runtime: script backend '{}' for '{}' (instance {})\n", mq_script_lang(one), rel, inst.id);
        std::string src;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, vfs_path.c_str(), &d, &n) == MQ_VFS_OK) { src.assign((char*)d, n); mq_vfs_free(d); } }
        /* native 的“源码”是可选的：只发布二进制的包没有 .cpp 也能加载（resolver 找产物）。 */
        if (src.empty() && !is_native) {
            std::print("runtime: cannot read script '{}' (resolved '{}', instance {})\n", rel, vfs_path, inst.id);
            inst.error = "cannot read script";
            scene_last_error = inst.error;
            mq_script_vm_destroy(one);
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        inst.script_hashes.push_back(src.empty() ? 0 : mq_vfs_hash(src.data(), src.size()));
        SceneInstanceRuntime* prev = active_instance;
        const bool prev_ov = in_overlay;
        active_instance = &inst;
        in_overlay = false;
        const char* eval_name = is_native ? vfs_path.c_str() : rel.c_str();
        const int rc = mq_script_eval(one, src.c_str(), eval_name);
        active_instance = prev;
        in_overlay = prev_ov;
        if (rc != 0) {
            char ebuf[512] = { 0 };
            if (mq_script_last_error(one, ebuf, (int)sizeof ebuf) != 0) ebuf[0] = '\0';
            std::print(stderr, "runtime: script error in '{}' (instance {}): {}\n",
                       rel, inst.id, ebuf[0] ? ebuf : "eval failed");
            inst.error = "script eval failed";
            scene_last_error = inst.error;
            mq_script_vm_destroy(one);
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        mq_script_bus_add(bus, one, rel.c_str());
    }
    if (is_root) {
        script_paths = rels;
        script_hashes = inst.script_hashes;
        script_path = rels.empty() ? std::string() : rels[0];
        script_hash = inst.script_hashes.empty() ? 0 : inst.script_hashes[0];
        vm = mq_script_bus_vm_at(bus, 0);
        if (rels.size() > 1)
            std::print("runtime: multi-script bus with {} VM(s) (dispatch order = declaration order)\n",
                       mq_script_bus_count(bus));
    }
    return true;
}

bool Engine::build_instance(SceneInstanceRuntime& inst, int depth) {
    (void)depth;
    inst.roots.clear();
    if (!scene) { inst.error = "no world"; inst.state = 3; return false; }
    for (const auto& s : scene_chain) {
        if (s == inst.source) {
            inst.error = "cyclic scene instance reference";
            scene_last_error = "cyclic scene instance: " + inst.source;
            std::print(stderr, "runtime: cyclic scene instance reference '{}' — skipped\n", inst.source);
            mq_obs_event(obs, "runtime", 3, "scene_instance_cycle");
            inst.state = 3;
            return false;
        }
    }
    if ((int)scene_chain.size() >= 16) {
        inst.error = "scene instance nesting too deep (>16)";
        scene_last_error = inst.error;
        std::print(stderr, "runtime: scene instance nesting too deep (>16) — '{}' skipped\n", inst.source);
        inst.state = 3;
        return false;
    }
    void* pd = nullptr; size_t pn = 0;
    if (mq_vfs_read(vfs, inst.source.c_str(), &pd, &pn) != MQ_VFS_OK) {
        inst.error = "scene not found: " + inst.source;
        scene_last_error = inst.error;
        std::print(stderr, "runtime: scene instance source not found '{}'\n", inst.source);
        mq_obs_event(obs, "runtime", 3, "scene_instance_missing");
        inst.state = 3;
        return false;
    }
    std::string text((char*)pd, pn);
    mq_vfs_free(pd);
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) {
        inst.error = "scene json parse failed";
        scene_last_error = inst.error;
        inst.state = 3;
        return false;
    }
    scn::Scene nd{};
    if (scene_type) (void)mq_persist_apply_json(reflect, scene_type, dom, &nd);

    std::vector<std::string> rel_scripts;
    if (!nd.scripts.empty()) rel_scripts = nd.scripts;
    else if (!nd.script.empty()) rel_scripts.push_back(nd.script);

    /* docs/32 §5.1 D1：嵌套场景只有“行为/节点”生效；世界级字段忽略并**显式警告**（每个源一次）。 */
    if ((nd.width != 320 || nd.height != 240 || nd.seed != 0 || nd.gravity_y != -900.0F ||
         nd.views != 1 || nd.render_scale != 1.0F || nd.msaa_samples != 0 || !nd.overlay_script.empty() ||
         !nd.global_script.empty()) &&
        instance_warned.insert(inst.source).second) {
        std::print(stderr, "runtime: nested scene '{}' ignores world-level fields "
                           "(width/height/seed/gravity/views/render_scale/msaa/overlay_script/global_script) — one world only\n",
                   inst.source);
    }

    scene_chain.push_back(inst.source);
    const int prev_inst = building_instance;
    const std::string prev_base = building_base;
    building_instance = inst.id;
    building_base = inst.base;
    const int attach = inst.attach_entity;
    const mq_json* jents = mq_json_get(dom, "entities");
    const uint32_t count = mq_json_len(jents);
    for (uint32_t i = 0; i < count; ++i) load_entity(*this, *scene, mq_json_at(jents, i), attach, nullptr, 1);
    bool ok = true;
    if (!rel_scripts.empty()) ok = load_instance_scripts(inst, rel_scripts, false);
    building_instance = prev_inst;
    building_base = prev_base;
    scene_chain.pop_back();
    mq_json_free(dom);

    if (SceneNode* an = attach >= 0 ? scene->at(attach) : nullptr) {
        for (int eid : an->children)
            if (entity_instance.find(eid) != entity_instance.end() && entity_instance[eid] == inst.id)
                inst.roots.push_back(eid);
    } else {
        for (int eid : scene->roots)
            if (entity_instance.find(eid) != entity_instance.end() && entity_instance[eid] == inst.id)
                inst.roots.push_back(eid);
    }
    if (!ok) { inst.state = 3; return false; }
    inst.state = 1;
    rebase_instance_assets(inst.id);                 // 嵌套场景内资源路径 → 实例 base 前缀
    std::print("runtime: scene instance #{} '{}' built ({} roots, {} scripts)\n",
               inst.id, inst.source, inst.roots.size(), inst.scripts.size());
    mq_obs_event(obs, "runtime", 1, "scene_instance_built");
    return true;
}

int Engine::dispatch_instance_on_start(int id) {
    SceneInstanceRuntime* inst = instance_at(id);
    if (!inst || inst->state != 1) return 0;
    int hits = 0;
    std::vector<int> kids = inst->children;         // 复制：回调里不会立即改结构（动态请求排队）
    for (int c : kids) hits += dispatch_instance_on_start(c);
    hits += call_instance_event(*inst, "on_start", 0.0, 0.0, 0.0, nullptr);
    return hits;
}

int Engine::dispatch_instance_on_destroy(int id) {
    SceneInstanceRuntime* inst = instance_at(id);
    if (!inst) return 0;
    if (inst->state != 1 && inst->state != 4) return 0;
    int hits = 0;
    std::vector<int> kids = inst->children;
    for (int c : kids) hits += dispatch_instance_on_destroy(c);
    hits += call_instance_event(*inst, "on_destroy", 0.0, 0.0, 0.0, nullptr);
    return hits;
}

void Engine::gc_all_instance_scripts() {
    for (auto& [id, up] : instances) {
        (void)id;
        if (up && up->bus) mq_script_bus_gc(up->bus);
    }
    if (global_bus) mq_script_bus_gc(global_bus);
    if (overlay_bus) mq_script_bus_gc(overlay_bus);
    if (module_vm) mq_script_frame_boundary(module_vm);
}

uint32_t Engine::instance_script_errors() const {
    uint32_t n = module_errors;
    for (auto& [id, up] : instances) {
        (void)id;
        if (up && up->bus) n += mq_script_bus_error_count(up->bus);
    }
    if (global_bus) n += mq_script_bus_error_count(global_bus);
    if (overlay_bus) n += mq_script_bus_error_count(overlay_bus);
    return n;
}

void Engine::destroy_instance_content(SceneInstanceRuntime& inst, bool fire_destroy) {
    std::vector<int> kids = inst.children;
    for (int c : kids) {
        if (SceneInstanceRuntime* ci = instance_at(c)) destroy_instance_content(*ci, fire_destroy);
    }
    if (fire_destroy && (inst.state == 1 || inst.state == 4)) (void)dispatch_instance_on_destroy(inst.id);
    destroy_module_instances(inst);
    if (inst.bus) {
        if (inst.bus == bus) { bus = nullptr; vm = nullptr; }
        mq_script_bus_destroy(inst.bus);
        inst.bus = nullptr;
    }
    std::vector<int> owned;
    for (auto& [eid, owner] : entity_instance)
        if (owner == inst.id) owned.push_back(eid);
    std::sort(owned.begin(), owned.end());
    if (scene) {
        for (int eid : owned) scene->remove(eid);
    }
    for (int eid : owned) {
        entity_instance.erase(eid);
        node_events.erase(eid);
    }
    /* 动态实例自动建的挂载壳属于**父**实例；卸载子实例后必须一并删除，
     * 否则每波敌人都会在世界根留下一个空壳（组合机制的资源泄漏，docs/32 §16.4）。 */
    if (inst.owns_shell && inst.attach_entity >= 0) {
        const int shell = inst.attach_entity;
        if (scene) scene->remove(shell);
        entity_instance.erase(shell);        // 归属映射也必须清：实体删除不会自动清 Engine 侧的表
        node_events.erase(shell);            // 壳上的事件处理器随壳消失
        inst.attach_entity = -1;
    }
    inst.owns_shell = false;
    inst.roots.clear();
    inst.children.clear();                   // 子孙已递归销毁；释放 id 列表（记录随后由 prune 移除）
    inst.state = 2;
}

void Engine::prune_destroyed_instances(int keep_id) {
    for (auto it = instances.begin(); it != instances.end();) {
        const bool destroyed = !it->second || it->second->state == 2;
        if (destroyed && it->first != keep_id) it = instances.erase(it);
        else ++it;
    }
}

void Engine::reset_instances() {
    active_instance = nullptr;
    in_overlay = false;
    in_global = false;
    for (auto& [id, up] : instances) {
        (void)id;
        if (!up) continue;
        destroy_module_instances(*up);
        if (up->bus) {
            if (up->bus == bus) { bus = nullptr; vm = nullptr; }
            mq_script_bus_destroy(up->bus);
            up->bus = nullptr;
        }
        up->state = 2;
    }
    instances.clear();
    entity_instance.clear();
    node_events.clear();                 // 处理器记录带实例 id，重建实例后必须重注册
    scene_chain.clear();
    instance_ops.clear();
    instance_warned.clear();
    active_instance = nullptr;
    in_overlay = false;
    in_global = false;
    building_instance = 0;
    building_base.clear();
}

/* 帧边界：提交动态实例化/卸载/重载请求（docs/32 §7.1）。
 * 本边界内新产生的请求（如新实例 on_start 里再 scene_instantiate）留到下一帧边界。 */
void Engine::apply_instance_ops() {
    if (instance_ops.empty()) return;
    std::vector<InstanceOp> ops;
    ops.swap(instance_ops);
    for (const InstanceOp& op : ops) {
        SceneInstanceRuntime* inst = instance_at(op.instance);
        if (!inst) continue;
        if (op.kind == 0) {                                  // instantiate
            if (inst->state != 0) continue;
            /* 动态实例（parent=-1）：自动建一个挂载壳（归属父实例），让子场景根节点
             * 仍然只有一个统一的 `scene_self_root()`；以后落盘/定位也一致。 */
            if (inst->attach_entity < 0 && inst->id != 0 && scene) {
                const int shell = scene->spawn();
                if (shell >= 0) {
                    entity_instance[shell] = inst->parent >= 0 ? inst->parent : 0;
                    inst->attach_entity = shell;
                    inst->owns_shell = true;
                    if (SceneNode* se = scene->at(shell)) {
                        scene->attach(shell, -1);       // 动态壳 = 根节点
                        se->name = "SceneInstance#" + std::to_string(inst->id);
                    }
                }
            }
            if (inst->attach_entity >= 0 && (!scene || !scene->at(inst->attach_entity))) {
                inst->state = 3;
                inst->error = "attach entity was destroyed";
                scene_last_error = inst->error;
                continue;
            }
            if (!build_instance(*inst, 1)) continue;
            if (inst->attach_entity >= 0) ensure_instance_component(inst->attach_entity, inst->id);
            (void)dispatch_instance_on_start(inst->id);
        } else if (op.kind == 1) {                            // unload
            SceneInstanceRuntime* parent = instance_at(inst->parent);
            if (parent) parent->children.erase(std::remove(parent->children.begin(), parent->children.end(), inst->id),
                                               parent->children.end());
            destroy_instance_content(*inst, true);
            inst->state = 2;
            prune_destroyed_instances();                      // 释放本实例（含被递归销毁的子孙）的记录
            inst = nullptr;                                   // erase 后指针失效
            continue;
        } else if (op.kind == 2) {                            // reload（状态重置；失败进入 error）
            const std::string src = inst->source;
            destroy_instance_content(*inst, true);
            prune_destroyed_instances(inst->id);          // 旧子实例记录释放；保留被重建的本条
            inst->children.clear();
            inst->state = 0;
            inst->error.clear();
            inst->source = src;
            if (build_instance(*inst, 1)) {
                if (inst->attach_entity >= 0) ensure_instance_component(inst->attach_entity, inst->id);
                (void)dispatch_instance_on_start(inst->id);
            }
        }
    }
}


/* ---- 实例资源基址 / 动态壳组件 / 嵌套脚本热重载 / 快照实例表（docs/32 §5/§6） ---- */

/* 组件里的字符串资源路径字段（显式名单 + PBR 的 *_texture 槽）。 */
static bool is_instance_asset_field(const std::string& name) {
    if (name == "texture" || name == "material" || name == "mesh_path" || name == "clip" ||
        name == "shader" || name == "font" || name == "skeleton" || name == "atlas" ||
        name == "env_texture")
        return true;
    return name.size() > 8 && name.compare(name.size() - 8, 8, "_texture") == 0;
}
static bool is_absolute_asset(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p.rfind("builtin:", 0) == 0 || p.find("://") != std::string::npos ||
                          p.rfind("mem:", 0) == 0 || p.rfind("zip:", 0) == 0);
}
static void snapshot_str(SnapshotWriter& w, const std::string& s) {
    w.pod((uint32_t)s.size());
    if (!s.empty()) w.bytes(s.data(), s.size());
}

/* 实例 source 的**可序列化形式**：根实例相对场景目录；嵌套实例相对父场景 base；
 * 落在 base 之外的路径保留绝对 VFS 路径（远程/共享资源）。 */
std::string Engine::serialized_instance_source(const SceneInstanceRuntime& inst) {
    const std::string& src = inst.source;
    if (src.empty() || src[0] != '/') return src;
    if (inst.parent > 0) {
        const SceneInstanceRuntime* p = instance_at(inst.parent);
        if (p && !p->base.empty()) {
            const std::string prefix = "/" + p->base;
            if (src.rfind(prefix, 0) == 0) return src.substr(prefix.size());
        }
        return src;
    }
    return src.substr(1);                        // 根场景挂载点 = 场景目录
}

void Engine::ensure_instance_component(int shell, int instance_id) {
    if (!scene || !reflect) return;
    SceneNode* se = scene->at(shell);
    SceneInstanceRuntime* inst = instance_at(instance_id);
    if (!se || !inst) return;
    const mq_type_desc* sid = mq_reflect_type_by_name(reflect, "scn.SceneInstance");
    if (!sid) return;
    if (se->comps.find(sid->type_id) == se->comps.end())
        attach_component(*this, shell, "scn.SceneInstance");
    auto it = se->comps.find(sid->type_id);
    if (it == se->comps.end()) return;
    std::vector<uint8_t>& blob = it->second.bytes;
    if (const mq_field_desc* f = mq_reflect_field(sid, "source"))
        field_set_str(f, blob.data(), serialized_instance_source(*inst).c_str());
    if (const mq_field_desc* f = mq_reflect_field(sid, "params"))
        field_set_str(f, blob.data(), inst->params.c_str());
    if (const mq_field_desc* f = mq_reflect_field(sid, "paused"))
        field_set_bool(f, blob.data(), inst->paused);
    if (const mq_field_desc* f = mq_reflect_field(sid, "active"))
        field_set_bool(f, blob.data(), inst->active);
    if (const mq_field_desc* f = mq_reflect_field(sid, "detached"))
        field_set_bool(f, blob.data(), inst->detached);
    if (const mq_field_desc* f = mq_reflect_field(sid, "instance_id"))
        field_set_i32(f, blob.data(), inst->id);
    if (const mq_field_desc* f = mq_reflect_field(sid, "state"))
        field_set_i32(f, blob.data(), inst->state);
}

/* 把实例拥有的组件资源路径改写为「实例 base 前缀」——让既有的 VFS 相对根场景目录的
 * 加载路径（preload / mesh / spine / audio / text）无需逐个改函数即可找到嵌套资源。 */
void Engine::rebase_instance_assets(int instance_id) {
    SceneInstanceRuntime* inst = instance_at(instance_id);
    if (!inst || inst->base.empty() || !scene) return;
    for (auto& [eid, owner] : entity_instance) {
        if (owner != instance_id) continue;
        SceneNode* e = scene->at(eid);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = blob.desc;
            if (!d) continue;
            for (uint32_t i = 0; i < d->field_count; ++i) {
                const mq_field_desc* f = &d->fields[i];
                if (f->kind != MQ_TK_STRING || !is_instance_asset_field(f->name)) continue;
                std::string* s = (std::string*)field_ptr_mut(f, blob.data());
                if (s->empty()) continue;
                static constexpr const char* kRt = "rendertexture://";
                if (s->rfind(kRt, 0) == 0) continue;  /* 运行期按 consumer instance 解析，不改写持久化文本 */
                if (is_absolute_asset(*s)) continue;
                const std::string joined = vfs_join(inst->base, *s);   // "/base/rel"
                if (joined.size() > 1) *s = joined.substr(1);          // 去前导 '/'（既有代码会补）
            }
        }
    }
}

bool Engine::reload_instance_script(int instance_id, const std::string& rel) {
    SceneInstanceRuntime* inst = instance_at(instance_id);
    if (!inst || !inst->bus || rel.empty()) return false;
    if (!inst->module_instances.empty()) return false;   // docs/34 P0：模块热重载 P1

    int slot = -1;
    for (size_t i = 0; i < inst->scripts.size(); ++i)
        if (inst->scripts[i] == rel) { slot = (int)i; break; }    // 按文件增量：只重建该槽位
    if (slot < 0) return false;
    const char* lang = mq_script_lang_for_path(rel.c_str());
    const bool is_native = std::strcmp(lang, "native") == 0;
    void* d = nullptr; size_t n = 0;
    const std::string vpath = vfs_join(inst->base, rel);
    const bool read_ok = mq_vfs_read(vfs, vpath.c_str(), &d, &n) == MQ_VFS_OK && d;
    if (!read_ok && !is_native) return false;
    std::string src = read_ok ? std::string((char*)d, n) : std::string();
    if (read_ok) mq_vfs_free(d);
    const uint64_t h = src.empty() ? 0 : mq_vfs_hash(src.data(), src.size());
    if (!is_native && (size_t)slot < inst->script_hashes.size() && inst->script_hashes[(size_t)slot] == h)
        return false;
    mq_script_vm* fresh = make_script_vm(lang, vpath);
    if (!fresh) return false;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    active_instance = inst;
    in_overlay = false;
    const char* eval_name = is_native ? vpath.c_str() : rel.c_str();
    const int rc = mq_script_eval(fresh, src.c_str(), eval_name);
    active_instance = prev;
    in_overlay = prev_ov;
    if (rc != 0) {                                   // 原子：eval 失败保留旧 VM
        char ebuf[512] = { 0 };
        mq_script_last_error(fresh, ebuf, (int)sizeof ebuf);
        if (inst->id == 0) {
            std::print(stderr, "runtime: hot reload failed for '{}': {} (旧脚本保留)\n", rel, ebuf);
            std::print(stderr, "runtime: instance 0 script hot reload failed '{}': {} (旧脚本保留)\n",
                       rel, ebuf);
        } else {
            std::print(stderr, "runtime: instance {} script hot reload failed '{}': {} (旧脚本保留)\n",
                       inst->id, rel, ebuf);
        }
        mq_script_vm_destroy(fresh);
        return false;
    }
    mq_script_bus_swap(inst->bus, (uint32_t)slot, fresh, rel.c_str());
    /* 同上：先摘掉该脚本槽位的旧注册，再让新 VM 的 on_start 重新注册一份。 */
    purge_node_handlers(inst->id, slot, fresh, inst->bus, mq_script_bus_count(inst->bus) == 1);
    while (inst->script_hashes.size() <= (size_t)slot) inst->script_hashes.push_back(0);
    inst->script_hashes[(size_t)slot] = h;
    if (inst->id == 0) {
        script_hashes = inst->script_hashes;
        if (!script_hashes.empty()) script_hash = script_hashes.front();
        vm = mq_script_bus_vm_at(inst->bus, 0);
    }
    /* 换入后只对新 VM 派发 on_start（重新注册事件处理器）。 */
    prev = active_instance; active_instance = inst;
    (void)mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr);
    active_instance = prev;
    if (inst->id == 0) {
        /* 根实例保留两条措辞：reload_script 的根路径（“hot reload script”）与
         * 实例路径（“hot reload instance #0 script”）都是既有测试/工具依赖的诊断契约。 */
        std::print("runtime: hot reload script '{}' ({} bytes, slot {})\n", rel, src.size(), slot);
        std::print("runtime: hot reload instance #0 script '{}' (slot {})\n", rel, slot);
    } else {
        std::print("runtime: hot reload instance #{} script '{}' (slot {})\n", inst->id, rel, slot);
    }
    mq_obs_event(obs, "runtime", 1, "instance_script_reloaded");
    return true;
}

/* 快照扩展：全局状态块（docs/32 §16.3）。放在实例表**之前**：restore 时先恢复状态，
 * 再重建实例 VM 并派发 on_start（脚本能读到恢复后的全局状态）。
 * 旧快照没有该块 → restore_globals 探测 magic 失败，原样跳过。 */
void Engine::encode_globals(SnapshotWriter& w) const {
    w.pod((uint32_t)0x4D51474Cu);                  // magic "MQGL"
    w.pod((uint32_t)1u);                           // 版本
    w.pod((uint32_t)global_nums.size());
    for (auto& [k, v] : global_nums) { snapshot_str(w, k); w.pod(v); }
    w.pod((uint32_t)global_texts.size());
    for (auto& [k, v] : global_texts) { snapshot_str(w, k); snapshot_str(w, v); }
}

int Engine::restore_globals(const uint8_t* data, size_t n, size_t& off) {
    auto need = [&](size_t bytes) { return off + bytes <= n; };
    uint32_t magic = 0, ver = 0;
    if (!need(8)) return 0;
    std::memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4D51474Cu) { off -= 4; return 0; }      // 旧快照：无该块
    std::memcpy(&ver, data + off, 4); off += 4;
    if (ver != 1) return 0;
    auto rstr = [&](std::string& out) {
        uint32_t len = 0;
        if (!need(4)) return false;
        std::memcpy(&len, data + off, 4); off += 4;
        if (!need(len) || len > (1u << 20)) return false;
        out.assign((const char*)data + off, len); off += len;
        return true;
    };
    uint32_t nums = 0;
    if (!need(4)) return 0;
    std::memcpy(&nums, data + off, 4); off += 4;
    if (nums > (1u << 20)) return 0;
    std::map<std::string, double> new_nums;
    for (uint32_t i = 0; i < nums; ++i) {
        std::string k; double v = 0;
        if (!rstr(k) || !need(8)) return 0;
        std::memcpy(&v, data + off, 8); off += 8;
        new_nums[std::move(k)] = v;
    }
    uint32_t texts = 0;
    if (!need(4)) return 0;
    std::memcpy(&texts, data + off, 4); off += 4;
    if (texts > (1u << 20)) return 0;
    std::map<std::string, std::string> new_texts;
    for (uint32_t i = 0; i < texts; ++i) {
        std::string k, v;
        if (!rstr(k) || !rstr(v)) return 0;
        new_texts[std::move(k)] = std::move(v);
    }
    global_nums = std::move(new_nums);
    global_texts = std::move(new_texts);
    return 1;
}

/* 快照扩展：实例表 + 实体归属。放在快照末尾（旧快照没有该块 → 跳过）。 */
void Engine::encode_instances(SnapshotWriter& w) const {
    w.pod((uint32_t)0x4D515349u);                  // magic "MQSI"
    w.pod((uint32_t)2u);                           // 版本（v2 = +params；v1 仍可读）
    uint32_t live = 0;
    for (auto& [id, up] : instances) {
        (void)id;
        if (up && up->state != 2) ++live;
    }
    w.pod(live);
    for (auto& [id, up] : instances) {
        (void)id;
        if (!up || up->state == 2) continue;
        w.pod((int32_t)up->id);
        w.pod((int32_t)up->parent);
        w.pod((int32_t)up->attach_entity);
        w.pod((int32_t)up->state);
        w.pod((uint8_t)(up->paused ? 1 : 0));
        w.pod((uint8_t)(up->active ? 1 : 0));
        w.pod((uint8_t)(up->detached ? 1 : 0));
        w.pod((uint8_t)(up->owns_shell ? 1 : 0));
        snapshot_str(w, up->source);
        snapshot_str(w, up->base);
        snapshot_str(w, up->params);
        w.pod((uint32_t)up->roots.size());
        for (int r : up->roots) w.pod((int32_t)r);
        w.pod((uint32_t)up->scripts.size());
        for (const auto& s : up->scripts) snapshot_str(w, s);
    }
    std::vector<std::pair<int, int>> owners(entity_instance.begin(), entity_instance.end());
    std::sort(owners.begin(), owners.end());
    w.pod((uint32_t)owners.size());
    for (auto& [eid, oid] : owners) { w.pod((int32_t)eid); w.pod((int32_t)oid); }
}

int Engine::restore_instances(const uint8_t* data, size_t n, size_t& off) {
    auto need = [&](size_t bytes) { return off + bytes <= n; };
    uint32_t magic = 0, ver = 0;
    if (!need(8)) return 0;
    std::memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4D515349u) { off -= 4; return 0; }
    std::memcpy(&ver, data + off, 4); off += 4;
    if (ver != 1 && ver != 2) return 0;                 // v1 = 无 params/owns_shell（旧快照）
    uint32_t count = 0;
    if (!need(4)) return 0;
    std::memcpy(&count, data + off, 4); off += 4;
    if (count > (uint32_t)std::max(instance_limit * 4, 1024)) return 0;

    /* 先解析到临时表；再决定"保留现有 VM（结构完全相同）"还是重建。 */
    std::vector<std::unique_ptr<SceneInstanceRuntime>> recs;
    recs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto up = std::make_unique<SceneInstanceRuntime>();
        int32_t id = -1, parent = -1, attach = -1, state = 1;
        uint8_t paused = 0, active = 1, detached = 0, owns_shell = 0;
        if (!need(4 * 4 + 3)) return 0;
        std::memcpy(&id, data + off, 4); off += 4;
        std::memcpy(&parent, data + off, 4); off += 4;
        std::memcpy(&attach, data + off, 4); off += 4;
        std::memcpy(&state, data + off, 4); off += 4;
        paused = data[off++]; active = data[off++]; detached = data[off++];
        if (ver >= 2) {
            if (!need(1)) return 0;
            owns_shell = data[off++];
        }
        auto rstr = [&](std::string& out) {
            uint32_t len = 0;
            if (!need(4)) return false;
            std::memcpy(&len, data + off, 4); off += 4;
            if (!need(len)) return false;
            out.assign((const char*)data + off, len); off += len;
            return true;
        };
        if (!rstr(up->source) || !rstr(up->base)) return 0;
        if (ver >= 2 && !rstr(up->params)) return 0;
        uint32_t rn = 0;
        if (!need(4)) return 0;
        std::memcpy(&rn, data + off, 4); off += 4;
        if (rn > 1u << 20) return 0;
        for (uint32_t k = 0; k < rn; ++k) {
            int32_t r = -1;
            if (!need(4)) return 0;
            std::memcpy(&r, data + off, 4); off += 4;
            up->roots.push_back(r);
        }
        uint32_t sn = 0;
        if (!need(4)) return 0;
        std::memcpy(&sn, data + off, 4); off += 4;
        if (sn > 1024) return 0;
        for (uint32_t k = 0; k < sn; ++k) {
            std::string s;
            if (!rstr(s)) return 0;
            up->scripts.push_back(s);
        }
        up->id = id; up->parent = parent; up->attach_entity = attach;
        up->state = state; up->paused = paused != 0; up->active = active != 0; up->detached = detached != 0;
        up->owns_shell = owns_shell != 0;
        recs.push_back(std::move(up));
    }
    std::vector<std::pair<int, int>> owners;
    uint32_t owner_count = 0;
    if (!need(4)) return 0;
    std::memcpy(&owner_count, data + off, 4); off += 4;
    if (owner_count > (1u << 22)) return 0;
    owners.reserve(owner_count);
    for (uint32_t i = 0; i < owner_count; ++i) {
        int32_t eid = -1, oid = -1;
        if (!need(8)) return 0;
        std::memcpy(&eid, data + off, 4); off += 4;
        std::memcpy(&oid, data + off, 4); off += 4;
        owners.emplace_back(eid, oid);
    }

    /* 结构相同 → **保留现有 VM/脚本状态**（纯 undo/回滚更自然；docs/32 §6.3）。
     * 实体表已由 Scene::decode 恢复（id 不变），因此只需刷新归属/暂停/显隐/roots。 */
    instance_ops.clear();                              // 回滚取消尚未提交的动态增删请求
    int live_now = 0;
    for (auto& [id, up] : instances) {
        (void)id;
        if (up && up->state != 2) ++live_now;
    }
    bool same = (size_t)live_now == recs.size();
    for (auto& rec : recs) {
        if (!same) break;
        SceneInstanceRuntime* cur = instance_at(rec->id);
        if (!cur || cur->state == 2) { same = false; break; }
        if (cur->parent != rec->parent || cur->attach_entity != rec->attach_entity ||
            cur->source != rec->source || cur->base != rec->base || cur->params != rec->params ||
            cur->scripts != rec->scripts || cur->roots != rec->roots) { same = false; break; }
    }
    if (same) {
        for (auto& rec : recs) {
            SceneInstanceRuntime* cur = instance_at(rec->id);
            if (!cur) { same = false; break; }
            cur->paused = rec->paused;
            cur->active = rec->active;
            cur->detached = rec->detached;
            cur->state = rec->state;
            cur->roots = rec->roots;
            cur->owns_shell = rec->owns_shell;
        }
        if (same) {
            entity_instance.clear();
            for (auto& [eid, oid] : owners) entity_instance[eid] = oid;
            return 2;                                  // VM 状态保留，无需 on_start
        }
    }

    reset_instances();                                 // 结构变化：重建（VM 状态不落盘）
    for (auto& rec : recs) {
        const int id = rec->id;
        if (id >= next_instance_id) next_instance_id = id + 1;   // 恢复后水位仍单调
        instances.emplace(id, std::move(rec));
    }
    entity_instance.clear();
    for (auto& [eid, oid] : owners) entity_instance[eid] = oid;
    for (auto& [id, up] : instances) {
        (void)id;
        up->children.clear();
        if (up->parent > 0) {
            SceneInstanceRuntime* p = instance_at(up->parent);
            if (p) p->children.push_back(up->id);
        }
    }
    for (auto& [id, up] : instances) {
        (void)id;
        if (!up) continue;
        if (!up->scripts.empty()) {
            if (!load_instance_scripts(*up, up->scripts, up->id == 0)) up->state = 3;
        } else {
            up->bus = nullptr;
            up->state = 1;
        }
    }
    return 1;                                          // 已重建 → 调用方派发 on_start
}

uint64_t Engine::instance_checksum() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (auto& [id, up] : instances) {
        (void)id;
        if (!up || up->state == 2) continue;
        mix((uint64_t)(uint32_t)up->id);
        mix((uint64_t)(uint32_t)up->parent);
        mix((uint64_t)(uint32_t)up->attach_entity);
        mix((uint64_t)(uint32_t)up->state);
        mix(up->paused ? 1u : 0u);
        mix(up->active ? 1u : 0u);
        /* 参数是实例组成的一部分（静态/动态都算）；空串不混入 → 老场景 checksum 零变化。 */
        if (!up->params.empty()) {
            mix(0x9E3779B97F4A7C15ull);
            for (unsigned char c : up->params) mix((uint64_t)c);
        }
        for (int r : up->roots) mix((uint64_t)(uint32_t)r);
    }
    std::vector<std::pair<int, int>> owners(entity_instance.begin(), entity_instance.end());
    std::sort(owners.begin(), owners.end());
    for (auto& [eid, oid] : owners) { mix((uint64_t)(uint32_t)eid); mix((uint64_t)(uint32_t)oid); }
    return h;
}

}  // namespace maque::rt
