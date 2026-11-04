/* 脚本宿主 · 热重载与资源监视（docs/08 §热重载、docs/14）：只在帧边界执行
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 热重载（docs/08 §热重载、docs/14）：只在帧边界执行 ---- */

bool Engine::reload_script(std::string* err, const std::string* only) {
    if (module_pkg.active) { if (err) *err = "module hot reload is P1"; return false; }
    if (script_paths.empty() || !bus) { if (err) *err = "no script"; return false; }
    bool any = false;
    for (size_t i = 0; i < script_paths.size(); ++i) {
        const std::string& sp = script_paths[i];
        if (only && !only->empty() && *only != sp) continue;      // 只重载改动的那个脚本
        void* d = nullptr; size_t n = 0;
        if (mq_vfs_read(vfs, ("/" + sp).c_str(), &d, &n) != MQ_VFS_OK) continue;
        std::string src((char*)d, n);
        mq_vfs_free(d);
        const uint64_t h = mq_vfs_hash(src.data(), src.size());
        if (i < script_hashes.size() && h == script_hashes[i]) continue;   // 内容未变 → 跳过
        /* 原子热重载：先建**全新 VM** 并 eval；成功才换入槽位（顺序不变，其它脚本状态保留）。
         * 失败则销毁新 VM、保留旧的 —— 写坏一个脚本不会让正在跑的游戏失去该逻辑。 */
        const char* lang = mq_script_lang_for_path(sp.c_str());
        mq_script_vm* fresh = mq_script_vm_create_lang(lang);
        if (!fresh) { if (err) *err = "backend unavailable"; return false; }
        mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
        mq_bind_generated(fresh, this);
        if (mq_script_eval(fresh, src.c_str(), sp.c_str()) != 0) {
            char ebuf[512] = { 0 };
            std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
            mq_script_vm_destroy(fresh);                       // 保留旧 VM：游戏继续用旧逻辑
            if (err) *err = e;
            std::print(stderr, "runtime: hot reload failed for '{}': {} (旧脚本保留)\n", sp, e);
            return false;
        }
        mq_script_bus_swap(bus, (uint32_t)i, fresh, sp.c_str());
        /* 旧 VM 的 node_on 注册先摘掉：on_start 只应重新注册一份（否则每次热更叠加，
         * 同一事件调用 N 次，处理器字符串也不断增长）。 */
        purge_node_handlers(0, (int)i, fresh, bus, mq_script_bus_count(bus) == 1);
        /* 换入后**只对新 VM** 派发 on_start：脚本重新初始化并重新注册它的事件处理器。
         * （不加这一步，热更后的脚本会失去 node_on 注册 → 收不到任何事件，功能静默失效。） */
        if (mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr) != 0) {
            char ebuf[512] = { 0 };
            std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "on_start failed";
            std::print(stderr, "runtime: hot reload on_start failed for '{}': {}\n", sp, e);
        }
        if (script_hashes.size() <= i) script_hashes.resize(i + 1, 0);
        script_hashes[i] = h;
        if (script_paths.size() == 1) { script_hash = h; vm = fresh; }
        any = true;
        mq_obs_event(obs, "runtime", 1, "script_reloaded");
        std::print("runtime: hot reload script '{}' ({} bytes, slot {})\n", sp, src.size(), i);
    }
    if (any) ++reloads;
    return any;
}

bool Engine::reload_scene() {
    if (scn_vfs_path.empty() || !scene) return false;
    void* d = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, scn_vfs_path.c_str(), &d, &n) != MQ_VFS_OK) return false;
    std::string text((char*)d, n);
    mq_vfs_free(d);
    /* 场景文件热重载：Spine/资源缓存按旧世界释放（相对路径缓存键在新挂载点下会串场景）。 */
    spine_release();
    clear_asset_caches();
    /* 场景文件热重载（docs/32 §5.3）：根实例的 VM 保留（与旧行为一致），
     * 子实例随旧世界销毁并在装配时按新文件重建；实体归属/节点事件重新登记。 */
    for (auto& [id, up] : instances) {
        (void)id;
        if (!up || up->id == 0) continue;
        /* 正常子实例：后序 on_destroy；error/pending 也走内容销毁（可能已建了一半总线/实体）。 */
        destroy_instance_content(*up, up->state == 1);
    }
    prune_destroyed_instances();              // 旧子实例记录随内容一起释放（否则每次热重载叠一层）
    instance_ops.clear();                     // 旧世界的动态增删请求不得落到新世界
    if (SceneInstanceRuntime* root = instance_at(0)) {
        root->children.clear();
        root->roots.clear();
    }
    entity_instance.clear();
    node_events.clear();
    const int instance_mark = next_instance_id;   // 本次重载新建的子实例 id 从这个水位开始
    /* 热重载：新世界沿用旧世界的实体 id 水位 —— `Scene::spawn` 的"永不复用"契约
     * 必须跨场景重建也成立，否则常驻脚本持有的旧 id 会别名到新场景实体。 */
    auto fresh = std::make_unique<Scene>();
    fresh->next_id = scene ? scene->next_id : 0;
    scene_chain.clear();
    scene_chain.push_back(scn_vfs_path);
    building_instance = 0;
    building_base.clear();
    fresh->deserialize(*this, text);
    fresh->instantiate(*this);
    scene_chain.pop_back();
    /* 旧场景的所有权：根场景默认是 Engine **内嵌**的 scene_store（不是 new 出来的），
     * 只能重置内容 —— `delete &scene_store` 对非堆地址调用 operator delete，
     * 在 ASan 下是 "attempting free on address which was not malloc()-ed"，
     * 在 Release 下直接破坏堆。热重载建立的堆场景由 scene_owned 持有，
     * 下一次替换时由 unique_ptr 释放（否则 scene_owned 为空 → 整棵 Scene 泄漏）。 */
    if (scene == &scene_store) scene_store = Scene();
    scene_owned = std::move(fresh);
    scene = scene_owned.get();
    scene->update_world(*this);
    if (SceneInstanceRuntime* root = instance_at(0)) {
        for (int eid : scene->roots)
            if (entity_instance.find(eid) != entity_instance.end() && entity_instance[eid] == 0)
                root->roots.push_back(eid);
    }
    /* 新装配出的子实例需要一次 on_start（后序）；根实例保持"世界重建但 VM 状态不丢"的旧语义。
     * 先收集 id 再派发：on_start 里可能继续 scene_instantiate，不能边遍历边改表。 */
    std::vector<int> fresh_children;
    for (auto& [id, up] : instances) {
        if (id >= instance_mark && up && up->state == 1 && up->parent == 0) fresh_children.push_back(id);
    }
    for (int id : fresh_children) (void)dispatch_instance_on_start(id);
    setup_render_textures();               // 相机离屏目标/URI 随新世界重建
    preload_scene_assets();                // 新世界的 Sprite/材质引用重新解析
    setup_3d();                         // 重载可能改变 2D/3D 组成 → 重算 has_3d/cube_mesh
    setup_shader_passes();              // 场景资源 shader pass（.wgsl）随场景重载重建
    setup_shader_graphs();              // 多通道 shader 图（program JSON）随场景重载重建
    /* 热重载重建了世界：全局/覆盖层 UI 实体属于旧世界 → 让常驻脚本重建（VM 状态保留）。 */
    if (global_bus) mq_script_bus_call3(global_bus, "on_start", 0.0, 0.0, 0.0, nullptr);
    if (overlay_bus) mq_script_bus_call3(overlay_bus, "on_start", 0.0, 0.0, 0.0, nullptr);
    ++reloads;
    mq_obs_event(obs, "runtime", 1, "scene_reloaded");
    std::print("runtime: hot reload scene '{}'\n", scn_vfs_path);
    return true;
}

uint32_t Engine::poll_watcher() {
    if (!watcher) return 0;
    struct Ctx { Engine* eng; };
    Ctx cx{ this };
    const uint32_t changed = mq_watcher_poll(watcher, [](void* ud, const char* path, mq_watch_kind kind) {
        auto* c = (Ctx*)ud;
        if (kind == MQ_WATCH_REMOVED) return 0;
        const std::string p = path ? path : "";
        /* 全局脚本 / 常驻覆盖层脚本优先（它们不在 script_paths 里，跨场景保留）。 */
        if (!c->eng->global_abs.empty() && p.size() >= c->eng->global_abs.size() &&
            p.compare(p.size() - c->eng->global_abs.size(), c->eng->global_abs.size(),
                      c->eng->global_abs) == 0) {
            c->eng->reload_global_script();
            return 0;
        }
        if (!c->eng->overlay_abs.empty() && p.size() >= c->eng->overlay_abs.size() &&
            p.compare(p.size() - c->eng->overlay_abs.size(), c->eng->overlay_abs.size(),
                      c->eng->overlay_abs) == 0) {
            c->eng->reload_overlay_script();
            return 0;
        }
        /* 脚本热重载（docs/32 §5.3）：根场景与嵌套实例按文件增量，只重建对应实例的那个槽位。 */
        const auto ends_with = [&](const std::string& full, const std::string& tail) {
            return !tail.empty() && full.size() >= tail.size() &&
                   full.compare(full.size() - tail.size(), tail.size(), tail) == 0;
        };
        bool handled = false;
        for (auto& [iid, up] : c->eng->instances) {
            (void)iid;
            if (!up || up->state != 1) continue;
            for (const std::string& sp : up->scripts) {
                const std::string local = (std::filesystem::path(c->eng->scn_dir) /
                                           vfs_join(up->base, sp).substr(1)).string();
                if (ends_with(p, local)) {
                    c->eng->reload_instance_script(up->id, sp);
                    handled = true;
                    break;
                }
            }
        }
        if (!handled) {
            /* 嵌套场景源文件变化：帧边界重建该实例（保留旧实例直到重建成功）。 */
            for (auto& [iid, up] : c->eng->instances) {
                (void)iid;
                if (!up || up->id == 0 || up->state != 1) continue;
                const std::string local = (std::filesystem::path(c->eng->scn_dir) / up->source.substr(1)).string();
                if (ends_with(p, local)) {
                    c->eng->instance_ops.push_back({ 2, up->id, -1, std::string() });
                    handled = true;
                    break;
                }
            }
        }
        if (!handled && !c->eng->scn_vfs_path.empty() &&
            ends_with(p, c->eng->scn_vfs_path))
            c->eng->reload_scene();
        return 0;
    }, &cx);
    return changed;
}

/* 注册 handler 时判定它属于哪个脚本槽位：在同实例 bus 里找**定义了该全局函数**的 VM。
 * 绝大多数脚本用 `function on_xxx()` 声明处理器，按名可唯一判定；多脚本同名处理器
 * 取声明顺序第一个（与 bus 的派发顺序一致）。判不出 → -1（单脚本总线仍会被整体清理）。 */
int Engine::script_slot_for_handler(const char* handler) const {
    if (!handler || !*handler) return -1;
    mq_script_bus* b = in_global ? global_bus
                      : in_overlay ? overlay_bus
                      : (active_instance && active_instance->bus ? active_instance->bus : bus);
    if (!b) return -1;
    const uint32_t n = mq_script_bus_count(b);
    for (uint32_t i = 0; i < n; ++i)
        if (mq_script_has(mq_script_bus_vm_at(b, i), handler)) return (int)i;
    return -1;
}

/* 热重载前清理旧 VM 的节点事件注册：按 (实例, 脚本槽位) 精确摘除；
 * 单脚本总线直接清空该实例的全部注册（覆盖 handler 无法按名判定/已被删除的情况）；
 * 多脚本且槽位未知时，只清理新 VM 里仍存在的同名 handler（避免误删其它脚本的注册）。 */
void Engine::purge_node_handlers(int instance, int slot, mq_script_vm* fresh, mq_script_bus* b,
                                 bool single_vm) {
    for (auto it = node_events.begin(); it != node_events.end();) {
        auto& v = it->second;
        std::erase_if(v, [&](const NodeHandler& h) {
            if (h.instance != instance) return false;
            if (single_vm) return true;
            if (slot >= 0 && h.slot == slot) return true;
            if (h.slot >= 0) return false;
            /* 槽位未知：新 VM 仍定义同名处理器 → 视为被它替换；新 VM 没有、
             * 且整条总线也没有任何 VM 定义它 → 旧脚本遗留的注册（新代码已删掉该 handler）。 */
            if (fresh && mq_script_has(fresh, h.handler.c_str())) return true;
            return b && !mq_script_bus_has(b, h.handler.c_str());
        });
        if (v.empty()) it = node_events.erase(it);
        else ++it;
    }
}

}  // namespace maque::rt
