/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */

#include "runtime_engine.h"

#include <filesystem>


namespace maque::rt {

/* MAQUE_KEEP_OPEN：忽略脚本 quit()（演示用；见 host::quit） */
bool keep_open = false;

/* ---- docs/34 P0：包 / 模块 / 实例（Engine 级单 JS VM）---- */

static std::string normalize_module_rel(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i <= in.size()) {
        size_t j = in.find('/', i);
        if (j == std::string::npos) j = in.size();
        const std::string seg = in.substr(i, j - i);
        if (seg.empty() || seg == ".") {
            /* skip */
        } else if (seg == "..") {
            const size_t p = out.rfind('/');
            out = (p == std::string::npos) ? std::string() : out.substr(0, p);
        } else {
            if (!out.empty()) out += '/';
            out += seg;
        }
        i = j + 1;
    }
    return out;
}

static const char* module_read_cb(void* user, const char* key, size_t* out_len) {
    auto* eng = static_cast<Engine*>(user);
    if (out_len) *out_len = 0;
    if (!eng || !key) return nullptr;
    const std::string prefix = eng->module_pkg.key + "/";
    if (std::strncmp(key, prefix.c_str(), prefix.size()) != 0) return nullptr;
    const std::string rel = std::string(key + prefix.size());
    std::ifstream f(std::filesystem::path(eng->module_pkg.root_dir) / rel, std::ios::binary);
    if (!f) return nullptr;
    eng->module_src_pool.emplace_back((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (out_len) *out_len = eng->module_src_pool.back().size();
    return eng->module_src_pool.back().c_str();
}

static const char* module_resolve_cb(void* user, const char* importer, const char* spec) {
    auto* eng = static_cast<Engine*>(user);
    if (!eng || !importer || !spec) return nullptr;
    const std::string s = spec;
    if (s.rfind("./", 0) != 0 && s.rfind("../", 0) != 0) return nullptr;  // P0：只允许包内相对导入
    const std::string base = importer;
    const size_t slash = base.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : base.substr(0, slash);
    const std::string joined = dir + "/" + s;
    eng->module_resolved_pool.push_back(normalize_module_rel(joined));
    const std::string& resolved = eng->module_resolved_pool.back();
    const std::string prefix = eng->module_pkg.key + "/";
    if (resolved.rfind(prefix, 0) != 0) return nullptr;                    // 不得越出包根
    return resolved.c_str();
}

bool Engine::detect_package() {
    ModulePackage next;
    /* P0 只支持本地包（远程场景的包根/读取另做）。 */
    const bool remote = scn_dir.rfind("http://", 0) == 0 || scn_dir.rfind("https://", 0) == 0;
    std::filesystem::path root;
    if (!remote && !scn_dir.empty()) {
        const std::filesystem::path scene_abs =
            std::filesystem::absolute(std::filesystem::path(scn_dir)).lexically_normal();
        std::filesystem::path dir = scene_abs;
        for (int depth = 0; depth < 12; ++depth) {
            std::error_code ec;
            if (std::filesystem::exists(dir / "maque.package.json", ec) && !ec) { root = dir; break; }
            const std::filesystem::path parent = dir.parent_path();
            if (parent.empty() || parent == dir) break;
            dir = parent;
        }
        if (!root.empty()) {
            const std::filesystem::path manifest = root / "maque.package.json";
            std::ifstream f(manifest, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            mq_json* dom = mq_json_parse(text.c_str(), text.size());
            if (dom) {
                const char* id = mq_json_str(mq_json_get(dom, "id"), "");
                const char* ver = mq_json_str(mq_json_get(dom, "version"), "0");
                bool js_backend = true;
                const mq_json* backends = mq_json_get(dom, "backends");
                if (backends && mq_json_len(backends) > 0) {
                    js_backend = false;
                    for (uint32_t i = 0; i < mq_json_len(backends); ++i) {
                        const char* b = mq_json_str(mq_json_at(backends, i), "");
                        if (std::strcmp(b, "js") == 0) js_backend = true;
                    }
                }
                if (id && *id && js_backend) {
                    next.active = true;
                    next.id = id;
                    next.version = ver;
                    next.key = next.id + "@" + next.version;
                    next.root_dir = root.generic_string();
                    std::error_code ec;
                    const std::filesystem::path rel = std::filesystem::relative(scene_abs, root, ec);
                    next.scene_rel = ec ? std::string() : rel.generic_string();
                    if (next.scene_rel == ".") next.scene_rel.clear();
                }
                mq_json_free(dom);
            } else {
                std::print(stderr, "runtime: bad package manifest '{}'\n", manifest.string());
            }
        }
    }
    if (next.active) {
        std::print("runtime: package '{}' (root '{}', scene '{}')\n",
                   next.key, next.root_dir, next.scene_rel);
    }
    /* 切到另一个包才换掉常驻脚本；切到无清单场景时保持旧行为（不声明 = 保持）。 */
    if (module_pkg.active && next.active &&
        (next.key != module_pkg.key || next.root_dir != module_pkg.root_dir)) {
        unload_global();
        unload_overlay();
        module_src_pool.clear();
        module_resolved_pool.clear();
    }
    module_pkg = next;
    return module_pkg.active;
}

bool Engine::ensure_module_vm() {
    if (module_vm) return true;
    module_vm = mq_script_vm_create_lang("js");
    if (!module_vm) {
        scene_last_error = "js backend unavailable";
        std::print(stderr, "runtime: package scripts need the js backend\n");
        return false;
    }
    mq_script_set_rng(module_vm, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(module_vm, this);
    mq_script_vm_set_module_loader(module_vm, module_resolve_cb, module_read_cb, this);
    std::print("runtime: package script VM ready (one js VM per engine)\n");
    return true;
}

void Engine::destroy_module_instances(SceneInstanceRuntime& inst) {
    for (auto* ci : inst.module_instances) mq_script_instance_destroy(ci);
    inst.module_instances.clear();
    inst.module_keys.clear();
}

int Engine::call_module_event(mq_script_instance* instance, const char* name,
                              double a, double b, double c, const char* text) {
    if (!instance || !name) return 0;
    if (!mq_script_instance_has(instance, name)) return 0;
    int nnum = 3;
    bool with_text = false;
    const std::string n = name;
    if (n == "on_start" || n == "on_destroy") nnum = 0;
    else if (n == "update") nnum = 1;
    else if (n == "on_text") { nnum = 1; with_text = true; }
    else if (n == "on_key_down" || n == "on_key_up") nnum = 2;
    const double nums[3] = { a, b, c };
    mq_script_instance* prev = active_script_instance;
    active_script_instance = instance;
    const int rc = mq_script_instance_call(instance, name, nnum, nums, with_text ? text : nullptr, nullptr);
    active_script_instance = prev;
    if (rc == 2) ++module_errors;
    return 1;
}

bool Engine::load_module_scripts(SceneInstanceRuntime& inst, const std::vector<std::string>& rels, bool is_root) {
    if (!ensure_module_vm()) return false;
    destroy_module_instances(inst);
    inst.scripts = rels;
    inst.script_hashes.clear();
    for (const std::string& rel : rels) {
        const std::string vpath = vfs_join(inst.base, rel);
        std::string relpath = (vpath.size() > 1 && vpath[0] == '/') ? vpath.substr(1) : vpath;
        if (!module_pkg.scene_rel.empty()) relpath = module_pkg.scene_rel + "/" + relpath;
        relpath = normalize_module_rel(relpath);
        const std::string key = module_pkg.key + "/" + relpath;
        mq_script_module* module = mq_script_module_load_esm(module_vm, key.c_str());
        if (!module) {
            inst.error = "module load failed: " + key;
            scene_last_error = inst.error;
            destroy_module_instances(inst);
            return false;
        }
        mq_script_instance* ci = mq_script_module_instantiate(
            module, -1, inst.id, inst.params.c_str(), module_pkg.key.c_str());
        if (!ci) {
            inst.error = "module instantiate failed: " + key;
            scene_last_error = inst.error;
            destroy_module_instances(inst);
            return false;
        }
        inst.module_instances.push_back(ci);
        inst.module_keys.push_back(key);
        inst.script_hashes.push_back(0);
    }
    if (is_root) {
        script_paths = rels;
        script_hashes = inst.script_hashes;
        script_path = rels.empty() ? std::string() : rels[0];
        script_hash = 0;
        bus = nullptr;
        vm = nullptr;
    }
    std::print("runtime: module scripts for instance {} ({} module instance(s), package '{}')\n",
               inst.id, inst.module_instances.size(), module_pkg.key);
    return true;
}

void Engine::shutdown_module_vm() {
    if (!module_vm) return;
    mq_script_vm_destroy(module_vm);
    module_vm = nullptr;
    module_src_pool.clear();
    module_resolved_pool.clear();
}

/* 事件 kind → 处理器名 → **总线广播**（多脚本/多语言的唯一事件入口；定义见下方） */
int dispatch_event(Engine& eng, int kind, double a, double b, double c, const char* text);

/* JSON 字符串转义（与 persist 规则一致；runtime 侧轻量实现）。 */
void write_json_string_pub(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    out += '"';
}

/* 组件 blob 生命周期（关键）：组件含 std::string/容器时，裸字节缓冲**不是有效对象**；
 * 必须显式构造（默认构造 = 反射默认值）后再反序列化，覆盖前显式析构。 */
void destroy_blob(Engine& eng, uint32_t type_id, std::vector<uint8_t>& blob) {
    const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, type_id);
    if (!d || blob.size() < d->size) { blob.clear(); return; }
    if (d->flags & MQ_TYPE_DYNAMIC) {
        /* 动态成员按字段类型逐个析构：string 显式析构；vector 通过容器算子清空（不释放容量，
         * 缓冲随即被释放 → 内存由 vector 自身管理）。 */
        for (uint32_t i = 0; i < d->field_count; ++i) {
            const mq_field_desc* f = &d->fields[i];
            if (f->kind == MQ_TK_STRING) {
                reinterpret_cast<std::string*>(blob.data() + f->offset)->~basic_string();
            } else if (f->kind == MQ_TK_ARRAY_DYN && f->container_clear) {
                f->container_clear(blob.data() + f->offset);
            }
        }
    }
    blob.clear();
}

/* 默认构造组件 blob（走"空 JSON"反序列化，schema 默认值生效）。 */
void construct_blob(Engine& eng, const mq_type_desc* d, std::vector<uint8_t>& blob) {
    (void)eng;
    blob.assign(d->size, 0);
    /* 默认构造由描述符提供（codegen 生成 init_defaults → placement-new 生成的 C++ 类型）：
     *   · 应用 schema 里的字段默认值（此前零填充不会应用默认值 —— 曾导致 RichText 颜色全透明等）；
     *   · 正确构造 string / vector 等动态成员（零填充缓冲不是合法对象）。 */
    if (d->init_defaults) {
        d->init_defaults(blob.data());
    } else if (d->flags & MQ_TYPE_DYNAMIC) {
        for (uint32_t i = 0; i < d->field_count; ++i) {
            const mq_field_desc* f = &d->fields[i];
            if (f->kind == MQ_TK_STRING) new (blob.data() + f->offset) std::string();
            else if (f->kind == MQ_TK_ARRAY_DYN && f->container_clear) f->container_clear(blob.data() + f->offset);
        }
    }
}

/* 组件挂载：由**描述符**构造默认实例（schema 默认值生效）→ 引擎不硬编码组件类型。 */
void attach_component(Engine& eng, int idx, const char* type_name) {
    SceneNode* e = eng.scene->at(idx);
    const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, type_name);
    if (!e || !d) return;
    auto& blob = e->comps[d->type_id];
    if (!blob.empty()) destroy_blob(eng, d->type_id, blob);
    construct_blob(eng, d, blob);        // 含动态成员（string/vector）的合法构造 + schema 默认值
}

/* ---- 终止信号（docs/09 §5）：处理器只置位并恢复默认行为，写盘在帧边界 ---- */
volatile sig_atomic_t g_signal_hit = 0;
const char* g_signal_name = nullptr;
/* 信号安全崩溃落盘（docs/09 §5）：处理器内不得分配/加锁/调用 iostream。
 * 因此引擎在每个帧边界把"崩溃上下文"预渲染进 g_crash_buf，并预打开报告 fd；
 * 处理器只做 write(fd, buf, len) 后恢复默认行为再 raise。 */
int  g_crash_fd = -1;
char g_crash_buf[16384];
volatile sig_atomic_t g_crash_len = 0;
/* 优雅退出标志（宿主 kill / CI 超时清理用）。
 * 注意：SDL2 会**接管 SIGTERM/SIGINT 并只投递 SDL_QUIT 事件** —— 无窗口（headless）时没人处理该事件，
 * 进程就杀不掉了。这里自己装处理器置标志，主循环据此走正常退出路径（teardown/端点清理）。 */
volatile sig_atomic_t g_quit_requested = 0;
inline void on_quit_signal(int) { g_quit_requested = 1; }

inline void on_fatal_signal(int sig) {
    g_signal_hit = sig;
    g_signal_name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT"
                  : sig == SIGFPE ? "SIGFPE" : sig == SIGINT ? "SIGINT" : "signal";
    if (g_crash_fd >= 0 && g_crash_len > 0) {
        const maque::compat::ssize_t ignored =
            maque::compat::write_fd(g_crash_fd, g_crash_buf, (size_t)g_crash_len);
        (void)ignored;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- 反射读取辅助（不依赖具体类型，仅按字段名） ---- */
bool read_f32(const mq_type_desc* d, const void* blob, const char* name, float* out) {
    const mq_field_desc* f = mq_reflect_field(d, name);
    if (!f || f->kind != MQ_TK_F32) return false;
    if (f->accessor && f->accessor->get_f64) {
        *out = (float)f->accessor->get_f64(blob);
        return true;
    }
    *out = field_get_f32(f, (const uint8_t*)blob);
    return true;
}
const std::string* read_str(const mq_type_desc* d, const void* blob, const char* name) {
    const mq_field_desc* f = mq_reflect_field(d, name);
    if (!f || f->kind != MQ_TK_STRING) return nullptr;
    if (f->accessor && f->accessor->get_ptr) return (const std::string*)f->accessor->get_ptr(blob);
    return (const std::string*)((const uint8_t*)blob + f->offset);
}

bool write_ppm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::print(f, "P6\n{} {}\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

/* ---- Engine ---- */
bool Engine::world_snapshot(mq_snapshot* snap) {
    if (!scene || !snap) return false;
    SnapshotWriter w;
    w.buf.reserve(1 << 16);
    scene->encode(*this, w);                            // 先前置容量（快照池缓冲复用，避免抖动）
    /* 物理状态（docs/10 §4）：物理是回放最易漂移环节 → 必须进快照 */
    const size_t phys_size = physics ? mq_physics_state_size(physics) : 0;
    w.pod((uint64_t)phys_size);
    if (phys_size) {
        std::vector<uint8_t> pstate(phys_size);
        const size_t n = mq_physics_save_state(physics, pstate.data(), pstate.size());
        w.bytes(pstate.data(), n);
    }
    /* 粒子状态（docs/15 §4）：也是模拟状态 → 必须进快照，否则回滚后世界不一致 */
    w.pod(frame);                                     // 模拟帧号（时间位置；回滚必须一并回退）
    w.pod(rng);                                       // 引擎 RNG 状态（D2：回滚后随机序列延续）
    w.pod((uint32_t)emitter_accum.size());
    for (int acc : emitter_accum) w.pod((int32_t)acc);
    w.pod((uint32_t)particles.size());
    for (auto& p : particles) {
        w.pod((int32_t)p.owner);
        w.pod(p.x); w.pod(p.y); w.pod(p.vx); w.pod(p.vy);
        w.pod(p.life); w.pod(p.max_life); w.pod(p.size);
        w.pod(p.r); w.pod(p.g); w.pod(p.b);
    }
    encode_globals(w);                                // 全局状态（docs/32 §16.3；旧快照无此块）
    encode_instances(w);                              // 实例表 + 实体归属（docs/32 §6.3）
    void* out = mq_snapshot_begin(snap, w.buf.size() ? w.buf.size() : 1);
    if (!out) return false;
    if (!w.buf.empty()) std::memcpy(out, w.buf.data(), w.buf.size());
    mq_snapshot_commit(snap, w.buf.size());
    return true;
}

/* 快照 → 世界（道具/子节点/组件逐字段恢复；prefab 就地实例化，docs/12 §5）。 */
void Engine::world_restore(const mq_snapshot* snap) {
    if (!scene || !snap) return;
    size_t n = 0;
    const void* bytes = mq_snapshot_bytes(const_cast<mq_snapshot*>(snap), &n);
    if (!bytes || !n) return;
    const size_t used = scene->decode(*this, (const uint8_t*)bytes, n);
    if (used && used + 8 <= n) {
        uint64_t phys_size = 0;
        std::memcpy(&phys_size, (const uint8_t*)bytes + used, 8);
        const uint8_t* ps = (const uint8_t*)bytes + used + 8;
        size_t off = used + 8;
        if (physics && phys_size && off + phys_size <= n) {
            mq_physics_load_state(physics, ps, phys_size);
            off += phys_size;
        }
        particles.clear();
        if (off + 8 <= n) {                                  // 模拟帧号
            uint64_t fr = 0;
            std::memcpy(&fr, (const uint8_t*)bytes + off, 8);
            off += 8;
            frame = fr;
        }
        if (off + 4 <= n) {                                  // 引擎 RNG 状态
            uint32_t rng_state = 0;
            std::memcpy(&rng_state, (const uint8_t*)bytes + off, 4);
            off += 4;
            rng = rng_state;
        }
        if (off + 4 <= n) {                                  // 发射累加器（发射相位，确定性）
            uint32_t acount = 0;
            std::memcpy(&acount, (const uint8_t*)bytes + off, 4);
            off += 4;
            emitter_accum.clear();
            for (uint32_t i = 0; i < acount && off + 4 <= n; ++i) {
                int32_t v = 0;
                std::memcpy(&v, (const uint8_t*)bytes + off, 4);
                off += 4;
                emitter_accum.push_back(v);
            }
        }
        if (off + 4 <= n) {
            uint32_t pcount = 0;
            std::memcpy(&pcount, (const uint8_t*)bytes + off, 4);
            off += 4;
            if (pcount < 100000) {
                particles.reserve(pcount);
                for (uint32_t i = 0; i < pcount; ++i) {
                    if (off + sizeof(Particle) - sizeof(int) > n) break;
                    Particle p{};
                    std::memcpy(&p.owner, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.x, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.y, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.vx, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.vy, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.life, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.max_life, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.size, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.r, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.g, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.b, (const uint8_t*)bytes + off, 4); off += 4;
                    particles.push_back(p);
                }
            }
        }
        /* 全局状态（docs/32 §16.3）：必须在实例 on_start 之前恢复（脚本可能读它）。
         * 旧快照没有该块 → restore_globals 探测 magic 失败、原样跳过。 */
        (void)restore_globals((const uint8_t*)bytes, n, off);
        /* 实例表（docs/32 §6.3）：结构相同时保留 VM 状态；结构变化才重建并 on_start 重注册。 */
        const int inst_mode = restore_instances((const uint8_t*)bytes, n, off);
        if (inst_mode == 1) {
            (void)dispatch_instance_on_start(0);
            if (global_bus) (void)call_global_event("on_start", 0.0, 0.0, 0.0, nullptr);
            if (overlay_bus) (void)call_overlay_event("on_start", 0.0, 0.0, 0.0, nullptr);
        }
    }
    scene->update_world(*this);
}

/* 全局状态校验和（docs/32 §16.3）：空 → 0（老场景的 world_trail 零变化）。
 * 键有序（std::map）→ 遍历确定；数值按位混入（-0.0 与 +0.0 不同，符合快照字节语义）。 */
uint64_t Engine::global_checksum() {
    if (global_nums.empty() && global_texts.empty()) return 0;
    uint64_t h = 1469598103934665603ull;
    auto mix_bytes = [&](const std::string& s) {
        for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ull; }
    };
    for (auto& [k, v] : global_nums) {
        mix_bytes(k);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h ^= bits; h *= 1099511628211ull;
    }
    for (auto& [k, v] : global_texts) {
        h ^= 0x9E3779B97F4A7C15ull; h *= 1099511628211ull;   // 数值/文本段分开
        mix_bytes(k);
        mix_bytes(v);
    }
    return h;
}

/* 世界校验和：场景（实体/组件标量）+ 物理求解器状态 + 粒子（都是模拟状态，docs/10 §5）。 */
uint64_t Engine::world_checksum() {
    uint64_t h = scene ? scene->checksum(*this) : 1469598103934665603ull;
    h ^= instance_checksum() * 1099511628211ull;      // 实例结构（docs/32 §6.3/§8）
    h ^= global_checksum() * 1099511628211ull;        // 全局状态（空 → 0，零回归）
    if (physics) h ^= mq_physics_checksum(physics) * 1099511628211ull;
    for (auto& p : particles) {
        h ^= (uint64_t)(int64_t)(p.x * 4096.0F); h *= 1099511628211ull;
        h ^= (uint64_t)(int64_t)(p.y * 4096.0F); h *= 1099511628211ull;
        h ^= (uint64_t)(int64_t)(p.life * 4096.0F); h *= 1099511628211ull;
    }
    return h;
}

/* 校验和轨迹（逐帧；用于失步定位与回放对比，docs/10 §5/§7）。 */
uint64_t Engine::world_trail() const {
    uint64_t h = 1469598103934665603ull;
    for (auto& [f, c] : trail) {
        h ^= f; h *= 1099511628211ull;
        h ^= c; h *= 1099511628211ull;
    }
    return h;
}

/* 单帧推进：① 采样 ② 输入帧 ③ 脚本 update+GC ④ 校验和/快照 ⑤ 渲染。
 * 主循环与控制面 Runtime.step 走**同一条帧序** → 行为一致（docs/10 D1、docs/11）。 */

void Engine::step_frame(bool render_frame) {
    MQ_TRACY_ZONE(runtime);                          // Tracy：一次模拟步（含渲染+present）总 zone
    tracy_tick();                                    // Tracy：MAQUE_TRACY 环境启动 + 1Hz CPU/内存采样
    engine_options_parse_env();                      // F4 引擎选项的启动初值（MAQUE_ENGINE_OPTIONS；只解析一次）
    /* 输入帧缓冲：**必须预分配**（平台按 pointer_cap/key_cap 写入；容量 0 = 静默丢弃全部设备事件）。
     * 这是"真实鼠标/键盘事件永远到不了脚本"的根因（docs/19 §3：宿主预分配语义）。 */
    static constexpr uint32_t kMaxPointers = 16, kMaxKeys = 32;
    static thread_local mq_input_frame iframe{};
    static thread_local mq_platform_pointer_event iframe_pointers[kMaxPointers]{};
    static thread_local mq_platform_key_event iframe_keys[kMaxKeys]{};
    iframe.pointers = iframe_pointers; iframe.pointer_cap = kMaxPointers;
    iframe.keys = iframe_keys;         iframe.key_cap = kMaxKeys;
    /* 帧起点 + FPS（按**上一次成帧**的总耗时；首帧为 0）。
     * 补步的中间步不上屏（主循环只让最后一步 render_frame）→ 这里也不能刷新 stat_t0，
     * 否则帧间隔会被算成"半步"，fps 曲线被补步拉成方波。 */
    if (render_frame) {
        const auto now = std::chrono::steady_clock::now();
        if (stat_t0.time_since_epoch().count() != 0) {
            const double total_ms = std::chrono::duration<double, std::milli>(now - stat_t0).count();
            if (total_ms > 0.0) {
                const float fps = (float)(1000.0 / total_ms);
                stat_fps = stat_fps > 0 ? (stat_fps * 0.9F + fps * 0.1F) : fps;
            }
        }
        stat_t0 = now;
    }
    // ① 采样（设备事件随时入队，固定处理点统一消费，docs/19 §3）
    /* 帧首清零设备队列：有窗口时 `mq_platform_poll` 会重填，但**无窗口/headless**
     * 没有平台轮询 → 不清零会让上一帧的事件永远滞留（表现为同一个按键每帧都触发）。 */
    iframe.pointer_count = 0;
    iframe.key_count = 0;
    if (platform) {
        MQ_TRACY_ZONE_N(runtime, "input.sample");    // Tracy：平台事件泵 + 原始事件（窗口像素）
        window_event_hooks();                        // CI/演示：MAQUE_WINDOW_EVENT_AT → 真实 SDL 窗口事件
        mq_platform_pump(platform);
        if (window) mq_platform_poll(platform, &iframe);
        /* 窗口 resize/最大化 → 自动重建渲染目标（FBO）。必须在 canvas_resolve / 渲染之前：
         * 画布映射、文本栅格化倍率（ss）、视图几何都读 TW/ss（下一段代码）。 */
        if (window) sync_window_size(iframe.width, iframe.height);
        /* 测试注入：MAQUE_IMGUI_POINTER（合成指针 → 本帧设备队列，与真实鼠标同路径） */
        imgui_fake_input(iframe.pointers, iframe.pointer_count, iframe.pointer_cap);
        /* ImGui 先吃**窗口像素**事件（它工作在窗口空间）；之后才把同一批转成世界坐标。
         * 顺序不能反：转成世界坐标后就没有原始窗口坐标了。 */
        imgui_input_feed(iframe.pointers, iframe.pointer_count, iframe.keys, iframe.key_count);
        /* 设备事件：窗口像素 → 世界坐标（只在此处转换一次；引擎内部一律世界坐标） */
        for (uint32_t i = 0; i < iframe.pointer_count; ++i)
            device_event_to_world(*this, &iframe.pointers[i]);
        if (std::getenv("MAQUE_TRACE_INPUT") && (iframe.pointer_count || iframe.key_count))
            std::print("runtime: polled pointers={} keys={} (frame {})\n",
                       iframe.pointer_count, iframe.key_count, frame);
    }
    // ① 帧首：控制端点轮询（inspector/MCP；非阻塞、单线程、帧边界一次 → 不改确定性）
    /* OrbitControls：鼠标左键拖拽环绕、滚轮缩放（3D 场景；与 ImGui 同帧时也生效，便于调试） */
    if (has_3d) {
        for (uint32_t i = 0; i < iframe.pointer_count; ++i) {
            const mq_platform_pointer_event& pe = iframe.pointers[i];
            if (pe.pointer_type != MQ_PLATFORM_POINTER_MOUSE) continue;
            if (pe.type == MQ_PLATFORM_POINTER_DOWN && pe.button == 1) {
                orbit_dragging = true;
                orbit_last_x = pe.x; orbit_last_y = pe.y;
            } else if (pe.type == MQ_PLATFORM_POINTER_UP || pe.type == MQ_PLATFORM_POINTER_CANCEL) {
                orbit_dragging = false;
            } else if (pe.type == MQ_PLATFORM_POINTER_MOVE && orbit_dragging) {
                orbit_enabled = true;
                orbit_yaw -= (pe.x - orbit_last_x) * 0.01F;
                orbit_pitch += (pe.y - orbit_last_y) * 0.01F;
                if (orbit_pitch > 1.45F) orbit_pitch = 1.45F;
                if (orbit_pitch < -1.45F) orbit_pitch = -1.45F;
                orbit_last_x = pe.x; orbit_last_y = pe.y;
            } else if (pe.type == MQ_PLATFORM_POINTER_WHEEL) {
                orbit_enabled = true;
                orbit_dist *= std::exp(-pe.scroll_y * 0.12F);
                if (orbit_dist < 0.05F) orbit_dist = 0.05F;
                if (orbit_dist > 500.0F) orbit_dist = 500.0F;
            }
        }
    }
    if (ctl_server) mq_control_server_poll(ctl_server);
    // ① 帧首：释放上一帧退役的字形图集纹理（此时已无图元引用旧 id）
    if (!retired_textures.empty()) {
        if (rapi && rself && rapi->destroy_texture)
            for (int t : retired_textures) rapi->destroy_texture(rself, t);
        retired_textures.clear();
    }
    // ①b 输入模拟器：把该帧到期的事件注入输入流（与设备事件同一条路径；回放时跳过）
    /* ①a 画布解析（docs/17）：每帧重算"设计 → 设备像素"映射。
     * 每帧而非"仅尺寸变化"：脚本/控制面可在运行时改 fit_mode / design_* / 安全区，
     * 必须立即生效；纯函数（几次乘法）开销可忽略。窗口/显示器变化同样被覆盖。 */
    canvas_resolve();
    if (platform) (void)mq_platform_display_changed(platform);
    if (direct_requested && rapi && rself && rapi->attach_window) {
        mq_platform_native_window nw{};
        const bool have_native = mq_platform_get_native_window(platform, &nw) != 0 && nw.handle;
        if (!have_native) {
            if (direct_present) {
                mq_renderer_window none{};
                (void)rapi->attach_window(rself, &none);
            }
            direct_present = false;
            direct_window_handle = nullptr;
        } else if (!direct_present || nw.handle != direct_window_handle) {
            mq_renderer_window rw{};
            rw.kind = nw.kind; rw.display = nw.display; rw.handle = nw.handle; rw.extra = nw.extra;
            mq_platform_drawable_size(platform, &rw.width, &rw.height);
            const int32_t rc = rapi->attach_window(rself, &rw);
            direct_present = rc == 0;
            direct_window_handle = direct_present ? nw.handle : nullptr;
            std::print("runtime: direct present reattach kind={} size={}x{} rc={}\n",
                       rw.kind, rw.width, rw.height, rc);
        }
    }

    sim_feed(frame);
    /* 测试钩子：`MAQUE_SIM_KEY_AT="<帧>:<键码>[,<帧>:<键码>...]"` —— 向**本帧设备事件队列**
     * 注入一次"按下"事件（走与真实键盘完全相同的路径：iframe → imgui_input_feed → 热键判断）。
     * headless/CI 没有真实键盘，这是回归"按 F2 开关面板"这条路径的唯一手段。
     * 例：MAQUE_SIM_KEY_AT="30:1002"（第 30 帧按下 F2）。 */
    if (const char* sk = std::getenv("MAQUE_SIM_KEY_AT"); sk && *sk) {
        std::string s(sk), item;
        for (size_t k = 0; k <= s.size(); ++k) {
            if (k < s.size() && s[k] != ',') { item.push_back(s[k]); continue; }
            const size_t colon = item.find(':');
            if (colon != std::string::npos && iframe.key_count < iframe.key_cap) {
                const int at = std::atoi(item.substr(0, colon).c_str());
                const int code = std::atoi(item.c_str() + colon + 1);
                if ((int)frame == at) {
                    mq_platform_key_event& ev = iframe.keys[iframe.key_count++];
                    std::memset(&ev, 0, sizeof ev);
                    ev.key = (uint32_t)code;
                    ev.down = 1;
                }
            }
            item.clear();
        }
    }
    /* **F2 引擎级热键**：开关 ImGui 调参面板（用户要求"引擎层面监听 F2，收到后打开"）。
     * 关键点：
     *   · **沿检测**（按住不放只切换一次）：SDL 在按住时会持续发 KEYDOWN（`repeat=1`），
     *     若"见到按下就切换"会变成每帧开关一次（实测日志 ON/OFF 刷屏、反复 init/shutdown）。
     *     这里维护 `imgui_f2_held`，只在 `未按住 → 按住` 的跳变时 toggle 一次，并忽略 repeat 事件。
     *   · 放在脚本 update 之前：无论脚本是否写过 imgui_*，都能开/关面板。 */
    {
        bool f2_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F2 || k.repeat) continue;      // 忽略键盘自动重复
            if (k.down) f2_down = true;
        }
        if (f2_down && !imgui_f2_held) imgui_toggle();          // 只有上升沿才切换
        imgui_f2_held = f2_down;
    }
    /* **F4 引擎级热键**：弹出/收起「引擎选项」面板；首次只开选项，不附带 F2 stats。
     * 与 F2 一样沿检测（按住只切一次）。与 F2 的**互不干扰**规则：
     *   · ImGui 本来关着 → F4 顺手打开它并记住"是 F4 开的"；再按 F4 收起面板时恢复到关
     *     （不会只关面板却把 stats 留在屏幕上）。
     *   · ImGui 本来就开着（F2 / MAQUE_IMGUI / 脚本）→ F4 只开关选项面板，**不动** stats 的开关。
     *   · F2 关整个 ImGui 时 `imgui_enable(false)` 会清掉选项面板状态 → F2 再开不会"复活"F4 面板。 */
    {
        bool f4_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F4 || k.repeat) continue;
            if (k.down) f4_down = true;
        }
        if (f4_down && !opt_f4_held) {
            if (opt_panel) {
                opt_panel = false;
                std::print("runtime: engine options panel OFF\n");
                if (opt_opened_imgui) {          // F4 自己开的 ImGui → 一并恢复关闭
                    opt_opened_imgui = false;
                    imgui_enable(false);
                }
            } else {
                opt_panel = true;
                if (!imgui_on) {
                    imgui_enable(true, false);   // F4 不附带打开 F2 stats
                    opt_opened_imgui = true;
                } else {
                    opt_opened_imgui = false;    // 已有 ImGui（F2/MAQUE_IMGUI/脚本）→ 收起时保持它开着
                }
                std::print("runtime: engine options panel ON\n");
            }
        }
        opt_f4_held = f4_down;
    }
    /* **F3 引擎级热键**：启动 Tracy（若未启动）+ 拉起 profiler GUI（docs/29）。
     * 沿检测同上（按住只触发一次）；已启动也允许再按 F3 —— 只补拉 GUI（不会重复弹窗）。 */
    {
        bool f3_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F3 || k.repeat) continue;
            if (k.down) f3_down = true;
        }
        if (f3_down && !tracy_f3_held) {
            tracy_start("F3");
            tracy_launch_gui();
        }
        tracy_f3_held = f3_down;
    }
    /* **引擎热键 F2/F4 从本帧输入里消费掉**：热键判断之后，同一批设备事件还会被 input 模块
     * 派发给脚本/玩法 —— 不消费的话"按 F2 开面板"会同时触发游戏自己的 F2 动作（双触发）。
     * 注意：ImGui 的输入转发在平台轮询阶段**更早**已经发生，这里只影响脚本可见输入。 */
    {
        uint32_t out = 0;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const uint32_t k = iframe.keys[i].key;
            if (k == MQ_KEY_F2 || k == MQ_KEY_F4) continue;   // 引擎调试面板热键（不传给游戏）
            if (out != i) iframe.keys[out] = iframe.keys[i];
            ++out;
        }
        iframe.key_count = out;
    }
    /* ImGui：开始一帧（**必须在脚本 update 之前** —— 脚本里要调 imgui_begin/控件/imgui_end）。
     * 内部还会按需初始化、上传字体图集纹理，并在 ImGui 要吃输入时清空本帧设备事件（不点穿）。 */
    imgui_frame_begin(1.0F / 60.0F);
    /* ③ 不点穿：ImGui 要吃鼠标/键盘 → 清空本帧设备事件，世界输入即被阻断。
     * 判据来自 ImGui 本帧 NewFrame（上一帧的面板状态），这是 ImGui 的标准用法。
     * 注：sim 注入走 input 模块内部路径，不受这里影响（CI 仍能驱动玩法）。 */
    if (imgui_on && imgui_inited) {
        if (mq_imgui_want_mouse()) iframe.pointer_count = 0;
        if (mq_imgui_want_keyboard()) iframe.key_count = 0;
    }
    // ② 输入帧：状态/事件（回放时忽略设备输入，只喂记录）
    if (input) {
        if (!mq_input_replaying(input) && window) mq_input_begin_frame(input, &iframe, frame);
        else mq_input_begin_frame(input, nullptr, frame);
        mq_input_dispatch(input);
        if (mq_input_quit(input)) quit = true;
    }
    // ②b 帧边界热重载：资源变化只在此处生效（docs/08/14）
    if (watcher && (frame % 6 == 0)) poll_watcher();
    // ②c 帧边界切场景（director.loadScene：帧内不加载资源，docs/10 D7）
    if (scene && !pending_scene.empty()) {
        const std::string path = pending_scene;
        pending_scene.clear();
        if (!load_scene_into(path)) {
            std::print(stderr, "runtime: director.loadScene('{}') failed\n", path);
            mq_obs_event(obs, "runtime", 3, "scene_load_failed");
        }
    }
    // ③ 脚本：update(dt) → 帧边界 GC（确定性，docs/14）
    std::vector<mq_renderer_item> draws;
    /* Spine 网格图元（骨骼动画）：与 draws 同一帧序提交，交给 render_mesh_views 统一排序 */
    std::vector<mq_renderer_mesh> spine_meshes;
    const float dt = 1.0F / 60.0F;
    if (scene && director_paused) {
        /* director.pause()：跳过模拟（脚本/动画/粒子/导航/物理/UI 布局），仅保留采样与渲染
         * （暂停语义；输入仍被采样，便于 resume 后继续）。 */
    } else if (scene) {
        MQ_TRACY_ZONE_N(runtime, "sim");             // Tracy：逻辑阶段总 zone（脚本+动画+物理+提取）
        ++script_frames;
        {
            MQ_TRACY_ZONE_N(anim, "tween");          // Tween（补间）
            step_tweens(dt);
            step_schedules(dt);                      // 定时器
        }
        /* 脚本错误的判定：总线**错误计数**的增量（每个 VM 各自受保护调用 → 错误隔离但不静默）。
         * 注意不能拿 dispatch_event 的返回值判断：它返回的是命中数，不是错误码。 */
        {
            MQ_TRACY_ZONE_N(script, "update");       // Tracy：脚本入口（update 回调派发）
            const uint32_t err_before = instance_script_errors();
            dispatch_event(*this, MQ_EVENT_UPDATE, (double)dt, 0.0, 0.0, nullptr);
            if (instance_script_errors() != err_before) {
                char ebuf[512] = { 0 };
                if (bus) mq_script_bus_last_error(bus, ebuf, (int)sizeof ebuf);
                if (!ebuf[0] && overlay_bus)
                    mq_script_bus_last_error(overlay_bus, ebuf, (int)sizeof ebuf);
                mq_obs_event(obs, "script", 3, "update_failed");
                write_crash_context(ebuf[0] ? ebuf : "script update failed");      // 可精确重放（docs/09 §5）
            }
            gc_all_instance_scripts();               // 帧边界：逐实例逐 VM 推进 GC（docs/10 D7）
            /* 动态实例化/卸载：脚本阶段之后的固定边界执行（docs/32 §7.1）；
             * 本边界内 on_start 里再发的请求留到下一帧边界（FIFO，禁止递归构建）。 */
            apply_instance_ops();
        }
        { MQ_TRACY_ZONE_N(anim, "anim.step"); anim_step(dt); }           // 动画采样写回（docs/15 §2）
        { shader_pass_step(dt); }                                        // 场景 shader pass 时间推进（docs/13）
        { MQ_TRACY_ZONE_N(anim, "particles"); particles_step(dt); }      // 粒子推进（docs/15 §4）
        { MQ_TRACY_ZONE_N(nav, "nav.step"); nav_step(); }                // 导航代理推进（A* + 追踪）
        { MQ_TRACY_ZONE_N(physics, "physics.step"); physics_components_step(dt); }   // 组件驱动物理
        flush_pending_destroy();                         // node.destroy()：帧边界统一销毁
        { MQ_TRACY_ZONE_N(ui, "ui.step"); ui_step(); }   // UI 组件语义（layout/progress/slider/...）
        { MQ_TRACY_ZONE_N(runtime, "flatten"); scene->update_world(*this); }
        /* 音频：组件同步（autoplay/参数/空间化）→ 逻辑 playhead/状态按模拟 dt 推进
         * （与设备是否出声无关，docs/30 §5）。 */
        {
            MQ_TRACY_ZONE_N(audio, "step");
            audio_step(dt);
            if (audio) mq_audio_update(audio, dt);
            if (audio_cap) audio_capture_step(dt);
        }
        /* Spine 事件记录的清空点：**脚本之后、step 之前**（见 Engine::spine_clear_events 注释：
         * 放错了会导致 pull 面永远读到 0 条）。 */
        spine_clear_events();
        {
            MQ_TRACY_ZONE_N(anim, "spine.step");         // Tracy：Spine 步进
            spine_step(dt);                              // Spine：步进 + 出绘制数据（依赖展平后的世界位置）
        }
        { MQ_TRACY_ZONE_N(runtime, "extract"); scene->extract(*this, draws); }
        { MQ_TRACY_ZONE_N(anim, "spine.extract"); spine_extract(spine_meshes); }
        { MQ_TRACY_ZONE_N(text, "text.flush"); text_flush(draws); }   // 文本阶段 2：图集纹理 + 最终 UV
    }
    if (scene) sync_render_textures();                  // 动态组件/尺寸/name 变化统一在帧边界提交
    /* 静态场景判定（RenderTexture 默认 EveryFrame，必须在视觉输入不变时跳过 RT pass）。
     * 哈希覆盖实体树/组件 blob/派生世界矩阵；动画/粒子/Spine/shader/ImGui/输入/截图一律视为动态。 */
    if (scene) {
        const bool visual_dynamic =
            gltf_dynamic || !spines.empty() || !particles.empty() || has_shader_pass || has_shader_graph ||
            imgui_on || imgui_backend || !shot_path.empty() || !shot_seq.empty() ||
            !frames_dir.empty() || !camera_captures.empty() || frame_capture_requested ||
            std::getenv("MAQUE_CAPTURE_WGPU") != nullptr;
        bool rt_dirty = false;
        for (const auto& rt : render_textures)
            if (rt.enabled && (rt.dirty || !rt.ready)) { rt_dirty = true; break; }
        const uint64_t svh = visual_dynamic ? 0 : compute_scene_visual_hash();
        scene_visual_unchanged =
            !visual_dynamic && !rt_dirty && iframe.pointer_count == 0 &&
            iframe.key_count == 0 && !iframe.resized && svh == scene_visual_hash_value;
        scene_visual_hash_value = svh;
    } else {
        scene_visual_unchanged = false;
        scene_visual_hash_value = 0;
    }
    /* F4 的 2D/UI 开关只隐藏游戏层；随后提取的 ImGui 网格必须保留，否则调试面板也会消失。 */
    if (!opt_2d_enabled) { draws.clear(); spine_meshes.clear(); }
    /* stats：只计游戏每帧逻辑提交量，调试 ImGui 面板自身不计入。
     * UI drawcall = 图元 + Spine／网格批次；图元固定 4 顶点 / 2 三角形。 */
    stat_ui_calls = (int)(draws.size() + spine_meshes.size());
    stat_3d_calls = 0;
    stat_calls = stat_ui_calls;
    stat_verts = (int)draws.size() * 4;
    stat_tris = (int)draws.size() * 2;
    for (const auto& sm : spine_meshes) {
        stat_verts += (int)sm.vertex_count;
        stat_tris += (int)(sm.index_count / 3);
    }
    /* VRAM 是后端存活的纹理 + 网格缓冲估算；不是驱动/OS 的整进程显存使用量。 */
    stat_vram_bytes = (rapi && rself && rapi->resource_bytes) ? rapi->resource_bytes(rself) : 0;
    /* ImGui：结束帧 → 绘制命令转成网格图元（**grid 链模式**；官方后端模式下唤醒 Render）。
     * 放在这里（脚本/暂停分支之外）是刻意的：
     *   · 每帧都必须 end，暂停/无场景帧也不能漏（否则 ImGui 帧状态错乱、面板冻结）；
     *   · 引擎所有图层（含 ui pass 的 layer=1）都已提取完 → 面板以 MQ_IMGUI_LAYER 提交，
     *     天然画在最上层；且与其它图元一起进同一像素缓冲 → 截图里也有面板（可自证）。 */
    imgui_extract(spine_meshes);
    // ④ 世界校验和 + 周期快照（确定性验证 + 回滚点，docs/10 §4/§5）
    if (scene) {
        last_checksum = world_checksum();
        trail.emplace_back(frame, last_checksum);
        if (snaps && (frame % 60 == 0)) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
        if (cp_active && (frame % cp_interval == 0)) checkpoint_push();
        if (frame % 30 == 0) refresh_crash_buffer();         // 预渲染崩溃上下文（docs/09 §5）
    }
    mq_obs_counter_add(obs, "frames", 1.0);
    mq_obs_gauge_set(obs, "entities", scene ? (double)scene->count() : 0.0);
    mq_obs_gauge_set(obs, "draws", (double)draws.size());
    if (std::getenv("MAQUE_TRACE_TEXT")) {
        int textured = 0, solid = 0;
        for (const auto& it : draws) (it.texture >= 0 ? textured : solid)++;
        std::print("[text] draws={} textured={} solid={}\n", draws.size(), textured, solid);
    }
    if (!render_frame) {
        ++frame;
        return;
    }
    // ⑤ 渲染（只读快照 → 渲染不回写，docs/13 §8）
    const int SHpx = TH;
    const auto stat_render0 = std::chrono::steady_clock::now();
    {
        MQ_TRACY_ZONE_N(render, "render_textures");
        render_camera_textures(draws, spine_meshes);
    }
    /* CPU 像素只为离线输出、按需控制面抓帧或 SDL 呈现分配；WGPU 直呈常态保持空。 */
    const bool explicit_readback = !shot_path.empty() || !shot_seq.empty() || !frames_dir.empty();
    /* 控制面只在**真的有人要像素**时才回读：Capture.screen 会置 frame_capture_requested。
     * 旧实现 ctl_server 非空就每帧回读 —— 开着 inspector 的直呈会话等于白丢一次 GPU 同步。 */
    const bool inspector_readback = ctl_server != nullptr && frame_capture_requested;
    const bool need_pixels = explicit_readback || inspector_readback || frame_capture_requested || !direct_present;
    std::vector<uint8_t> pixels;
    if (need_pixels) pixels.resize((size_t)TW * SHpx * 4);
    /* 后处理（F4）默认**只叠加 3D 层**：只有 3D/场景 shader pass 存在时才需要走 frame3d 通路；
     * 纯 2D 场景仅在显式勾选「包含 2D / UI」（post_2d=1）时才改走 frame3d（整帧后处理）。
     * 关闭后处理时条件与旧实现完全一致（零回归）。 */
    const bool post_needs_frame3d = opt_post_enabled && opt_post_include_2d;
    if ((has_3d || has_shader_pass || has_shader_graph || post_needs_frame3d) && rapi && rself && rapi->render_frame3d) {
        /* M2：3D 场景 = 3D pass + 2D 覆盖层同帧（直呈时 out=null → present；否则离屏回读）
         * docs/13：场景自带全屏 shader pass 也走这条通路（纯 shader 场景 view_count=0 同样支持）。 */
        MQ_TRACY_ZONE_N(render, "frame.3d");
        std::vector<mq_renderer_view3d> views3d;
        std::vector<mq_renderer_light> lights3d;
        std::vector<mq_renderer_pbr_material> mats3d;
        std::vector<mq_renderer_draw3d> draws3d;
        std::vector<mq_renderer_skin> skins3d;
        extract_3d(TW, SHpx, views3d, lights3d, mats3d, draws3d, skins3d);
        if (opt_3d_enabled) {
            stat_3d_calls = (int)draws3d.size();
            for (const auto& dr : draws3d) {
                auto it = mesh_stats.find(dr.mesh);
                if (it == mesh_stats.end()) continue;
                stat_verts += (int)it->second.vertex_count;
                stat_tris += (int)(it->second.index_count / 3);
            }
        }
        stat_calls = stat_ui_calls + stat_3d_calls;
        std::vector<mq_renderer_shader_pass> passes3d;
        extract_shader_passes(passes3d);
        /* 多通道 shader 图（render.ShaderGraph）：按顺序执行各阶段，target<0 的画到主目标。 */
        std::vector<mq_renderer_shader_graph_pass> graphs3d;
        extract_shader_graphs(graphs3d);
        /* 精灵形态（width/height>0）：各自一条阶段链，gfx 先渲染到离屏纹理再合成 2D 图元。 */
        std::vector<mq_renderer_shader_graph_sprite> graph_sprites3d;
        extract_shader_graph_sprites(graph_sprites3d);
        /* shader 精灵（docs/13）：2D 图元式记录，gfx 侧先离屏渲染再合成（与 item 同层排序）。 */
        std::vector<mq_renderer_shader_sprite> sprites3d;
        extract_shader_sprites(sprites3d);
        mq_renderer_frame3d fr{};
        fr.width = TW;
        fr.height = SHpx;
        fr.views = views3d.data(); fr.view_count = opt_3d_enabled ? (uint32_t)views3d.size() : 0;
        fr.lights = lights3d.data(); fr.light_count = (uint32_t)lights3d.size();
        fr.materials = mats3d.data(); fr.material_count = (uint32_t)mats3d.size();
        fr.draws = draws3d.data(); fr.draw_count = opt_3d_enabled ? (uint32_t)draws3d.size() : 0;
        fr.skins = skins3d.data(); fr.skin_count = (uint32_t)skins3d.size();
        fr.views2d = views.data(); fr.view2d_count = (uint32_t)views.size();
        fr.items = draws.data(); fr.item_count = (uint32_t)draws.size();
        fr.meshes = spine_meshes.data(); fr.mesh_count = (uint32_t)spine_meshes.size();
        fr.passes = passes3d.data(); fr.pass_count = (uint32_t)passes3d.size();
        fr.graph_passes = graphs3d.data(); fr.graph_pass_count = (uint32_t)graphs3d.size();
        fr.graph_sprites = graph_sprites3d.data(); fr.graph_sprite_count = (uint32_t)graph_sprites3d.size();
        fr.shader_sprites = sprites3d.data(); fr.shader_sprite_count = (uint32_t)sprites3d.size();
        fr.present_x = 0;
        fr.present_y = 0;
        fr.present_w = 0;                         /* 0 = 整个 surface；目标等于屏幕时可直接渲染 */
        fr.present_h = 0;
        fr.render_mode = opt_render_mode;
        /* 后处理 PostEffect：纯视图层（F4 / MAQUE_ENGINE_OPTIONS）。enabled=0 → gfx 不建中间
         * 目标、不跑 pass（旧画面逐字节不变）；参数只在开启时被读取。 */
        engine_options_fill_post(fr.post);
        for (int i = 0; i < 4; ++i) fr.bg[i] = bg[i];
        /* MSAA：与每张 RenderTexture 同一解析链（Engine::resolve_msaa）——
         * F4 引擎选项 > MAQUE_MSAA > scn.Scene.msaa_samples（0/1=关、4=4x）。
         * 默认关 → 148 模型批量预览与旧场景逐像素不变；gfx 不支持 4x 时自动回退。 */
        fr.msaa_samples = resolve_msaa(0, false);
        opt_last_msaa = fr.msaa_samples;
        /* 面板回显（上一帧实况）：与 render_camera_textures 同一套解析，不额外决定任何东西。
         * 只统计启用中的 3D 目标 —— 2D 图元层恒单采样，把 2D 目标算进「4x 张数」会误导。 */
        opt_last_rt_total = 0;
        opt_last_rt_msaa4 = 0;
        for (const auto& s : render_textures) {
            if (!s.enabled || !s.is_3d) continue;
            ++opt_last_rt_total;
            if (resolve_msaa(s.msaa_samples, true) > 1) ++opt_last_rt_msaa4;
        }
        fr.env.sky_mode = 1;
        /* 环境光：scn.Scene.env_color/env_intensity + analytic studio IBL 参数（plans/19）。
         * env_sky_color/env_ground_color 全 0 = 由 env_color 派生（旧场景不改也有半球环境）。 */
        float env_col[3] = { 0.27F, 0.28F, 0.31F };
        float env_int = 1.6F;
        float sky_col[3] = { 0.0F, 0.0F, 0.0F };
        float ground_col[3] = { 0.0F, 0.0F, 0.0F };
        float env_hi = 1.0F;
        float env_rot_deg = 0.0F;
        if (scene) {
            env_col[0] = scene->desc.env_color.r;
            env_col[1] = scene->desc.env_color.g;
            env_col[2] = scene->desc.env_color.b;
            env_int = scene->desc.env_intensity;
            sky_col[0] = scene->desc.env_sky_color.r;
            sky_col[1] = scene->desc.env_sky_color.g;
            sky_col[2] = scene->desc.env_sky_color.b;
            ground_col[0] = scene->desc.env_ground_color.r;
            ground_col[1] = scene->desc.env_ground_color.g;
            ground_col[2] = scene->desc.env_ground_color.b;
            env_hi = scene->desc.env_highlight;
            env_rot_deg = scene->desc.env_rotation;
        }
        const bool sky_zero = (sky_col[0] == 0.0F && sky_col[1] == 0.0F && sky_col[2] == 0.0F);
        const bool ground_zero = (ground_col[0] == 0.0F && ground_col[1] == 0.0F && ground_col[2] == 0.0F);
        for (int i = 0; i < 3; ++i) {
            if (sky_zero) sky_col[i] = env_col[i] * 1.15F;
            if (ground_zero) ground_col[i] = env_col[i] * 0.45F;
            fr.env.env_color[i] = env_col[i];
            fr.env.env_sky_color[i] = sky_col[i];
            fr.env.env_ground_color[i] = ground_col[i];
        }
        fr.env.env_sky_color[3] = 1.0F;
        fr.env.env_ground_color[3] = 1.0F;
        fr.env.env_color[3] = 1.0F;
        fr.env.intensity = env_int;
        fr.env.env_highlight = env_hi;
        fr.env.env_rotation = env_rot_deg * 3.14159265F / 180.0F;
    /* envmap/IBL：显式 scene.env_texture 才启用（默认保留 analytic studio IBL 基线）；
     * MAQUE_ENV_IBL=1 强制用内建 studio_env，MAQUE_ENV_IBL=0 强制关闭。 */
    int32_t env_tex_id = -1, env_irr_id = -1;
    std::string env_path = scene->desc.env_texture;
    const char* env_force = std::getenv("MAQUE_ENV_IBL");
    if (env_force && env_force[0] == '1') env_path = "builtin:/textures/studio_env";
    if (env_force && env_force[0] == '0') env_path.clear();
    if (!env_path.empty()) {
        load_env_textures(env_path, env_tex_id, env_irr_id);
        if (env_tex_id < 0) {
            std::print(stderr, "runtime: env_texture '{}' 加载失败，回退 analytic IBL\n", env_path);
            env_tex_id = -1; env_irr_id = -1;
        }
    }
    /* 立方体环境贴图（第 26 轮）：env_cubemap 非空 → 6 面 cube（优先于 equirect），sky_mode=2。 */
    int32_t env_cube_id = -1, env_cube_irr_id = -1;
    if (!scene->desc.env_cubemap.empty()) {
        load_env_cubemap(scene->desc.env_cubemap, env_cube_id, env_cube_irr_id);
        if (env_cube_id < 0)
            std::print(stderr, "runtime: env_cubemap '{}' 不可用，回退 {} IBL\n",
                       scene->desc.env_cubemap, env_tex_id >= 0 ? "equirect" : "analytic");
    }
    fr.env.env_texture = env_tex_id;
    fr.env.env_irradiance_texture = env_irr_id;
    fr.env.env_cubemap = env_cube_id;
    fr.env.env_cubemap_irradiance = env_cube_irr_id;
    if (env_cube_id >= 0)
        fr.env.sky_mode = scene->desc.env_background ? 2 : 1;
    else
        fr.env.sky_mode = (env_tex_id >= 0 && scene->desc.env_background) ? 3 : 1;
        /* 静态帧跳过（docs/29 / docs/bugs/009）：画面输入完全不变时不再提交 GPU。
         * 只对"没有逐帧动画/外部像素消费者/覆盖层"的 3D 直呈帧启用；脚本/物理仍每帧推进。
         * 每 60 帧强制重画一次，兜住窗口 expose/驱动 surface 变化。 */
        bool skip3d = false;
        {
            const bool idle_ok =
                direct_present && !need_pixels && !frame_capture_requested &&
                !std::getenv("MAQUE_CAPTURE_WGPU") &&
                !imgui_on && !imgui_backend && !has_shader_pass && !has_shader_graph &&
                !gltf_dynamic && spines.empty() && particles.empty() &&
                morph_meshes.empty() && skins3d.empty() &&
                (render_textures.empty() || scene_visual_unchanged) && spine_meshes.empty() &&
                iframe.pointer_count == 0 && iframe.key_count == 0 && !iframe.resized;
            if (idle_ok) {
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p;
                    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
                };
                auto mixv = [&](const auto& v) { mix(v.data(), v.size() * sizeof(v[0])); };
                mix(&TW, sizeof TW); mix(&SHpx, sizeof SHpx);
                mix(bg, sizeof bg); mix(&opt_render_mode, sizeof opt_render_mode);
                mix(&fr.msaa_samples, sizeof fr.msaa_samples);
                mix(&fr.env, sizeof fr.env);
                mix(&fr.post, sizeof fr.post);      /* 后处理参数变化必须重画（否则面板调参无效果） */
                mixv(views); mixv(draws);
                mixv(views3d); mixv(lights3d); mixv(mats3d); mixv(draws3d);
                mixv(skins3d); mixv(passes3d);
                if (h == idle_render_hash && idle_render_frames < 60) {
                    skip3d = true;
                    ++idle_render_frames;
                } else {
                    idle_render_hash = h;
                    idle_render_frames = 0;
                }
            } else {
                idle_render_hash = 0;
                idle_render_frames = 0;
            }
        }
        if (!skip3d) {
        const bool direct3d = direct_present && !need_pixels;
        fr.out = direct3d ? nullptr : pixels.data();
        fr.out_size = (uint32_t)pixels.size();
        fr.present_after_readback = direct_present && need_pixels ? 1 : 0;
        const bool present_frame = direct3d || fr.present_after_readback != 0;
        const int32_t rc = rapi->render_frame3d(rself, &fr);
        if (rc == 0 && present_frame) {
            if (rapi->present) rapi->present(rself);
        } else if (rc != 0 && !(present_frame && rc == -7)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::print(stderr, "runtime: 3D frame 渲染失败 rc={} → 本帧回退 2D\n", rc);
            }
            if (present_frame) {
                mq_renderer_window none{};
                (void)rapi->attach_window(rself, &none);
                direct_present = false;
                direct_requested = false;
                direct_window_handle = nullptr;
            } else {
                has_3d = false;
            }
        }
        }  /* !skip3d */
    } else if (direct_present && rapi && rself && rapi->render_frame3d) {
        MQ_TRACY_ZONE_N(render, "frame.direct");
        mq_renderer_frame3d f{};
        f.width = TW;
        f.height = SHpx;
        f.views2d = views.data();
        f.view2d_count = (uint32_t)views.size();
        f.items = draws.data();
        f.item_count = (uint32_t)draws.size();
        f.meshes = spine_meshes.data();
        f.mesh_count = (uint32_t)spine_meshes.size();
        f.present_x = 0;
        f.present_y = 0;
        f.present_w = 0;
        f.present_h = 0;
        f.out = need_pixels ? pixels.data() : nullptr;
        f.out_size = (uint32_t)pixels.size();
        f.present_after_readback = need_pixels ? 1 : 0;
        for (int i = 0; i < 4; ++i) f.bg[i] = bg[i];
        f.env.sky_mode = 1;                       /* M1：背景色直出（3D 天空盒在 M3） */
        f.env.env_texture = -1;                   /* 2D 直呈路径无 3D env（避免 0 号纹理被误当 env） */
        f.env.env_irradiance_texture = -1;
        for (int i = 0; i < 4; ++i) f.env.sky_color[i] = bg[i];
        bool skip2d = false;
        {
            const bool idle_ok =
                direct_present && !need_pixels && !frame_capture_requested &&
                !std::getenv("MAQUE_CAPTURE_WGPU") &&
                !imgui_on && !imgui_backend && !has_shader_pass && !has_shader_graph &&
                spine_meshes.empty() && particles.empty() && render_textures.empty() &&
                iframe.pointer_count == 0 && iframe.key_count == 0 && !iframe.resized;
            if (idle_ok) {
                uint64_t h = 1099511628211ull;
                auto mix = [&](const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p;
                    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
                };
                auto mixv = [&](const auto& v) { mix(v.data(), v.size() * sizeof(v[0])); };
                mix(&TW, sizeof TW); mix(&SHpx, sizeof SHpx); mix(bg, sizeof bg);
                mixv(views); mixv(draws);
                if (h == idle_render_hash && idle_render_frames < 60) {
                    skip2d = true;
                    ++idle_render_frames;
                } else {
                    idle_render_hash = h;
                    idle_render_frames = 0;
                }
            } else {
                idle_render_hash = 0;
                idle_render_frames = 0;
            }
        }
        if (!skip2d) {
        const bool present_frame = !need_pixels || f.present_after_readback != 0;
        const int32_t rc = rapi->render_frame3d(rself, &f);
        if (rc == 0 && present_frame && rapi->present) rapi->present(rself);
        else if (rc != 0 && !(present_frame && rc == -7)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::print(stderr, "runtime: direct present 失败 rc={} → 回退 SDL 呈现\n", rc);
            }
            mq_renderer_window none{};
            (void)rapi->attach_window(rself, &none);
            direct_present = false;
            direct_requested = false;
            direct_window_handle = nullptr;
        }
        }  /* !skip2d */
    } else if (rapi && rself) {
        MQ_TRACY_ZONE_N(render, "frame");   // Tracy：渲染插件成帧 + 回读（颜色 = render 模块）
        /* 有网格（骨骼动画）时走 render_mesh_views（图元 + 网格一次成帧、统一 layer/z 排序）；
         * 没有网格时保持原路径（老插件/纯精灵场景零行为变化）。 */
        if (!spine_meshes.empty() && rapi->render_mesh_views)
            rapi->render_mesh_views(rself, TW, SHpx, views.data(), (uint32_t)views.size(),
                                    draws.data(), (uint32_t)draws.size(),
                                    spine_meshes.data(), (uint32_t)spine_meshes.size(),
                                    bg, pixels.data(), (uint32_t)pixels.size());
        else
            rapi->render_views(rself, TW, SHpx, views.data(), (uint32_t)views.size(),
                               draws.data(), (uint32_t)draws.size(), bg, pixels.data(), (uint32_t)pixels.size());
        if (std::getenv("MAQUE_TRACE_SPINE") && (frame % 20 == 0))
            std::print("[spine] render frame={} items={} meshes={}\n",
                       (long long)frame, draws.size(), spine_meshes.size());
    }
    /* stats：渲染耗时（到 present 之前）+ 整帧耗时 → FPS；EMA 平滑避免数字乱跳。 */
    {
        const auto now = std::chrono::steady_clock::now();
        const double rms = std::chrono::duration<double, std::milli>(now - stat_render0).count();
        const double lms = std::chrono::duration<double, std::milli>(now - stat_t0).count() - rms;
        stat_render_ms = stat_render_ms > 0 ? (float)(stat_render_ms * 0.9 + rms * 0.1) : (float)rms;
        stat_logic_ms = stat_logic_ms > 0 ? (float)(stat_logic_ms * 0.9 + lms * 0.1) : (float)lms;
    }
    const auto stat_present0 = std::chrono::steady_clock::now();
    /* 呈现（docs/05、M5）：**游戏帧与 ImGui 面板共用同一个 SDL_Renderer，只 present 一次**。
     *   ① mq_platform_draw_rect：引擎帧（CPU 像素）→ renderer 后台缓冲（游戏画面）
     *   ② mq_imgui_backend_render：ImGui 官方后端在同一后台缓冲上再画一趟（调试面板，
     *      独立 render pass —— ImGui 的所有指令在脚本阶段只是**录制**，到这里才真正渲染）
     *   ③ mq_platform_present_ui：抓帧（MAQUE_CAPTURE）+ 单次 SDL_RenderPresent → 一起上屏
     * 这样不会再出现「先写窗口表面、又被 renderer present 覆盖 → 游戏画面不上屏」。
     * 无窗口 / MAQUE_PRESENT=off 时各步骤均为安全空操作。 */
    if (!direct_present && platform && window && rapi && rself) {
        MQ_TRACY_ZONE_N(platform, "present");   // Tracy：上传纹理 + ImGui pass + 单次 present
        /* 渲染目标已经包含 Canvas viewport/留边；这里只做目标到 drawable 的最终输出缩放。 */
        const int dx = 0, dy = 0;
        const int dw = (int)(canvas.window_w + 0.5F), dh = (int)(canvas.window_h + 0.5F);
        mq_platform_draw_rect(platform, pixels.data(), TW, SHpx, dx, dy, dw, dh, -1);
        if (imgui_on && imgui_backend) mq_imgui_backend_render();   // ② ImGui 独立 pass（叠在游戏帧上）
        mq_platform_present_ui(platform);                            // ③ 单次 present（含抓帧）
    }
    /* 诊断：MAQUE_TRACE_STATS=1 时每 30 帧打印一次逐项耗时（logic / render / present）。
     * 三项相加 ≈ 每步成本，**屏幕 FPS ≈ 1000 / 该值**（主循环每个模拟步都渲染+present 一次）。
     * 用来回答"这个场景为什么只有 N fps"：先看是逻辑（脚本/Spine/文本）还是渲染/呈现。 */
    {
        const double pms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stat_present0).count();
        stat_present_ms = stat_present_ms > 0 ? (float)(stat_present_ms * 0.9 + pms * 0.1) : (float)pms;
        if (const char* ts = std::getenv("MAQUE_TRACE_STATS"); ts && *ts && *ts != '0' && frame % 30 == 0)
            std::print("[stats] frame={} fps={:.1f} total={:.1f}ms logic={:.1f} render={:.1f} present={:.1f} "
                       "draws={} ui_draws={} 3d_draws={} tris={} verts={} vram_bytes={} target={}x{} imgui={}\n",
                       (long long)frame, stat_fps,
                       stat_logic_ms + stat_render_ms + stat_present_ms,
                       stat_logic_ms, stat_render_ms, stat_present_ms,
                       stat_calls, stat_ui_calls, stat_3d_calls, stat_tris, stat_verts, stat_vram_bytes,
                       TW, SHpx, imgui_on ? 1 : 0);
    }
    /* Tracy：**基础数据**（帧时间/逻辑/渲染/呈现/FPS/draw/实体）+ 帧标记。
     * 未启动 Tracy 时全部 no-op（每次只是一个原子读 + 返回）。 */
    MQ_TRACY_FRAME();
    MQ_TRACY_PLOT("frametime", stat_logic_ms + stat_render_ms + stat_present_ms);
    MQ_TRACY_PLOT("logic", stat_logic_ms);
    MQ_TRACY_PLOT("render", stat_render_ms);
    MQ_TRACY_PLOT("present", stat_present_ms);
    MQ_TRACY_PLOT("fps", stat_fps);
    MQ_TRACY_PLOT("draws", stat_calls);
    MQ_TRACY_PLOT("entities", scene ? (double)scene->count() : 0.0);
    /* Tracy：内存域（docs/29 §10）。每 MAQUE_TRACY_MEM_INTERVAL（默认 1s）采样一次：
     * node / texture / gpu / font 是"读当前真相"的采样型域；script / heap 由分配器钩子实时记账。
     * memory_due 在关闭（MAQUE_TRACY_MEM=0）或未编入时恒 false → 连采样都不做。 */
    if (maque::tracy::memory_due(dt)) {
        memory_sample();
        maque::tracy::memory_publish();
    }
    if (!frames_dir.empty()) {
        char fn[64]; std::snprintf(fn, sizeof(fn), "/frame_%04d.ppm", (int)frame);
        write_ppm(frames_dir + fn, TW, SHpx, pixels);
    }
    /* 截图（--shot / MAQUE_SHOT）：渲染到指定帧后写一张图片并按扩展名选格式，随后退出 */
    if (!shot_path.empty() && (int)frame == shot_frame) {
        const bool png = shot_path.size() > 4 && shot_path.compare(shot_path.size() - 4, 4, ".png") == 0;
        const int rc = png ? mq_image_write_png(shot_path.c_str(), (uint32_t)TW, (uint32_t)SHpx,
                                                pixels.data(), (uint32_t)TW * 4u)
                           : (write_ppm(shot_path, TW, SHpx, pixels) ? 0 : 1);
        if (rc == 0) std::print("runtime: shot '{}' {}x{} frame={}\n", shot_path, TW, SHpx, (int)frame);
        else std::print(stderr, "runtime: shot 写入失败 '{}'\n", shot_path);
        quit = true;                                        // 单张截图为一次性任务（CI/预览：一条命令即得图）
    }
    /* 批量截图（--shot-seq）：按帧号列表 or 每 N 帧拍一张。写完**最后一张**才退出 ——
     * 这样一条命令就能拿到"第 1/30/60 帧"三张图，用来验证动画推进、物理轨迹、UI 变化。 */
    if (!shot_seq.empty() && !shot_list.empty()) {
        const bool due = std::find(shot_list.begin(), shot_list.end(), (int)frame) != shot_list.end();
        if (due) write_seq_shot(pixels, TW, SHpx);
    } else if (!shot_seq.empty() && shot_every > 0) {
        /* 从第 shot_every 帧开始，每 shot_every 帧一张（第 0 帧通常还没布局完）。 */
        if ((int)frame > 0 && (int)frame % shot_every == 0) write_seq_shot(pixels, TW, SHpx);
    }
    if ((ctl_server || frame_capture_requested) && !pixels.empty()) {
        frame_pixels = pixels;
        frame_w = TW;
        frame_h = SHpx;
        frame_capture_requested = false;
    }
    ++frame;
}

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
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + rel;
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK || !d) {
        std::print(stderr, "runtime: overlay script not found '{}'\n", rel);
        mq_obs_event(obs, "runtime", 3, "overlay_script_missing");
        return false;
    }
    std::string src((char*)d, n);
    mq_vfs_free(d);
    const char* lang = mq_script_lang_for_path(rel.c_str());
    mq_script_vm* one = mq_script_vm_create_lang(lang);
    if (!one) {
        std::print(stderr, "runtime: overlay script backend '{}' unavailable for '{}'\n", lang, rel);
        mq_obs_event(obs, "runtime", 3, "overlay_script_backend");
        return false;
    }
    mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(one, this);
    if (mq_script_eval(one, src.c_str(), rel.c_str()) != 0) {
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
    mq_script_vm* fresh = mq_script_vm_create_lang(lang);
    if (!fresh) return false;
    mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(fresh, this);
    if (mq_script_eval(fresh, src.c_str(), overlay_rel.c_str()) != 0) {
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
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + rel;
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK || !d) {
        std::print(stderr, "runtime: global script not found '{}'\n", rel);
        mq_obs_event(obs, "runtime", 3, "global_script_missing");
        return false;
    }
    std::string src((char*)d, n);
    mq_vfs_free(d);
    const char* lang = mq_script_lang_for_path(rel.c_str());
    mq_script_vm* one = mq_script_vm_create_lang(lang);
    if (!one) {
        std::print(stderr, "runtime: global script backend '{}' unavailable for '{}'\n", lang, rel);
        mq_obs_event(obs, "runtime", 3, "global_script_backend");
        return false;
    }
    mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(one, this);
    /* 顶层代码（默认值/初始化）也在"全局脚本上下文"里执行：node_on 归属 -3，
     * 且不能误以为自己是某个场景实例。 */
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_g = in_global;
    const bool prev_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const int rc = mq_script_eval(one, src.c_str(), rel.c_str());
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
    mq_script_vm* fresh = mq_script_vm_create_lang(lang);
    if (!fresh) return false;
    mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(fresh, this);
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_g = in_global;
    const bool prev_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const int rc = mq_script_eval(fresh, src.c_str(), global_rel.c_str());
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

/* 载入回放日志（MAQUE_REPLAY）：此后所有步进都只消费记录（设备输入被忽略）。 */
bool Engine::prepare_replay() {
    const char* rp = std::getenv("MAQUE_REPLAY");
    if (!rp || !input) return false;
    std::ifstream f(rp, std::ios::binary);
    if (!f) { std::print("runtime: cannot open replay log '{}'\n", rp); return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty() || mq_input_log_load_json(input, text.c_str()) != 0) {
        std::print("runtime: cannot load replay log '{}'\n", rp);
        return false;
    }
    const char* mf = mq_input_log_get_meta(input, "frames");
    std::print("runtime: replay '{}' ({} frames expected)\n", rp, mf ? mf : "?");
    return true;
}

/* 控制面 Runtime.step：确定性推进 n 帧（AI/CI 驱动同一帧序）。 */
bool Engine::run_frames(uint64_t n) {
    for (uint64_t i = 0; i < n && !quit; ++i) {
#ifdef MQ_RENDERER_STATIC_NULL
        step_frame(false);
#else
        step_frame(true);
#endif
    }
    return !quit;
}

/* 检查点快照（docs/10 §5 分歧定位）：环状保存最近 N 个（默认每 30 帧），
 * 回放失步时可用 Replay.seek 跳回任一检查点做二分/逐步定位。 */
void Engine::checkpoint_push() {
    if (checkpoints.empty()) {
        checkpoints.reserve(cp_count);
        for (uint32_t i = 0; i < cp_count; ++i) checkpoints.push_back(mq_snapshot_create());
    }
    mq_snapshot* s = checkpoints[cp_cursor % checkpoints.size()];
    cp_cursor++;
    if (!world_snapshot(s)) return;
    mq_snapshot_set_frame(s, frame);
    mq_snapshot_set_desc(s, ("checkpoint@" + std::to_string(frame)).c_str());
}

/* 崩溃/异常上下文（docs/09 §5）：把"精确重放所需的一切"落盘为 JSON —— 
 * seed、场景、帧号、最近检查点、输入日志（若在录制）、世界校验和、指标。
 * 触发点：① 脚本异常（可重放定位）；② 信号（abort/段错误，见 install_crash_handler）。 */
/* 把崩溃上下文预渲染进信号安全缓冲并确保报告 fd 已打开（每帧边界调用）。 */
void Engine::refresh_crash_buffer() {
    if (g_crash_fd < 0) {
        const char* out = std::getenv("MAQUE_CRASH_REPORT");
        if (!out || !*out) return;
        g_crash_fd = maque::compat::open_fd(out, maque::compat::kOpenWriteCreate);
        if (g_crash_fd < 0) return;
    }
    uint64_t cp_frame = 0, cp_sum = 0;
    for (auto* s : checkpoints)
        if (s && mq_snapshot_frame(s) > cp_frame) { cp_frame = mq_snapshot_frame(s); cp_sum = mq_snapshot_checksum(s); }
    const int n = std::snprintf(g_crash_buf, sizeof g_crash_buf,
        "{\"reason\":\"fatal signal\",\"scene\":\"%s\",\"frame\":%llu,\"seed\":%u,"
        "\"world_trail\":\"%llu\",\"checkpoint\":{\"frame\":%llu,\"checksum\":\"%llu\"},\n"
        " \"replay\":{\"command\":\"MAQUE_REPLAY=<log> %s\"},\n \"metrics\":%s}\n",
        (scn_full_path.empty() ? scn_vfs_path : scn_full_path).c_str(),
        (unsigned long long)frame, scene ? scene->desc.seed : 0u,
        (unsigned long long)world_trail(),
        (unsigned long long)cp_frame, (unsigned long long)cp_sum,
        (scn_full_path.empty() ? scn_vfs_path : scn_full_path).c_str(),
        mq_obs_dump_json(obs));
    g_crash_len = n > 0 ? (n < (int)sizeof g_crash_buf ? n : (int)sizeof g_crash_buf - 1) : 0;
    if (g_crash_fd >= 0 && g_crash_len > 0) {          // 同步落盘（保证即使不崩也有当前快照）
        const maque::compat::ssize_t ig =
            maque::compat::pwrite_fd(g_crash_fd, g_crash_buf, (size_t)g_crash_len, 0);
        (void)ig;
    }
}

void Engine::write_crash_context(const char* why) {
    const char* out = std::getenv("MAQUE_CRASH_REPORT");
    if (!out || !*out) return;
    std::ofstream f(out, std::ios::binary);
    if (!f) return;
    uint64_t trail = world_trail();
    /* 最近检查点 */
    uint64_t cp_frame = 0, cp_sum = 0;
    for (auto* s : checkpoints)
        if (s && mq_snapshot_frame(s) > cp_frame) { cp_frame = mq_snapshot_frame(s); cp_sum = mq_snapshot_checksum(s); }
    std::string json = "{\n";
    json += "  \"reason\": ";
    write_json_string_pub(json, why ? why : "unknown");
    json += ",\n  \"scene\": ";
    write_json_string_pub(json, scn_full_path.empty() ? scn_vfs_path : scn_full_path);
    json += ",\n  \"frame\": " + std::to_string(frame);
    json += ",\n  \"seed\": " + std::to_string(scene ? scene->desc.seed : 0);
    json += ",\n  \"world_trail\": \"" + std::to_string(trail) + "\"";
    json += ",\n  \"last_checksum\": \"" + std::to_string(last_checksum) + "\"";
    json += ",\n  \"checkpoint\": {\"frame\": " + std::to_string(cp_frame) +
            ", \"checksum\": \"" + std::to_string(cp_sum) + "\"}";
    json += ",\n  \"replay\": {\"command\": \"MAQUE_REPLAY=<log> ";
    json += scn_vfs_path + "\", \"recorded\": ";
    json += (input && mq_input_recording(input)) ? "true" : "false";
    json += "}";
    json += ",\n  \"metrics\": " + std::string(mq_obs_dump_json(obs)) + "\n}\n";
    f << json;
    std::print(stderr, "runtime: crash context → '{}' (frame={} reason={})\n", out, frame, why ? why : "?");
}

/* 导出世界快照（崩溃复现/AI undo 的落点，docs/10 §7）。控制面每次 step 后也会调用。 */
void Engine::export_final_snapshot() {
    const char* sp = std::getenv("MAQUE_SNAPSHOT");
    if (!sp || !snaps) return;
    if (!snap_last) snap_last = mq_snapshot_pool_acquire(snaps);
    if (!world_snapshot(snap_last)) return;
    size_t n = 0;
    const void* b = mq_snapshot_bytes(snap_last, &n);
    std::ofstream f(sp, std::ios::binary);
    if (f && b && n) {
        mq_snapshot_set_frame(snap_last, frame);
        f.write((const char*)b, (std::streamsize)n);
        std::print("runtime: snapshot → '{}' ({} bytes, frame {})\n", sp, n, frame);
    }
}

/* 终局导出：快照 + 录制日志（frames/trail 元数据 → 回放失步检测的基准）。 */
void Engine::finalize_run() {
    if (const char* td = std::getenv("MAQUE_TRAIL_OUT"); td && *td) {   // 失步定位：逐帧校验和（docs/10 §5）
        std::ofstream f(td, std::ios::binary);
        for (auto& [fr, sum] : trail) f << fr << " " << sum << "\n";
    }
    if (snaps) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
    export_final_snapshot();
    if (input && mq_input_recording(input)) {
        mq_input_log_set_meta(input, "frames", std::to_string(frame).c_str());
        char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)world_trail());
        mq_input_log_set_meta(input, "trail", tb);
        if (const char* rp = std::getenv("MAQUE_RECORD")) {
            std::ofstream f(rp, std::ios::binary);
            const char* json = mq_input_log_json(input);
            if (f && json) f << json;
            std::print("runtime: recorded input log → '{}' ({} frames)\n", rp, frame);
        }
        mq_input_record_end(input);
    }
}

void Engine::main_loop(const std::string& frames_dir, const std::string& name) {
    this->frames_dir = frames_dir;
    this->name = name;
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);
    uint64_t cap = 60 * 60;
    if (const char* mf = std::getenv("MAQUE_MAX_FRAMES")) { long v = std::strtol(mf, nullptr, 10); if (v > 0) cap = (uint64_t)v; }
    if (const char* sf = std::getenv("MAQUE_STOP_FRAME")) { long v = std::strtol(sf, nullptr, 10); if (v > 0) { cap = (uint64_t)v + 1; stop_frame = (uint64_t)v; } }

    /* 回放（docs/10 §3/§5）：输入是数据 → 同一日志应逐帧复现 */
    std::string replay_trail;
    if (prepare_replay()) {
        if (const char* mt = input ? mq_input_log_get_meta(input, "trail") : nullptr) replay_trail = mt;
        /* 日志元数据 frames = **已执行帧数**（finalize_run 写入 loop 计数）→ 回放跑同样多帧；
         * 曾经 +1（误当"末帧索引"）导致回放多跑一帧、末尾校验和必然不等（docs/10 §5）。 */
        if (const char* mf = input ? mq_input_log_get_meta(input, "frames") : nullptr) {
            const uint64_t n = std::strtoull(mf, nullptr, 10);
            if (n > 0) cap = n;
        }
    }
    if (input && std::getenv("MAQUE_RECORD")) mq_input_record_begin(input);

    mq_obs_event(obs, "runtime", 1, "loop_start");
    frame = 0;

    /* 墙钟限速（见 pace_wall_clock 注释）：只在"真窗口 + 非自动化"时启用。
     * 判定为自动化、因此**不限速**（保持"尽快出结果"的既有语义）的情形：
     *   · 无窗口（headless / MAQUE_WINDOW=0x0 / 无显示环境）
     *   · --shot / MAQUE_SHOT（只求一张图，帧数与时间无关）
     *   · 录制/回放（docs/10 §3/§5：要求逐帧复现，不该受墙钟影响）
     * 注意 `MAQUE_MAX_FRAMES`/`MAQUE_STOP_FRAME` **不算**自动化信号 —— 它们只是"跑多少帧"
     * 的上限，与"要不要按真实时间跑"无关（无头跑它们本来就是全速的）。 */
    const bool shot_mode = std::getenv("MAQUE_SHOT") != nullptr;
    const bool recorded = std::getenv("MAQUE_RECORD") != nullptr || !replay_trail.empty();
    pace_wall_clock = window && !shot_mode && !recorded;
    if (const char* pc = std::getenv("MAQUE_PACE")) {          // 显式覆盖：0/off 关闭，1/on 强制开启
        const std::string v(pc);
        if (v == "0" || v == "off") pace_wall_clock = false;
        else if (v == "1" || v == "on") pace_wall_clock = true;
    }
    /* 墙钟限速：**绝对时间基准**（仿真时间 = frame × dt，目标墙钟时刻 = pace_start + 仿真时间）。
     * 每轮按"落后多少"决定补几步（1..8），跑完睡到"下一帧应开始"的时刻：
     *   · deadline 固定推进 → 睡眠过冲下一轮用更短的睡眠吸收，不累积漂移；
     *   · 补步只让**最后一步**成帧（见下）→ 补步不再"越补越慢"；
     *   · 落后超过 8 步（卡顿/断点/挂起）才重锚：丢弃积压，宁可让游戏时间变慢，也不爆追。 */
    auto pace_start = std::chrono::steady_clock::now();
    const auto pace_t0 = pace_start;              // 真实起点（诊断用；pace_start 会因重锚前移）
    double pace_sleep = 0.0;
    double pace_step_cost = 0.0;                  // 上一轮实测的"单步成本"（秒）→ 补步预算
    uint64_t pace_reanchor = 0;
    uint64_t pace_steps = 0;
    /* 逐帧输出（截图/抓帧/逐帧导出）必须**每帧都成帧**，不允许把两步合成一帧 —— 否则
     * `--shot-frame N` / `MAQUE_CAPTURE` 的帧号与画面会对不上。 */
    const bool per_frame_output = !shot_path.empty() || !shot_seq.empty() || !frames_dir.empty() ||
                                  std::getenv("MAQUE_CAPTURE") != nullptr;

    for (; frame < cap && !quit; ) {
        MQ_TRACY_ZONE_N(runtime, "loop");   // Tracy：一轮主循环（含可能的多步补偿 + 墙钟 sleep）
        if (stop_frame && frame > stop_frame) break;
        if (g_quit_requested) {                          // SIGTERM/SIGINT → 优雅退出（正常收尾 + 清理端点）
            quit = true;
            mq_obs_event(obs, "runtime", 1, "terminated");
            break;
        }
        /* 墙钟 → 固定步数（docs/10 D1 的确定性不变：**步长仍恒为 1/60**，变的只是
         * "这一轮屏幕刷新要跑几步"）。两个方向都要处理：
         *   · 机器比实时快 → 只跑 1 步会让动画跟着快（实测 2.7x）→ 用 sleep 压到 1x；
         *   · 机器比实时慢（渲染每帧 ~29ms 实测）→ 只跑 1 步会让动画跟着慢（实测 0.58x）
         *     → 这一轮多补几步，让**游戏时间始终跟墙钟走**。
         * 上限 8 步、超出即重锚（见上）。 */
        int steps_this_frame = 1;
        if (pace_wall_clock && !per_frame_output) {
            constexpr double kDt = 1.0 / 60.0;
            const auto now = std::chrono::steady_clock::now();
            double lag = std::chrono::duration<double>(now - pace_start).count() - (double)frame * kDt;
            const auto drop_backlog = [&](bool stall) {
                /* 丢弃积压 = 把"当前时刻"重新锚成仿真时刻（游戏时间变慢，但不再爆追）。 */
                pace_start += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(lag));
                lag = 0.0;
                if (stall) ++pace_reanchor;
            };
            if (lag > 8.0 * kDt) {
                drop_backlog(true);                  // 卡顿/断点/挂起
            } else if (pace_step_cost > kDt && lag > kDt) {
                /* 上一轮单步成本已超过 dt：这台机器本来就跑不动 1x，补步只会越补越慢 →
                 * 丢掉积压，按"每轮 1 步"的最快速度跑（Tracy 里看速率 <1x 是事实，不是抖动）。 */
                drop_backlog(false);
            }
            steps_this_frame = 1 + (int)std::floor(lag / kDt);
            /* 补步预算：一轮最多多做 ≈1 帧的工作量。上一轮实测单步成本已经 ≈dt 时，多跑一步只会
             * 把这一轮拖得更长（"越补越慢"的自激：Debug+ASan 下重文本场景实测 0.52x / 29 次重锚）。 */
            if (pace_step_cost > 0.0) {
                const int budget = (int)(kDt / pace_step_cost);
                if (steps_this_frame - 1 > budget) steps_this_frame = budget + 1;
            }
            if (steps_this_frame < 1) steps_this_frame = 1;
            if (steps_this_frame > 8) steps_this_frame = 8;
        }
        const auto pace_work0 = std::chrono::steady_clock::now();
        int pace_ran = 0;
        for (int s = 0; s < steps_this_frame && frame < cap && !quit; ++s) {
            if (stop_frame && frame > stop_frame) break;
            /* 补步：只有**最后一步**成帧（渲染 + present + Tracy FrameMark），中间步只推进模拟。
             * 旧实现每步都完整 render+present → 补 2 步 = 渲染两帧，越补越慢；
             * Tracy 里就表现为 22.25/15.32/21.42/11.59ms 这种"长短帧成对"的抖动。 */
            step_frame(s + 1 == steps_this_frame);
            ++pace_steps;
            ++pace_ran;
        }
        if (pace_wall_clock && pace_ran > 0)
            pace_step_cost = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - pace_work0).count() / pace_ran;
        if (pace_wall_clock) {
            /* 睡到"下一帧应开始"的绝对时刻；落后（due 已过）就不睡，由上一段的补步追。 */
            const auto due = pace_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>((double)frame / 60.0));
            const auto now = std::chrono::steady_clock::now();
            if (due > now) {
                const auto d = due - now;
                pace_sleep += std::chrono::duration<double>(d).count();
                std::this_thread::sleep_for(d);
            }
        }
        if (g_signal_hit) {                              // 收到终止信号 → 落盘上下文后退出
            write_crash_context(g_signal_name ? g_signal_name : "signal");
            mq_obs_event(obs, "runtime", 3, g_signal_name ? g_signal_name : "signal");
            break;
        }
    }

    /* 循环是因为"到帧数上限"退出的 → 记下来（退出码按成功算，见 frames_capped 注释）。 */
    if (!quit && (frame >= cap || (stop_frame && frame > stop_frame))) frames_capped = true;

    finalize_run();

    if (const char* td = std::getenv("MAQUE_TRAIL_OUT"); td && *td) {   // 失步定位（docs/10 §5）
        std::ofstream f(td, std::ios::binary);
        for (auto& [fr, sum] : trail) f << fr << " " << sum << "\n";
    }
    // 失步检测：回放必须有记录里的校验和（docs/10 §5）
    if (!replay_trail.empty()) {
        char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)world_trail());
        if (replay_trail == tb) std::print("runtime: replay matches recorded trail ({})\n", tb);
        else std::print("runtime: DESYNC — replay trail {} != recorded {}\n", tb, replay_trail);
    }
    mq_obs_gauge_set(obs, "frames_total", (double)frame);
    if (const char* pt = std::getenv("MAQUE_TRACE_PACE")) {
        (void)pt;
        /* 墙钟从**真实起点**算（pace_start 会因重锚前移，用它算速率会虚高）。 */
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - pace_t0).count();
        std::print("[pace] frames={} 墙钟={:.3f}s 仿真={:.3f}s 速率={:.2f}x wall_clock={} 睡觉={:.3f}s 重锚定={} 次\n",
                   (long long)frame, wall, (double)frame / 60.0, wall > 0 ? (frame / 60.0) / wall : 0.0,
                   pace_wall_clock ? 1 : 0, pace_sleep, (long long)pace_reanchor);
    }
    mq_obs_event(obs, "runtime", 1, quit ? "quit" : "timeout");
    std::print("runtime: '{}' exited after {} frames (quit={}) world_trail=0x{:016x}\n",
               name, frame, quit ? 1 : 0, world_trail());
}

/* ---- Scene（场景树：Node + Components）---- */
int Scene::spawn() {
    const int idx = next_id++;                   // 单调：永不复用（陈旧 id 不会别名新节点）
    nodes.emplace(idx, SceneNode{});
    roots.insert(std::lower_bound(roots.begin(), roots.end(), idx), idx);
    return idx;
}
SceneNode* Scene::at(int index) {
    if (index < 0) return nullptr;
    auto it = nodes.find(index);
    return it == nodes.end() ? nullptr : &it->second;
}
const SceneNode* Scene::at(int index) const {
    if (index < 0) return nullptr;
    auto it = nodes.find(index);
    return it == nodes.end() ? nullptr : &it->second;
}

/* 挂到 parent 下（parent<0 = 场景根）。children 保持 id 升序：
 * 加载/动态创建天然递增 → 顺序 == 声明序；重挂时也维持旧 childAt 的确定性语义。 */
void Scene::attach(int child, int parent) {
    if (child < 0) return;
    auto cit = nodes.find(child);
    if (cit == nodes.end()) return;
    if (cit->second.parent == parent) return;
    detach(child);
    cit = nodes.find(child);                     // detach 只改容器内容，不 erase 节点
    if (parent >= 0 && parent != child) {
        auto pit = nodes.find(parent);
        if (pit != nodes.end()) {
            auto& kids = pit->second.children;
            kids.insert(std::lower_bound(kids.begin(), kids.end(), child), child);
            cit->second.parent = parent;
            return;
        }
    }
    roots.insert(std::lower_bound(roots.begin(), roots.end(), child), child);
    cit->second.parent = -1;
}

void Scene::detach(int child) {
    auto it = nodes.find(child);
    if (it == nodes.end()) return;
    const int p = it->second.parent;
    if (p >= 0) {
        auto pit = nodes.find(p);
        if (pit != nodes.end()) {
            auto& kids = pit->second.children;
            auto k = std::lower_bound(kids.begin(), kids.end(), child);
            if (k != kids.end() && *k == child) kids.erase(k);
        }
    }
    auto r = std::lower_bound(roots.begin(), roots.end(), child);
    if (r != roots.end() && *r == child) roots.erase(r);
    it->second.parent = -1;
}

/* 从 parent 字段重建整棵树（快照 decode / 反序列化后调用）。
 * 脏数据保护：父不存在、自环、祖先环 → 该节点降为根（绝不产生环或悬垂 children）。 */
void Scene::rebuild_tree() {
    roots.clear();
    std::vector<int> ids;
    ids.reserve(nodes.size());
    for (auto& [id, n] : nodes) { n.children.clear(); ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        SceneNode& n = nodes[id];
        const int p = n.parent;
        if (p == id || p < 0 || nodes.find(p) == nodes.end()) { n.parent = -1; roots.push_back(id); continue; }
        bool cycle = false;
        for (int cur = p, guard = 0; cur >= 0 && guard < 4096; ++guard) {
            if (cur == id) { cycle = true; break; }
            auto ci = nodes.find(cur);
            if (ci == nodes.end()) break;
            cur = ci->second.parent;
        }
        if (cycle) { n.parent = -1; roots.push_back(id); continue; }
        nodes[p].children.push_back(id);
    }
    std::sort(roots.begin(), roots.end());
    for (auto& [id, n] : nodes) std::sort(n.children.begin(), n.children.end());
}

int Scene::child_count(int parent) const {
    if (parent < 0) return (int)roots.size();
    auto it = nodes.find(parent);
    return it == nodes.end() ? 0 : (int)it->second.children.size();
}

int Scene::child_at(int parent, int index) const {
    if (index < 0) return -1;
    if (parent < 0) return index < (int)roots.size() ? roots[(size_t)index] : -1;
    auto it = nodes.find(parent);
    if (it == nodes.end() || index >= (int)it->second.children.size()) return -1;
    return it->second.children[(size_t)index];
}

void Scene::remove(int index) {
    if (nodes.find(index) == nodes.end()) {
        if (eng) {
            eng->entity_instance.erase(index);
            eng->node_events.erase(index);
        }
        return;
    }
    /* 销毁**整棵子树**（Node 树语义：父节点消失则后代一并消失）。
     * 先收集后删除：组件析构/事件表清理都按 id 进行，不能边遍历边 erase。 */
    std::vector<int> doomed;
    std::unordered_set<int> seen;
    std::vector<int> stack{index};
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        auto it = nodes.find(cur);
        if (it == nodes.end() || !seen.insert(cur).second) continue;
        doomed.push_back(cur);
        for (int k : it->second.children) stack.push_back(k);
    }
    detach(index);                       // 只把子树根从父节点/根表摘下
    for (int id : doomed) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        if (eng) for (auto& [t, b] : it->second.comps) destroy_blob(*eng, t, b);
        nodes.erase(it);
        auto r = std::lower_bound(roots.begin(), roots.end(), id);
        if (r != roots.end() && *r == id) roots.erase(r);
    }
    if (eng) for (int id : doomed) {
        eng->entity_instance.erase(id);
        eng->node_events.erase(id);
    }
    /* 不回收 id（单调分配）→ 之后 entity_valid(旧 id) 恒为 false */
}

bool Scene::deserialize(Engine& eng, const std::string& text) {
    text_ = text;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    int rc = mq_persist_apply_json(eng.reflect, eng.scene_type, dom, &desc);
    mq_json_free(dom);
    return rc == 0;
}

/* VFS 路径拼接（docs/32 §5.1）：`base` 是相对 VFS 根的目录（"" 或 "dir/"）；
 * `rel` 为绝对路径（/x、http(s)://、builtin:/…）时原样返回，否则相对 base 解析并做
 * "." / ".." 归一化（不允许逃出 VFS 根）。 */
std::string vfs_join(const std::string& base, const std::string& rel) {
    if (rel.empty()) return std::string();
    if (rel[0] == '/' || rel.find("://") != std::string::npos || rel.rfind("builtin:", 0) == 0)
        return rel;
    std::string joined = (base.empty() ? std::string() : base) + rel;
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= joined.size()) {
        size_t slash = joined.find('/', i);
        std::string seg = joined.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "..") { if (!segs.empty()) segs.pop_back(); }
        else if (!seg.empty() && seg != ".") segs.push_back(seg);
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    std::string out = "/";
    for (size_t k = 0; k < segs.size(); ++k) { if (k) out += '/'; out += segs[k]; }
    return out;
}

/* 场景资产所在目录（相对 VFS 根，含结尾 '/'）——用于嵌套实例的资源基准。 */
static std::string vfs_dirname(const std::string& vfs_path) {
    const size_t slash = vfs_path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return vfs_path.substr(1, slash == 0 ? 0 : slash - 1) + (slash > 1 ? "/" : "");
}

/* 加载实体树；prefab 就地展开。
 * docs/12 §7：**禁止循环实例化** —— 用 chain（当前展开链上的 prefab 路径）+ 深度上限检测，
 * 命中则报结构化错误并跳过该实例（绝不递归到栈溢出）。 */
static int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent,
                       std::vector<std::string>* chain, int depth);

/* 载入并展开一个 prefab：把它的根实体挂到 `parent` 下（-1 = 场景根）。
 * 供两处使用：
 *   · `scn.PrefabInstance` 组件 → 展开为**该实体的子实体**（包一层壳，便于整体变换/显隐）；
 *   · 实体的 `source` 简写 → **平级展开**（prefab 的根直接成为兄弟，不套壳）。
 * `name_prefix` 非空时给展开出的**根实体**名加前缀 —— 同一个 prefab 实例多次时，
 * 名字必须能区分，否则场景里会出现一堆同名实体（脚本按名查找就只会命中第一个）。
 * `chain` 为当前展开链（循环引用检测，docs/12 §7）；`depth` 限制递归层数。 */
static void expand_prefab(Engine& eng, Scene& sc, const std::string& src, int parent,
                          std::vector<std::string>* chain, int depth,
                          const std::string& name_prefix = std::string()) {
    /* 相对路径按**当前场景实例的资源基准**解析（嵌套场景内的 prefab 不再按根场景目录解析）。 */
    const std::string src_vfs = vfs_join(eng.building_base, src);
    if (chain) {
        for (const auto& c : *chain)
            if (c == src_vfs) {
                std::print(stderr, "runtime: cyclic prefab reference '{}' — skipped\n", src);
                mq_obs_event(eng.obs, "runtime", 3, "prefab_cycle");
                return;
            }
    }
    void* pd = nullptr; size_t pn = 0;
    if (mq_vfs_read(eng.vfs, src_vfs.c_str(), &pd, &pn) != MQ_VFS_OK) {
        std::print(stderr, "runtime: prefab not found '{}' (resolved '{}')\n", src, src_vfs);
        return;
    }
    std::string ptext((char*)pd, pn);
    mq_vfs_free(pd);
    mq_json* pdom = mq_json_parse(ptext.c_str(), ptext.size());
    if (!pdom) return;
    std::vector<std::string> child_chain = chain ? *chain : std::vector<std::string>{};
    child_chain.push_back(src_vfs);
    const mq_json* pents = mq_json_get(pdom, "entities");
    const uint32_t pcount = mq_json_len(pents);
    for (uint32_t i = 0; i < pcount; ++i) {
        int child = load_entity(eng, sc, mq_json_at(pents, i), parent, &child_chain, depth + 1);
        if (child >= 0 && !name_prefix.empty())
            if (SceneNode* ce = sc.at(child); ce && !ce->name.empty()) ce->name = name_prefix + ce->name;
    }
    mq_json_free(pdom);
    std::print("runtime: prefab '{}' instantiated under entity {}\n", src, parent);
}

static int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent,
                       std::vector<std::string>* chain = nullptr, int depth = 0) {
    if (depth > 64) {
        std::print(stderr, "runtime: prefab nesting too deep (>64), aborted at depth {}\n", depth);
        return -1;
    }
    const char* nm = mq_json_str(mq_json_get(e, "name"), nullptr);
    const mq_json* comps = mq_json_get(e, "components");
    uint32_t cn = mq_json_len(comps);
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
    const char* inline_src = mq_json_str(mq_json_get(e, "source"), nullptr);

    /* **平级展开**（`source` 简写，且自身无组件/无子节点）：纯展开指令，**不 spawn 壳实体**。
     * 用于"重复的平级对象"——坦克的 20 个履带印、5 个残骸本来是各自独立的根级实体，
     * 用 PrefabInstance 会多套一层壳（改变层级与 entity_find 语义，还白增 25 个空实体）。
     * 带 components/children 时仍走下面的"壳 + 子实体"路径（那种情况需要一个实体挂它们）。 */
    if (inline_src && *inline_src && cn == 0 && kn == 0) {
        const char* pfx = mq_json_str(mq_json_get(e, "name_prefix"), nullptr);
        expand_prefab(eng, sc, inline_src, parent, chain, depth, pfx ? pfx : "");
        return parent;                                  // 未创建实体 → 返回父（调用方按 <0 判错）
    }

    int idx = sc.spawn();
    if (idx < 0) return -1;
    eng.entity_instance[idx] = eng.building_instance;    // 归属（事件作用域/序列化过滤用）
    sc.attach(idx, parent);                              // 层级真相：父的 children（id 升序）
    SceneNode* ent = sc.at(idx);
    if (nm) ent->name = nm;

    for (uint32_t i = 0; i < cn; ++i) {
        const mq_json* c = mq_json_at(comps, i);
        const char* tname = mq_json_str(mq_json_get(c, "type"), nullptr);
        if (!tname) continue;
        const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, tname);
        if (!d) { std::print("runtime: unknown component '{}'\n", tname); continue; }
        std::vector<uint8_t>& blob = ent->comps[d->type_id];
        if (!blob.empty()) destroy_blob(eng, d->type_id, blob);
        construct_blob(eng, d, blob);
        mq_persist_apply_json(eng.reflect, d, c, blob.data());
    }
    /* Prefab 实例（docs/12 §5）：按 source 载入 .prefab.json，就地展开为子实体。
     * detached=true 表示**已展开**（内容已是本场景的一部分），不再展开。
     * ⚠️ 只认 `scn.PrefabInstance`：`scn.SceneInstance` 也有 source/detached 字段，
     * 若按字段名盲扫会被当成 prefab 再展开一次（docs/32 §5 的两类实例必须分开）。 */
    for (auto& [tid, blob] : ent->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d || std::strcmp(d->name, "scn.PrefabInstance") != 0) continue;
        const std::string* src = read_str(d, blob.data(), "source");
        if (!src || src->empty()) continue;
        bool detached = false;
        if (const mq_field_desc* fd = mq_reflect_field(d, "detached")) detached = field_get_bool(fd, blob.data());
        if (detached) continue;
        expand_prefab(eng, sc, *src, idx, chain, depth);
    }

    /* 场景实例（docs/32 §5）：`scn.SceneInstance.source` 指向一个 .scn → 在该实体下实例化
     * 子场景（独立 VM 组/生命周期/事件作用域）。detached=true 表示已烘焙，不再加载源。 */
    {
        const mq_type_desc* sid = mq_reflect_type_by_name(eng.reflect, "scn.SceneInstance");
        if (sid) {
            auto it = ent->comps.find(sid->type_id);
            if (it != ent->comps.end()) {
                std::vector<uint8_t>& blob = it->second;
                const std::string* src = read_str(sid, blob.data(), "source");
                const mq_field_desc* det = mq_reflect_field(sid, "detached");
                const bool detached = det && field_get_bool(det, blob.data());
                if (!detached && src && !src->empty()) {
                    std::string src_vfs;
                    const int iid = eng.resolve_instance_path(*src, eng.building_base, src_vfs)
                                        ? eng.create_instance(eng.building_instance, idx, src_vfs)
                                        : -1;
                    int state = 0; uint16_t err = 0;
                    if (iid < 0) { state = 3; err = 1; }
                    else {
                        SceneInstanceRuntime* ci = eng.instance_at(iid);
                        /* 静态实例参数（docs/32 §16.2）：构建前写入，子场景 on_start 可见。 */
                        if (ci) {
                            if (const std::string* pp = read_str(sid, blob.data(), "params"))
                                ci->params = *pp;
                        }
                        if (!ci || !eng.build_instance(*ci, (int)eng.scene_chain.size())) {
                            state = 3; err = (uint16_t)(ci ? 2 : 1);
                            if (ci) { ci->state = 3; ci->error = "instance build failed"; }
                        } else {
                            state = 1;
                            if (const mq_field_desc* f = mq_reflect_field(sid, "paused"))
                                ci->paused = field_get_bool(f, blob.data());
                            if (const mq_field_desc* f = mq_reflect_field(sid, "active"))
                                ci->active = field_get_bool(f, blob.data());
                            if (!ci->active) { if (SceneNode* se = eng.scene->at(idx)) se->active = false; }
                        }
                    }
                    if (const mq_field_desc* f = mq_reflect_field(sid, "instance_id"))
                        field_set_i32(f, blob.data(), (int32_t)iid);
                    if (const mq_field_desc* f = mq_reflect_field(sid, "state"))
                        field_set_i32(f, blob.data(), state);
                    if (const mq_field_desc* f = mq_reflect_field(sid, "error"))
                        *(uint16_t*)(blob.data() + f->offset) = err;
                }
            }
        }
    }

    /* 实体的 `source` 简写（平级展开）：prefab 的**根实体直接成为本实体的兄弟**，不套壳。
     * `source` 与自身组件同时出现属于**误用**（组件会挂在一个多余的空壳上）：
     * 这里明确警告，但仍保留壳（否则那些组件无处安放）。 */
    if (inline_src && *inline_src) {
        std::print(stderr, "runtime: entity '{}' 同时给了 source 与 components/children —— "
                           "source 本意是平级展开、不套壳；此处为了容纳组件仍创建了壳实体\n",
                   nm ? nm : "?");
        const char* pfx = mq_json_str(mq_json_get(e, "name_prefix"), nullptr);
        expand_prefab(eng, sc, inline_src, idx, chain, depth, pfx ? pfx : "");
    }
    for (uint32_t i = 0; i < kn; ++i) load_entity(eng, sc, mq_json_at(ch, i), idx, chain, depth);
    return idx;
}

void Scene::instantiate(Engine& eng) {
    this->eng = &eng;
    mq_json* dom = mq_json_parse(text_.c_str(), text_.size());
    if (!dom) return;
    const mq_json* ents_json = mq_json_get(dom, "entities");
    uint32_t n = mq_json_len(ents_json);
    for (uint32_t i = 0; i < n; ++i) load_entity(eng, *this, mq_json_at(ents_json, i), -1);
    mq_json_free(dom);
    std::print("runtime: instantiated {} root entities\n", n);
}

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
        mq_script_vm* one = mq_script_vm_create_lang(want);
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
        mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
        mq_bind_generated(one, this);
        std::string src;
        const std::string vfs_path = vfs_join(inst.base, rel);
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, vfs_path.c_str(), &d, &n) == MQ_VFS_OK) { src.assign((char*)d, n); mq_vfs_free(d); } }
        if (src.empty()) {
            std::print("runtime: cannot read script '{}' (resolved '{}', instance {})\n", rel, vfs_path, inst.id);
            inst.error = "cannot read script";
            scene_last_error = inst.error;
            mq_script_vm_destroy(one);
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        inst.script_hashes.push_back(mq_vfs_hash(src.data(), src.size()));
        SceneInstanceRuntime* prev = active_instance;
        const bool prev_ov = in_overlay;
        active_instance = &inst;
        in_overlay = false;
        const int rc = mq_script_eval(one, src.c_str(), rel.c_str());
        active_instance = prev;
        in_overlay = prev_ov;
        if (rc != 0) {
            std::print("runtime: script error in '{}' (instance {})\n", rel, inst.id);
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
    std::vector<uint8_t>& blob = it->second;
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
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
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
    void* d = nullptr; size_t n = 0;
    const std::string vpath = vfs_join(inst->base, rel);
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK || !d) return false;
    std::string src((char*)d, n);
    mq_vfs_free(d);
    const uint64_t h = mq_vfs_hash(src.data(), src.size());
    if ((size_t)slot < inst->script_hashes.size() && inst->script_hashes[(size_t)slot] == h) return false;
    const char* lang = mq_script_lang_for_path(rel.c_str());
    mq_script_vm* fresh = mq_script_vm_create_lang(lang);
    if (!fresh) return false;
    mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(fresh, this);
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    active_instance = inst;
    in_overlay = false;
    const int rc = mq_script_eval(fresh, src.c_str(), rel.c_str());
    active_instance = prev;
    in_overlay = prev_ov;
    if (rc != 0) {                                   // 原子：eval 失败保留旧 VM
        char ebuf[512] = { 0 };
        mq_script_last_error(fresh, ebuf, (int)sizeof ebuf);
        std::print(stderr, "runtime: instance {} script hot reload failed '{}': {} (旧脚本保留)\n",
                   inst->id, rel, ebuf);
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
    std::print("runtime: hot reload instance #{} script '{}' (slot {})\n", inst->id, rel, slot);
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


/* ---- 世界快照（docs/10 §4）：实体/组件用二进制 codec 逐字段序列化。
 * 布局：u32 实体数；每实体 {i32 index, i32 parent, u32 name_len+name,
 *        u32 组件数, 每组件 {u32 type_id, u32 bytes, payload}}。
 * 转换按 index 排序 → 不依赖哈希迭代顺序（docs/10 D4）。 ---- */
void Scene::encode(Engine& eng, SnapshotWriter& w) {
    std::vector<int> idx;
    for (auto& [k, v] : nodes) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    w.pod((uint32_t)idx.size());
    for (int k : idx) {
        SceneNode& e = nodes[k];
        w.pod((int32_t)k);
        w.pod((int32_t)e.parent);
        w.pod((uint8_t)(e.active ? 1 : 0));
        w.pod(e.scale);
        w.pod(e.angle);
        w.pod(e.opacity);
        w.pod((int32_t)e.z_index);
        w.pod((uint32_t)e.name.size());
        w.bytes(e.name.data(), e.name.size());
        std::vector<uint32_t> tids;
        for (auto& [t, b] : e.comps) tids.push_back(t);
        std::sort(tids.begin(), tids.end());
        w.pod((uint32_t)tids.size());
        for (uint32_t t : tids) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, t);
            auto& blob = e.comps[t];
            std::vector<uint8_t> tmp(blob.size() + 64);
            size_t n = d ? mq_persist_encode(eng.reflect, d, blob.data(), tmp.data(), tmp.size()) : 0;
            w.pod(t);
            w.pod((uint32_t)n);
            if (n) w.bytes(tmp.data(), n);
        }
    }
}

size_t Scene::decode(Engine& eng, const uint8_t* in, size_t n) {
    this->eng = &eng;
    const int id_floor = next_id;      // 回滚也不回收已发放的 id（单调契约）
    size_t pos = 0;
    auto take = [&](void* dst, size_t bytes) -> bool {
        if (pos + bytes > n) return false;
        std::memcpy(dst, in + pos, bytes);
        pos += bytes;
        return true;
    };
    uint32_t count = 0;
    if (!take(&count, 4) || count > (1u << 20)) return 0;
    for (auto& [k, ent] : nodes)
        for (auto& [t, b] : ent.comps) destroy_blob(eng, t, b);
    nodes.clear();
    roots.clear();
    for (uint32_t i = 0; i < count; ++i) {
        int32_t k = 0, parent = -1;
        uint32_t name_len = 0;
        uint8_t active = 1;
        float scale = 1.0F, angle = 0.0F, opacity = 1.0F;
        int32_t z_index = 0;
        if (!take(&k, 4) || !take(&parent, 4) || !take(&active, 1)
            || !take(&scale, 4) || !take(&angle, 4) || !take(&opacity, 4) || !take(&z_index, 4)
            || !take(&name_len, 4)) return 0;
        if (name_len > 4096 || pos + name_len > n) return 0;
        std::string nm((const char*)in + pos, name_len);
        pos += name_len;
        uint32_t comps = 0;
        if (!take(&comps, 4) || comps > 4096) return 0;
        SceneNode e;
        e.name = std::move(nm);
        e.parent = parent;
        e.active = active != 0;
        e.scale = scale;
        e.angle = angle;
        e.opacity = opacity;
        e.z_index = (int)z_index;
        if (k < 0) return 0;              // 实体索引按快照重建（脚本 id 稳定，docs/10 D4）
        for (uint32_t c = 0; c < comps; ++c) {
            uint32_t tid = 0, len = 0;
            if (!take(&tid, 4) || !take(&len, 4) || pos + len > n) return 0;
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            std::vector<uint8_t> blob;
            if (d) {
                construct_blob(eng, d, blob);          // 先构造（析构由 destroy_blob 负责）
                if (len && mq_persist_decode(eng.reflect, d, in + pos, len, blob.data()) < 0) {
                    destroy_blob(eng, tid, blob);
                    return 0;
                }
                e.comps[tid] = std::move(blob);
            }
            pos += len;
        }
        nodes[k] = std::move(e);
        if (k + 1 > next_id) next_id = k + 1;
    }
    if (id_floor > next_id) next_id = id_floor;
    rebuild_tree();                 // children/roots 是派生结构：快照只存 parent，恢复后重建
    return pos;
}

/* 世界校验和：按 id/type 排序，哈希各组件的标量字段（不确定性来源如字符串/动态容器跳过）。
 * 反射驱动，与具体组件无关。用于确定性验证（docs/10）。 */
uint64_t Scene::checksum(Engine& eng) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    std::vector<int> idx;
    for (auto& [k, v] : nodes) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    for (int k : idx) {
        SceneNode& e = nodes[k];
        mix(&k, sizeof k); mix(&e.parent, sizeof e.parent); mix(&e.world_x, sizeof e.world_x); mix(&e.world_y, sizeof e.world_y);
        mix(&e.active, sizeof e.active); mix(&e.scale, sizeof e.scale); mix(&e.angle, sizeof e.angle);
        mix(&e.opacity, sizeof e.opacity); mix(&e.z_index, sizeof e.z_index);
        std::vector<uint32_t> tids;
        for (auto& [t, b] : e.comps) tids.push_back(t);
        std::sort(tids.begin(), tids.end());
        for (uint32_t t : tids) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, t);
            auto& blob = e.comps[t];
            mix(&t, sizeof t);
            if (!d) continue;
            for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                const mq_field_desc* f = &d->fields[fi];
                /* transient 字段是派生/装饰状态（排版度量、控件 phase、rect…）：
                 * 不进快照（docs/10 §4）→ 也不进校验和，否则回滚后必然不等 */
                if (f->attrs_flags & MQ_ATTR_TRANSIENT) continue;
                switch (f->kind) {
                    case MQ_TK_BOOL: case MQ_TK_I8: case MQ_TK_U8:
                    case MQ_TK_I16: case MQ_TK_U16: case MQ_TK_I32: case MQ_TK_U32:
                    case MQ_TK_I64: case MQ_TK_U64: case MQ_TK_F32: case MQ_TK_F64:
                        mix(blob.data() + f->offset, f->kind == MQ_TK_F32 ? 4 :
                            (f->kind == MQ_TK_F64 || f->kind == MQ_TK_I64 || f->kind == MQ_TK_U64) ? 8 :
                            (f->kind == MQ_TK_I16 || f->kind == MQ_TK_U16) ? 2 : 1);
                        break;
                    default: break;   // 字符串/容器/引用：跳过（非确定性来源）
                }
            }
        }
    }
    return h;
}

/* 组件局部位置：直接读生成的 Transform2D struct（兼容三种声明形态）。 */
static void read_local_pos(Engine& eng, const SceneNode& e, float* lx, float* ly) {
    *lx = e.local_x; *ly = e.local_y;                      /* 无 Transform2D：显式局部兜底（真值在节点上） */
    for (auto& [tid, blob] : e.comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        const uint8_t* p = blob.data();
        if (const auto* t = as_component<scn::Transform2D>(d, p)) {
            *lx = t->x; *ly = t->y; return;
        }
        if (const auto* t = as_component<scene::Transform2D>(d, p)) {
            *lx = t->local.position.x; *ly = t->local.position.y; return;
        }
        if (const auto* t = as_component<math::Transform2D>(d, p)) {
            *lx = t->position.x; *ly = t->position.y; return;
        }
    }
}

/* 每帧一趟树访问（Scene::visit，前序）：把局部变换累积成世界坐标，同时算出
 * active/angle/scale/opacity 的有效值。派生值写回节点，后续提取/命中直接 O(1) 读取，
 * 不再对每个节点沿父链重复上溯，也不再重建 "id→父" 的扁平索引。
 * 顺序确定：根 id 升序、兄弟 id 升序（children 有序）→ 与快照/校验和一致（docs/10 D4）。 */
void Scene::update_world(Engine& eng) {
    static const bool tv_on = [] { const char* e = std::getenv("MAQUE_TRACE_FLATTEN"); return e && *e && *e != '0'; }();
    const auto t0 = tv_on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    visit([&](int idx, SceneNode& e, int depth) {
        (void)idx;
        float lx = 0, ly = 0;
        read_local_pos(eng, e, &lx, &ly);
        float px = 0, py = 0, pang = 0, psc = 1, pop = 1;
        bool pvis = true;
        if (SceneNode* pe = e.parent >= 0 ? at(e.parent) : nullptr) {
            px = pe->world_x; py = pe->world_y;
            pang = pe->world_angle; psc = pe->world_scale; pop = pe->world_opacity;
            pvis = pe->visible;
        }
        e.world_x = px + lx;
        e.world_y = py + ly;
        e.world_angle = pang + e.angle;
        e.world_scale = psc * e.scale;
        e.world_opacity = pop * e.opacity;
        if (e.world_opacity < 0.0F) e.world_opacity = 0.0F;
        if (e.world_opacity > 1.0F) e.world_opacity = 1.0F;
        e.visible = pvis && e.active;
        e.depth = depth;
    });
    if (tv_on) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::print("[visit] {:.3f}ms nodes={} roots={}\n", ms, nodes.size(), roots.size());
    }
}

/* 3D 世界矩阵（plans/22 §6.3）：与 update_world 同一趟树序（父先于子）。
 * local = scene.Transform3D.local 叠加 Engine::anim_pose（clips 采样结果）；
 * 有 rotation 轨道用四元数路径（m4_from_quat_trs，无万向锁），否则走 Euler TRS（m4_from_trs）；
 * world_m = parent.world_m * local。平级场景 world_m == m4_from_trs(local)，旧场景零漂移。 */
void Scene::update_world3d(Engine& eng) {
    visit([&](int idx, SceneNode& e, int) {
        math::Transform3D trs{};
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (const auto* t3 = as_component<scene::Transform3D>(d, blob.data())) {
                trs = t3->local;
                break;
            }
        }
        AnimPose* pose = nullptr;
        auto pit = eng.anim_pose.find(idx);
        if (pit != eng.anim_pose.end()) pose = &pit->second;
        if (pose && pose->has_pos) trs.position = pose->pos;
        if (pose && pose->has_scale) trs.scale = pose->scale;
        float local[16];
        if (pose && pose->has_rot) m4_from_quat_trs(trs.position, pose->rot, trs.scale, local);
        else m4_from_trs(trs, local);
        if (SceneNode* pe = e.parent >= 0 ? at(e.parent) : nullptr) m4_mul(pe->world_m, local, e.world_m);
        else std::memcpy(e.world_m, local, sizeof local);
    });
}

/* 节点有效属性：update_world 已沿树累积到节点上（O(1) 读取）。
 * 语义与 2D 直觉一致：角度相加、缩放相乘、透明度相乘并夹 [0,1]、active 沿祖先与。 */
void accumulate_node_attrs(Engine& eng, int idx, float& angle_deg, float& scale, float& opacity) {
    angle_deg = 0.0F; scale = 1.0F; opacity = 1.0F;
    if (!eng.scene) return;
    if (SceneNode* e = eng.scene->at(idx)) {
        angle_deg = e->world_angle;
        scale = e->world_scale;
        opacity = e->world_opacity;
    }
}

/* active 沿祖先与（docs/18：node_set_active 影响自身与子树）—— 有效值由 update_world 缓存。 */
bool node_active_effective(Engine& eng, int idx) {
    if (!eng.scene) return true;
    SceneNode* e = eng.scene->at(idx);
    return e ? e->visible : true;
}

void Scene::extract(Engine& eng, std::vector<mq_renderer_item>& out) {
    /* 粒子（docs/15 §4）：只读提取，alpha 随寿命衰减；在实体图元之后按稳定顺序输出 */
    for (auto& p : eng.particles) {
        const float t = p.max_life > 0 ? p.life / p.max_life : 1.0F;
        mq_renderer_item it{};
        it.x = p.x; it.y = (float)eng.H - p.y; it.size = p.size;
        it.r = p.r; it.g = p.g; it.b = p.b;
        it.a = t < 1.0F ? 1.0F - t : 0.0F;
        it.texture = -1; it.layer = 0; it.z = 100000;      // 粒子在世界层最上
        out.push_back(it);
    }
    visit([&](int idx, SceneNode& e, int) {
        if (!e.visible) return;                       // 隐藏整棵子树（update_world 已沿树累积）
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (!d) continue;
            /* UI 控件：overlay 层（layer=1）——屏幕空间绘制，不受相机影响（docs/13 §分层） */
            if (d->role & MQ_ROLE_UI_BUTTON) {
                const auto* btn = as_component<scn::Button>(d, blob.data());
                if (!btn) continue;
                const float x = btn->x, y = btn->y, w = btn->width, h = btn->height;
                const float r = btn->r, g = btn->g, b = btn->b;
                // 矩形控件以"中心 + 半尺寸"图元表达（half_w/half_h 都 >0 时渲染器画矩形；
                // 只填 size 会退化成正方形 —— 96x28 的按钮曾显示成 96x96 方块）
                mq_renderer_item it{};
                it.x = e.world_x + x + w * 0.5F; it.y = (float)eng.H - (e.world_y + y) - h * 0.5F;
                it.size = w * 0.5F > h * 0.5F ? w * 0.5F : h * 0.5F;
                it.half_w = w * 0.5F;
                it.half_h = h * 0.5F;
                it.r = r; it.g = g; it.b = b; it.a = 1.0F;
                it.texture = -1; it.layer = 1; it.z = btn->z;
                out.push_back(it);
                continue;
            }
            if (d->role & (MQ_ROLE_UI_LABEL | MQ_ROLE_UI_TEXT)) {
                eng.extract_text(eng, idx, e, d, out, e.world_opacity);   // 真实字形渲染（docs/13/17）
                continue;
            }
            if (!(d->role & MQ_ROLE_SPRITE)) continue;
            /* 节点属性（active/angle/scale/opacity）此前被**整个忽略** ——
             * 实测：node_set_angle 不旋转、node_set_active(0) 不隐藏、node_set_scale /
             * node_set_opacity 对 sprite 无效。渲染项 ABI 本来就有这些字段
             *（rotation/a/half_w/half_h），只是提取时没填。 */
            /* 节点有效属性由 update_world 的树访问累积（角度相加、缩放相乘、透明度相乘）。 */
            const float node_angle = e.world_angle;
            const float node_scale = e.world_scale;
            const float node_opacity = e.world_opacity;
            const auto* spr = as_component<scn::Sprite>(d, blob.data());
            if (!spr) continue;
            float radius = spr->radius, r = spr->r, g = spr->g, b = spr->b;
            const float sw = spr->width, sh = spr->height;
            /* 只读提取（docs/13 §8 / docs/15 §1）：纹理与材质已在装配期预解析并缓存，
             * 此处仅查表，**不得**触发加载或改写任何引擎状态。 */
            std::string texref = spr->texture;
            if (!spr->material.empty()) {
                auto mi = eng.material_cache.find(spr->material);
                if (mi != eng.material_cache.end()) {
                    r = mi->second.base_color.x; g = mi->second.base_color.y; b = mi->second.base_color.z;
                    if (!mi->second.texture.empty()) texref = mi->second.texture;
                }
            }
            int tex = -1;
            if (!texref.empty()) {
                const std::string resolved = eng.scoped_texture_ref(idx, texref);
                auto ti = eng.tex_cache.find(resolved);
                tex = ti == eng.tex_cache.end() ? -1 : ti->second;
            }
            mq_renderer_item it{};
            it.x = e.world_x; it.y = (float)eng.H - e.world_y;
            /* width/height > 0 → **矩形**图元（瓦片/方块/非方形贴图）；否则 radius 的圆。
             * 节点缩放对两者都生效（半径/半宽半高同时乘）。 */
            const bool rect = sw > 0.0F && sh > 0.0F;
            it.size = radius * node_scale;
            it.half_w = rect ? sw * 0.5F * node_scale : 0.0F;
            it.half_h = rect ? sh * 0.5F * node_scale : 0.0F;
            /* 角度：实体存度，渲染项要弧度，且 y 轴已翻到屏幕空间 → 取负保持视觉方向一致
             *（世界 y 向上、屏幕 y 向下；不取负的话"抬头"会变成"低头"）。 */
            it.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
            it.r = r; it.g = g; it.b = b;
            it.a = node_opacity;
            it.texture = tex;
            if (tex >= 0) {
                for (const auto& rt : eng.render_textures)
                    if (rt.handle == tex) { it.filter = rt.filter == 0 ? 1 : 0; break; }
            }
            it.layer = 0;                                        // 世界层
            /* 节点 z_index 必须参与排序（docs/bugs/013）：此前只写实体 id，
             * 于是 scn.Sprite 图层永远按创建顺序画、z_index 静默无效 —— 树无法盖住坦克。
             * 与 UI/Spine 用同一套 "z_index*100000+id" 稳定键（同 z 时仍按 id，禁指针序）。 */
            it.z = e.z_index * 100000 + (int)idx;
            out.push_back(it);
        }
    });
}

/* ---- UI 命中测试（docs/19 §4）：z-order + 树深 稳定排序，命中最上层 ---- */
struct HitBest { int entity = -1; int z = INT32_MIN; int depth = -1; float lx = 0, ly = 0; };

void collect_hits(Engine& eng, int idx, int depth, float px, float py, HitBest& best) {
    SceneNode* e = eng.scene->at(idx);
    if (!e) return;
    /* 有效 z = 自身 z + 祖先 z（层级叠加，稳定） */
    int zsum = 0;
    /* ui.Control 提供盒模型（anchor/offset/size），交互性由 role 决定 */
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
        if (d->role & MQ_ROLE_UI_CONTROL) {
            const auto* ctrl = as_component<ui::Control>(d, blob.data());
            if (!ctrl) continue;
            const float w = ctrl->offset_max.x - ctrl->offset_min.x;
            const float h = ctrl->offset_max.y - ctrl->offset_min.y;
            zsum += ctrl->z_index;
            bool interactive = false;
            for (auto& [t2, b2] : e->comps) {
                const mq_type_desc* d2 = mq_reflect_type_by_id(eng.reflect, t2);
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
                const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
                if (auto* btn = (d && (d->role & MQ_ROLE_UI_BUTTON))
                                    ? as_component_mut<scn::Button>(d, blob.data()) : nullptr)
                    btn->pressed = true;
            }
        if (e && ev->type == MQ_INPUT_POINTER_UP)
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
                if (auto* btn = (d && (d->role & MQ_ROLE_UI_BUTTON))
                                    ? as_component_mut<scn::Button>(d, blob.data()) : nullptr)
                    btn->pressed = false;
            }
        /* Slider：按下进入拖拽，拖动中按水平比例改值（滑块语义） */
        if (e) {
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
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
                    const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
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
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (d && (d->role & role)) return &blob;
    }
    return nullptr;
}

/* 直接按类型名找组件（getComponent("Type") 语义；支持短名与 fq 名）。 */
std::vector<uint8_t>* comp_by_name(Engine& eng, int id, const char* type_name,
                                   const mq_type_desc** out_desc) {
    SceneNode* e = eng.scene->at(id);
    if (!e || !type_name) return nullptr;
    const mq_type_desc* want = mq_reflect_type_by_name(eng.reflect, type_name);
    if (!want) {                                   // 允许短名（"Sprite" → "scn.Sprite"）
        char fq[128];
        std::snprintf(fq, sizeof fq, "scn.%s", type_name);
        want = mq_reflect_type_by_name(eng.reflect, fq);
        if (!want) { std::snprintf(fq, sizeof fq, "ui.%s", type_name);
                     want = mq_reflect_type_by_name(eng.reflect, fq); }
        if (!want) { std::snprintf(fq, sizeof fq, "physics.%s", type_name);
                     want = mq_reflect_type_by_name(eng.reflect, fq); }
    }
    if (!want) {
        /* 通用短名：任意 namespace 的 "Ns.Type" 后缀匹配（audio.AudioSource 等），
         * 避免每加一个模块都要在这里补一个前缀。重名时取注册顺序第一个。 */
        const std::string suffix = std::string(".") + type_name;
        const uint32_t n = mq_reflect_count(eng.reflect);
        for (uint32_t i = 0; i < n && !want; ++i) {
            const mq_type_desc* t = mq_reflect_type_at(eng.reflect, i);
            if (!t || !t->name) continue;
            const std::string nm(t->name);
            if (nm.size() > suffix.size() &&
                nm.compare(nm.size() - suffix.size(), suffix.size(), suffix) == 0) {
                want = t;
            }
        }
    }
    if (!want) return nullptr;
    auto it = e->comps.find(want->type_id);
    if (it == e->comps.end()) return nullptr;
    if (out_desc) *out_desc = want;
    return &it->second;
}

float read_num(const mq_type_desc* d, const void* base, const char* field, float def) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (!f) return def;
    if (f->accessor && f->accessor->get_f64) return (float)f->accessor->get_f64(base);
    switch (f->kind) {
        case MQ_TK_F32:  return field_get_f32(f, (const uint8_t*)base);
        case MQ_TK_F64:  return (float)*(const double*)((const uint8_t*)base + f->offset);
        case MQ_TK_I8:   return (float)*(const int8_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U8:   return (float)*(const uint8_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_I16:  return (float)*(const int16_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U16:  return (float)*(const uint16_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_I32:  return (float)field_get_i32(f, (const uint8_t*)base);
        case MQ_TK_U32:  return (float)field_get_u32(f, (const uint8_t*)base);
        case MQ_TK_I64:  return (float)*(const int64_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U64:  return (float)*(const uint64_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_BOOL: return field_get_bool(f, (const uint8_t*)base) ? 1.0F : 0.0F;
        case MQ_TK_ENUM: {
            /* 枚举：按底层整型宽度读（BodyType::Dynamic 等，docs/06） */
            const uint8_t* p = (const uint8_t*)base + f->offset;
            if (f->count == 1) return (float)*(const int8_t*)p;
            if (f->count == 2) return (float)*(const int16_t*)p;
            return (float)*(const int32_t*)p;
        }
        default: return def;
    }
}

bool write_num(const mq_type_desc* d, void* base, const char* field, double v) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (!f) return false;
    if (f->accessor && f->accessor->set_f64) { f->accessor->set_f64(base, v); return true; }
    uint8_t* p = (uint8_t*)base + f->offset;
    switch (f->kind) {
        case MQ_TK_ENUM: {
            if (f->count == 1) { *(int8_t*)p = (int8_t)v; return true; }
            if (f->count == 2) { *(int16_t*)p = (int16_t)v; return true; }
            *(int32_t*)p = (int32_t)v; return true;
        }
        case MQ_TK_F32:  *(float*)p = (float)v; return true;
        case MQ_TK_F64:  *(double*)p = v; return true;
        case MQ_TK_I8:   *(int8_t*)p = (int8_t)v; return true;
        case MQ_TK_U8:   *(uint8_t*)p = (uint8_t)v; return true;
        case MQ_TK_I16:  *(int16_t*)p = (int16_t)v; return true;
        case MQ_TK_U16:  *(uint16_t*)p = (uint16_t)v; return true;
        case MQ_TK_I32:  *(int32_t*)p = (int32_t)v; return true;
        case MQ_TK_U32:  *(uint32_t*)p = (uint32_t)v; return true;
        case MQ_TK_I64:  *(int64_t*)p = (int64_t)v; return true;
        case MQ_TK_U64:  *(uint64_t*)p = (uint64_t)v; return true;
        case MQ_TK_BOOL: *(bool*)p = v != 0.0; return true;
        default: return false;
    }
}

/* Slider：把指针 x 映射为值（Slider 的 progress 语义）。 */
void slider_apply_pointer(Engine& eng, int id, const mq_type_desc* d, void* base, float px) {
    SceneNode* e = eng.scene->at(id);
    if (!e) return;
    auto* sl = as_component_mut<ui::Slider>(d, base);
    if (!sl) return;
    float w = 100.0F;
    if (auto* ctrl = comp_by_role(eng, id, MQ_ROLE_UI_CONTROL)) {
        const mq_type_desc* cd = mq_reflect_type_by_name(eng.reflect, "ui.Control");
        if (const auto* c = cd ? as_component<ui::Control>(cd, ctrl->data()) : nullptr)
            w = c->offset_max.x;
    }
    if (w <= 0.0F) w = 1.0F;
    float t = (px - (e->world_x - w * 0.5F)) / w;
    t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
    if (sl->invert) t = 1.0F - t;
    const float lo = sl->min_value;
    const float hi = sl->max_value;
    float v = lo + (hi - lo) * t;
    const float step = sl->step;
    if (step > 0.0F) v = lo + std::round((v - lo) / step) * step;
    sl->value = v;
    sl->ratio = t;
    host::node_emit_number(&eng, id, "changed", v);
}


/* 文本：装配期选字体
 *   MAQUE_FONT → **项目字体（scenes/fonts/ 下首个 .ttf，仓库自带含中文）** → 常见系统字体
 *   → 内置 5x7 位图字体。
 * AGENTS.md 一直这么写，但实现里缺了"项目字体"这一档：只查 MAQUE_FONT 与 Linux 系统路径，
 * 于是 Windows（没有 /usr/share/fonts）**恒回退内置 5x7**，中文与 CJK 全丢，
 * 且依赖真实字宽的排版用例（text_e2e 的折行行数）必然失败。
 * 图集纹理按字号惰性创建并缓存（确定性：同字号同图集，docs/10）。 */

/* ---- 键值存储（docs/25）----
 * 模式与根：MAQUE_STORAGE（spec）> 默认 file://<场景目录>/.maque（远程场景/只读目录由模块退到 XDG 数据目录）；
 * 命名空间：MAQUE_UID / MAQUE_SESSION（缺省 "default" / "main"）→ 键空间 <uid>/<session>/<key>。
 * 切场景时按新场景目录重建（旧实例销毁）。 */
void Engine::storage_setup() {
    if (storage) { mq_storage_destroy(storage); storage = nullptr; }
    const char* spec_env = std::getenv("MAQUE_STORAGE");
    const char* uid = std::getenv("MAQUE_UID");
    const char* session = std::getenv("MAQUE_SESSION");

    const bool remote = scn_full_path.rfind("http://", 0) == 0 || scn_full_path.rfind("https://", 0) == 0;
    std::string spec;
    if (spec_env && *spec_env) spec = spec_env;
#ifdef MQ_PLATFORM_WASM
    else spec = "mem://";
#else
    else if (!remote) {
        /* 默认：**场景目录**旁的 .maque（跑 `scenes/sokoban/sokoban.scn` → scenes/sokoban/.maque；
         * 场景就在 CWD 时 → <CWD>/.maque）。远程场景没有可写目录 → 交给模块的 XDG 兜底。 */
        std::error_code ec;
        const auto abs = std::filesystem::absolute(std::filesystem::path(scn_dir.empty() ? "." : scn_dir) / ".maque", ec);
        spec = "file://" + (ec ? (std::filesystem::path(scn_dir.empty() ? "." : scn_dir) / ".maque").string() : abs.string());
    } else {
        spec = "file://";
    }
#endif

    storage = mq_storage_create(spec.c_str(), uid, session);
    if (!storage) {
        std::print(stderr, "runtime: storage 创建失败（spec='{}'）→ 回退 mem://（不落盘）\n", spec);
        storage = mq_storage_create("mem://", uid, session);
    }
    if (storage) {
        std::print("runtime: storage mode='{}' root='{}' ns='{}'\n",
                   mq_storage_mode(storage), mq_storage_root(storage), mq_storage_ns(storage));
    }
}

/* ---- 画布与分辨率策略（docs/17）----
 * 设计分辨率只定义世界/UI 参考坐标；Canvas 解析到屏幕设备像素，再由 render_scale 映射进
 * 实际颜色附件。没有 ui.Canvas 实体时按 ShowAll + 居中兜底。 */
void Engine::canvas_setup() {
    canvas_entity = -1;
    canvas_design_warned = false;
    if (!scene) return;
    for (auto& [idx, e] : scene->nodes) {
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
            canvas_entity = idx;                       /* 升序首个画布 */
            canvas_active = true;
            std::print("runtime: canvas on entity {} (design {}x{})\n", idx,
                       read_num(d, blob.data(), "design_width", 0.0F),
                       read_num(d, blob.data(), "design_height", 0.0F));
            break;
        }
        if (canvas_entity >= 0) break;
    }
}

void Engine::canvas_resolve() {
    mq_canvas_desc desc{};
    desc.struct_size = (uint32_t)sizeof desc;
    /* 每个 view 使用一份设计宽度；多视图在屏幕中横向并排。 */
    const int view_count = std::max(1, nviews);
    desc.design_w = (float)W * (float)view_count;
    desc.design_h = (float)H;
    desc.fit = MQ_CANVAS_FIT_SHOW_ALL;
    desc.align = MQ_CANVAS_ALIGN_CENTER;
    desc.dpi_scale = 1.0F;
    canvas_use_dpi = true;
    desc.safe_left = canvas_safe[0]; desc.safe_top = canvas_safe[1];
    desc.safe_right = canvas_safe[2]; desc.safe_bottom = canvas_safe[3];
    if (canvas_entity >= 0) {
        SceneNode* e = scene->at(canvas_entity);
        if (e) {
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
                const float dw = read_num(d, blob.data(), "design_width", 0.0F);
                const float dh = read_num(d, blob.data(), "design_height", 0.0F);
                if (dw > 0.0F && dh > 0.0F &&
                    (std::fabs(dw - (float)W) > 1.0e-4F || std::fabs(dh - (float)H) > 1.0e-4F) &&
                    !canvas_design_warned) {
                    canvas_design_warned = true;
                    std::print(stderr, "runtime: ui.Canvas.design {}x{} 与 Scene 设计坐标 {}x{} 不一致，"
                                       "已忽略 Canvas.design_*（Scene.width/height 是唯一坐标基准）\n",
                               dw, dh, W, H);
                }
                desc.fit = (int32_t)read_num(d, blob.data(), "fit_mode", (float)MQ_CANVAS_FIT_SHOW_ALL);
                desc.align = (int32_t)read_num(d, blob.data(), "align", (float)MQ_CANVAS_ALIGN_CENTER);
                desc.min_scale = read_num(d, blob.data(), "min_scale", 0.0F);
                desc.max_scale = read_num(d, blob.data(), "max_scale", 0.0F);
                const bool dpi = read_num(d, blob.data(), "dpi_scale", 1.0F) != 0.0F;
                canvas_use_dpi = dpi;
                desc.dpi_scale = (dpi && platform && window) ? mq_platform_content_scale(platform) : 1.0F;
                /* 写回解析结果（transient）：脚本/控制面可读 */
                break;
            }
        }
    } else if (platform && window) {
        desc.dpi_scale = mq_platform_content_scale(platform);
    }
    /* Canvas 绘制区按 dpi_scale 选择 drawable 设备像素或逻辑窗口像素；无窗口用虚拟设计屏幕。 */
    desc.window_w = (float)W * (float)view_count;
    desc.window_h = (float)H;
    if (platform && window) {
        int32_t dw = 0, dh = 0;
        if (canvas_use_dpi) mq_platform_drawable_size(platform, &dw, &dh);
        else mq_platform_window_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) { desc.window_w = (float)dw; desc.window_h = (float)dh; }
    }
    const int rc = mq_canvas_resolve(&desc, &canvas);
    if (rc != 0 && std::getenv("MAQUE_TRACE_CANVAS"))
        std::print(stderr, "runtime: canvas resolve fallback (rc={})\n", rc);
    update_main_views();
    /* 回写 transient 字段 */
    if (canvas_entity >= 0) {
        SceneNode* e = scene->at(canvas_entity);
        if (e) {
            std::vector<uint8_t>* blob = nullptr;
            const mq_type_desc* d = nullptr;
            for (auto& [tid, b] : e->comps) {
                const mq_type_desc* t = mq_reflect_type_by_id(reflect, tid);
                if (t && (t->role & MQ_ROLE_UI_CANVAS)) { blob = &b; d = t; break; }
            }
            if (blob && d) {
                write_num(d, blob->data(), "scale", canvas.scale);
                write_num(d, blob->data(), "scale_x", canvas.scale_x);
                write_num(d, blob->data(), "scale_y", canvas.scale_y);
                write_num(d, blob->data(), "offset_x", canvas.offset_x);
                write_num(d, blob->data(), "offset_y", canvas.offset_y);
                write_num(d, blob->data(), "viewport_w", canvas.viewport_w);
                write_num(d, blob->data(), "viewport_h", canvas.viewport_h);
                write_num(d, blob->data(), "window_w", canvas.window_w);
                write_num(d, blob->data(), "window_h", canvas.window_h);
                write_num(d, blob->data(), "device_scale", canvas.device_scale);
            }
        }
    }
    if (std::getenv("MAQUE_TRACE_CANVAS"))
        std::print("[canvas] design={}x{} win={}x{} fit={} scale=({:.3f},{:.3f}) off=({:.1f},{:.1f}) dpi={:.2f}\n",
                   desc.design_w, desc.design_h, desc.window_w, desc.window_h, desc.fit,
                   canvas.scale_x, canvas.scale_y, canvas.offset_x, canvas.offset_y, canvas.device_scale);
}

/* ---- 输入模拟器（docs/19）----
 * MAQUE_SIM=<file.sim.json> 或控制面 Input.sim_load 载入场景；每帧把**到期**事件注入输入流
 * （与设备事件同一条路径：mq_input_inject → 输入模块 → 命中测试 → 脚本回调），
 * 因此同样被录制/回放覆盖（docs/10 D3：输入即数据）。 */
void Engine::sim_setup() {
    if (!sim) sim = mq_sim_create();
    const char* path = std::getenv("MAQUE_SIM");
    const char* clicks = std::getenv("MAQUE_SIM_CLICK");
    std::string json;
    if (path && *path) {
        if (!read_sim_scenario(vfs, path, json))
            std::print(stderr, "runtime: sim scenario not found: {}\n", path);
        sim_path = path;
    } else if (clicks && *clicks) {
        /* 简写：MAQUE_SIM_CLICK="frame:x:y[;frame:x:y]" → 内联场景 */
        json = "{\"name\":\"inline MAQUE_SIM_CLICK\",\"space\":\"window\",\"events\":[";
        const char* q = clicks; bool first = true;
        while (*q) {
            long f = 0; int cx = 0, cy = 0;
            if (std::sscanf(q, "%ld:%d:%d", &f, &cx, &cy) == 3) {
                if (!first) json += ",";
                json += "{\"frame\":" + std::to_string(f) + ",\"type\":\"click\",\"x\":" +
                        std::to_string(cx) + ",\"y\":" + std::to_string(cy) + "}";
                first = false;
            }
            const char* semi = std::strchr(q, ';');
            if (!semi) break;
            q = semi + 1;
        }
        json += "]}";
        sim_path = "(inline MAQUE_SIM_CLICK)";
    }
    if (json.empty()) return;
    char err[256] = { 0 };
    const int rc = mq_sim_load_json(sim, json.c_str(), json.size(), err, sizeof err);
    if (rc != 0) {
        std::print(stderr, "runtime: sim load failed ({}): {}\n", rc, err);
        return;
    }
    sim_active = true;
    std::print("runtime: sim '{}' loaded: {} event(s), last frame {}, loop={}\n",
               sim_path, mq_sim_count(sim), mq_sim_last_frame(sim), mq_sim_loop(sim));
}

void Engine::sim_feed(uint64_t frame) {
    if (!sim_active || !sim || !input) return;
    if (mq_input_replaying(input)) return;          // 回放只吃记录（设备/模拟输入都被忽略）
    mq_sim_event batch[16];
    uint32_t n = 0;
    uint32_t guard = 0;
    do {
        n = 0;
        if (mq_sim_due(sim, frame, batch, 16, &n) != 0 && n == 0) break;
        for (uint32_t i = 0; i < n; ++i) {
            const mq_sim_event& se = batch[i];
            mq_input_event ev{};
            ev.pointer_id = 0;
            ev.pointer_type = 0;                     // 鼠标
            ev.button = se.button;
            /* 键码统一到引擎规范键空间（MQ_KEY_* + 大写 ASCII）。
             * 为什么必须做：真实设备事件在平台边界经 key_from_sdl 归一化，而 .sim.json 里
             * 人手写的是 SDL 风格码（空格 32、'w' 119）。不归一化则"同一串按键走设备路径与
             * 走注入路径含义不同" —— 脚本被迫写两套码的防御分支，回放/热更/CI 都不可靠。 */
            ev.key = mq_sim_normalize_key(se.key);
            ev.x = se.x; ev.y = se.y;
            ev.dx = se.dx; ev.dy = se.dy;
            ev.scroll_x = se.dx; ev.scroll_y = se.dy;
            if (se.kind == MQ_SIM_TEXT) {
                ev.type = MQ_INPUT_TEXT;
                /* 输入事件的 text 是定长内联缓冲（16B，见 abi/input.h）→ 截断拷贝 */
                std::snprintf(ev.text, sizeof ev.text, "%s", se.text ? se.text : "");
            } else {
                ev.type = se.kind == MQ_SIM_POINTER_MOVE ? MQ_INPUT_POINTER_MOVE
                         : se.kind == MQ_SIM_POINTER_DOWN ? MQ_INPUT_POINTER_DOWN
                         : se.kind == MQ_SIM_POINTER_UP   ? MQ_INPUT_POINTER_UP
                         : se.kind == MQ_SIM_POINTER_WHEEL ? MQ_INPUT_POINTER_WHEEL
                         : se.kind == MQ_SIM_KEY_DOWN     ? MQ_INPUT_KEY_DOWN
                         : MQ_INPUT_KEY_UP;
            }
            /* 坐标空间：window（窗口像素）→ 世界坐标（含 y 翻转与方向量翻转）；world 直接用 */
            if (se.space == MQ_SIM_SPACE_WINDOW) {
                window_to_world(*this, &ev.x, &ev.y);
                ev.dy = -ev.dy; ev.scroll_y = -ev.scroll_y;
            }
            if (trace_hits)
                std::print("[sim] frame={} kind={} -> input type={} pos=({:.1f},{:.1f})\n",
                           frame, se.kind, (int)ev.type, ev.x, ev.y);
            mq_input_inject(input, &ev);
        }
        ++guard;
    } while (n == 16 && guard < 16);
}


/* UI 组件语义（role 驱动；引擎不认具体类型名，docs/17）：
 *   · ui_layout：H/V/GRID 自动布局（Layout）
 *   · ui_progress：按 value 调整填充子节点宽度（ProgressBar）
 *   · ui_safe_area：按内缩重排目标节点（SafeArea/Widget）
 *   · ui_scroll：内容偏移（ScrollView 的 scrollOffset） */
void Engine::ui_step() {
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
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
                    if (auto* ctrl = comp_by_role(*this, k, MQ_ROLE_UI_CONTROL)) {
                        const mq_type_desc* cd = mq_reflect_type_by_name(reflect, "ui.Control");
                        if (const auto* c = as_component<ui::Control>(cd, ctrl->data())) {
                            kw = c->offset_max.x; kh = c->offset_max.y;   // 内容尺寸
                        }
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
                    if (auto* ctrl = comp_by_role(*this, k, MQ_ROLE_UI_CONTROL)) {
                        const mq_type_desc* cd = mq_reflect_type_by_name(reflect, "ui.Control");
                        auto* c = as_component_mut<ui::Control>(cd, ctrl->data());
                        if (!c) continue;
                        float full = host::node_width(this, (float)id);
                        if (full <= 0.0F) full = c->offset_max.x > 0.0F ? c->offset_max.x : 100.0F;
                        c->offset_max.x = full * v;
                        break;
                    }
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
                    if (auto* ctrl = comp_by_role(*this, target, MQ_ROLE_UI_CONTROL)) {
                        const mq_type_desc* cd = mq_reflect_type_by_name(reflect, "ui.Control");
                        if (auto* c = as_component_mut<ui::Control>(cd, ctrl->data())) {
                            c->offset_max.x = (float)W - il - ir;
                            c->offset_max.y = (float)H - it - ib;
                        }
                    }
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


/* Tween（tween().to()）：在固定步长内把字段从 from 插值到 to；
 * 缓动 0=linear 1=ease_in 2=ease_out 3=ease_in_out；完成派发 "tween_complete"。 */
void Engine::step_tweens(float dt) {
    if (tweens.empty()) return;
    std::vector<size_t> done;
    for (size_t i = 0; i < tweens.size(); ++i) {
        Tween& tw = tweens[i];
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
    for (auto it = done.rbegin(); it != done.rend(); ++it) tweens.erase(tweens.begin() + (long)*it);
}

/* 定时器（定时器）：按固定步长累计，到点调用脚本函数（按名字）。 */
void Engine::step_schedules(float dt) {
    if (schedules.empty() || !bus) return;
    std::vector<size_t> drop;
    for (size_t i = 0; i < schedules.size(); ++i) {
        Scheduled& sc = schedules[i];
        sc.remaining -= dt;
        if (sc.remaining > 0.0F) continue;
        mq_script_bus_call(bus, sc.handler.c_str(), (double)dt, 0.0, nullptr);
        if (sc.repeat) sc.remaining += sc.interval > 0.0F ? sc.interval : (1.0F / 60.0F);
        else drop.push_back(i);
    }
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) schedules.erase(schedules.begin() + (long)*it);
}

/* node.destroy()：延迟到帧边界统一销毁（销毁在当前帧末生效）。 */
void Engine::flush_pending_destroy() {
    if (pending_destroy.empty()) return;
    if (std::getenv("MAQUE_TRACE_UI")) std::print("[ui] destroy {} node(s)\n", pending_destroy.size());
    std::vector<int> ids = pending_destroy;
    pending_destroy.clear();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) if (scene->at(id)) scene->remove(id);
}

/* 物理组件（RigidBody2D/Collider2D 等价）：装配期建体，帧内步进并回写变换。
 * 只在场景里存在 physics_body role 时启用（不影响脚本显式驱动 physics_step 的场景）。 */
void Engine::physics_components_setup() {
    if (!scene || !physics) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    physics_role_bodies.clear();
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        auto* body_blob = comp_by_role(*this, id, MQ_ROLE_PHYSICS_BODY);
        if (!body_blob) continue;
        const mq_type_desc* bd = nullptr;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (d && (d->role & MQ_ROLE_PHYSICS_BODY)) { bd = d; break; }
        }
        const auto* rb = bd ? as_component<physics::RigidBody>(bd, body_blob->data()) : nullptr;
        if (!rb) continue;
        /* BodyType: Static=0 / Kinematic=1 / Dynamic=2（Kinematic 暂按 static 处理并记录） */
        const double body_type = (double)rb->body_type;
        const double is_static = body_type == 0.0 ? 1.0 : 0.0;
        double radius = 0.0, hw = 0.0, hh = 0.0;
        double shape_type = 0.0, friction = 0.5, restitution = 0.0;
        if (auto* col = comp_by_role(*this, id, MQ_ROLE_PHYSICS_COLLIDER)) {
            const mq_type_desc* cd = nullptr;
            for (auto& [tid, b] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (d && (d->role & MQ_ROLE_PHYSICS_COLLIDER)) { cd = d; break; }
            }
            if (const auto* collider = cd ? as_component<physics::Collider>(cd, col->data()) : nullptr) {
                shape_type = (double)collider->shape_type;
                radius = collider->radius;
                friction = collider->friction;
                restitution = collider->restitution;
                hw = collider->half_extents.x;
                hh = collider->half_extents.y;
                /* Capsule(2)/Cylinder(3) 的轴向半高在 half_height；2D 下沿用为半高 */
                if (shape_type == 2.0 || shape_type == 3.0) hh = collider->half_height;
            }
        }
        /* 形状按**声明的 shape_type** 决定，而不是靠"radius 是否 > 0"猜。
         * Box → Cuboid；Circle（以及 2D 下退化的 Capsule）→ Ball；
         * Convex/TriMesh/HeightField 求解器不支持：明确告警并退化为 Box。 */
        bool is_circle = (shape_type == 1.0 || shape_type == 2.0);
        if (shape_type >= 4.0) {
            std::print(stderr, "runtime: entity {} '{}' 的 Collider.shape_type={} 在 2D 求解器下不支持；"
                               "已按 Box 处理\n", id, e->name, (int)shape_type);
            is_circle = false;
        }
        if (is_circle) {
            hw = hh = 0.0;
            if (radius <= 0.0) {
                std::print(stderr, "runtime: entity {} '{}' 的 Collider 是 Circle 但 radius<=0；已按 8 兜底\n",
                           id, e->name);
                radius = 8.0;
            }
        } else {
            radius = 0.0;
            /* 尺寸非正 = 无效碰撞体：**不静默兜底**（静默兜底正是上面那个 bug 藏住的原因），
             * 明确告警并按圆兜底（旧行为），让作者一眼看到"忘了写尺寸"。 */
            if (hw <= 0.0 || hh <= 0.0) {
                std::print(stderr, "runtime: entity {} '{}' 的 Collider 是 Box 但 half_extents 非正"
                                   "（{:.2f},{:.2f}）；已按半径 8 的圆兜底 —— 请显式声明尺寸\n",
                           id, e->name, hw, hh);
                hw = hh = 0.0; radius = 8.0; is_circle = true;
            }
        }
        mq_physics_body_desc desc{};
        desc.struct_size = (uint32_t)sizeof desc;
        desc.type = is_static != 0.0 ? MQ_PHYSICS_BODY_STATIC : MQ_PHYSICS_BODY_DYNAMIC;
        desc.shape = radius > 0.0 ? MQ_PHYSICS_SHAPE_BALL : MQ_PHYSICS_SHAPE_CUBOID;
        desc.x = e->world_x; desc.y = e->world_y;
        desc.radius = (float)radius;
        desc.half_w = (float)hw; desc.half_h = (float)hh;
        /* 摩擦/弹性来自组件声明（此前写死 0.5/0.2，声明被静默忽略 —— 表现是
         * "把 Collider.restitution 从 0.0 改成 0.95，逐帧位置完全一致"，回弹比恒为 0.5）。
         * 注意：ABI 的 mq_physics_body_desc 没有 layer/mask 字段，按层过滤暂不可用。 */
        desc.mass = 1.0F;
        desc.restitution = (float)restitution;
        desc.friction = (float)friction;
        mq_physics_body out_body{};
        if (mq_physics_add_body(physics, &desc, &out_body) == MQ_PHYSICS_OK)
            physics_role_bodies.push_back({ id, out_body });
    }
}

void Engine::physics_components_step(float) {
    static bool reported = false;
    if (!reported && !physics_role_bodies.empty()) {
        std::print("runtime: {} physics body(ies) from components\n", physics_role_bodies.size());
        reported = true;
    }
    contacts_dirty = true;                          // 新一轮 step 后接触对需重建（见 refresh_contacts）
    if (!physics || physics_role_bodies.empty()) return;
    mq_physics_step(physics);                       // 固定步长在世界配置里（docs/10 D1）
    /* 诊断（MAQUE_TRACE_PHYSICS=1）：逐帧打印实体位置 + 本帧接触对（排查"为什么判定不中"） */
    if (std::getenv("MAQUE_TRACE_PHYSICS")) {
        static int trace_n = 0;
        if (trace_n < 40) {
            std::print(stderr, "[phys] f{}", trace_n);
            for (auto& [id, body] : physics_role_bodies) {
                float x = 0, y = 0, vx = 0, vy = 0;
                mq_physics_get_transform(physics, body, &x, &y);
                mq_physics_get_velocity(physics, body, &vx, &vy);
                std::print(stderr, " e{}@({:.1f},{:.1f})v({:.0f},{:.0f})", id, x, y, vx, vy);
            }
            const uint32_t nc = mq_physics_contact_count(physics);
            std::print(stderr, " contacts={}", nc);
            for (uint32_t i = 0; i < nc; ++i) {
                mq_physics_contact c{};
                if (mq_physics_contact_at(physics, i, &c) == 0)
                    std::print(stderr, " [a={} b={} n=({:.1f},{:.1f}) d={:.2f}]", c.a_index, c.b_index, c.nx, c.ny, c.depth);
            }
            std::print(stderr, "\n");
            ++trace_n;
        }
    }
    for (auto& [id, body] : physics_role_bodies) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        float x = e->world_x, y = e->world_y;
        if (mq_physics_get_transform(physics, body, &x, &y) == MQ_PHYSICS_OK) {
            e->world_x = x; e->world_y = y;
            host::entity_set_pos(this, (float)id, x, y);   // 写回 Transform2D（否则被 update_world 覆盖）
        }
    }
}

/* 把求解器**本帧**的接触对映射回实体 id（docs/15 §3、docs/19）。
 *
 * 设计取舍：
 *   · 引擎只提供**查询**，不提供"碰撞回调"——玩法（撞管死亡/得分）属于脚本层，
 *     引擎不硬编码任何组件名或事件语义（与"引擎即浏览器、游戏即资源"一致）；
 *   · 结果保持求解器顺序（a_index 升序）→ 确定性（docs/10 D4）；
 *   · 不认识的 body.index（例如 spawn_ball 的临时球）映射为 -1，脚本据此跳过；
 *   · 撞世界边界是**平面接触**（b_index = MQ_PHYSICS_PLANE_INDEX）→ 该对映射为 (-1,-1)，
 *     脚本用 physics_contact_pair() 区分"实体对实体"与"撞边界"；
 *   · 惰性重建：一次 step 后首次查询建表，之后复用；下一帧 step 置脏。 */
void Engine::refresh_contacts() {
    if (!contacts_dirty) return;
    contacts_dirty = false;
    contact_entities.clear();
    if (!physics) return;
    const uint32_t n = mq_physics_contact_count(physics);
    contact_entities.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        mq_physics_contact c{};
        if (mq_physics_contact_at(physics, i, &c) != 0) { contact_entities.emplace_back(-1, -1); continue; }
        int ea = -1, eb = -1;
        for (const auto& [id, body] : physics_role_bodies) {
            if (body.index == c.a_index) ea = id;
            if (body.index == c.b_index) eb = id;
        }
        contact_entities.emplace_back(ea, eb);
    }
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

/* 宿主 API / 控制面 / 嵌入宿主在原匿名命名空间里直接可见全部内部名；
 * 具名命名空间后用 using-directive 恢复**同一语义**（仅本 TU 有效）。 */
using namespace maque::rt;



bool mount_scene_source(mq_vfs* vfs, const std::string& scn_path, std::string& vfs_path, std::string& dir_out) {
    if (!vfs || scn_path.empty()) return false;
    const bool remote = scn_path.rfind("http://", 0) == 0 || scn_path.rfind("https://", 0) == 0;
    const bool asset = scn_path.rfind("asset://", 0) == 0;
    std::string dir, file, source;
    if (remote) {
        const size_t slash = scn_path.find_last_of('/');
        if (slash == std::string::npos || slash + 1 >= scn_path.size()) return false;
        dir = scn_path.substr(0, slash + 1);                  // 含结尾 '/' —— URL 基址
        file = scn_path.substr(slash + 1);
        source = dir;
    } else if (asset) {
        std::string logical = scn_path.substr(8);
        while (!logical.empty() && logical.front() == '/') logical.erase(logical.begin());
        const size_t slash = logical.find_last_of('/');
        file = slash == std::string::npos ? logical : logical.substr(slash + 1);
        if (file.empty()) return false;
        const std::string base = slash == std::string::npos ? std::string() : logical.substr(0, slash + 1);
        dir = "asset://" + base;
        source = dir;
    } else {
        const std::filesystem::path scnp(scn_path);
        dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();
        file = scnp.filename().string();
        source = "file://" + dir;
    }
    const int rc = mq_vfs_mount(vfs, "/", source.c_str());
    if (rc != MQ_VFS_OK) {
        std::print(stderr, "runtime: mount scene source '{}' failed (rc={})\n", source, rc);
        return false;
    }
    dir_out = dir;
    vfs_path = "/" + file;
    return true;
}

/* MAQUE_MOUNT="/cdn=https://host/assets/,/extra=../shared"：额外挂载（后挂载优先，docs/04 §5）。
 * 场景目录挂载之后应用：本地开发时把资源指向 CDN，或从 URL 场景挂本地补丁目录。 */
void apply_env_mounts(mq_vfs* vfs) {
    const char* spec = std::getenv("MAQUE_MOUNT");
    if (!spec || !*spec || !vfs) return;
    const std::string s(spec);
    size_t i = 0;
    while (i <= s.size()) {
        const size_t comma = s.find(',', i);
        const std::string item = s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) {
            const std::string mp = item.substr(0, eq), src = item.substr(eq + 1);
            const int rc = mq_vfs_mount(vfs, mp.c_str(), src.c_str());
            if (rc == MQ_VFS_OK) std::print("runtime: mount '{}' <- {}\n", mp, src);
            else std::print(stderr, "runtime: mount '{}' <- {} failed (rc={})\n", mp, src, rc);
        } else {
            std::print(stderr, "runtime: MAQUE_MOUNT item '{}' ignored (expected <mountpoint>=<source>)\n", item);
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
}

/* 装配引擎：VFS → 反射 → 渲染插件 → 场景 → 平台/输入 → 脚本 → 控制面。
 * 两条入口（命令行 / 嵌入宿主）共用，保证行为一致。 */
bool Engine::setup(const std::string& scn_path, const char* renderer_override) {
    MQ_TRACY_ZONE_N(runtime, "engine.setup");   // Tracy：装配阶段（VFS/反射/插件/场景/平台/脚本）
    /* 优雅退出：**无条件**安装（宿主 kill / CI 超时清理）——
     * 注意 SDL_HINT_NO_SIGNAL_HANDLERS 已阻止 SDL 接管；这里必须在平台创建之后仍持有处理器。 */
    signal(SIGTERM, on_quit_signal);
    signal(SIGINT, on_quit_signal);
    if (const char* cr = std::getenv("MAQUE_CRASH_REPORT"); cr && *cr) {   // 两条入口共用
        signal(SIGSEGV, on_fatal_signal);
        signal(SIGABRT, on_fatal_signal);
        signal(SIGFPE, on_fatal_signal);
        signal(SIGINT, on_fatal_signal);
        if (g_crash_fd < 0) g_crash_fd = maque::compat::open_fd(cr, maque::compat::kOpenWriteCreate);
    }
    vfs = mq_vfs_create();
    if (const char* lim = std::getenv("MAQUE_SCENE_INSTANCE_LIMIT")) {
        const int v = std::atoi(lim);
        if (v > 0) instance_limit = v;              // 实例数预算（CI/诊断可调，docs/32 §8）
    }
    std::string scn_vfs;
    if (!mount_scene_source(vfs, scn_path, scn_vfs, scn_dir)) {
        std::print("runtime: bad scene source '{}'\n", scn_path);
        return false;
    }
    apply_env_mounts(vfs);

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, scn_vfs.c_str(), &d, &n) == MQ_VFS_OK) { text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) { std::print("runtime: cannot read scene '{}'\n", scn_path); return false; }

    reflect = mq_reflect_create();
    mq_reflect_load_schema(reflect);
    scene_type = mq_reflect_type_by_name(reflect, "scn.Scene");
    if (!scene_type) { std::print("runtime: no scn.Scene type\n"); return false; }

#ifdef MQ_RENDERER_STATIC_NULL
    (void)renderer_override;
    const char* plugin = "static:null";
    rapi = mq_renderer_null_api();
    if (!rapi) {
        std::print("runtime: static null renderer unavailable\n");
        return false;
    }
#elif defined(MQ_RENDERER_STATIC_GFX)
    (void)renderer_override;
    const char* plugin = "static:gfx";
    rapi = (const mq_renderer_v2*)mq_module_query(MQ_RENDERER_API_NAME, MQ_RENDERER_API_VERSION);
    if (!rapi) {
        std::print("runtime: static gfx renderer unavailable\n");
        return false;
    }
#else
    const char* plugin = renderer_override ? renderer_override : std::getenv("MAQUE_RENDERER");
#ifdef MQ_RENDERER_DEFAULT
    if (!plugin) plugin = MQ_RENDERER_DEFAULT;
#endif
    /* 平台默认插件文件名：Windows 是 maque_gfx.dll，类 UNIX 是 libmaque_gfx.so。 */
#if defined(_WIN32)
    if (!plugin) plugin = "maque_gfx.dll";
#else
    if (!plugin) plugin = "libmaque_gfx.so";
#endif
    host = mq_host_create();
    mq_loaded_module* rmod = nullptr;
    if (mq_host_load(host, plugin, &rmod) != 0 ||
        !(rapi = (const mq_renderer_v2*)mq_host_query(host, MQ_RENDERER_API_NAME, MQ_RENDERER_API_VERSION))) {
        std::print("runtime: cannot load renderer plugin '{}'\n", plugin);
        return false;
    }
#endif
    const char* backend = nullptr;
    rself = rapi->create(&backend);
    if (!rself) { std::print("runtime: renderer create failed\n"); return false; }
    std::print("runtime: renderer '{}' backend='{}'\n", plugin, backend ? backend : "?");

    audio_setup();                               // 音频实例先于场景：脚本 on_start 可 audio_play
    obs = mq_obs_create();                       // 场景装配期即可能产生事件（脚本错误等）
    if (!load_scene_into(scn_path)) return false;
    /* 窗口标题：MAQUE_TITLE 覆盖场景标题（默认用场景 title） */
    std::string win_title = scene->desc.title;
    if (const char* ts = std::getenv("MAQUE_TITLE"); ts && *ts) win_title = ts;
    platform = mq_platform_create(win_title.c_str(), win_w, win_h);
    window = mq_platform_has_window(platform) != 0;
    /* load_scene_into 发生在建窗前，只能用逻辑尺寸预估；现在用真实 drawable 校正 TW/TH。 */
    if (window) {
        mq_platform_drawable_size(platform, &drawable_last_w, &drawable_last_h);
        rebuild_render_target();
    }
    /* ---- 3D/PBR：能力自省 + 直接呈现探测（plans/18-pbr-3d.md M0）----
     *   · caps 总是调用（记录适配器能力，MAQUE_RENDER3D_INFO/wgpu 时打印）；
     *   · 呈现模式：`auto`（默认）| `sdl` | `wgpu` | `off`
     *       - auto：没有"必须读 renderer 后台缓冲像素"的消费者时**优先** wgpu 交换链直呈，
     *         省掉每帧 GPU→CPU 回读（回读是每帧一次 GPU 同步点，桌面/浏览器抢 GPU 就抖）；
     *         attach 失败自动回退 SDL。`MAQUE_CAPTURE`（SDL 平台抓帧）只能走 SDL。
     *       - sdl：强制 SDL_Renderer 呈现（老行为，像素可抓）。
     *       - wgpu：强制直呈（render3d_present 用例）。 */
    if (rapi->caps && rself) {
        const bool want_wgpu = [] {
            const char* p = std::getenv("MAQUE_PRESENT");
            if (p && (std::strcmp(p, "wgpu") == 0)) return true;
            if (p && *p && std::strcmp(p, "auto") != 0) return false;   // off/software/accelerated/sdl
            const char* cap = std::getenv("MAQUE_CAPTURE");             // SDL 抓帧：必须走 renderer
            return !(cap && *cap);
        }();
        const char* info = std::getenv("MAQUE_RENDER3D_INFO");
        mq_renderer_caps caps{};
        if (rapi->caps(rself, &caps) == 0 && (want_wgpu || (info && *info && *info != '0')))
            std::print("runtime: renderer caps tex={} lights={} target_fmt={} depth_fmt={} direct_present={} hdr={}\n",
                       caps.max_texture_size, caps.max_lights, caps.target_format, caps.depth_format,
                       caps.direct_present, caps.hdr);
        direct_requested = want_wgpu && window && rapi->attach_window;
        if (direct_requested) {
            mq_platform_native_window nw{};
            if (mq_platform_get_native_window(platform, &nw) != 0) {
                mq_renderer_window rw{};
                rw.kind = nw.kind;
                rw.display = nw.display;
                rw.handle = nw.handle;
                rw.extra = nw.extra;
                int32_t dw = 0, dh = 0;
                mq_platform_drawable_size(platform, &dw, &dh);
                rw.width = dw;
                rw.height = dh;
                const int rc = rapi->attach_window(rself, &rw);
                std::print("runtime: direct present attach_window kind={} size={}x{} rc={}（{}）\n",
                           rw.kind, rw.width, rw.height, rc, rc == 0 ? "wgpu swapchain" : "回退离屏+SDL");
                direct_present = (rc == 0);
                direct_window_handle = direct_present ? nw.handle : nullptr;
            } else {
                std::print("runtime: direct present 不可用（platform 未给出原生窗口句柄）\n");
            }
        }
    }
    input = mq_input_create();
    mq_input_set_hit_test(input, runtime_hit_test, this);
    mq_input_set_parent_fn(input, runtime_parent, this);
    mq_input_add_handler(input, runtime_input_handler, this);
    { const char* hs = std::getenv("MAQUE_HITS"); if (hs && *hs && *hs != '0') trace_hits = true; }
    { const char* ko = std::getenv("MAQUE_KEEP_OPEN"); if (ko && *ko && *ko != '0') keep_open = true; }
    if (const char* sp = std::getenv("MAQUE_SHOT"); sp && *sp) shot_path = sp;
    if (const char* sf = std::getenv("MAQUE_SHOT_FRAME"); sf && *sf) shot_frame = std::atoi(sf);
    /* 批量截图：一条命令拍多帧（动画/物理只拍首帧看不出对错）。 
     *   MAQUE_SHOT_SEQ   输出模板（`{}` → 帧号；无 `{}` 则插在扩展名前）
     *   MAQUE_SHOT_FRAMES 精确帧号，逗号分隔（如 "1,30,60"）
     *   MAQUE_SHOT_EVERY  每 N 帧拍一张；MAQUE_SHOT_COUNT 最多几张（默认到 shot_last 为止） */
    if (const char* sq = std::getenv("MAQUE_SHOT_SEQ"); sq && *sq) shot_seq = sq;
    if (const char* sl = std::getenv("MAQUE_SHOT_FRAMES"); sl && *sl) {
        std::string s(sl), cur;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ',') {
                if (!cur.empty()) shot_list.push_back(std::atoi(cur.c_str()));
                cur.clear();
            } else if (s[i] != ' ') cur.push_back(s[i]);
        }
    }
    if (const char* se = std::getenv("MAQUE_SHOT_EVERY"); se && *se) shot_every = std::atoi(se);
    if (const char* scn = std::getenv("MAQUE_SHOT_COUNT"); scn && *scn) shot_count = std::atoi(scn);
    snaps = mq_snapshot_pool_create(8, 1 << 16);
    { const char* cp = std::getenv("MAQUE_CHECKPOINTS"); if (cp && *cp && *cp != '0') cp_active = true; }
    /* 场景/脚本/资源/物理装配统一走 load_scene_into（与 director.loadScene 同一条路径） */
    control = mq_control_create();
    /* 控制面方法由 DSL（schema/ control_list）声明、codegen 生成注册表 —— 引擎零手写注册
     * （docs/21 §1/§5：公开接口只声明一次）。此处只需提供实例上下文。 */
    mq_control_register_generated(control, this);
    /* MCP 工具表（含类型化 inputSchema）同样是生成物（docs/21 §6.3） */
    mq_control_set_tools_json(control, [](void*) { return mq_control_tools_json(); }, nullptr);
    std::print("runtime: platform window={} input=on\n", window ? 1 : 0);
    return true;
}

/* ---- 窗口 resize / 最大化：屏幕、渲染目标与设计坐标分层 --------------------
 *   · Scene.width/height：世界与 UI 的参考坐标，不分配颜色附件；
 *   · drawable：屏幕设备像素，默认就是主颜色附件尺寸；
 *   · render_scale：相对 drawable 的独立输出倍率，只有 !=1 时主目标才与屏幕不同；
 *   · Canvas：把设计坐标映射到目标内的 viewport/scissor，不参与目标分辨率选择。
 * 无窗口的截图/嵌入宿主没有 drawable，以设计尺寸（多视图横向并排）作为虚拟屏幕。 */
void Engine::resolve_render_scale() {
    render_scale_req = scene && scene->desc.render_scale > 0.0F ? scene->desc.render_scale : 1.0F;
    /* 旧场景兼容：Canvas.render_scale 曾是唯一入口；显式 >0 时继续生效。 */
    if (scene && canvas_entity >= 0) {
        if (SceneNode* ce = scene->at(canvas_entity)) {
            for (auto& [tid, blob] : ce->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
                float legacy = 0.0F;
                read_f32(d, blob.data(), "render_scale", &legacy);
                if (legacy > 0.0F) render_scale_req = legacy;
                break;
            }
        }
    }
    if (const char* e = std::getenv("MAQUE_RENDER_SCALE")) {
        const float value = (float)std::atof(e);
        if (value > 0.0F) render_scale_req = value;
    }
    render_scale_req = std::clamp(render_scale_req, 0.25F, 4.0F);
#ifdef MQ_RENDERER_STATIC_NULL
    window_intent = false;
#else
    const char* present_env = std::getenv("MAQUE_PRESENT");
    window_intent = !std::getenv("MAQUE_HEADLESS") && !std::getenv("MAQUE_SHOT") &&
                    !(present_env && (std::strcmp(present_env, "off") == 0 || std::strcmp(present_env, "0") == 0));
#endif
}

void Engine::rebuild_render_target() {
    if (!scene) return;
    int32_t screen_w = std::max(1, W * std::max(1, nviews));
    int32_t screen_h = std::max(1, H);
    if (window_intent) {
        int32_t dw = 0, dh = 0;
        if (platform && window) mq_platform_drawable_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) {
            screen_w = dw;
            screen_h = dh;
        } else if (win_w > 0 && win_h > 0) {
            /* 建窗前只有逻辑尺寸可用；平台创建后会用真实 drawable 再重建一次。 */
            screen_w = win_w;
            screen_h = win_h;
        }
    }
    TW = std::max(1, (int)std::lround((float)screen_w * render_scale_req));
    TH = std::max(1, (int)std::lround((float)screen_h * render_scale_req));
    /* Canvas 尚未解析时给文本/装配期一个保守倍率；帧首 canvas_resolve 会写入精确值。 */
    const float per_view_w = (float)TW / (float)std::max(1, nviews);
    ss = std::min(per_view_w / (float)std::max(1, W), (float)TH / (float)std::max(1, H));
    if (!(ss > 0.0F)) ss = 1.0F;
    views.assign((size_t)std::max(1, nviews), mq_renderer_view{});
}

void Engine::update_main_views() {
    if (!scene || TW <= 0 || TH <= 0) return;
    const int count = std::max(1, nviews);
    if ((int)views.size() != count) views.assign((size_t)count, mq_renderer_view{});
    const float ww = canvas.window_w > 0.0F ? canvas.window_w : (float)TW / render_scale_req;
    const float wh = canvas.window_h > 0.0F ? canvas.window_h : (float)TH / render_scale_req;
    const float rx = (float)TW / std::max(ww, 1.0F);
    const float ry = (float)TH / std::max(wh, 1.0F);
    const float whole_x = canvas.offset_x * rx;
    const float whole_y = canvas.offset_y * ry;
    const float whole_w = (canvas.viewport_w > 0.0F ? canvas.viewport_w : ww) * rx;
    const float whole_h = (canvas.viewport_h > 0.0F ? canvas.viewport_h : wh) * ry;
    const float view_w = whole_w / (float)count;
    const float design_zoom_x = view_w / (float)std::max(1, W);
    const float design_zoom_y = whole_h / (float)std::max(1, H);
    ss = std::min(design_zoom_x, design_zoom_y);
    if (!(ss > 0.0F)) ss = 1.0F;

    for (int i = 0; i < count; ++i) {
        mq_renderer_view& v = views[(size_t)i];
        v = mq_renderer_view{};
        v.vx = whole_x + view_w * (float)i;
        v.vy = whole_y;
        v.vw = view_w;
        v.vh = whole_h;
        v.pan = 0.0F;
        v.eye = (i - (count - 1) * 0.5F) * scene->desc.eye_sep;
        v.cx = W * 0.5F;
        v.cy = H * 0.5F;
        v.zoom = design_zoom_x;
        v.zoom_y = design_zoom_y;
    }
    for (auto& [idx, e] : scene->nodes) {
        if (comp_by_name(*this, idx, "render.RenderTexture", nullptr)) continue;
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_CAMERA)) continue;
            bool active = true;
            if (const mq_field_desc* fa = mq_reflect_field(d, "active")) active = field_get_bool(fa, blob.data());
            if (!active) continue;
            float cx = W * 0.5F, cy = H * 0.5F, zoom = 1.0F, rot = 0.0F;
            read_f32(d, blob.data(), "x", &cx);
            read_f32(d, blob.data(), "y", &cy);
            read_f32(d, blob.data(), "zoom", &zoom);
            read_f32(d, blob.data(), "rotation", &rot);
            for (auto& v : views) {
                v.cx = cx;
                v.cy = cy;
                const float camera_zoom = std::max(zoom, 1.0e-5F);
                v.zoom *= camera_zoom;
                v.zoom_y = (v.zoom_y > 0.0F ? v.zoom_y : v.zoom) * camera_zoom;
                v.rot = rot;
            }
        }
    }
}

/* 窗口事件给逻辑尺寸；实际目标始终从 drawable 设备像素重算。 */
void Engine::sync_window_size(int32_t log_w, int32_t log_h) {
    if (!platform || !window) return;
    if (log_w <= 0 || log_h <= 0) return;
    int32_t surface_w = 0, surface_h = 0;
    mq_platform_drawable_size(platform, &surface_w, &surface_h);
    if (surface_w <= 0 || surface_h <= 0) return;
    if (win_last_w <= 0) {
        win_last_w = log_w; win_last_h = log_h;
        drawable_last_w = surface_w; drawable_last_h = surface_h;
        return;
    }
    const bool logical_changed = log_w != win_last_w || log_h != win_last_h;
    const bool drawable_changed = surface_w != drawable_last_w || surface_h != drawable_last_h;
    if (!logical_changed && !drawable_changed) return;
    const int32_t old_w = win_last_w, old_h = win_last_h;
    const int old_tw = TW, old_th = TH;
    win_last_w = log_w;
    win_last_h = log_h;
    drawable_last_w = surface_w;
    drawable_last_h = surface_h;
    win_w = log_w;
    win_h = log_h;
    rebuild_render_target();

    if (direct_present && rapi && rself && rapi->resize_surface) {
        const int32_t rc = rapi->resize_surface(rself, surface_w, surface_h);
        if (rc != 0) std::print(stderr, "runtime: resize_surface({}x{}) 失败 rc={}\n",
                                surface_w, surface_h, rc);
    }
    if (TW != old_tw || TH != old_th) {
        frame_pixels.clear();
        frame_w = frame_h = 0;
    }
    std::print("runtime: window resized {}x{} → {}x{}: render target {}x{} → {}x{} "
               "(render_scale {:.3f}, views={})\n",
               old_w, old_h, log_w, log_h, old_tw, old_th, TW, TH, render_scale_req, nviews);
}

/* CI/演示钩子：MAQUE_WINDOW_EVENT_AT="<frame>:<op>[;...]" —— 在指定模拟帧对**真实窗口**
 * 下发操作（op = WxH 设尺寸 / max 最大化 / restore 还原），走真实 SDL 窗口事件路径
 * （SDL_SetWindowSize / SDL_MaximizeWindow → SIZE_CHANGED → 平台队列 → sync_window_size），
 * 与用户拖拽标题栏/点最大化按钮**同一条链路**。 */
void Engine::window_event_hooks() {
    if (!platform || !window) return;
    const char* spec = std::getenv("MAQUE_WINDOW_EVENT_AT");
    if (!spec || !*spec) return;
    std::string s(spec), item;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] != ';') { item.push_back(s[i]); continue; }
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const int at = std::atoi(item.substr(0, colon).c_str());
            const std::string op = item.substr(colon + 1);
            if ((int)frame == at) {
                if (op == "max") {
                    const int rc = mq_platform_set_window_maximized(platform, 1);
                    std::print("runtime: window event frame={} maximize (rc={})\n", (long long)frame, rc);
                } else if (op == "restore") {
                    const int rc = mq_platform_set_window_maximized(platform, 0);
                    std::print("runtime: window event frame={} restore (rc={})\n", (long long)frame, rc);
                } else {
                    int w = 0, h = 0;
                    if (std::sscanf(op.c_str(), "%dx%d", &w, &h) == 2) {
                        const int rc = mq_platform_set_window_size(platform, w, h);
                        std::print("runtime: window event frame={} resize {}x{} (rc={})\n", (long long)frame, w, h, rc);
                    } else {
                        std::print(stderr, "runtime: MAQUE_WINDOW_EVENT_AT 帧 {} 操作 '{}' 无法识别\n", at, op);
                    }
                }
            }
        }
        item.clear();
    }
}

/* 场景装配（setup 与 director.loadScene 共用）：解析 → 实例化 → 尺寸/视图 → 物理世界
 * → 脚本 VM/绑定 → 监视 → 资源预解析 → 物理组件建体。切场景同样走此处（docs/12/20）。 */
bool Engine::load_scene_into(const std::string& scn_path) {
    MQ_TRACY_ZONE_N(runtime, "scene.load");   // Tracy：场景加载（资源/实体/脚本装配）
    /* 场景来源挂载（本地目录 / http(s) URL 目录）——后挂载者优先 → 新场景来源生效 */
    std::string scn_vfs;
    if (!mount_scene_source(vfs, scn_path, scn_vfs, scn_dir)) return false;
    apply_env_mounts(vfs);        // MAQUE_MOUNT 覆盖目录保持最高优先级（幂等，不重复追加）

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, scn_vfs.c_str(), &d, &n) == MQ_VFS_OK) {
          text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) return false;

    /* docs/34 P0：包清单决定是否走"每语言一个 VM + 模块/实例"路径（在旧实例销毁前判定）。 */
    detect_package();

    /* ---- 世界重置（切场景：旧世界全部释放；脚本状态不跨场景保留，等价  onDestroy）---- */
    reset_instances();                   // 实例树/全部实例 VM（覆盖层是独立总线，不在此列）
    spine_release();                     // Spine 实例随世界一起释放（派生数据，重建即可）
    clear_asset_caches();                // ⚠️ 纹理/网格缓存键是相对路径，换场景挂载点后必须清空
    agent_paths.clear();
    node_events.clear();
    physics_role_bodies.clear();
    particles.clear();
    if (audio) {
        for (auto& kv : audio_clips) mq_audio_clip_release(kv.second);
        audio_clips.clear();
        last_listener_master = 1.0F;
    }
    director_paused = false;
    /* 实体 id 水位跨场景保留：旧场景的 id 永不复用（常驻脚本/快照引用的陈旧 id 必须
     * 保持无效，绝不能别名到新场景的同号实体）。 */
    const int preserved_entity_id = scene ? scene->next_id : 0;
    /* 热重载可能已把根场景换成堆上的 scene_owned；切场景/重装配必须释放它，
     * 否则旧的整棵 Scene 从 scene_store 之外泄漏（且下一个 reload 会再叠一层）。 */
    scene_owned.reset();
    scene_store = Scene();
    scene_store.next_id = preserved_entity_id;
    scene = &scene_store;
    scn_vfs_path = scn_vfs;              // 根实例 source（docs/32）
    scn_full_path = scn_path;
    /* 根场景 = 0 号实例：先建记录（实体归属/嵌套实例的父链都依赖它），再装配节点树。
     * 根场景的资源基准 = VFS 根（挂载点就是场景目录）；scene_chain 用于循环实例化检测。 */
    scene_chain.clear();
    scene_chain.push_back(scn_vfs);
    building_instance = 0;
    building_base.clear();
    if (create_instance(-1, -1, scn_vfs, 0) < 0) return false;   // 根实例固定 id=0
    if (SceneInstanceRuntime* root = instance_at(0)) { root->state = 1; }
    if (!scene->deserialize(*this, text)) return false;
    scene->instantiate(*this);
    scene_chain.pop_back();
    scene->update_world(*this);
    {
        SceneInstanceRuntime* root = instance_at(0);
        if (root) {
            for (int eid : scene->roots)
                if (entity_instance.find(eid) != entity_instance.end() && entity_instance[eid] == 0)
                    root->roots.push_back(eid);
        }
    }
    /* 窗口标题跟随场景（director.loadScene 切 demo 时标题栏同步；首次装配时 platform 尚未创建，
     * 起点标题由 setup 用场景 title 建窗）。MAQUE_TITLE 显式覆盖时不动。 */
    if (platform && !std::getenv("MAQUE_TITLE")) mq_platform_set_title(platform, scene->desc.title.c_str());

    W = scene->desc.width > 0 ? scene->desc.width : 320;
    H = scene->desc.height > 0 ? scene->desc.height : 240;
    nviews = scene->desc.views > 0 ? scene->desc.views : 1;
    /* 启动期覆盖（宿主/CI 用；docs/05、docs/22）：
     *   MAQUE_WINDOW=WxH  窗口尺寸（世界坐标不变，仅改可视区）
     *   MAQUE_VIEWS=n     视图数（1 = 单视图；>1 = 并排多视图/立体） */
    /* **默认窗口 800x650**（用户要求）：世界/渲染目标仍是场景尺寸（W×H），
     * 窗口内等比缩放 + 居中留边显示 —— 场景比窗口大就缩小，小就放大，都不裁剪。
     * 之前的默认是"窗口 = 场景尺寸"，大场景（如 spine 的 all.scn 1600x1300）
     * 在 1080p 屏上会超出显示器，必须改成固定小窗口。
     *
     * MAQUE_WINDOW=WxH 覆盖；`0x0` 表示**不建窗口**（CI/无显示环境用），
     * 所以解析成功时原样接受（含 0），不要被"必须 >0"的校验吃掉语义。
     * `director.loadScene` 切场景时窗口已存在 → 沿用**当前**窗口逻辑尺寸
     * （用户可能刚拖拽/最大化过；默认值/MAQUE_WINDOW 只决定首次建窗）。 */
    if (platform && window) {
        int32_t ww = 0, wh = 0;
        mq_platform_window_size(platform, &ww, &wh);
        if (ww > 0 && wh > 0) { win_w = ww; win_h = wh; }
        std::print("runtime: window {}x{} (current; world {}x{}, scale {:.2f}x)\n",
                   win_w, win_h, W, H,
                   std::min((float)win_w / (float)W, (float)win_h / (float)H));
    } else {
        win_w = 800; win_h = 650;
        if (const char* ws = std::getenv("MAQUE_WINDOW")) {
            int ow = 0, oh = 0;
            if (std::sscanf(ws, "%dx%d", &ow, &oh) == 2) {
                win_w = ow; win_h = oh;             // 只改窗口；世界/目标不变 → 窗口内等比缩放显示
                if (win_w > 0 && win_h > 0) {
                    std::print("runtime: window {}x{} (world {}x{}, scale {:.2f}x)\n",
                               win_w, win_h, W, H,
                               std::min((float)win_w / (float)W, (float)win_h / (float)H));
                } else {
                    std::print("runtime: window disabled (MAQUE_WINDOW={})\n", ws);
                }
            } else {
                std::print(stderr, "runtime: MAQUE_WINDOW='{}' 格式应为 WxH，已忽略\n", ws);
            }
        } else {
            std::print("runtime: window {}x{} (default; world {}x{}, scale {:.2f}x)\n",
                       win_w, win_h, W, H,
                       std::min((float)win_w / (float)W, (float)win_h / (float)H));
        }
    }
    if (const char* vs = std::getenv("MAQUE_VIEWS")) {
        const int v = std::atoi(vs);
        if (v >= 1 && v <= 4) nviews = v;
    }
    canvas_setup();                             // 先定位当前场景画布；render_scale 兼容入口依赖它
    resolve_render_scale();
    rebuild_render_target();
    win_last_w = win_w; win_last_h = win_h;   // resize 检测基线（首帧再按真实窗口校正）
    rng = scene->desc.seed * 2654435761u + 12345u;
    if (physics) { mq_physics_destroy(physics); physics = nullptr; }
    {
        /* 物理装配（坐标约定权威说明见 docs/15 §3.1）：
         *   · 物理世界**就是世界坐标（y 向上）**，`scn.Scene.gravity_y` 是该坐标系下的 y 分量
         *     → 直接透传，不取反；默认 -900（负 = 屏幕向下掉，正 = 向上加速）。
         *   · 边界 = 场景矩形在世界坐标下的四条平面：min=(0,0) 是**屏幕左下**、max=(W,H) 是右上。
         *     所以物体会被挡在 y ∈ [radius, H-radius]（这才是"屏幕底部的地面"）。 */
        mq_physics_config pcfg{};
        pcfg.struct_size = (uint32_t)sizeof pcfg;
        pcfg.gravity_y = scene->desc.gravity_y;
        pcfg.fixed_dt = 1.0F / 60.0F;
        pcfg.solver_iterations = 4;
        pcfg.max_bodies = 1024;
        pcfg.seed = scene->desc.seed;
        physics = mq_physics_create(&pcfg);
        mq_physics_set_bounds(physics, 0.0F, 0.0F, (float)W, (float)H);
    }
    if (nav) { mq_nav_destroy(nav); nav = nullptr; }
    std::print("runtime: scene '{}' title='{}' script='{}' scripts={} {}x{} views={}\n",
               scn_path, scene->desc.title, scene->desc.script, scene->desc.scripts.size(), W, H, nviews);

    /* 多脚本（多语言）：scripts 非空则按**声明顺序**逐个装载（顺序 = 事件投递顺序）。
     * 根场景 = 0 号实例；脚本装载走统一的实例 helper（与嵌套实例同一路径，docs/32）。 */
    std::vector<std::string> load_list;
    if (!scene->desc.scripts.empty()) load_list = scene->desc.scripts;
    else if (!scene->desc.script.empty()) load_list.push_back(scene->desc.script);
    SceneInstanceRuntime* root_inst = instance_at(0);
    if (!root_inst) return false;
    if (!load_instance_scripts(*root_inst, load_list, true)) return false;

    if (watcher) { mq_watcher_destroy(watcher); watcher = nullptr; }
    const bool remote_scene = scn_path.rfind("http://", 0) == 0 || scn_path.rfind("https://", 0) == 0;
    const char* watch = std::getenv("MAQUE_WATCH");
    if (watch && *watch && *watch != '0') {
        if (remote_scene) {
            // 远程场景没有本地文件可监视：热重载不可用（用 MAQUE_MOUNT 挂本地覆盖目录做开发迭代）
            std::print("runtime: hot reload disabled for remote scene '{}'\n", scn_path);
        } else {
            watcher = mq_watcher_create();
            mq_watcher_add(watcher, (fs::path(scn_dir) / scn_vfs_path.substr(1)).string().c_str());
            for (const std::string& sp : script_paths)          // 监视**全部**脚本（多脚本热重载）
                mq_watcher_add(watcher, (fs::path(scn_dir) / sp).string().c_str());
            /* 嵌套场景（docs/32 §5.3）：源 .scn + 各实例脚本都纳入监视（去重由 watcher 处理）。 */
            for (auto& [iid, up] : instances) {
                (void)iid;
                if (!up || up->id == 0 || up->source.empty()) continue;
                mq_watcher_add(watcher, (fs::path(scn_dir) / up->source.substr(1)).string().c_str());
                for (const std::string& sp : up->scripts)
                    mq_watcher_add(watcher, (fs::path(scn_dir) / vfs_join(up->base, sp).substr(1)).string().c_str());
            }
            std::print("runtime: hot reload watching {} file(s)\n", mq_watcher_count(watcher));
        }
    }
    setup_render_textures();                       /* 相机离屏目标先分配：消费者预载时 URI 已可解析 */
    preload_scene_assets();
    setup_3d();                                 /* 扫描 render.* 3D 组件 + 上传内建 cube（2D↔3D 切场景需重算） */
    setup_shader_passes();                      /* 场景资源全屏 shader（.wgsl，相对场景目录；docs/13） */
    setup_shader_graphs();                      /* 多通道 shader 图（program JSON，相对场景目录；docs/13） */
    spine_setup();                              // Spine：骨架/图集/页纹理装配（装配期做 IO，不在模拟帧内）
    text_setup();                               // 字体（MAQUE_FONT → 系统字体 → 内置 5x7）
    sim_setup();                                // 输入模拟器（MAQUE_SIM / MAQUE_SIM_CLICK）
    storage_setup();                            // 键值存储（docs/25：默认 <场景目录>/.maque）
    if (const char* sa = std::getenv("MAQUE_SAFE_AREA")) {   // "l,t,r,b"（设备像素；测试/刘海屏）
        float l = 0, t = 0, r = 0, b = 0;
        if (std::sscanf(sa, "%f,%f,%f,%f", &l, &t, &r, &b) == 4) {
            canvas_safe[0] = l; canvas_safe[1] = t; canvas_safe[2] = r; canvas_safe[3] = b;
            if (platform) mq_platform_set_safe_area(platform, l, t, r, b);
        }
    }
    /* 全局脚本 / 常驻覆盖层脚本：本场景声明了就激活/切换（同一路径复用已有 VM → 状态跨场景保留）；
     * 未声明则保持现有实例 —— director.loadScene 切场景时游戏服务/外壳不消失。 */
    if (!scene->desc.global_script.empty()) (void)ensure_global_script(scene->desc.global_script);
    if (!scene->desc.overlay_script.empty()) (void)ensure_overlay_script(scene->desc.overlay_script);
    physics_components_setup();
    /* 生命周期：on_start **后序**（子实例先 ready，根场景最后）；然后全局脚本，最后覆盖层
     * —— 全局服务在 HUD 之前拿到本场景状态（docs/32 §16.1）。 */
    const uint32_t err0 = instance_script_errors();
    (void)dispatch_instance_on_start(0);
    (void)call_global_event("on_start", 0.0, 0.0, 0.0, nullptr);
    (void)call_overlay_event("on_start", 0.0, 0.0, 0.0, nullptr);
    {
        char ebuf[512] = { 0 };
        if (instance_script_errors() != err0) {
            if (bus) mq_script_bus_last_error(bus, ebuf, (int)sizeof ebuf);
            if (!ebuf[0] && global_bus)
                mq_script_bus_last_error(global_bus, ebuf, (int)sizeof ebuf);
            if (!ebuf[0] && overlay_bus)
                mq_script_bus_last_error(overlay_bus, ebuf, (int)sizeof ebuf);
            if (!ebuf[0])
                std::snprintf(ebuf, sizeof ebuf, "scene instance script error");
            write_crash_context(ebuf);
        }
    }
    return true;
}

void Engine::teardown() {
    MQ_TRACY_ZONE_N(runtime, "engine.teardown");   // Tracy：退出清理
    /* ImGui：先关官方后端再销毁上下文（否则退出时断言 "Forgot to shutdown Renderer backend?"）。 */
    if (imgui_inited) {
        mq_imgui_shutdown();
        if (imgui_tex >= 0 && rapi && rself && rapi->destroy_texture) rapi->destroy_texture(rself, imgui_tex);
        imgui_tex = -1;
        imgui_inited = false;
        imgui_backend = false;
    }
    /* 控制端点：统一在此销毁（含 UDS socket 文件清理）——正常退出与 MAQUE_CONTROL 路径都经过这里 */
    if (ctl_server) { mq_control_server_destroy(ctl_server); ctl_server = nullptr; }
    /* 释放顺序：先释放"依赖 reflect 的资源"（clips 的字符串成员要经反射析构），再销毁 reflect。 */
    if (control) { mq_control_destroy(control); control = nullptr; }
    reset_instances();                                                      // 实例树 + 全部主脚本 VM
    unload_global();                                                        // 全局脚本 VM
    unload_overlay();                                                       // 常驻覆盖层 VM
    shutdown_module_vm();                                                   // docs/34：包脚本的单 JS VM
    if (obs) { mq_obs_destroy(obs); obs = nullptr; }
    if (reflect) {
        const mq_type_desc* ct = mq_reflect_type_by_name(reflect, "asset.AnimationClip");
        if (ct) for (auto& c : clips) destroy_blob(*this, ct->type_id, c.blob);
        clips.clear();
    } else {
        clips.clear();
    }
    agent_paths.clear();
    for (auto* s : checkpoints) mq_snapshot_destroy(s);
    checkpoints.clear();
    mq_snapshot_destroy(snap_manual); snap_manual = nullptr;
    mq_snapshot_pool_destroy(snaps); snaps = nullptr;
    mq_watcher_destroy(watcher); watcher = nullptr;
    /* 场景 blob 的**动态成员**（string/vector）析构要查反射类型表：必须在
     * mq_reflect_destroy 之前销毁根场景，否则 destroy_blob 取不到类型描述，
     * 只 clear() 掉 blob 而**跳过 std::string 析构** —— 组件里所有路径/名字的
     * 堆缓冲在退出时泄漏（scene_owned 里的热重载场景同理，整棵对象一起释放）。 */
    if (scene == &scene_store) scene_store = Scene();
    scene_owned.reset();
    scene = nullptr;
    if (reflect) { mq_reflect_destroy(reflect); reflect = nullptr; }
    if (sim) { mq_sim_destroy(sim); sim = nullptr; }
    if (font) { mq_font_destroy(font); font = nullptr; }
    font_atlases.clear();
    mq_nav_destroy(nav); nav = nullptr;
    mq_physics_destroy(physics); physics = nullptr;
    spine_release();                     // Spine 实例/骨架数据（纹理归 tex_cache → 随渲染器销毁）
    mq_input_destroy(input); input = nullptr;
    destroy_render_textures();           // 相机离屏目标（公共纹理句柄）
    destroy_shader_passes();             // 场景资源 shader pass（句柄归渲染器，先于销毁渲染器释放）
    destroy_shader_graphs();             // 多通道 shader 图（阶段句柄 + 中间缓冲纹理）
    audio_teardown();                    // 先停设备/join 回调，再拆平台（SDL）
    if (rapi && rself) { rapi->destroy(rself); rself = nullptr; }
    mq_platform_destroy(platform); platform = nullptr;
#if !defined(MQ_RENDERER_STATIC_NULL) && !defined(MQ_RENDERER_STATIC_GFX)
    mq_host_destroy(host);
#endif
    host = nullptr;
    mq_vfs_destroy(vfs); vfs = nullptr;
    if (storage) { mq_storage_destroy(storage); storage = nullptr; }
}

/* ---- 音频生命周期（docs/30）---- */
void Engine::audio_setup() {
    mq_audio_desc d{};
    d.struct_size = sizeof d;
    d.sample_rate = 48000;
    d.channels = 2;
    d.buffer_frames = 1024;
    d.max_voices = 64;
    d.backend = MQ_AUDIO_BACKEND_NULL;   // 默认静音：逻辑/脚本可用，CI 不弹设备
    bool want_device = false;
    if (const char* env = std::getenv("MAQUE_AUDIO"); env && *env) {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0 ||
            std::strcmp(env, "null") == 0) {
            d.backend = MQ_AUDIO_BACKEND_NULL;
        } else if (std::strcmp(env, "sdl") == 0) {
            d.backend = MQ_AUDIO_BACKEND_SDL;
            want_device = true;
        } else {   // 1 / on / auto
            d.backend = MQ_AUDIO_BACKEND_AUTO;
            want_device = true;
        }
    }
    if (const char* dev = std::getenv("MAQUE_AUDIO_DEVICE"); dev && *dev) d.device = dev;
    if (const char* buf = std::getenv("MAQUE_AUDIO_BUFFER"); buf && *buf) {
        const int v = std::atoi(buf);
        if (v > 0) d.buffer_frames = (uint32_t)v;
    }
    const char* cap_path = std::getenv("MAQUE_AUDIO_CAPTURE");
    const bool capture = cap_path && *cap_path;
    if (capture) {
        /* 抓音必须独占混音推进（否则设备回调 + 抓音各 mix 一次 → playhead 双倍消耗） */
        d.backend = MQ_AUDIO_BACKEND_NULL;
        want_device = false;
    }
    audio = mq_audio_create(&d);
    if (audio) {
        mq_audio_stats st{};
        st.struct_size = sizeof st;
        mq_audio_get_stats(audio, &st);
        audio_cap_rate = st.sample_rate;
        audio_cap_channels = st.channels;
        std::print("runtime: audio {} backend rate={} ch={}\n",
                   capture ? "capture(null pump)"
                           : (want_device ? "device requested" : "null (MAQUE_AUDIO=off/null)"),
                   st.sample_rate, st.channels);
        if (capture) audio_capture_start(cap_path);
    } else {
        std::print(stderr, "runtime: audio init failed（脚本 audio_* 将返回失败）\n");
    }
}

void Engine::audio_teardown() {
    audio_capture_finish();
    if (audio) {
        mq_audio_destroy(audio);   // 内含 stop/close（join 音频线程）
        audio = nullptr;
    }
    for (auto& kv : audio_clips) mq_audio_clip_release(kv.second);
    audio_clips.clear();
}

void Engine::audio_capture_start(const char* path) {
    audio_cap = std::fopen(path, "wb");
    if (!audio_cap) {
        std::print(stderr, "runtime: audio capture open failed '{}'\n", path);
        return;
    }
    std::array<uint8_t, 44> header{};   // 先占位，退出时回填
    std::fwrite(header.data(), 1, header.size(), audio_cap);
    audio_cap_path = path;
    audio_cap_frames = 0;
    audio_cap_frac = 0.0;
    std::print("runtime: audio capture -> {}\n", path);
}

void Engine::audio_capture_step(float dt) {
    if (!audio_cap || !audio) return;
    audio_cap_frac += (double)dt * (double)audio_cap_rate;
    uint32_t frames = (uint32_t)audio_cap_frac;
    if (frames == 0) return;
    audio_cap_frac -= (double)frames;
    if (frames > 4800) frames = 4800;                 // 单帧上限（防 dt 异常）
    audio_cap_buf.assign((size_t)frames * 2, 0.0F);
    if (mq_audio_mix(audio, audio_cap_buf.data(), frames) != MQ_AUDIO_OK) return;
    std::vector<int16_t> pcm((size_t)frames * 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float v = std::clamp(audio_cap_buf[i], -1.0F, 1.0F);
        pcm[i] = (int16_t)std::lround(v * 32767.0F);
    }
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), audio_cap);
    audio_cap_frames += frames;
}

void Engine::audio_capture_finish() {
    if (!audio_cap) return;
    const uint32_t channels = audio_cap_channels ? audio_cap_channels : 2;
    const uint32_t data_bytes = (uint32_t)(audio_cap_frames * channels * 2u);
    const uint32_t byte_rate = audio_cap_rate * channels * 2u;
    std::array<uint8_t, 44> h{};
    auto w16 = [&](size_t off, uint16_t v) { h[off] = (uint8_t)(v & 0xFF); h[off + 1] = (uint8_t)(v >> 8); };
    auto w32 = [&](size_t off, uint32_t v) {
        h[off] = (uint8_t)(v & 0xFF); h[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        h[off + 2] = (uint8_t)((v >> 16) & 0xFF); h[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    };
    std::memcpy(h.data(), "RIFF", 4);
    w32(4, 36u + data_bytes);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);                       // PCM
    w16(22, (uint16_t)channels);
    w32(24, audio_cap_rate);
    w32(28, byte_rate);
    w16(32, (uint16_t)(channels * 2u));
    w16(34, 16);
    std::memcpy(h.data() + 36, "data", 4);
    w32(40, data_bytes);
    std::fseek(audio_cap, 0, SEEK_SET);
    std::fwrite(h.data(), 1, h.size(), audio_cap);
    std::fclose(audio_cap);
    audio_cap = nullptr;
    std::print("runtime: audio capture done '{}' frames={}\n", audio_cap_path,
               (unsigned long long)audio_cap_frames);
}

mq_audio_clip* Engine::audio_clip(const std::string& path) {
    if (!audio || path.empty()) return nullptr;
    auto it = audio_clips.find(path);
    if (it != audio_clips.end()) return it->second;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data || n == 0) {
        std::print(stderr, "runtime: audio clip not found '{}'\n", path);
        return nullptr;
    }
    mq_audio_clip* clip = nullptr;
    const int rc = mq_audio_load_memory(audio, data, n, path.c_str(), &clip);
    mq_vfs_free(data);
    if (rc != MQ_AUDIO_OK || !clip) return nullptr;
    audio_clips[path] = clip;
    return clip;
}

extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;
    auto* eng = new Engine();
    if (!eng->setup(scn_path, nullptr)) { eng->teardown(); delete eng; return 2; }

    /* 控制端点：inspector 用（MAQUE_INSPECT=7777 / tcp:7777 / uds:/tmp/x.sock） */
    if (const char* spec = std::getenv("MAQUE_INSPECT"); spec && *spec && std::strcmp(spec, "off") != 0) {
        const bool rw = std::getenv("MAQUE_INSPECT_RW") != nullptr;
        eng->inspect_read_only = !rw;
        eng->ctl_server = mq_control_server_create(eng->control, spec, eng->inspect_read_only ? 1 : 0);
        if (eng->ctl_server)
            std::print("runtime: control endpoint {} ({})\n", mq_control_server_url(eng->ctl_server),
                       eng->inspect_read_only ? "read-only" : "read-write");
        else
            std::print(stderr, "runtime: control endpoint 创建失败: {}\n", spec);
    }

    if (const char* req = std::getenv("MAQUE_CONTROL")) {
        const bool replayed = eng->prepare_replay();
        if (!replayed && std::getenv("MAQUE_RECORD")) mq_input_record_begin(eng->input);
        /* stdout 接管道时是块缓冲：逐行控制（inspector/e2e 的"写一条读一条"）必须每次响应后
         * flush，否则对端读不到 → 交互式驱动永久阻塞。 */
        auto serve = [&](const char* r) {
            std::print("{}\n", mq_control_dispatch(eng->control, r));
            std::fflush(stdout);
        };
        if (std::strcmp(req, "-") == 0) {
            char line[8192];
            while (std::fgets(line, sizeof line, stdin)) { line[std::strcspn(line, "\n")] = 0; if (*line) serve(line); }
        } else serve(req);
        eng->finalize_run();
        if (replayed) {
            const char* rt = mq_input_log_get_meta(eng->input, "trail");
            if (rt) {
                char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)eng->world_trail());
                if (rt == std::string(tb)) std::print("runtime: replay matches recorded trail ({})\n", tb);
                else std::print("runtime: DESYNC — replay trail {} != recorded {}\n", tb, rt);
            }
        }
        eng->teardown(); delete eng;
        maque::tracy::shutdown();        // Tracy：冲刷数据（若本次运行启动过；普通运行是 no-op）
        return 0;
    }

    std::string replay_trail;
    if (eng->prepare_replay()) {
        if (const char* mt = mq_input_log_get_meta(eng->input, "trail")) replay_trail = mt;
    }
    if (std::getenv("MAQUE_RECORD")) mq_input_record_begin(eng->input);

    eng->main_loop(frames_dir ? frames_dir : "", std::filesystem::path(scn_path).filename().string());
    const bool quit = eng->quit || eng->frames_capped;
    eng->teardown();
    delete eng;
    maque::tracy::shutdown();            // Tracy：冲刷数据（未启动时 no-op）
    return quit ? 0 : 1;
}

/* ---------------- 嵌入宿主接口（P3，docs/05）：主循环归宿主 ---------------- */

struct mq_host_scene {
    Engine* eng = nullptr;
    std::string response;
};

extern "C" mq_host_scene* mq_host_scene_create(const char* scn_path, const mq_host_desc* desc) {
    if (!scn_path) return nullptr;
    auto* hs = new mq_host_scene();
    hs->eng = new Engine();
    const char* renderer = desc ? desc->renderer : nullptr;
    if (!hs->eng->setup(scn_path, renderer)) {
        hs->eng->teardown();
        delete hs->eng;
        delete hs;
        return nullptr;
    }
    return hs;
}

extern "C" void mq_host_scene_destroy(mq_host_scene* s) {
    if (!s) return;
    if (s->eng) { s->eng->teardown(); delete s->eng; }
    delete s;
    /* 嵌入宿主：如果本进程启动过 Tracy（F3/MAQUE_TRACY），退出时冲刷。多实例同时销毁时
     * 第一个 destroy 就会 shutdown —— 嵌入式 profile 默认按单实例使用（docs/29）。 */
    maque::tracy::shutdown();
}

extern "C" int mq_host_step(mq_host_scene* s) {
    if (!s || !s->eng) return 1;
    s->eng->step_frame(false);
    return 0;
}

extern "C" int mq_host_tick(mq_host_scene* s, uint8_t* out, size_t out_size) {
    if (!s || !s->eng) return 1;
    s->eng->step_frame(true);
    Engine& e = *s->eng;
    // 渲染该帧到宿主缓冲（step_frame 内部已渲染；这里补一次输出到调用方缓冲）
    std::vector<mq_renderer_item> draws;
    std::vector<mq_renderer_mesh> meshes;
    if (e.scene) { e.scene->extract(e, draws); e.spine_extract(meshes); }
    /* 宿主缓冲尺寸就是当前实际渲染目标；无窗口时默认等于设计尺寸 × render_scale。 */
    const int SHpx = e.TH;
    if (!out || out_size < (size_t)e.TW * (size_t)SHpx * 4) return 2;
    MQ_TRACY_ZONE_N(render, "frame");   // Tracy：宿主侧补渲染（mq_host_tick 的额外一次成帧）
    float bg[4] = { e.bg[0], e.bg[1], e.bg[2], e.bg[3] };
    const int rc = (!meshes.empty() && e.rapi->render_mesh_views)
        ? e.rapi->render_mesh_views(e.rself, e.TW, SHpx, e.views.data(), (uint32_t)e.views.size(),
                                    draws.data(), (uint32_t)draws.size(),
                                    meshes.data(), (uint32_t)meshes.size(), bg, out, (uint32_t)out_size)
        : e.rapi->render_views(e.rself, e.TW, SHpx, e.views.data(), (uint32_t)e.views.size(),
                               draws.data(), (uint32_t)draws.size(), bg, out, (uint32_t)out_size);
    return rc == 0 ? 0 : 3;
}

extern "C" void mq_host_size(mq_host_scene* s, int* w, int* h) {
    if (!s || !s->eng) return;
    if (w) *w = s->eng->TW;
    if (h) *h = s->eng->TH;
}

extern "C" void mq_host_push_pointer(mq_host_scene* s, int type, int button, float x, float y) {
    if (!s || !s->eng || !s->eng->input) return;
    mq_input_event ev{};
    ev.type = (uint32_t)type;
    ev.button = (uint32_t)button;
    ev.x = x; ev.y = y;
    ev.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
    ev.pressure = type == MQ_INPUT_POINTER_DOWN ? 1.0F : 0.0F;
    mq_input_inject(s->eng->input, &ev);
}

extern "C" void mq_host_push_key(mq_host_scene* s, int key, int down, const char* text) {
    if (!s || !s->eng || !s->eng->input) return;
    mq_input_event ev{};
    ev.type = text ? MQ_INPUT_TEXT : (down ? MQ_INPUT_KEY_DOWN : MQ_INPUT_KEY_UP);
    ev.key = (uint32_t)key;
    if (text) { std::strncpy(ev.text, text, sizeof(ev.text) - 1); }
    mq_input_inject(s->eng->input, &ev);
}

extern "C" uint64_t mq_host_frame(mq_host_scene* s) { return s && s->eng ? s->eng->frame : 0; }
extern "C" uint64_t mq_host_checksum(mq_host_scene* s) { return s && s->eng ? s->eng->world_checksum() : 0; }
extern "C" int mq_host_should_quit(mq_host_scene* s) { return s && s->eng && s->eng->quit ? 1 : 0; }

extern "C" const char* mq_host_control(mq_host_scene* s, const char* request) {
    if (!s || !s->eng || !s->eng->control) return nullptr;
    s->response = mq_control_dispatch(s->eng->control, request ? request : "{}");
    return s->response.c_str();
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
