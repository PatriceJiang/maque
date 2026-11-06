/* 脚本宿主 · 常驻覆盖层（scn.Scene.overlay_script）：跨 director_load_scene 保留
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 常驻覆盖层脚本（scn.Scene.overlay_script）----
 * 独立 VM/总线：世界重置（director.loadScene）时主脚本随 `bus` 重建，覆盖层 VM 不动 →
 * 外壳/HUD/导航的 JS 状态跨场景保留；UI 实体仍属于当前世界（切场景即销毁），
 * 所以每次场景装配完成后由 on_start 让脚本重建 UI。 */
void Engine::unload_overlay() {
    /* 覆盖层 VM 即将销毁：它注册的节点事件必须先摘掉（换覆盖层脚本/退出时不留陈旧处理器）。 */
    purge_node_handlers(-2, -1, nullptr, nullptr, true);
    if (overlay_script_instance) {
        mq_script_instance_destroy(overlay_script_instance);
        overlay_script_instance = nullptr;
    }
    if (overlay_bus) { mq_script_bus_destroy(overlay_bus); overlay_bus = nullptr; }
    overlay_rel.clear();
    overlay_abs.clear();
    overlay_hash = 0;
}

bool Engine::ensure_overlay_script(const std::string& rel) {
    if (rel.empty()) return false;
    /* docs/34 P0：包内 JS 覆盖层 → 单 VM 的引擎级实例。 */
    if (module_pkg.active && std::strcmp(mq_script_lang_for_path(rel.c_str()), "js") == 0) {
        if (overlay_script_instance && rel == overlay_rel) return true;
        unload_overlay();
        if (!ensure_module_vm()) return false;
        const std::string key = module_pkg.key + "/" +
            normalize_module_rel(module_pkg.scene_rel.empty() ? rel : module_pkg.scene_rel + "/" + rel);
        mq_script_module* m = mq_script_module_load_esm(module_vm, key.c_str());
        if (!m) {
            std::print(stderr, "runtime: overlay module load failed '{}'\n", key);
            return false;
        }
        overlay_script_instance = mq_script_module_instantiate(m, -1, 0, "", module_pkg.key.c_str());
        if (!overlay_script_instance) {
            std::print(stderr, "runtime: overlay module instantiate failed '{}'\n", key);
            return false;
        }
        overlay_rel = rel;
        overlay_abs = scn_dir.empty() ? rel : (std::filesystem::path(scn_dir) / rel).string();
        if (watcher) mq_watcher_add(watcher, overlay_abs.c_str());
        std::print("runtime: overlay script '{}' active (persists across scene loads)\n", rel);
        return true;
    }
    /* 同一脚本：复用已有 VM（状态跨场景保留，只重新收 on_start）。 */
    if (overlay_bus && rel == overlay_rel) return true;
    unload_overlay();
    const char* lang = mq_script_lang_for_path(rel.c_str());
    const bool is_native = std::strcmp(lang, "native") == 0;
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + rel;
    const bool read_ok = mq_vfs_read(vfs, vpath.c_str(), &d, &n) == MQ_VFS_OK && d;
    if (!read_ok && !is_native) {
        std::print(stderr, "runtime: overlay script not found '{}'\n", rel);
        mq_obs_event(obs, "runtime", 3, "overlay_script_missing");
        return false;
    }
    std::string src = read_ok ? std::string((char*)d, n) : std::string();
    if (read_ok) mq_vfs_free(d);
    mq_script_vm* one = make_script_vm(lang, vpath);
    if (!one) {
        std::print(stderr, "runtime: overlay script backend '{}' unavailable for '{}'\n", lang, rel);
        mq_obs_event(obs, "runtime", 3, "overlay_script_backend");
        return false;
    }
    const char* eval_name = is_native ? vpath.c_str() : rel.c_str();
    if (mq_script_eval(one, src.c_str(), eval_name) != 0) {
        char ebuf[512] = { 0 };
        mq_script_last_error(one, ebuf, (int)sizeof ebuf);
        std::print(stderr, "runtime: overlay script eval failed '{}': {}\n", rel, ebuf);
        mq_script_vm_destroy(one);
        mq_obs_event(obs, "runtime", 3, "overlay_script_failed");
        return false;
    }
    overlay_bus = mq_script_bus_create();
    if (!overlay_bus) { mq_script_vm_destroy(one); return false; }
    mq_script_bus_add(overlay_bus, one, rel.c_str());
    overlay_rel = rel;
    overlay_abs = scn_dir.empty() ? rel : (std::filesystem::path(scn_dir) / rel).string();
    overlay_hash = mq_vfs_hash(src.data(), src.size());
    if (watcher) mq_watcher_add(watcher, overlay_abs.c_str());
    std::print("runtime: overlay script '{}' active (persists across scene loads)\n", rel);
    mq_obs_event(obs, "runtime", 1, "overlay_script_loaded");
    return true;
}

bool Engine::reload_overlay_script() {
    if (overlay_script_instance) return false;           // docs/34 P0：模块热重载 P1
    if (!overlay_bus || overlay_abs.empty() || !scene) return false;
    std::ifstream f(overlay_abs, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (src.empty()) return false;
    const uint64_t h = mq_vfs_hash(src.data(), src.size());
    if (h == overlay_hash) return false;
    const char* lang = mq_script_lang_for_path(overlay_rel.c_str());
    const bool is_native = std::strcmp(lang, "native") == 0;
    const std::string vpath = "/" + overlay_rel;
    mq_script_vm* fresh = make_script_vm(lang, vpath);
    if (!fresh) return false;
    const char* eval_name = is_native ? vpath.c_str() : overlay_rel.c_str();
    if (mq_script_eval(fresh, src.c_str(), eval_name) != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
        mq_script_vm_destroy(fresh);                 // 保留旧 VM：写坏脚本不打断运行时
        std::print(stderr, "runtime: overlay hot reload failed '{}': {} (旧脚本保留)\n", overlay_rel, e);
        return false;
    }
    mq_script_bus_swap(overlay_bus, 0, fresh, overlay_rel.c_str());
    purge_node_handlers(-2, 0, fresh, overlay_bus, true);
    overlay_hash = h;
    /* 换入后只对新 VM 派发 on_start：重建 UI 并重新初始化。与常规派发一样标记
     * in_overlay 上下文 —— 否则 on_start 里的 node_on 会按根实例登记（归属/总线都错）。 */
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = nullptr;
    in_overlay = true;
    in_global = false;
    const int hs = mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr);
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    if (hs != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "on_start failed";
        std::print(stderr, "runtime: overlay hot reload on_start failed '{}': {}\n", overlay_rel, e);
    }
    std::print("runtime: hot reload overlay script '{}' ({} bytes)\n", overlay_rel, src.size());
    mq_obs_event(obs, "runtime", 1, "overlay_script_reloaded");
    return true;
}

}  // namespace maque::rt
