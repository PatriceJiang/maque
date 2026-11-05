/* 脚本 · 包/模块 VM（docs/34 P0）：每 Engine 一个 JS VM，每挂载点一个实例
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- docs/34 P0：包 / 模块 / 实例（Engine 级单 JS VM）---- */

std::string normalize_module_rel(const std::string& in) {
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
    ComponentSlot& slot = component_slot(*e, d);
    if (!slot.bytes.empty()) destroy_blob(eng, d->type_id, slot.bytes);
    construct_blob(eng, d, slot.bytes);   // 含动态成员（string/vector）的合法构造 + schema 默认值
}

}  // namespace maque::rt
