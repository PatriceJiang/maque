/* 脚本宿主 · 全局脚本（scn.Scene.global_script，docs/32 §16.1）：游戏级服务层
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 全局脚本（scn.Scene.global_script，docs/32 §16.1）----
 * 与覆盖层同形：独立 VM/总线、跨场景保留、每次装配后 on_start。差别在**语义与顺序**：
 * 它是游戏级服务（进度/存档/跨场景规则），生命周期在实例之后、覆盖层之前；
 * 其他 VM 可用 global_emit(event, value) 显式调用它的命名处理器。
 * 同一路径复用已有 VM（跨场景状态保留）；不同路径 → 换脚本（状态重置）。 */
void Engine::unload_global() {
    /* 同 unload_overlay：全局脚本的节点事件随它的 VM 一起失效。 */
    purge_node_handlers(-3, -1, nullptr, nullptr, true);
    if (global_script_instance) {
        mq_script_instance_destroy(global_script_instance);
        global_script_instance = nullptr;
    }
    if (global_bus) { mq_script_bus_destroy(global_bus); global_bus = nullptr; }
    global_rel.clear();
    global_abs.clear();
    global_hash = 0;
}

bool Engine::ensure_global_script(const std::string& rel) {
    if (rel.empty()) return false;
    /* docs/34 P0：包内 JS 全局脚本 → 单 VM 的引擎级实例。 */
    if (module_pkg.active && std::strcmp(mq_script_lang_for_path(rel.c_str()), "js") == 0) {
        if (global_script_instance && rel == global_rel) return true;
        unload_global();
        if (!ensure_module_vm()) return false;
        const std::string key = module_pkg.key + "/" +
            normalize_module_rel(module_pkg.scene_rel.empty() ? rel : module_pkg.scene_rel + "/" + rel);
        mq_script_module* m = mq_script_module_load_esm(module_vm, key.c_str());
        if (!m) {
            std::print(stderr, "runtime: global module load failed '{}'\n", key);
            return false;
        }
        global_script_instance = mq_script_module_instantiate(m, -1, 0, "", module_pkg.key.c_str());
        if (!global_script_instance) {
            std::print(stderr, "runtime: global module instantiate failed '{}'\n", key);
            return false;
        }
        global_rel = rel;
        global_abs = scn_dir.empty() ? rel : (std::filesystem::path(scn_dir) / rel).string();
        if (watcher) mq_watcher_add(watcher, global_abs.c_str());
        std::print("runtime: global script '{}' active (persists across scene loads)\n", rel);
        return true;
    }
    if (global_bus && rel == global_rel) return true;
    unload_global();
    const char* lang = mq_script_lang_for_path(rel.c_str());
    const bool is_native = std::strcmp(lang, "native") == 0;
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + rel;
    const bool read_ok = mq_vfs_read(vfs, vpath.c_str(), &d, &n) == MQ_VFS_OK && d;
    if (!read_ok && !is_native) {
        std::print(stderr, "runtime: global script not found '{}'\n", rel);
        mq_obs_event(obs, "runtime", 3, "global_script_missing");
        return false;
    }
    std::string src = read_ok ? std::string((char*)d, n) : std::string();
    if (read_ok) mq_vfs_free(d);
    mq_script_vm* one = make_script_vm(lang, vpath);
    if (!one) {
        std::print(stderr, "runtime: global script backend '{}' unavailable for '{}'\n", lang, rel);
        mq_obs_event(obs, "runtime", 3, "global_script_backend");
        return false;
    }
    /* 顶层代码（默认值/初始化）也在"全局脚本上下文"里执行：node_on 归属 -3，
     * 且不能误以为自己是某个场景实例。 */
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_g = in_global;
    const bool prev_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const char* eval_name = is_native ? vpath.c_str() : rel.c_str();
    const int rc = mq_script_eval(one, src.c_str(), eval_name);
    active_instance = prev;
    in_global = prev_g;
    in_overlay = prev_ov;
    if (rc != 0) {
        char ebuf[512] = { 0 };
        mq_script_last_error(one, ebuf, (int)sizeof ebuf);
        std::print(stderr, "runtime: global script eval failed '{}': {}\n", rel, ebuf);
        mq_script_vm_destroy(one);
        mq_obs_event(obs, "runtime", 3, "global_script_failed");
        return false;
    }
    global_bus = mq_script_bus_create();
    if (!global_bus) { mq_script_vm_destroy(one); return false; }
    mq_script_bus_add(global_bus, one, rel.c_str());
    global_rel = rel;
    global_abs = scn_dir.empty() ? rel : (std::filesystem::path(scn_dir) / rel).string();
    global_hash = mq_vfs_hash(src.data(), src.size());
    if (watcher) mq_watcher_add(watcher, global_abs.c_str());
    std::print("runtime: global script '{}' active (persists across scene loads)\n", rel);
    mq_obs_event(obs, "runtime", 1, "global_script_loaded");
    return true;
}

bool Engine::reload_global_script() {
    if (global_script_instance) return false;            // docs/34 P0：模块热重载 P1
    if (!global_bus || global_abs.empty() || !scene) return false;
    std::ifstream f(global_abs, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (src.empty()) return false;
    const uint64_t h = mq_vfs_hash(src.data(), src.size());
    if (h == global_hash) return false;
    const char* lang = mq_script_lang_for_path(global_rel.c_str());
    const bool is_native = std::strcmp(lang, "native") == 0;
    const std::string vpath = "/" + global_rel;
    mq_script_vm* fresh = make_script_vm(lang, vpath);
    if (!fresh) return false;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_g = in_global;
    const bool prev_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const char* eval_name = is_native ? vpath.c_str() : global_rel.c_str();
    const int rc = mq_script_eval(fresh, src.c_str(), eval_name);
    active_instance = prev;
    in_global = prev_g;
    in_overlay = prev_ov;
    if (rc != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
        mq_script_vm_destroy(fresh);
        std::print(stderr, "runtime: global hot reload failed '{}': {} (旧脚本保留)\n", global_rel, e);
        return false;
    }
    mq_script_bus_swap(global_bus, 0, fresh, global_rel.c_str());
    purge_node_handlers(-3, 0, fresh, global_bus, true);
    global_hash = h;
    /* 换入后只对新 VM 派发 on_start（重建 HUD / 重新注册事件）；上下文标记为全局脚本，
     * 否则 on_start 里的 node_on/entity_spawn 会落到错误的作用域。 */
    SceneInstanceRuntime* prev2 = active_instance;
    const bool prev2_g = in_global;
    const bool prev2_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const int hs = mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr);
    active_instance = prev2;
    in_global = prev2_g;
    in_overlay = prev2_ov;
    if (hs != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "on_start failed";
        std::print(stderr, "runtime: global hot reload on_start failed '{}': {}\n", global_rel, e);
    }
    std::print("runtime: hot reload global script '{}' ({} bytes)\n", global_rel, src.size());
    mq_obs_event(obs, "runtime", 1, "global_script_reloaded");
    return true;
}

}  // namespace maque::rt
