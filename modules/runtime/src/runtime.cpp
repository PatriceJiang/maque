/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */

#include "runtime_engine.h"

#include <filesystem>

namespace fs = std::filesystem;   /* 全文件唯一别名（放外层：跨 TU 不会与 maque::rt::fs 撞名） */

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
/* ---- 贴图路径 sampler 后缀（plans/20：glTF sampler 的 wrap/filter 落到 VFS 路径上）----
 * 路径形如 `textures/x.png#wrap=clamp,mirror&filter=nearest`：
 *   · '#' 之前是真正的 VFS 路径（读文件/解码用它）；
 *   · wrap=repeat|clamp|mirror → create_texture2 的 wrap_u/wrap_v（0/1/2）；可写一个（u=v）或 "u,v"；
 *   · filter=nearest|linear → create_texture2 的 filter（0/1）；
 *   · 无 '#'：文件贴图默认 repeat+linear（glTF sampler 缺省）；builtin 贴图由调用方默认 clamp。
 * 缓存 key 用**完整字符串**：同一张图配不同 sampler 是两个独立的 GPU 纹理对象。 */
struct TexPathParams {
    std::string vfs;                 /* 去掉 sampler 后缀的 VFS 路径 */
    int wrap_u = 0, wrap_v = 0;      /* 0=repeat 1=clamp 2=mirror（文件贴图默认 repeat） */
    int filter = 1;                  /* 0=nearest 1=linear */
    bool wrap_set = false, filter_set = false;
};

static int wrap_mode_code(const std::string& s) {
    if (s == "repeat") return 0;
    if (s == "clamp") return 1;
    if (s == "mirror") return 2;
    return -1;                       /* 未知值 → 忽略该项（用默认） */
}

static TexPathParams parse_tex_path(const std::string& ref) {
    TexPathParams p;
    p.vfs = ref;
    const size_t hash = ref.find('#');
    if (hash == std::string::npos) return p;
    p.vfs.assign(ref, 0, hash);
    const std::string query = ref.substr(hash + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string key = kv.substr(0, eq);
            const std::string val = kv.substr(eq + 1);
            if (key == "wrap") {
                const size_t comma = val.find(',');
                const int u = wrap_mode_code(comma == std::string::npos ? val : val.substr(0, comma));
                const int v = wrap_mode_code(comma == std::string::npos ? val : val.substr(comma + 1));
                if (u >= 0) { p.wrap_u = u; p.wrap_v = (v >= 0 ? v : u); p.wrap_set = true; }
            } else if (key == "filter") {
                if (val == "nearest") { p.filter = 0; p.filter_set = true; }
                else if (val == "linear") { p.filter = 1; p.filter_set = true; }
            }
        }
        pos = amp + 1;
    }
    return p;
}

std::string Engine::scoped_texture_ref(int entity, const std::string& ref) const {
    static constexpr const char* kRt = "rendertexture://";
    if (ref.rfind(kRt, 0) != 0) return ref;
    const std::string tail = ref.substr(std::strlen(kRt));
    if (tail.find('/') != std::string::npos) return ref;       /* 已是规范 URI */
    auto it = entity_instance.find(entity);
    const int owner = it == entity_instance.end() ? 0 : it->second;
    return std::string(kRt) + std::to_string(owner) + "/" + tail;
}

int Engine::load_texture(const std::string& path) {
    auto it = tex_cache.find(path);
    if (it != tex_cache.end()) return it->second;
    /* 旧 create_texture 没有 sampler 参数：这里只把 '#' 后缀剥掉再读文件（sampler 由 2D 全局采样器决定）；
     * PBR 贴图走 load_texture_srgb/mips → create_texture2 才真正生效。 */
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int id = rapi->create_texture(rself, rgba, (int)w, (int)h);
    mq_image_free(rgba);
    tex_cache[path] = id;
    return id;
}

/* plans/19：CPU box-filter mip 链（RGBA8，各层连续存放）。
 * 无 mip 时高频贴图（布纹/车漆细节）在远处会强烈闪烁/摩尔纹——PBR 正确性的重要一环。
 * srgb=true（M4g albedo/emissive）：RGB 必须**先解码到线性再平均、再编码回 sRGB**——
 * 直接在 sRGB 编码字节上平均会让明暗混合显著偏暗（黑白棋盘 2×2 的线性均值 0.5 →
 * 编码域平均只剩 ~0.214，线性亮度 -57%）。alpha 始终是线性数据，直接平均。
 * 线性数据贴图（MR/AO/normal）走 srgb=false，与旧实现逐字节一致。 */
static void srgb_mip_tables(float* decode, float* encode_thr) {
    for (int i = 0; i < 256; ++i) {
        const double c = (double)i / 255.0;
        decode[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    /* encode_thr[b] = 线性域中「编码值恰为 (b+0.5)/255」的门槛：值 >= 门槛即进位到 b+1，
     * 二分即得 round(255*srgb(linear))，避免逐像素 pow。 */
    for (int b = 0; b < 255; ++b) {
        const double s = ((double)b + 0.5) / 255.0;
        encode_thr[b] = (float)(s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4));
    }
}

static uint8_t srgb_encode_from_linear(float linear, const float* encode_thr) {
    if (linear <= 0.0F) return 0;
    if (linear >= 1.0F) return 255;
    int lo = 0, hi = 255;                       /* 找第一个 > linear 的门槛 → 输出 lo */
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (linear < encode_thr[mid]) hi = mid; else lo = mid + 1;
    }
    return (uint8_t)lo;
}

static std::vector<uint8_t> build_mip_chain(const uint8_t* src, int w0, int h0, int& levels,
                                            bool srgb) {
    levels = 0;
    std::vector<uint8_t> out;
    if (!src || w0 <= 0 || h0 <= 0) return out;
    out.assign(src, src + (size_t)w0 * (size_t)h0 * 4u);
    levels = 1;
    float decode[256];
    float encode_thr[255];
    if (srgb) srgb_mip_tables(decode, encode_thr);
    int w = w0, h = h0;
    size_t prev_off = 0;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const size_t dst_off = out.size();
        out.resize(dst_off + (size_t)nw * (size_t)nh * 4u);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                int acc[4] = { 0, 0, 0, 0 };
                float lin[3] = { 0.0F, 0.0F, 0.0F };
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(w - 1, x * 2 + dx);
                        const int sy = std::min(h - 1, y * 2 + dy);
                        const uint8_t* p = &out[prev_off + ((size_t)sy * (size_t)w + (size_t)sx) * 4u];
                        if (srgb) {
                            for (int c = 0; c < 3; ++c) lin[c] += decode[p[c]];
                            acc[3] += p[3];
                        } else {
                            for (int c = 0; c < 4; ++c) acc[c] += p[c];
                        }
                    }
                }
                uint8_t* q = &out[dst_off + ((size_t)y * (size_t)nw + (size_t)x) * 4u];
                if (srgb) {
                    for (int c = 0; c < 3; ++c) q[c] = srgb_encode_from_linear(lin[c] * 0.25F, encode_thr);
                    q[3] = (uint8_t)((acc[3] + 2) / 4);
                } else {
                    for (int c = 0; c < 4; ++c) q[c] = (uint8_t)((acc[c] + 2) / 4);
                }
            }
        }
        prev_off = dst_off;
        w = nw;
        h = nh;
        ++levels;
    }
    return out;
}

/* M4g：PBR albedo 贴图必须按 sRGB 采样（文件存 sRGB、shader 需要线性）——
 * create_texture2 的 RGBA8_SRGB；MR/AO/normal 是线性数据，仍走 load_texture_mips（RGBA8Unorm）。 */

/* HDR env 解码 helper 的提前声明（定义在 load_texture_ref 之前）。 */
static bool parse_radiance_hdr(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba);
/* OpenEXR scanline（NONE/RLE/ZIP/ZIPS；half/float 通道）→ 线性 RGBA float。 */
static bool parse_exr_env(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba);
static void make_studio_env_base(std::vector<float>& out, int& w, int& h);
/* PMREM：GGX 粗糙度预过滤（specular mip 链）+ 余弦卷积（diffuse irradiance）。 */
static void build_pmrem(const std::vector<float>& base, int w, int h,
                        std::vector<uint8_t>& spec_chain, int& levels,
                        std::vector<uint8_t>& irr, int& irr_w, int& irr_h);
static void build_pmrem_cube(const std::vector<std::vector<float>>& base, int w,
                             std::vector<uint8_t>& spec_chain, int& levels,
                             std::vector<uint8_t>& irr, int& irr_w);

/* envmap 加载（PMREM）：.hdr（RGBE）/ EXR / PNG/JPEG / builtin studio → RGBA16F 预过滤 mip 链 + irradiance。
 * 全部在装配期 CPU 预计算（确定性；与 GPU/后端无关），失败时 spec/irr 均为 -1 → shader 回退 analytic。 */
void Engine::load_env_textures(const std::string& path, int32_t& spec_id, int32_t& irr_id) {
    spec_id = irr_id = -1;
    auto cached = env_tex_cache.find(path);
    if (cached != env_tex_cache.end()) { spec_id = cached->second.first; irr_id = cached->second.second; return; }
    const TexPathParams tp = parse_tex_path(path);
    int w = 0, h = 0;
    std::vector<float> base;
    if (tp.vfs == "builtin:/textures/studio_env") {
        make_studio_env_base(base, w, h);
    } else if (!load_env_image_linear(path, w, h, base)) {
        env_tex_cache[path] = { -1, -1 };
        return;
    }
    if (w <= 0 || h <= 0 || base.empty()) {
        env_tex_cache[path] = { -1, -1 };
        return;
    }
    std::vector<uint8_t> spec_chain, irr;
    int levels = 1, irr_w = 0, irr_h = 0;
    const auto t0 = std::chrono::steady_clock::now();
    build_pmrem(base, w, h, spec_chain, levels, irr, irr_w, irr_h);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rapi && rself && rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = w; td.height = h;
        td.pixel_format = 2;                       /* 2 = RGBA16F（线性 HDR / 预过滤结果） */
        td.kind = 0; td.mip_levels = levels;
        td.filter = 1;                             /* linear */
        td.wrap_u = 0; td.wrap_v = 1;              /* 经度 repeat、纬度 clamp */
        td.data = spec_chain.data(); td.data_size = (uint64_t)spec_chain.size();
        if (rapi->create_texture2(rself, &td, &spec_id) != 0) spec_id = -1;
        mq_renderer_texture_desc id_ = {};
        id_.width = irr_w; id_.height = irr_h;
        id_.pixel_format = 2; id_.kind = 0; id_.mip_levels = 1;
        id_.filter = 1; id_.wrap_u = 0; id_.wrap_v = 1;
        id_.data = irr.data(); id_.data_size = (uint64_t)irr.size();
        if (rapi->create_texture2(rself, &id_, &irr_id) != 0) irr_id = -1;
    }
    if (spec_id < 0) std::print(stderr, "runtime: env_texture '{}' PMREM 上传失败\n", path);
    else
        std::print("runtime: env PMREM '{}' {}x{} → {} mips (GGX) + irradiance {}x{} ({:.1f} ms)\n",
                   path, w, h, levels, irr_w, irr_h, ms);
    env_tex_cache[path] = { spec_id, irr_id };
}

/* env 图片解码（.hdr RGBE / EXR / PNG/JPEG）→ 线性 RGBA float；供 equirect 与 6 面 cube 复用。 */
bool Engine::load_env_image_linear(const std::string& path, int& w, int& h, std::vector<float>& base) {
    w = 0; h = 0; base.clear();
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print(stderr, "runtime: env_texture not found '{}'\n", path);
        return false;
    }
    const bool is_hdr = (n >= 4 && data && ((const uint8_t*)data)[0] == '#' &&
                         ((const uint8_t*)data)[1] == '?') ||
                        (tp.vfs.size() >= 4 &&
                         (tp.vfs.compare(tp.vfs.size() - 4, 4, ".hdr") == 0 ||
                          tp.vfs.compare(tp.vfs.size() - 4, 4, ".HDR") == 0));
    const bool is_exr = n >= 4 && data && ((const uint8_t*)data)[0] == 0x76 &&
                        ((const uint8_t*)data)[1] == 0x2f && ((const uint8_t*)data)[2] == 0x31 &&
                        ((const uint8_t*)data)[3] == 0x01;
    if (is_hdr) {
        const bool ok = parse_radiance_hdr((const uint8_t*)data, n, w, h, base);
        mq_vfs_free(data);
        if (!ok) {
            std::print(stderr, "runtime: env_texture '{}' HDR(RGBE) 解析失败，回退 analytic\n", path);
            return false;
        }
        if (const char* info = std::getenv("MAQUE_RENDER3D_INFO"); info && info[0] == '1') {
            const size_t c = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            std::print(stderr, "runtime: env HDR {}x{} p0=({:.4f},{:.4f},{:.4f}) "
                               "pc=({:.4f},{:.4f},{:.4f})\n",
                       w, h, base[0], base[1], base[2], base[c], base[c + 1], base[c + 2]);
        }
        return true;
    }
    if (is_exr) {
        const bool ok = parse_exr_env((const uint8_t*)data, n, w, h, base);
        mq_vfs_free(data);
        if (!ok) {
            std::print(stderr, "runtime: env_texture '{}' EXR 解析失败（仅支持 scanline NONE/RLE/ZIP/ZIPS，"
                               "half/float 通道），回退 analytic\n", path);
            return false;
        }
        if (const char* info = std::getenv("MAQUE_RENDER3D_INFO"); info && info[0] == '1') {
            float mx = 0.0F;
            for (float v : base) mx = std::max(mx, v);
            const size_t c = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            std::print(stderr, "runtime: env EXR {}x{} p0=({:.4f},{:.4f},{:.4f}) "
                               "pc=({:.4f},{:.4f},{:.4f}) max={:.3f}\n",
                       w, h, base[0], base[1], base[2], base[c], base[c + 1], base[c + 2], mx);
        }
        return true;
    }
    uint8_t* rgba = nullptr; uint32_t iw = 0, ih = 0;
    char err[128] = { 0 };
    const int rc = mq_image_decode(data, n, &rgba, &iw, &ih, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) {
        std::print(stderr, "runtime: env_texture '{}' 解码失败: {}\n", path, err);
        return false;
    }
    /* PNG/JPEG 存的是 sRGB 编码值：先解码到线性域再做 PMREM（否则高光/平均亮度错误）。 */
    w = (int)iw; h = (int)ih;
    base.assign((size_t)w * h * 4, 1.0f);
    for (size_t i = 0, np = (size_t)w * h; i < np; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float sv = (float)rgba[i * 4 + (size_t)c] / 255.0f;
            base[i * 4 + (size_t)c] = (sv <= 0.04045f) ? sv / 12.92f
                                                       : std::pow((sv + 0.055f) / 1.055f, 2.4f);
        }
        base[i * 4 + 3] = 1.0f;
    }
    mq_image_free(rgba);
    return true;
}

/* 立方体 env 加载（第 26 轮）：基名 + 6 面后缀 → GGX 预过滤 cube（mip 链）+ irradiance cube。
 * 失败时 spec/irr 均为 -1 → shader 回退 equirect/analytic（不崩）。 */
void Engine::load_env_cubemap(const std::string& prefix, int32_t& spec_id, int32_t& irr_id) {
    spec_id = irr_id = -1;
    auto cached = env_cube_cache.find(prefix);
    if (cached != env_cube_cache.end()) { spec_id = cached->second.first; irr_id = cached->second.second; return; }
    static const char* kSuffix[6] = { "_px", "_nx", "_py", "_ny", "_pz", "_nz" };
    static const char* kExts[5] = { ".hdr", ".exr", ".png", ".jpg", ".jpeg" };
    std::vector<std::vector<float>> faces(6);
    int w = 0;
    bool ok = false;
    for (const char* ext : kExts) {
        int fw = 0, fh = 0;
        bool all = true;
        for (int f = 0; f < 6; ++f) {
            const std::string p = prefix + kSuffix[f] + std::string(ext);
            int iw = 0, ih = 0;
            std::vector<float> img;
            if (!load_env_image_linear(p, iw, ih, img)) { all = false; break; }
            if (f == 0) { fw = iw; fh = ih; }
            else if (iw != fw || ih != fh) { all = false; break; }
            faces[(size_t)f] = std::move(img);
        }
        if (all && fw > 0 && fw == fh) { w = fw; ok = true; break; }
    }
    if (!ok) {
        std::print(stderr, "runtime: env_cubemap '{}' 6 面加载失败（<基名>_px/_nx/_py/_ny/_pz/_nz + "
                           ".hdr/.exr/.png/.jpg），回退 equirect/analytic\n", prefix);
        env_cube_cache[prefix] = { -1, -1 };
        return;
    }
    std::vector<uint8_t> spec_chain, irr;
    int levels = 1, irr_w = 0;
    const auto t0 = std::chrono::steady_clock::now();
    build_pmrem_cube(faces, w, spec_chain, levels, irr, irr_w);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rapi && rself && rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = w; td.height = w;
        td.pixel_format = 2;                        /* RGBA16F */
        td.kind = 1;                                /* cube（6 面 × mip 链） */
        td.mip_levels = levels;
        td.filter = 1; td.wrap_u = 1; td.wrap_v = 1;
        td.data = spec_chain.data(); td.data_size = (uint64_t)spec_chain.size();
        if (rapi->create_texture2(rself, &td, &spec_id) != 0) spec_id = -1;
        mq_renderer_texture_desc id_{};
        id_.width = irr_w; id_.height = irr_w;
        id_.pixel_format = 2; id_.kind = 1; id_.mip_levels = 1;
        id_.filter = 1; id_.wrap_u = 1; id_.wrap_v = 1;
        id_.data = irr.data(); id_.data_size = (uint64_t)irr.size();
        if (rapi->create_texture2(rself, &id_, &irr_id) != 0) irr_id = -1;
    }
    if (spec_id < 0) std::print(stderr, "runtime: env_cubemap '{}' PMREM 上传失败\n", prefix);
    else
        std::print("runtime: env CUBEMAP '{}' {}x{} ×6 → {} mips (GGX) + irradiance {}x{}×6 ({:.1f} ms)\n",
                   prefix, w, w, levels, irr_w, irr_w, ms);
    env_cube_cache[prefix] = { spec_id, irr_id };
}

int Engine::load_texture_srgb(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    /* builtin: 是程序化纹理（checker 等），沿用既有 UNorm 路径，不做 sRGB 解码 */
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "srgb:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print("runtime: srgb texture not found '{}'\n", path);
        return -1;
    }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: srgb decode failed '{}': {}\n", path, err); return -1; }
    int32_t id = -1;
    if (rapi && rself && rapi->create_texture2 && w > 0 && h > 0) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, /*srgb=*/true);
        mq_renderer_texture_desc td{};
        td.width = (int32_t)w; td.height = (int32_t)h;
        td.pixel_format = 1;                  /* 1 = RGBA8_SRGB */
        td.kind = 0; td.mip_levels = levels;
        td.filter = tp.filter;                /* glTF sampler 后缀（缺省 linear） */
        td.wrap_u = tp.wrap_u; td.wrap_v = tp.wrap_v;   /* 缺省 repeat */
        td.data = mips.data(); td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    mq_image_free(rgba);
    if (id < 0) std::print("runtime: srgb texture upload failed '{}'\n", path);
    tex_cache[key] = id;
    return id;
}

/* plans/19：线性数据贴图（MR/AO/normal/clearcoat normal）+ CPU mip 链；采样 RGBA8Unorm。 */
int Engine::load_texture_mips(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "mip:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print("runtime: texture not found '{}'\n", path);
        return -1;
    }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int32_t id = -1;
    if (rapi && rself && rapi->create_texture2 && w > 0 && h > 0) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, /*srgb=*/false);
        mq_renderer_texture_desc td{};
        td.width = (int32_t)w; td.height = (int32_t)h;
        td.pixel_format = 0;                  /* 0 = RGBA8（线性数据） */
        td.kind = 0; td.mip_levels = levels;
        td.filter = tp.filter;                /* glTF sampler 后缀（缺省 linear） */
        td.wrap_u = tp.wrap_u; td.wrap_v = tp.wrap_v;   /* 缺省 repeat */
        td.data = mips.data(); td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    mq_image_free(rgba);
    if (id < 0) id = load_texture(path);      /* create_texture2 不可用时退回无 mip 路径（内部会剥后缀） */
    tex_cache[key] = id;
    return id;
}

/* builtin:/ → 引擎内建（此处程序化生成）；否则 assets（VFS）。
 * ref 可带 sampler 后缀（`...` 见 parse_tex_path）；缓存 key 用完整字符串。 */

/* 内建 equirect studio 环境（512x256，**线性浮点**）：天空-地面渐变 + 4 个软光箱
 *（方向与 analytic studio IBL 一致）；作为 PMREM 的源 radiance，与 .hdr 走同一条预过滤路径。 */
static void make_studio_env_base(std::vector<float>& out, int& w, int& h) {
    w = 512; h = 256;
    constexpr float kPi = 3.14159265f;
    struct Box { float dx, dy, dz; float r, g, b; float amp; float sharp; };
    static const Box boxes[4] = {
        { -0.55f, 0.78f, 0.55f, 1.00f, 0.97f, 0.92f, 4.2f, 18.0f },  /* key softbox */
        { -0.30f, 0.84f, 0.62f, 1.00f, 0.98f, 0.95f, 2.0f, 22.0f },  /* fill */
        {  0.80f, 0.30f, 0.45f, 0.85f, 0.92f, 1.00f, 1.3f, 10.0f },  /* cool rim */
        {  0.05f, 0.25f, -0.95f,0.92f, 0.95f, 1.00f, 0.8f, 12.0f }, /* back */
    };
    auto norm = [](float x, float y, float z, float& ox, float& oy, float& oz) {
        const float l = std::sqrt(x * x + y * y + z * z) + 1e-8f;
        ox = x / l; oy = y / l; oz = z / l;
    };
    out.assign((size_t)w * h * 4, 1.0f);
    for (int y = 0; y < h; ++y) {
        const float lat = kPi * (0.5f - (float(y) + 0.5f) / (float)h);
        const float cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < w; ++x) {
            const float lon = kPi * 2.0f * ((float(x) + 0.5f) / (float)w) - kPi;
            const float dx = cl * std::cos(lon), dy = sl, dz = cl * std::sin(lon);
            /* 半球渐变：地面暖灰、天空冷灰、地平线略亮 */
            const float t = std::clamp(dy * 0.5f + 0.5f, 0.0f, 1.0f);
            float r = 0.16f + (0.48f - 0.16f) * t;
            float g = 0.15f + (0.52f - 0.15f) * t;
            float b = 0.14f + (0.60f - 0.14f) * t;
            const float horizon = std::exp(-std::fabs(dy) * 8.0f);
            r += horizon * 0.10f; g += horizon * 0.10f; b += horizon * 0.11f;
            /* 软光箱（方向余弦的锐利幂） */
            for (const Box& bx : boxes) {
                float nx, ny, nz; norm(bx.dx, bx.dy, bx.dz, nx, ny, nz);
                const float c = std::max(0.0f, dx * nx + dy * ny + dz * nz);
                const float spot = std::pow(c, bx.sharp) * bx.amp;
                r += bx.r * spot; g += bx.g * spot; b += bx.b * spot;
            }
            float* px = &out[((size_t)y * w + x) * 4];
            px[0] = r; px[1] = g; px[2] = b; px[3] = 1.0f;
        }
    }
}


/* ---- envmap：Radiance .hdr（RGBE）解码 + 线性 mip（PMREM 式预过滤近似） ---- */
static uint16_t f32_to_f16(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = (int32_t)((x >> 23) & 0xffu) - 127;
    uint32_t mantissa = x & 0x7fffffu;
    if (exponent == 128) return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent > 15) return (uint16_t)(sign | 0x7c00u);
    if (exponent < -14) {
        if (exponent < -24) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(-exponent - 14);
        return (uint16_t)(sign | (mantissa >> (13 + shift)));
    }
    return (uint16_t)(sign | ((uint32_t)(exponent + 15) << 10) | (mantissa >> 13));
}

static void append_half_rgba(std::vector<uint8_t>& out, float r, float g, float b) {
    const uint16_t h[4] = { f32_to_f16(r), f32_to_f16(g), f32_to_f16(b), f32_to_f16(1.0f) };
    const uint8_t* p = (const uint8_t*)h;
    out.insert(out.end(), p, p + sizeof h);
}

/* 解析 Radiance RGBE（new RLE 扫描线；老格式按平铺 RGBE 兼容）。
 * 头部按行读，**容忍 CRLF**：Radiance 规范只要求 \n 结尾，但不少工具（以及 core.autocrlf=true
 * 的检出）会把头部写成 \r\n —— 不剥掉 \r 的话"空行结束头部"判定失效，会把扫描线数据当分辨率行
 * 去 sscanf → 整张 env 静默回退 analytic（实测 studio_cube 的 _py/_ny 两面就是这样废掉的）。
 * 注意这只救头部：数据区若被换行转换插过字节（含 0x0A 的扫描线）无从恢复，
 * 二进制资产必须在 .gitattributes 里标 -text。 */
static bool parse_radiance_hdr(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba) {
    size_t pos = 0;
    auto get_line = [&](std::string& line) {
        line.clear();
        while (pos < n && d[pos] != '\n') line.push_back((char)d[pos++]);
        if (pos < n) ++pos;
        if (!line.empty() && line.back() == '\r') line.pop_back();
    };
    std::string line;
    get_line(line);
    if (line.rfind("#?", 0) != 0) return false;
    bool rgbe = false;
    for (;;) {
        get_line(line);
        if (line.empty() || pos >= n) break;
        if (line.rfind("FORMAT=", 0) == 0) {
            if (line.find("32-bit_rle_rgbe") != std::string::npos) rgbe = true;
            else return false;
        }
    }
    if (!rgbe) return false;
    get_line(line);                       /* 分辨率行：-Y <h> +X <w>（也支持 +Y 等简单变体） */
    int h = 0, w = 0;
    char s1 = 0, s2 = 0;
    if (std::sscanf(line.c_str(), "%c%c %d %c%c %d", &s1, &s2, &h, &s1, &s2, &w) != 6) return false;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 64u * 1024u * 1024u) return false;
    W = w; H = h;
    rgba.assign((size_t)w * h * 4, 0.0f);
    std::vector<uint8_t> scan((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        if (pos + 4 > n) return false;
        const uint8_t b0 = d[pos], b1 = d[pos + 1], b2 = d[pos + 2], b3 = d[pos + 3];
        bool new_rle = (b0 == 2 && b1 == 2 && (((int)b2 << 8) | b3) == w);
        if (new_rle) {
            pos += 4;
            for (int ch = 0; ch < 4; ++ch) {
                int x = 0;
                while (x < w) {
                    if (pos >= n) return false;
                    const int cnt = d[pos++];
                    if (cnt > 128) {
                        const int run = cnt - 128;
                        if (pos >= n) return false;
                        const uint8_t v = d[pos++];
                        for (int i = 0; i < run && x < w; ++i) scan[(size_t)(x++) * 4 + ch] = v;
                    } else {
                        for (int i = 0; i < cnt && x < w; ++i) {
                            if (pos >= n) return false;
                            scan[(size_t)(x++) * 4 + ch] = d[pos++];
                        }
                    }
                }
            }
        } else {
            /* 老格式/平铺 RGBE：直接读 w*4 字节 */
            if (pos + (size_t)w * 4 > n) return false;
            std::memcpy(scan.data(), d + pos, (size_t)w * 4);
            pos += (size_t)w * 4;
        }
        for (int x = 0; x < w; ++x) {
            const uint8_t* p4 = &scan[(size_t)x * 4];
            float* out = &rgba[((size_t)y * w + x) * 4];
            if (p4[3] == 0) { out[0] = out[1] = out[2] = 0.0f; }
            else {
                const float f = std::ldexp(1.0f, (int)p4[3] - 136);
                out[0] = p4[0] * f; out[1] = p4[1] * f; out[2] = p4[2] * f;
            }
            out[3] = 1.0f;
        }
    }
    return true;
}

/* ---- OpenEXR scanline 解码（envmap 用）----
 * 支持：单 part scanline、compression NONE/RLE/ZIP(16 行)/ZIPS(1 行)、通道类型 HALF/FLOAT/UINT、
 * 通道名 R/G/B(/A) 或 Y（灰度）。PIZ/DWAA/B44/深数据/平铺图不支持（返回 false → 调用方回退 analytic）。
 * 输出线性 RGBA float（EXR 本身是线性域，无需 sRGB 转换）。 */
struct ExrChannel { std::string name; int type = 2; int bytes = 4; };
static float exr_half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;                       /* ±0 */
        else {                                           /* subnormal */
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);         /* inf/NaN */
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f = 0.0F;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
/* OpenEXR 的 ZIP/RLE 压缩在 deflate/RLE 之前做了「predictor + 字节交错」两步变换（ImfZip/ImfRle），
 * 解压后必须逆变换：先按字节累加还原（b[i] = b[i-1] + b[i] - 128），再把数据从中间劈成两半交错。
 * 少这一步会得到量级离谱的浮点垃圾（EXR 刚接入时踩过）。 */
static void exr_unpredict_reorder(std::vector<uint8_t>& buf) {
    const size_t n = buf.size();
    if (n < 2) return;
    for (size_t i = 1; i < n; ++i)
        buf[i] = (uint8_t)((int)buf[i - 1] + (int)buf[i] - 128);
    std::vector<uint8_t> out(n);
    const size_t half = (n + 1) / 2;
    size_t p1 = 0, p2 = half;
    for (size_t i = 0; i < n; i += 2) {
        out[i] = buf[p1++];
        if (i + 1 < n) out[i + 1] = buf[p2++];
    }
    buf.swap(out);
}
static bool exr_rle_decode(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
    out.clear(); out.reserve(expected);
    size_t i = 0;
    while (out.size() < expected && i < n) {
        const int cnt = (int)(int8_t)src[i++];
        if (cnt < 0) {
            const size_t lit = (size_t)(-cnt);
            if (i + lit > n) return false;
            out.insert(out.end(), src + i, src + i + lit);
            i += lit;
        } else {
            if (i >= n) return false;
            out.insert(out.end(), (size_t)cnt + 1, src[i++]);
        }
    }
    return out.size() == expected;
}
static bool exr_unzip(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
#ifdef MQ_RUNTIME_HAVE_ZLIB
    out.resize(expected);
    uLongf dst = (uLongf)expected;
    if (uncompress(out.data(), &dst, src, (uLong)n) != Z_OK) return false;
    out.resize((size_t)dst);
    return dst == (uLongf)expected;
#else
    (void)src; (void)n; (void)expected; (void)out;
    return false;
#endif
}
static bool parse_exr_env(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba) {
    if (!d || n < 8) return false;
    if (d[0] != 0x76 || d[1] != 0x2f || d[2] != 0x31 || d[3] != 0x01) return false;
    uint32_t version = 0;
    std::memcpy(&version, d + 4, 4);
    if ((version & 0xFFu) != 2u) return false;            /* version 2 */
    if (version & 0xFFFFFE00u) return false;              /* tiled/long-names/deep/multipart 不支持 */
    size_t pos = 8;
    std::vector<ExrChannel> channels;
    int compression = -1;
    int line_order = 0;
    int xmin = 0, ymin = 0, xmax = -1, ymax = -1;
    auto need = [&](size_t k) { return pos + k <= n; };
    while (pos < n) {
        if (d[pos] == 0) { ++pos; break; }                /* header 结束 */
        const size_t name_beg = pos;
        while (pos < n && d[pos] != 0) ++pos;
        if (pos >= n) return false;
        std::string name((const char*)d + name_beg, pos - name_beg);
        ++pos;
        const size_t type_beg = pos;
        while (pos < n && d[pos] != 0) ++pos;
        if (pos >= n) return false;
        std::string type((const char*)d + type_beg, pos - type_beg);
        ++pos;
        if (!need(4)) return false;
        uint32_t size = 0;
        std::memcpy(&size, d + pos, 4); pos += 4;
        if (!need(size)) return false;
        const uint8_t* v = d + pos;
        if (name == "channels" && type == "chlist") {
            size_t q = 0;
            while (q < size && v[q] != 0) {
                const size_t nb = q;
                while (q < size && v[q] != 0) ++q;
                if (q >= size) return false;
                ExrChannel ch;
                ch.name.assign((const char*)v + nb, q - nb);
                ++q;
                if (q + 16 > size) return false;           /* type4 + pLinear/reserved4 + x/ySampling8 */
                int ptype = 0;
                std::memcpy(&ptype, v + q, 4); q += 4;
                q += 4;                                    /* pLinear + 3 reserved */
                int32_t xs = 0, ys = 0;
                std::memcpy(&xs, v + q, 4); q += 4;
                std::memcpy(&ys, v + q, 4); q += 4;
                if (xs != 1 || ys != 1) return false;      /* 子采样通道不支持 */
                if (ptype < 0 || ptype > 2) return false;
                ch.type = ptype;
                ch.bytes = (ptype == 1) ? 2 : 4;
                channels.push_back(std::move(ch));
            }
        } else if (name == "compression") {
            if (size < 1) return false;
            compression = v[0];
        } else if (name == "lineOrder") {
            if (size < 1) return false;
            line_order = v[0];
        } else if (name == "dataWindow") {
            if (size < 16) return false;
            std::memcpy(&xmin, v, 4); std::memcpy(&ymin, v + 4, 4);
            std::memcpy(&xmax, v + 8, 4); std::memcpy(&ymax, v + 12, 4);
        }
        pos += size;
    }
    if (channels.empty()) return false;
    if (compression < 0) compression = 0;
    if (line_order != 0) return false;                    /* 只支持 increasing Y（HDRI 常态） */
    const int w = xmax - xmin + 1, h = ymax - ymin + 1;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 64u * 1024u * 1024u) return false;
    int lines_per_block = 1;
    if (compression == 3) lines_per_block = 16;           /* ZIP；ZIPS(2)/RLE(1)/NONE(1) 均为 1 行 */
    else if (compression > 3) return false;
    const int blocks = (h + lines_per_block - 1) / lines_per_block;
    if (!need((size_t)blocks * 8)) return false;
    std::vector<uint64_t> offsets((size_t)blocks);
    std::memcpy(offsets.data(), d + pos, (size_t)blocks * 8);
    size_t bytes_per_pixel = 0;
    for (const auto& ch : channels) bytes_per_pixel += (size_t)ch.bytes;
    int ri = -1, gi = -1, bi = -1, ai = -1, yi = -1;
    for (int i = 0; i < (int)channels.size(); ++i) {
        if (channels[(size_t)i].name == "R") ri = i;
        else if (channels[(size_t)i].name == "G") gi = i;
        else if (channels[(size_t)i].name == "B") bi = i;
        else if (channels[(size_t)i].name == "A") ai = i;
        else if (channels[(size_t)i].name == "Y") yi = i;
    }
    const bool rgb = ri >= 0 && gi >= 0 && bi >= 0;
    if (!rgb && yi < 0) return false;
    auto channel_offset = [&](int c) {
        size_t off = 0;
        for (int i = 0; i < c; ++i) off += (size_t)channels[(size_t)i].bytes;
        return off * (size_t)w;      /* 同一扫描线内按通道整行排列（不是逐像素交错） */
    };
    auto sample = [](const uint8_t* p, int type) -> float {
        if (type == 1) { uint16_t hv = 0; std::memcpy(&hv, p, 2); return exr_half_to_float(hv); }
        if (type == 2) { float f = 0.0F; std::memcpy(&f, p, 4); return f; }
        uint32_t u = 0; std::memcpy(&u, p, 4); return (float)u;
    };
    W = w; H = h;
    rgba.assign((size_t)w * h * 4, 0.0f);
    for (size_t i = 0; i < (size_t)w * h; ++i) rgba[i * 4 + 3] = 1.0f;
    std::vector<uint8_t> raw, dec;
    for (int b = 0; b < blocks; ++b) {
        const uint64_t off = offsets[(size_t)b];
        if (off > (uint64_t)n || (uint64_t)n - off < 8) return false;
        int32_t by = 0, dsize = 0;
        std::memcpy(&by, d + (size_t)off, 4); std::memcpy(&dsize, d + (size_t)off + 4, 4);
        if (dsize < 0 || (uint64_t)dsize > (uint64_t)n - off - 8) return false;
        const int nrows = std::min(lines_per_block, h - (by - ymin));
        if (nrows <= 0 || by < ymin || by > ymax) return false;
        const size_t expected = (size_t)nrows * w * bytes_per_pixel;
        const uint8_t* src = d + off + 8;
        const uint8_t* rows = nullptr;
        if (compression == 0) {
            if ((size_t)dsize != expected) return false;
            rows = src;
        } else if (compression == 1) {
            if (!exr_rle_decode(src, (size_t)dsize, expected, dec)) return false;
            exr_unpredict_reorder(dec);
            rows = dec.data();
        } else {
            if (!exr_unzip(src, (size_t)dsize, expected, dec)) return false;
            exr_unpredict_reorder(dec);
            rows = dec.data();
        }
        for (int r = 0; r < nrows; ++r) {
            const uint8_t* row = rows + (size_t)r * w * bytes_per_pixel;
            for (int x = 0; x < w; ++x) {
                float* out = &rgba[((size_t)(by - ymin + r) * w + x) * 4];
                if (rgb) {
                    out[0] = sample(row + channel_offset(ri) + (size_t)x * channels[(size_t)ri].bytes,
                                    channels[(size_t)ri].type);
                    out[1] = sample(row + channel_offset(gi) + (size_t)x * channels[(size_t)gi].bytes,
                                    channels[(size_t)gi].type);
                    out[2] = sample(row + channel_offset(bi) + (size_t)x * channels[(size_t)bi].bytes,
                                    channels[(size_t)bi].type);
                } else {
                    const float y = sample(row + channel_offset(yi) + (size_t)x * channels[(size_t)yi].bytes,
                                           channels[(size_t)yi].type);
                    out[0] = out[1] = out[2] = y;
                }
                if (ai >= 0)
                    out[3] = sample(row + channel_offset(ai) + (size_t)x * channels[(size_t)ai].bytes,
                                    channels[(size_t)ai].type);
            }
        }
    }
    return true;
}

/* ---- 真正 PMREM（IBL 预过滤，CPU、确定性）----
 * three.js / UE 的 split-sum IBL：镜面 = GGX importance-sample 预过滤的粗糙度 mip 链
 *（V=N=R 假设下按 NdotL 加权归一），漫反射 = 余弦加权半球卷积的 irradiance 贴图。
 * 之前实现是 2×2 box mip 近似：粗糙度增大时只是"更糊"，不是 GGX 能量分布，
 * 且漫反射仍用 analytic —— 这两点在此替换。EXR 已支持；cubemap kind=1 仍列后续。 */
static constexpr float kEnvPi = 3.14159265358979f;

static void env_dir_from_uv(float u, float v, float out[3]) {
    const float phi = (u - 0.5f) * 2.0f * kEnvPi;
    const float theta = v * kEnvPi;
    const float st = std::sin(theta);
    out[0] = st * std::cos(phi);
    out[1] = std::cos(theta);
    out[2] = st * std::sin(phi);
}
static void env_uv_from_dir(const float d[3], float& u, float& v) {
    u = std::atan2(d[2], d[0]) / (2.0f * kEnvPi) + 0.5f;
    v = std::acos(std::clamp(d[1], -1.0f, 1.0f)) / kEnvPi;
}
static void env_normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-20f;
    v[0] /= l; v[1] /= l; v[2] /= l;
}
static float env_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
/* 以 n 为 z 轴的切线基（避免极点退化）。 */
static void env_basis(const float n[3], float t[3], float b[3]) {
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (std::fabs(n[1]) > 0.999f) { up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f; }
    t[0] = up[1] * n[2] - up[2] * n[1];
    t[1] = up[2] * n[0] - up[0] * n[2];
    t[2] = up[0] * n[1] - up[1] * n[0];
    env_normalize(t);
    b[0] = n[1] * t[2] - n[2] * t[1];
    b[1] = n[2] * t[0] - n[0] * t[2];
    b[2] = n[0] * t[1] - n[1] * t[0];
}
/* equirect 双线性采样：经度 wrap、纬度 clamp（与 GPU 侧 sampler 语义一致）。 */
static void env_sample_bilinear(const std::vector<float>& img, int w, int h,
                                const float d[3], float out[3]) {
    float u = 0.0f, v = 0.0f;
    env_uv_from_dir(d, u, v);
    const float fx = u * (float)w - 0.5f;
    const float fy = v * (float)h - 0.5f;
    const int x0 = (int)std::floor(fx);
    const int y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto at = [&](int x, int y) -> const float* {
        x %= w; if (x < 0) x += w;
        y = std::clamp(y, 0, h - 1);
        return &img[((size_t)y * (size_t)w + (size_t)x) * 4];
    };
    const float* p00 = at(x0, y0);
    const float* p10 = at(x0 + 1, y0);
    const float* p01 = at(x0, y0 + 1);
    const float* p11 = at(x0 + 1, y0 + 1);
    for (int c = 0; c < 3; ++c) {
        const float a = p00[c] + (p10[c] - p00[c]) * tx;
        const float b = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = a + (b - a) * ty;
    }
}
/* Hammersley 低差异序列（radical inverse base 2）。 */
static float radical_inverse_vdc(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}
/* GGX/Trowbridge-Reitz 重要性采样：返回围绕 n 的半程向量 H。 */
static void env_importance_sample_ggx(float xi1, float xi2, const float n[3], float rough, float h[3]) {
    const float a = rough * rough;
    const float a2 = a * a;
    const float phi = 2.0f * kEnvPi * xi1;
    const float cos_theta = std::sqrt((1.0f - xi2) / (1.0f + (a2 - 1.0f) * xi2));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    float t[3], b[3];
    env_basis(n, t, b);
    const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
    for (int i = 0; i < 3; ++i) h[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
}
/* ---- cubemap env 辅助（第 26 轮）----
 * 面序：0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z；面内 u 向右、v 向下（图片行 0 = 顶部），
 * 与 tools/equirect_to_cube.py 的 face_dir 逐式一致；env_face_uv_from_dir 是其逆映射。 */
static void env_face_dir(int face, float u, float v, float d[3]) {
    const float s = 2.0f * u - 1.0f, t = 2.0f * v - 1.0f;
    switch (face) {
    case 0: d[0] = 1.0f;  d[1] = -t;    d[2] = -s;    break;   /* +X */
    case 1: d[0] = -1.0f; d[1] = -t;    d[2] = s;     break;   /* -X */
    case 2: d[0] = s;     d[1] = 1.0f;  d[2] = t;     break;   /* +Y */
    case 3: d[0] = s;     d[1] = -1.0f; d[2] = -t;    break;   /* -Y */
    case 4: d[0] = s;     d[1] = -t;    d[2] = 1.0f;  break;   /* +Z */
    default: d[0] = -s;   d[1] = -t;    d[2] = -1.0f; break;   /* -Z */
    }
}
static void env_face_uv_from_dir(const float d[3], int& face, float& u, float& v) {
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    const float ma = std::max(ax, std::max(ay, az));
    if (ma <= 1e-8f) { face = 0; u = 0.5f; v = 0.5f; return; }
    if (ax >= ay && ax >= az) {
        if (d[0] >= 0.0f) {
            face = 0; u = 0.5f * (1.0f - d[2] / ma); v = 0.5f * (1.0f - d[1] / ma);
        } else {
            face = 1; u = 0.5f * (1.0f - d[2] / ma); v = 0.5f * (1.0f + d[1] / ma);
        }
    } else if (ay >= az) {
        if (d[1] >= 0.0f) {
            face = 2; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f + d[2] / ma);
        } else {
            face = 3; u = 0.5f * (1.0f - d[0] / ma); v = 0.5f * (1.0f + d[2] / ma);
        }
    } else {
        if (d[2] >= 0.0f) {
            face = 4; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f - d[1] / ma);
        } else {
            face = 5; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f + d[1] / ma);
        }
    }
}
static void env_sample_cube(const std::vector<std::vector<float>>& faces, int w,
                            const float dir[3], float out[3]) {
    float n[3] = { dir[0], dir[1], dir[2] };
    env_normalize(n);
    int face = 0;
    float u = 0.5f, v = 0.5f;
    env_face_uv_from_dir(n, face, u, v);
    const std::vector<float>& img = faces[(size_t)face];
    const float x = u * (float)w - 0.5f, y = v * (float)w - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - (float)x0, fy = y - (float)y0;
    auto px = [&](int xx, int yy, float c[3]) {
        xx = std::clamp(xx, 0, w - 1);
        yy = std::clamp(yy, 0, w - 1);
        const float* p = &img[((size_t)yy * (size_t)w + (size_t)xx) * 4];
        c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
    };
    float c00[3], c10[3], c01[3], c11[3];
    px(x0, y0, c00); px(x0 + 1, y0, c10); px(x0, y0 + 1, c01); px(x0 + 1, y0 + 1, c11);
    for (int i = 0; i < 3; ++i)
        out[i] = (c00[i] * (1.0f - fx) + c10[i] * fx) * (1.0f - fy) +
                 (c01[i] * (1.0f - fx) + c11[i] * fx) * fy;
}
/* cube PMREM：mip0 = 原始 6 面（镜面），mip L = GGX 预过滤（rough = L/(levels-1)），
 * 另外生成 16×16×6 的余弦卷积 irradiance cube。输出布局 = “每个 mip 依次 6 面”（gfx create_texture2 约定）。 */
static void build_pmrem_cube(const std::vector<std::vector<float>>& base, int w,
                             std::vector<uint8_t>& spec_chain, int& levels,
                             std::vector<uint8_t>& irr, int& irr_w) {
    levels = 1;
    for (int sz = w; sz > 1; sz >>= 1) ++levels;
    spec_chain.clear();
    for (int f = 0; f < 6; ++f) {
        for (size_t i = 0, np = (size_t)w * (size_t)w; i < np; ++i) {
            append_half_rgba(spec_chain, base[(size_t)f][i * 4], base[(size_t)f][i * 4 + 1],
                             base[(size_t)f][i * 4 + 2]);
        }
    }
    for (int lvl = 1; lvl < levels; ++lvl) {
        const int ow = std::max(1, w >> lvl);
        const float rough = (levels > 1) ? (float)lvl / (float)(levels - 1) : 0.0f;
        const int samples = std::clamp(32 + (int)(rough * 96.0f), 32, 128);
        for (int f = 0; f < 6; ++f) {
            for (int y = 0; y < ow; ++y) {
                for (int x = 0; x < ow; ++x) {
                    float n[3];
                    env_face_dir(f, ((float)x + 0.5f) / (float)ow, ((float)y + 0.5f) / (float)ow, n);
                    env_normalize(n);
                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    float wsum = 0.0f;
                    for (int s = 0; s < samples; ++s) {
                        const float xi1 = ((float)s + 0.5f) / (float)samples;
                        const float xi2 = radical_inverse_vdc((uint32_t)s);
                        float hv[3], ldir[3];
                        env_importance_sample_ggx(xi1, xi2, n, std::max(rough, 1e-3f), hv);
                        const float ndh = env_dot(n, hv);
                        for (int i = 0; i < 3; ++i) ldir[i] = 2.0f * ndh * hv[i] - n[i];
                        env_normalize(ldir);
                        const float ndl = env_dot(n, ldir);
                        if (ndl <= 0.0f) continue;
                        float c[3];
                        env_sample_cube(base, w, ldir, c);
                        for (int i = 0; i < 3; ++i) acc[i] += c[i] * ndl;
                        wsum += ndl;
                    }
                    if (wsum > 0.0f) { for (int i = 0; i < 3; ++i) acc[i] /= wsum; }
                    append_half_rgba(spec_chain, acc[0], acc[1], acc[2]);
                }
            }
        }
    }
    irr_w = 16;
    const int irr_samples = 128;
    irr.clear();
    for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < irr_w; ++y) {
            for (int x = 0; x < irr_w; ++x) {
                float n[3];
                env_face_dir(f, ((float)x + 0.5f) / (float)irr_w, ((float)y + 0.5f) / (float)irr_w, n);
                env_normalize(n);
                float t[3], b[3];
                env_basis(n, t, b);
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                for (int s = 0; s < irr_samples; ++s) {
                    const float xi1 = radical_inverse_vdc((uint32_t)s);
                    const float xi2 = ((float)s + 0.5f) / (float)irr_samples;
                    const float phi = 2.0f * kEnvPi * xi1;
                    const float cos_theta = std::sqrt(std::max(0.0f, 1.0f - xi2));
                    const float sin_theta = std::sqrt(xi2);
                    const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
                    float ldir[3];
                    for (int i = 0; i < 3; ++i) ldir[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
                    env_normalize(ldir);
                    float c[3];
                    env_sample_cube(base, w, ldir, c);
                    for (int i = 0; i < 3; ++i) acc[i] += c[i];
                }
                for (int i = 0; i < 3; ++i) acc[i] /= (float)irr_samples;
                append_half_rgba(irr, acc[0], acc[1], acc[2]);
            }
        }
    }
}

static void build_pmrem(const std::vector<float>& base, int w, int h,
                        std::vector<uint8_t>& spec_chain, int& levels,
                        std::vector<uint8_t>& irr, int& irr_w, int& irr_h) {
    levels = 1;
    for (int s = std::max(w, h); s > 1; s >>= 1) ++levels;
    spec_chain.clear();
    /* mip0 = 原始 radiance（roughness 0 = 镜面，shader 采样它必须保持锐利） */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float* p = &base[((size_t)y * (size_t)w + (size_t)x) * 4];
            append_half_rgba(spec_chain, p[0], p[1], p[2]);
        }
    }
    for (int lvl = 1; lvl < levels; ++lvl) {
        const int ow = std::max(1, w >> lvl), oh = std::max(1, h >> lvl);
        const float rough = (levels > 1) ? (float)lvl / (float)(levels - 1) : 0.0f;
        const int samples = std::clamp(32 + (int)(rough * 96.0f), 32, 128);
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                float n[3];
                env_dir_from_uv(((float)x + 0.5f) / (float)ow, ((float)y + 0.5f) / (float)oh, n);
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                float wsum = 0.0f;
                for (int s = 0; s < samples; ++s) {
                    const float xi1 = ((float)s + 0.5f) / (float)samples;
                    const float xi2 = radical_inverse_vdc((uint32_t)s);
                    float hv[3], ldir[3];
                    env_importance_sample_ggx(xi1, xi2, n, std::max(rough, 1e-3f), hv);
                    const float ndh = env_dot(n, hv);
                    /* V=N=R 假设：L = reflect(-V, H) = 2*(V·H)*H - V */
                    for (int i = 0; i < 3; ++i) ldir[i] = 2.0f * ndh * hv[i] - n[i];
                    env_normalize(ldir);
                    const float ndl = env_dot(n, ldir);
                    if (ndl <= 0.0f) continue;
                    float c[3];
                    env_sample_bilinear(base, w, h, ldir, c);
                    for (int i = 0; i < 3; ++i) acc[i] += c[i] * ndl;
                    wsum += ndl;
                }
                if (wsum > 0.0f) { for (int i = 0; i < 3; ++i) acc[i] /= wsum; }
                append_half_rgba(spec_chain, acc[0], acc[1], acc[2]);
            }
        }
    }
    /* 漫反射 irradiance：余弦加权半球采样 → E/π（= 与 Lambert albedo/π 相乘后的等效辐亮度）。
     * 低分辨率即可（irradiance 在方向域很平滑）：64×32，256 样本。 */
    irr_w = 64; irr_h = 32;
    const int irr_samples = 256;
    irr.clear();
    for (int y = 0; y < irr_h; ++y) {
        for (int x = 0; x < irr_w; ++x) {
            float n[3];
            env_dir_from_uv(((float)x + 0.5f) / (float)irr_w, ((float)y + 0.5f) / (float)irr_h, n);
            float t[3], b[3];
            env_basis(n, t, b);
            float acc[3] = { 0.0f, 0.0f, 0.0f };
            for (int s = 0; s < irr_samples; ++s) {
                const float xi1 = radical_inverse_vdc((uint32_t)s);
                const float xi2 = ((float)s + 0.5f) / (float)irr_samples;
                const float phi = 2.0f * kEnvPi * xi1;
                const float cos_theta = std::sqrt(std::max(0.0f, 1.0f - xi2));
                const float sin_theta = std::sqrt(xi2);
                const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
                float ldir[3];
                for (int i = 0; i < 3; ++i) ldir[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
                env_normalize(ldir);
                float c[3];
                env_sample_bilinear(base, w, h, ldir, c);
                for (int i = 0; i < 3; ++i) acc[i] += c[i];
            }
            for (int i = 0; i < 3; ++i) acc[i] /= (float)irr_samples;
            append_half_rgba(irr, acc[0], acc[1], acc[2]);
        }
    }
}

int Engine::load_texture_ref(const std::string& ref) {
    const TexPathParams tp = parse_tex_path(ref);
    if (tp.vfs.rfind("builtin:", 0) == 0) {
        auto it = tex_cache.find(ref);
        if (it != tex_cache.end()) return it->second;
        int w = 1, h = 1;
        std::vector<uint8_t> px;
        int builtin_mips = 1;
        int builtin_format = 0;             /* 0=RGBA8Unorm 1=RGBA8UnormSrgb */
        int builtin_wrap_u = -1, builtin_wrap_v = -1;
        if (tp.vfs == "builtin:/textures/checker") {
            w = h = 16; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                bool alt = (((x / 4) + (y / 4)) & 1) != 0;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = alt ? 255 : 230; c[1] = alt ? 255 : 140; c[2] = alt ? 255 : 60; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/toon_ramp") {
            /* Toon 渐变：256×1 四档硬阶（与 toon_steps=4 对应；线性 clamp 采样） */
            w = 256; h = 1; px.resize((size_t)w * 4);
            static const uint8_t steps[4] = { 64, 132, 200, 255 };
            for (int x = 0; x < w; ++x) {
                const uint8_t v = steps[std::min(3, x * 4 / w)];
                uint8_t* c = &px[(size_t)x * 4]; c[0] = v; c[1] = v; c[2] = v; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/matcap_studio") {
            /* Matcap：256×256 冷色相影棚球（左上高光、右下暗），线性采样/clamp；用作 view-space 法线查表 */
            w = h = 256; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const float nx = (float(x) + 0.5F) / w * 2.0F - 1.0F;
                const float ny = (float(y) + 0.5F) / h * 2.0F - 1.0F;
                const float r2 = nx * nx + ny * ny;
                const float nz = std::sqrt(std::max(0.0F, 1.0F - r2));
                /* 视空间光：左上前方（与 view 法线约定一致；纹理 v 向下 → ny 取反） */
                const float lx = -0.42F, ly = -0.50F, lz = 0.76F;
                const float ndl = std::max(0.0F, nx * lx + ny * ly + nz * lz);
                const float spec = std::pow(std::max(0.0F, nx * (-0.20F) + ny * (-0.26F) + nz * 0.94F), 28.0F);
                const float diff = 0.12F + 0.88F * ndl;
                const float edge = std::clamp(1.0F - r2, 0.0F, 1.0F);   /* 球外渐暗 */
                const float r = (0.20F * diff + spec * 0.95F) * edge;
                const float g = (0.34F * diff + spec * 0.98F) * edge;
                const float b = (0.62F * diff + spec * 0.95F) * edge;
                const float a = 1.0F;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = (uint8_t)std::clamp((int)(r * 255.0F + 0.5F), 0, 255);
                c[1] = (uint8_t)std::clamp((int)(g * 255.0F + 0.5F), 0, 255);
                c[2] = (uint8_t)std::clamp((int)(b * 255.0F + 0.5F), 0, 255);
                c[3] = (uint8_t)(a * 255.0F);
            }
        } else { px = { 255, 255, 255, 255 }; }
        int id = -1;
        if (rapi && rself && rapi->create_texture2) {
            /* builtin 查表纹理默认 clamp（matcap 球/渐变在边界外不能 wrap）；路径后缀可覆盖。 */
            const int wrap_u = tp.wrap_set ? tp.wrap_u : (builtin_wrap_u >= 0 ? builtin_wrap_u : 1);
            const int wrap_v = tp.wrap_set ? tp.wrap_v : (builtin_wrap_v >= 0 ? builtin_wrap_v : 1);
            const int filter = tp.filter_set ? tp.filter : 1;
            mq_renderer_texture_desc td{};
            td.width = w; td.height = h; td.pixel_format = builtin_format; td.kind = 0;
            td.mip_levels = builtin_mips;
            td.filter = filter; td.wrap_u = wrap_u; td.wrap_v = wrap_v;
            td.data = px.data(); td.data_size = px.size();
            if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
        }
        if (id < 0) id = rapi->create_texture(rself, px.data(), w, h);
        tex_cache[ref] = id;
        return id;
    }
    return load_texture(ref);   // assets（load_texture 内部剥 sampler 后缀）
}

/* M4c：加载离线导入的 .mesh.json（tools/gltf_import.py 产出）→ 渲染器网格句柄。
 * 只读资产文件；按路径缓存（失败也缓存 -1，避免每帧重试/刷日志）。
 * plans/22 §6.1：vertex_layout 2/3 = 48/56B 基础 + joints u16x4 + weights f32x4（stride 72/80）；
 * morph 网格额外保存**打包后的基础顶点** + targets/default_weights，创建后立即应用默认权重。 */
int32_t Engine::load_mesh(const std::string& path) {
    auto it = mesh_cache.find(path);
    if (it != mesh_cache.end()) return it->second;
    if (!gltf_env_read) read_gltf_env();
    const auto t0 = std::chrono::steady_clock::now();
    int32_t id = -1;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != 0) {
        std::print(stderr, "runtime: mesh not found '{}'\n", path);
    } else {
        mq_json* dom = mq_json_parse((const char*)data, n);
        mq_vfs_free(data);
        if (!dom) {
            std::print(stderr, "runtime: mesh json 解析失败 '{}'\n", path);
        } else {
            std::vector<float> verts;
            std::vector<uint32_t> idx;
            /* 顶点布局（plans/22 §5.1）：
             *   0 = 旧 48B：pos3|normal3|tangent4|uv0 2（12 float/顶点）
             *   1 = 新 56B：pos3|normal3|color4|uv0 2|uv1 2（14 float/顶点）
             *   2 = 48B + joints u16x4 + weights f32x4（stride 72；JSON 行宽仍 12）
             *   3 = 56B + joints u16x4 + weights f32x4（stride 80；JSON 行宽仍 14）
             * 蒙皮数据在独立 joints/weights 数组 → 运行时按 GPU stride 打包。 */
            int32_t vertex_layout = 0;
            if (const mq_json* jlay = mq_json_get(dom, "vertex_layout"))
                vertex_layout = (int32_t)mq_json_num(jlay, 0.0);
            const bool base56 = vertex_layout == 1 || vertex_layout == 3;
            const uint32_t row_floats = base56 ? 14u : 12u;
            const uint32_t base_stride = base56 ? 56u : 48u;
            const bool skinned = vertex_layout >= 2 && vertex_layout <= 3;
            const uint32_t stride = base_stride + (skinned ? 24u : 0u);
            const mq_json* jv = mq_json_get(dom, "vertices");
            const uint32_t vn = mq_json_len(jv);
            verts.reserve((size_t)vn * row_floats);
            const mq_json* ji = mq_json_get(dom, "indices");
            const uint32_t in = mq_json_len(ji);
            idx.reserve(in);
            bool ok = vn > 0 && in > 0 && vertex_layout >= 0 && vertex_layout <= 3;
            if (!ok) std::print(stderr, "runtime: mesh '{}' 无效（vertices={} indices={} vertex_layout={}）\n",
                                path, vn, in, vertex_layout);
            for (uint32_t i = 0; ok && i < vn; ++i) {
                const mq_json* row = mq_json_at(jv, i);
                if (mq_json_len(row) != row_floats) { ok = false; break; }
                for (uint32_t k = 0; k < row_floats; ++k)
                    verts.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
            }
            for (uint32_t i = 0; ok && i < in; ++i)
                idx.push_back((uint32_t)mq_json_num(mq_json_at(ji, i), 0.0));
            /* 蒙皮属性打包（joints u16x4 + weights f32x4 追加在基础顶点之后） */
            std::vector<uint8_t> packed;
            const void* vptr = verts.data();
            uint64_t vbytes = (uint64_t)verts.size() * sizeof(float);
            if (ok && skinned) {
                const mq_json* jj = mq_json_get(dom, "joints");
                const mq_json* jw = mq_json_get(dom, "weights");
                ok = mq_json_len(jj) == vn && mq_json_len(jw) == vn;
                if (!ok) std::print(stderr, "runtime: mesh '{}' 蒙皮数据缺失/长度不符（joints={} weights={} vertices={}）\n",
                                    path, mq_json_len(jj), mq_json_len(jw), vn);
                if (ok) {
                    packed.resize((size_t)vn * stride);
                    for (uint32_t i = 0; i < vn; ++i) {
                        uint8_t* dst = packed.data() + (size_t)i * stride;
                        std::memcpy(dst, verts.data() + (size_t)i * row_floats, base_stride);
                        const mq_json* jr = mq_json_at(jj, i);
                        const mq_json* wr = mq_json_at(jw, i);
                        if (mq_json_len(jr) != 4 || mq_json_len(wr) != 4) { ok = false; break; }
                        uint16_t* jdst = (uint16_t*)(dst + base_stride);
                        for (uint32_t k = 0; k < 4; ++k) {
                            int v = (int)mq_json_num(mq_json_at(jr, k), 0.0);
                            if (v < 0) v = 0;
                            if (v > 65535) v = 65535;
                            jdst[k] = (uint16_t)v;
                        }
                        float* wdst = (float*)(dst + base_stride + 8);
                        for (uint32_t k = 0; k < 4; ++k)
                            wdst[k] = (float)mq_json_num(mq_json_at(wr, k), 0.0);
                    }
                    if (ok) { vptr = packed.data(); vbytes = packed.size(); }
                }
            }
            /* morph（可选）：default_weights + targets（逐顶点 pos/normal 增量；targets ≤ 8） */
            MorphMesh morph;
            if (ok) {
                if (const mq_json* jm = mq_json_get(dom, "morph")) {
                    const mq_json* jdw = mq_json_get(jm, "default_weights");
                    const uint32_t ndw = mq_json_len(jdw);
                    morph.default_weights.reserve(ndw);
                    for (uint32_t k = 0; k < ndw; ++k)
                        morph.default_weights.push_back((float)mq_json_num(mq_json_at(jdw, k), 0.0));
                    const mq_json* jts = mq_json_get(jm, "targets");
                    const uint32_t nts = mq_json_len(jts);
                    if (nts > 8)
                        std::print(stderr, "runtime: mesh '{}' morph targets={} 超出上限 8，后续忽略\n",
                                   path, nts);
                    for (uint32_t t = 0; t < nts && t < 8 && ok; ++t) {
                        const mq_json* jt = mq_json_at(jts, t);
                        MorphTarget tgt;
                        const mq_json* jp = mq_json_get(jt, "positions");
                        const mq_json* jn = mq_json_get(jt, "normals");
                        if (mq_json_len(jp) != vn) { ok = false; break; }
                        tgt.positions.reserve((size_t)vn * 3);
                        for (uint32_t i = 0; i < vn && ok; ++i) {
                            const mq_json* row = mq_json_at(jp, i);
                            if (mq_json_len(row) != 3) { ok = false; break; }
                            for (uint32_t k = 0; k < 3; ++k)
                                tgt.positions.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                        }
                        if (ok && mq_json_len(jn) == vn) {
                            tgt.normals.reserve((size_t)vn * 3);
                            for (uint32_t i = 0; i < vn && ok; ++i) {
                                const mq_json* row = mq_json_at(jn, i);
                                if (mq_json_len(row) != 3) { ok = false; break; }
                                for (uint32_t k = 0; k < 3; ++k)
                                    tgt.normals.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                            }
                        }
                        if (ok) morph.targets.push_back(std::move(tgt));
                    }
                    if (morph.default_weights.size() < morph.targets.size())
                        morph.default_weights.resize(morph.targets.size(), 0.0F);
                }
            }
            if (ok && rapi && rself && rapi->create_mesh) {
                mq_renderer_mesh_gpu md{};
                md.vertices = vptr;
                md.vertex_bytes = vbytes;
                md.vertex_stride = stride;
                md.vertex_layout = vertex_layout;
                md.indices = idx.data();
                md.index_count = (uint32_t)idx.size();
                const mq_json* bmin = mq_json_get(dom, "bounds_min");
                const mq_json* bmax = mq_json_get(dom, "bounds_max");
                for (uint32_t k = 0; k < 3 && k < mq_json_len(bmin); ++k)
                    md.bounds_min[k] = (float)mq_json_num(mq_json_at(bmin, k), 0.0);
                for (uint32_t k = 0; k < 3 && k < mq_json_len(bmax); ++k)
                    md.bounds_max[k] = (float)mq_json_num(mq_json_at(bmax, k), 0.0);
                if (rapi->create_mesh(rself, &md, &id) != 0) id = -1;
                if (id >= 0) mesh_stats[id] = { vn, (uint32_t)idx.size() };
            }
            /* morph：保存打包后的基础顶点（CPU 重算起点），创建后立即应用默认权重 */
            if (id >= 0 && !morph.targets.empty()) {
                morph.mesh = id;
                morph.vertex_count = vn;
                morph.stride = stride;
                morph.layout = vertex_layout;
                morph.base = std::move(packed);
                if (morph.base.empty()) {
                    morph.base.resize((size_t)vn * stride);
                    std::memcpy(morph.base.data(), verts.data(), (size_t)vn * base_stride);
                }
                morph.applied_weights = morph.default_weights;
                const std::string mp_key = path;
                morph_meshes[mp_key] = std::move(morph);
                bool any_w = false;
                for (float w : morph_meshes[mp_key].default_weights) if (w != 0.0F) { any_w = true; break; }
                if (any_w) (void)apply_morph(mp_key, morph_meshes[mp_key].default_weights);
                if (render3d_info)
                    std::print("runtime: morph mesh '{}' targets={} vertices={} stride={}\n",
                               path, (int)morph_meshes[mp_key].targets.size(),
                               (long long)morph_meshes[mp_key].vertex_count,
                               (long long)morph_meshes[mp_key].stride);
            }
            if (id < 0) std::print(stderr, "runtime: mesh 载入失败 '{}'\n", path);
            else if (render3d_info) {
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::print("runtime: mesh '{}' vertices={} indices={} ({:.1f} ms)\n",
                           path, vn, in, ms);
            }
            mesh_layout[path] = (id >= 0) ? vertex_layout : -1;
            mq_json_free(dom);
        }
    }
    mesh_cache[path] = id;
    return id;
}

/* ---- 运行时 glTF/GLB 直接加载（plans/23/24）----
 * mesh_path 指向 .gltf/.glb 时不必先跑 Python importer：装配期解析 + 上传一次，之后每帧只读缓存。
 * 资源（buffer/image）的相对 URI 按**模型文件所在目录**经 VFS 解析（远程挂载同样适用）。 */
bool Engine::read_vfs_file(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    if (!vfs) return false;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != 0 || !data) return false;
    out.assign((const uint8_t*)data, (const uint8_t*)data + n);
    mq_vfs_free(data);
    return true;
}

int32_t Engine::upload_texture_rgba(const uint8_t* rgba, int w, int h, bool srgb,
                                    int filter, int wrap_u, int wrap_v) {
    if (!rapi || !rself || !rgba || w <= 0 || h <= 0) return -1;
    int32_t id = -1;
    if (rapi->create_texture2) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, w, h, levels, srgb);
        mq_renderer_texture_desc td{};
        td.width = w;
        td.height = h;
        td.pixel_format = srgb ? 1 : 0;       /* 1 = RGBA8_SRGB，0 = RGBA8（线性数据） */
        td.kind = 0;
        td.mip_levels = levels;
        td.filter = filter;
        td.wrap_u = wrap_u;
        td.wrap_v = wrap_v;
        td.data = mips.data();
        td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    if (id < 0 && rapi->create_texture) {
        id = rapi->create_texture(rself, rgba, (int32_t)w, (int32_t)h);
    }
    return id;
}

Engine::GltfGpuModel* Engine::load_gltf_model(const std::string& path) {
    auto cached = gltf_models.find(path);
    if (cached != gltf_models.end()) return &cached->second;
    GltfGpuModel gpu;
    auto fail = [&](const char* why) -> GltfGpuModel* {
        std::print(stderr, "runtime: gltf 加载失败 '{}': {}\n", path, why);
        gpu.failed = true;
        gltf_models[path] = std::move(gpu);
        return &gltf_models[path];
    };
    std::vector<uint8_t> bytes;
    if (!read_vfs_file(path, bytes))
        return fail("读不到文件（mesh_path 相对场景目录 / VFS 根）");
    const auto start = std::chrono::steady_clock::now();
    auto readfn = [this](const std::string& p, std::vector<uint8_t>& out) {
        return read_vfs_file(p, out);
    };
    gltf::Model cpu = gltf::parse(path, bytes, readfn);
    if (cpu.failed || (cpu.primitives.empty() && cpu.lights.empty() && cpu.cameras.empty()))
        return fail(cpu.error.empty() ? "没有可导入的三角网格/灯光/相机" : cpu.error.c_str());

    /* ---- 图像解码（同一 image 只解码一次）---- */
    struct Decoded {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
    };
    std::vector<Decoded> decoded(cpu.images.size());
    auto decode_image = [&](int idx) -> Decoded* {
        if (idx < 0 || (size_t)idx >= cpu.images.size()) return nullptr;
        Decoded& d = decoded[(size_t)idx];
        if (d.ok) return &d;
        if (d.w < 0) return nullptr;
        const gltf::Image& im = cpu.images[(size_t)idx];
        if (!im.valid || im.bytes.empty()) {
            d.w = -1;
            return nullptr;
        }
        uint8_t* rgba = nullptr;
        uint32_t w = 0, h = 0;
        char err[128] = { 0 };
        if (mq_image_decode(im.bytes.data(), im.bytes.size(), &rgba, &w, &h, err, sizeof err) != 0) {
            std::print(stderr, "runtime: gltf '{}' image[{}] 解码失败（{}）\n", path, idx, err);
            d.w = -1;
            return nullptr;
        }
        d.rgba.assign(rgba, rgba + (size_t)w * (size_t)h * 4u);
        d.w = (int)w;
        d.h = (int)h;
        d.ok = true;
        mq_image_free(rgba);
        return &d;
    };

    /* ---- 纹理上传（同一 image + sampler + sRGB 组合只上传一次）---- */
    std::unordered_map<uint64_t, int32_t> tex_map;
    auto tex_for = [&](const gltf::TextureRef& ref, bool srgb) -> int32_t {
        if (!ref.present) return -1;
        Decoded* d = decode_image(ref.image);
        if (!d) return -1;
        const uint64_t key = ((uint64_t)(uint32_t)ref.image << 32) |
                             ((uint64_t)(srgb ? 1u : 0u) << 20) |
                             ((uint64_t)(ref.sampler.filter & 3) << 16) |
                             ((uint64_t)(ref.sampler.wrap_u & 3) << 8) |
                             (uint64_t)(ref.sampler.wrap_v & 3);
        auto hit = tex_map.find(key);
        if (hit != tex_map.end()) return hit->second;
        const int32_t id = upload_texture_rgba(d->rgba.data(), d->w, d->h, srgb,
                                               ref.sampler.filter, ref.sampler.wrap_u,
                                               ref.sampler.wrap_v);
        tex_map[key] = id;
        if (id >= 0) gpu.textures.push_back(id);
        if (render3d_info) {
            const std::string& label = cpu.images[(size_t)ref.image].uri;
            std::print("runtime: gltf texture image={} '{}' srgb={} -> {}\n",
                       ref.image, label, srgb ? 1 : 0, id);
        }
        return id;
    };

    /* ---- 材质（核心 PBR + KHR 扩展；默认值与 extract_3d 组件路径一致）---- */
    auto defaults = [&]() {
        mq_renderer_pbr_material mat{};
        for (int ti = 0; ti < 5; ++ti) mat.textures[ti] = -1;
        for (int ti = 0; ti < 3; ++ti) mat.textures2[ti] = -1;
        for (int ti = 0; ti < 7; ++ti) mat.textures3[ti] = -1;
        for (int ti = 0; ti < 2; ++ti) {
            mat.textures4[ti] = -1;
            mat.textures6[ti] = -1;
        }
        mat.textures5[0] = -1;
        mat.textures7[0] = -1;
        for (int ti = 0; ti < 2; ++ti) {
            mat.uv_scale[ti] = 1.0F;
            mat.uv_offset[ti] = 0.0F;
            mat.normal_uv_scale[ti] = 1.0F;
            mat.normal_uv_offset[ti] = 0.0F;
        }
        mat.shading_model = 0;
        mat.side = 2;
        mat.opacity = 1.0F;
        mat.emissive_intensity = 1.0F;
        mat.env_intensity = 1.0F;
        mat.normal_scale_y = 1.0F;
        mat.shininess = 30.0F;
        mat.specular_intensity = 1.0F;
        mat.ior = 1.5F;
        mat.specular[0] = mat.specular[1] = mat.specular[2] = 1.0F;
        mat.sheen_roughness = 0.5F;
        mat.iridescence_ior = 1.3F;
        mat.iridescence_thickness_min = 100.0F;
        mat.iridescence_thickness_max = 400.0F;
        mat.clearcoat_normal_scale = 1.0F;
        mat.clearcoat_roughness = 0.03F;
        mat.toon_steps = 3;
        mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = mat.base_color[3] = 1.0F;
        mat.metallic = 0.0F;
        mat.roughness = 0.6F;
        mat.normal_scale = 1.0F;
        mat.occlusion_strength = 1.0F;
        mat.alpha_cutoff = 0.5F;
        return mat;
    };

    gpu.mats.reserve(cpu.materials.size());
    for (const gltf::Material& m : cpu.materials) {
        mq_renderer_pbr_material mat = defaults();
        for (int k = 0; k < 4; ++k) mat.base_color[k] = m.base_color[k];
        for (int k = 0; k < 3; ++k) mat.emissive[k] = m.emissive[k];
        mat.metallic = m.metallic;
        mat.roughness = m.roughness;
        mat.normal_scale = m.normal_scale;
        mat.occlusion_strength = m.occlusion_strength;
        mat.alpha_cutoff = m.alpha_cutoff;
        mat.alpha_mode = m.alpha_mode;
        mat.flags = m.double_sided ? 1 : 0;
        mat.side = m.double_sided ? 2 : 0;
        mat.shading_model = m.unlit ? 2 : 0;
        mat.emissive_intensity = m.emissive_strength;
        int32_t uv1_mask = 0;
        const int32_t t_base = tex_for(m.base_color_tex, true);
        const int32_t t_mr = tex_for(m.metallic_roughness_tex, false);
        const int32_t t_ao = tex_for(m.occlusion_tex, false);
        const int32_t t_nrm = tex_for(m.normal_tex, false);
        const int32_t t_em = tex_for(m.emissive_tex, true);
        if (t_base >= 0) mat.textures[0] = t_base;
        if (t_mr >= 0) mat.textures[1] = t_mr;
        if (t_ao >= 0) mat.textures[2] = t_ao;
        if (t_nrm >= 0) mat.textures[3] = t_nrm;
        if (t_em >= 0) mat.textures[4] = t_em;
        if (m.base_color_tex.present && m.base_color_tex.texcoord > 0) uv1_mask |= 1 << 0;
        if (m.metallic_roughness_tex.present && m.metallic_roughness_tex.texcoord > 0) uv1_mask |= 1 << 1;
        if (m.occlusion_tex.present && m.occlusion_tex.texcoord > 0) uv1_mask |= 1 << 2;
        if (m.normal_tex.present && m.normal_tex.texcoord > 0) uv1_mask |= 1 << 3;
        if (m.emissive_tex.present && m.emissive_tex.texcoord > 0) uv1_mask |= 1 << 4;
        mat.flags2 |= (uv1_mask & 0xFF) << 4;
        if (m.base_color_tex.has_transform) {
            for (int k = 0; k < 2; ++k) {
                mat.uv_scale[k] = m.base_color_tex.scale[k];
                mat.uv_offset[k] = m.base_color_tex.offset[k];
            }
            mat.uv_rotation = m.base_color_tex.rotation;
        }
        if (m.normal_tex.has_transform) {
            for (int k = 0; k < 2; ++k) {
                mat.normal_uv_scale[k] = m.normal_tex.scale[k];
                mat.normal_uv_offset[k] = m.normal_tex.offset[k];
            }
            mat.normal_uv_rotation = m.normal_tex.rotation;
        }
        /* P4 KHR 材质扩展。 */
        mat.shading_model = m.unlit ? 2 : (m.physical ? 1 : 0);
        mat.opacity = m.opacity;
        mat.clearcoat = m.clearcoat;
        mat.clearcoat_roughness = m.clearcoat_roughness > 0.0F ? m.clearcoat_roughness : 0.03F;
        mat.sheen = m.sheen;
        mat.sheen_roughness = m.sheen > 0.0F ? m.sheen_roughness : 0.5F;
        for (int k = 0; k < 3; ++k) mat.sheen_color[k] = m.sheen_color[k];
        mat.iridescence = m.iridescence;
        mat.iridescence_ior = m.iridescence_ior;
        mat.iridescence_thickness_min = m.iridescence_thickness_min;
        mat.iridescence_thickness_max = m.iridescence_thickness_max;
        mat.specular_intensity = m.specular_intensity;
        for (int k = 0; k < 3; ++k) mat.specular[k] = m.specular_color[k];
        mat.ior = m.ior;
        mat.anisotropy = m.anisotropy;
        mat.anisotropy_rotation = m.anisotropy_rotation;
        mat.volume_thickness = m.volume_thickness;
        mat.attenuation_distance = m.attenuation_distance;
        for (int k = 0; k < 3; ++k) {
            mat.attenuation_color[k] = m.attenuation_color[k];
            mat.diffuse_transmission_color[k] = m.diffuse_transmission_color[k];
            mat.multiscatter_color[k] = m.multiscatter_color[k];
        }
        mat.dispersion = m.dispersion;
        if (m.alpha_mask_in_blend) mat.flags2 |= 0x80000;
        if (m.transmission > 0.001F) {
            mat.anisotropy_rotation = m.transmission;
            mat.flags2 |= 0x100000;
        }
        if (m.diffuse_transmission > 0.001F) {
            mat.diffuse_transmission_factor = m.diffuse_transmission;
            mat.flags2 |= 0x1000000;
        }
        auto tex3 = [&](const gltf::TextureRef& ref, int slot, bool srgb) {
            const int tid = tex_for(ref, srgb);
            if (tid >= 0) {
                mat.textures3[slot] = tid;
                mat.flags2 |= 1 << (12 + slot);
            }
        };
        tex3(m.clearcoat_tex, 0, false);
        tex3(m.clearcoat_roughness_tex, 1, false);
        tex3(m.sheen_color_tex, 2, true);
        tex3(m.sheen_roughness_tex, 3, false);
        tex3(m.iridescence_tex, 4, false);
        tex3(m.iridescence_thickness_tex, 5, false);
        tex3(m.anisotropy_tex, 6, false);
        {
            const int tid = tex_for(m.clearcoat_normal_tex, false);
            if (tid >= 0) { mat.textures2[2] = tid; mat.flags2 |= 8; }
        }
        {
            const int tid = tex_for(m.specular_tex, false);
            if (tid >= 0) { mat.textures4[0] = tid; mat.flags2 |= 1 << 21; }
            const int tid2 = tex_for(m.specular_color_tex, true);
            if (tid2 >= 0) { mat.textures4[1] = tid2; mat.flags2 |= 1 << 22; }
        }
        {
            const int tid = tex_for(m.transmission_tex, false);
            if (tid >= 0) { mat.textures5[0] = tid; mat.flags2 |= 0x800000; }
        }
        {
            const int tid = tex_for(m.diffuse_transmission_tex, false);
            if (tid >= 0) { mat.textures6[0] = tid; mat.flags2 |= 1 << 25; }
            const int tid2 = tex_for(m.diffuse_transmission_color_tex, true);
            if (tid2 >= 0) { mat.textures6[1] = tid2; mat.flags2 |= 1 << 26; }
        }
        {
            const int tid = tex_for(m.volume_thickness_tex, false);
            if (tid >= 0) { mat.textures7[0] = tid; mat.flags2 |= 0x8000000; }
        }
        if (render3d_info)
            std::print("runtime: gltf material[{}] '{}' shading={} clearcoat={:.3f} sheen={:.3f} "
                       "iridescence={:.3f} specular={:.3f} ior={:.3f} anisotropy={:.3f} "
                       "transmission={:.3f} volume={:.3f} diffuse_transmission={:.3f} tex3={} "
                       "tex4={}/{} tex5={} tex6={}/{} tex7={}\n",
                       gpu.mats.size(), m.name, mat.shading_model, (double)mat.clearcoat,
                       (double)mat.sheen, (double)mat.iridescence, (double)mat.specular_intensity,
                       (double)mat.ior, (double)mat.anisotropy, (double)m.transmission,
                       (double)mat.volume_thickness, (double)mat.diffuse_transmission_factor,
                       mat.textures3[0] >= 0 || mat.textures3[1] >= 0 || mat.textures3[2] >= 0 ||
                               mat.textures3[3] >= 0 || mat.textures3[4] >= 0 ||
                               mat.textures3[5] >= 0 || mat.textures3[6] >= 0,
                       mat.textures4[0], mat.textures4[1], mat.textures5[0], mat.textures6[0],
                       mat.textures6[1], mat.textures7[0]);
        gpu.mats.push_back(mat);
    }

    /* ---- 节点 / 蒙皮 / 动画 / 灯光 / 相机 ---- */
    gpu.nodes.reserve(cpu.nodes.size());
    for (const gltf::Node& n : cpu.nodes) {
        GltfGpuModel::Node out;
        out.parent = n.parent;
        for (int k = 0; k < 3; ++k) { out.t[k] = n.translation[k]; out.s[k] = n.scale[k]; }
        for (int k = 0; k < 4; ++k) out.r[k] = n.rotation[k];
        out.has_matrix = n.has_matrix;
        std::memcpy(out.m, n.matrix, sizeof out.m);
        gpu.nodes.push_back(out);
    }
    gpu.order = cpu.node_order;
    gpu.skins.reserve(cpu.skins.size());
    for (const gltf::Skin& s : cpu.skins) {
        GltfGpuModel::Skin out;
        out.joints = s.joints;
        out.inverse_bind = s.inverse_bind;
        gpu.skins.push_back(std::move(out));
    }
    gpu.anims.reserve(cpu.animations.size());
    for (const gltf::Animation& a : cpu.animations) {
        GltfGpuModel::Anim out;
        out.name = a.name;
        out.duration = a.duration;
        out.channels.reserve(a.channels.size());
        for (const gltf::AnimationChannel& c : a.channels) {
            GltfGpuModel::AnimChannel oc;
            oc.node = c.node;
            oc.path = c.path;
            oc.interp = c.interp;
            oc.comps = c.comps;
            oc.pointer_material = c.pointer_material;
            oc.pointer_field = c.pointer_field;
            oc.times = c.times;
            oc.values = c.values;
            out.channels.push_back(std::move(oc));
        }
        gpu.anims.push_back(std::move(out));
    }
    gpu.lights.reserve(cpu.lights.size());
    for (const gltf::Light& L : cpu.lights) {
        GltfGpuModel::Light out;
        out.node = L.node;
        out.type = L.type;
        for (int k = 0; k < 3; ++k) out.color[k] = L.color[k];
        out.intensity = L.intensity;
        out.range = L.range;
        const float inner = std::clamp(L.inner_angle, 0.0F, 1.5707963F);
        const float outer = std::clamp(L.outer_angle, inner, 1.5707963F);
        out.inner_cos = std::cos(inner);
        out.outer_cos = std::cos(outer);
        gpu.lights.push_back(out);
    }
    gpu.cameras.reserve(cpu.cameras.size());
    for (const gltf::Camera& C : cpu.cameras) {
        GltfGpuModel::Camera out;
        out.node = C.node;
        out.perspective = C.perspective;
        out.yfov = C.yfov;
        out.znear = C.znear;
        out.zfar = C.zfar;
        gpu.cameras.push_back(out);
    }
    gpu.variants = cpu.variants;
    {
        float radius = 0.0F;
        for (const gltf::Primitive& p : cpu.primitives) {
            for (int ci = 0; ci < 8; ++ci) {
                const float x = (ci & 1) ? p.bounds_max[0] : p.bounds_min[0];
                const float y = (ci & 2) ? p.bounds_max[1] : p.bounds_min[1];
                const float z = (ci & 4) ? p.bounds_max[2] : p.bounds_min[2];
                radius = std::max(radius, std::sqrt(x * x + y * y + z * z));
            }
        }
        gpu.radius = std::max(radius, 1e-3F);
    }
    if (render3d_info) {
        for (size_t i = 0; i < gpu.skins.size(); ++i)
            std::print("runtime: gltf skin '{}'[{}] joints={}\n",
                       path, i, (long long)gpu.skins[i].joints.size());
        for (const auto& a : gpu.anims)
            std::print("runtime: gltf anim '{}' channels={} duration={:.3f}\n",
                       a.name, (long long)a.channels.size(), (double)a.duration);
        for (size_t i = 0; i < gpu.lights.size(); ++i)
            std::print("runtime: gltf light[{}] node={} type={} intensity={:.3f} range={:.3f} "
                       "spot_cos={:.3f}/{:.3f}\n",
                       i, gpu.lights[i].node, gpu.lights[i].type,
                       (double)gpu.lights[i].intensity, (double)gpu.lights[i].range,
                       (double)gpu.lights[i].inner_cos, (double)gpu.lights[i].outer_cos);
        for (size_t i = 0; i < gpu.cameras.size(); ++i)
            std::print("runtime: gltf camera[{}] node={} perspective={} yfov={:.3f} z=[{:.3f},{:.3f}]\n",
                       i, gpu.cameras[i].node, gpu.cameras[i].perspective ? 1 : 0,
                       (double)gpu.cameras[i].yfov, (double)gpu.cameras[i].znear,
                       (double)gpu.cameras[i].zfar);
        std::print("runtime: gltf lights={} cameras={} radius={:.3f} variants={}\n",
                   (long long)gpu.lights.size(), (long long)gpu.cameras.size(),
                   (double)gpu.radius, (long long)gpu.variants.size());
    }

    /* ---- 网格上传：无蒙皮 48/56B；蒙皮再追加 joints u16x4 + weights f32x4（stride 72/80）---- */
    struct PendingMorph { size_t prim = 0; };
    std::vector<PendingMorph> pending_morph;
    gpu.prims.reserve(cpu.primitives.size());
    for (const gltf::Primitive& p : cpu.primitives) {
        if (!rapi || !rself || !rapi->create_mesh) break;
        if (p.vertices.empty() || p.indices.empty()) continue;
        const bool skinned = p.skin >= 0 && !p.joints.empty() && !p.weights.empty();
        const uint32_t row_floats = (p.vertex_layout == 1) ? 14u : 12u;
        const uint32_t base_stride = (p.vertex_layout == 1) ? 56u : 48u;
        if (p.vertices.size() % row_floats != 0) {
            std::print(stderr, "runtime: gltf '{}' primitive 顶点行宽不符（layout={}）→ 跳过\n",
                       path, p.vertex_layout);
            continue;
        }
        const uint32_t vn = (uint32_t)(p.vertices.size() / row_floats);
        const uint32_t stride = base_stride + (skinned ? 24u : 0u);
        const int layout = p.vertex_layout + (skinned ? 2 : 0);
        std::vector<uint8_t> packed;
        const void* vptr = p.vertices.data();
        uint64_t vbytes = (uint64_t)p.vertices.size() * sizeof(float);
        if (skinned) {
            packed.resize((size_t)vn * stride);
            for (uint32_t i = 0; i < vn; ++i) {
                uint8_t* dst = packed.data() + (size_t)i * stride;
                std::memcpy(dst, p.vertices.data() + (size_t)i * row_floats, base_stride);
                uint16_t* jdst = (uint16_t*)(dst + base_stride);
                const float* wdst = (const float*)(dst + base_stride + 8);
                for (int k = 0; k < 4; ++k)
                    jdst[k] = (uint16_t)p.joints[(size_t)i * 4 + (size_t)k];
                std::memcpy((void*)wdst, p.weights.data() + (size_t)i * 4, 16);
            }
            vptr = packed.data();
            vbytes = packed.size();
        }
        mq_renderer_mesh_gpu md{};
        md.vertices = vptr;
        md.vertex_bytes = vbytes;
        md.vertex_stride = stride;
        md.vertex_layout = layout;
        md.indices = p.indices.data();
        md.index_count = (uint32_t)p.indices.size();
        for (int k = 0; k < 3; ++k) {
            md.bounds_min[k] = p.bounds_min[k];
            md.bounds_max[k] = p.bounds_max[k];
        }
        int32_t mesh_id = -1;
        if (rapi->create_mesh(rself, &md, &mesh_id) != 0 || mesh_id < 0) {
            std::print(stderr, "runtime: gltf '{}' primitive 上传失败（node={}）\n", path, p.node);
            continue;
        }
        mesh_stats[mesh_id] = { vn, (uint32_t)p.indices.size() };
        GltfGpuModel::Prim pr;
        pr.mesh = mesh_id;
        pr.material = p.material;
        pr.node = p.node;
        pr.skin = skinned ? p.skin : -1;
        pr.layout = layout;
        pr.vertex_count = vn;
        pr.stride = stride;
        pr.default_weights = p.morph_weights;
        pr.applied_weights = p.morph_weights;
        pr.variant_materials = p.variant_materials;
        pr.morph_targets.reserve(p.morph_targets.size());
        for (const gltf::MorphTarget& mt : p.morph_targets) {
            GltfGpuModel::MorphTarget out;
            out.positions = mt.positions;
            out.normals = mt.normals;
            pr.morph_targets.push_back(std::move(out));
        }
        if (!pr.morph_targets.empty()) {
            pr.base.resize((size_t)vn * stride);
            std::memcpy(pr.base.data(), vptr, pr.base.size());
            pending_morph.push_back(PendingMorph{ gpu.prims.size() });
        }
        gpu.prims.push_back(std::move(pr));
    }
    if (gpu.prims.empty() && gpu.lights.empty() && gpu.cameras.empty())
        return fail("没有可上传的网格/灯光/相机");

    bool need_default = false;
    for (const auto& pr : gpu.prims)
        if (pr.material < 0 || (size_t)pr.material >= gpu.mats.size()) need_default = true;
    if (need_default) {
        gpu.mats.push_back(defaults());
        const int32_t def = (int32_t)gpu.mats.size() - 1;
        for (auto& pr : gpu.prims)
            if (pr.material < 0 || (size_t)pr.material >= gpu.mats.size()) pr.material = def;
    }
    if (morph_enabled) {
        for (const PendingMorph& pm : pending_morph) {
            GltfGpuModel::Prim& pr = gpu.prims[pm.prim];
            bool any = false;
            for (float w : pr.default_weights) if (w != 0.0F) { any = true; break; }
            if (any) (void)apply_gltf_morph(-1, gpu, pm.prim, pr.default_weights);
        }
    }

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start).count();
    if (render3d_info) {
        size_t n_morph = 0, n_targets = 0;
        for (const auto& pr : gpu.prims) {
            if (!pr.morph_targets.empty()) { ++n_morph; n_targets += pr.morph_targets.size(); }
        }
        std::print("runtime: gltf model '{}' primitives={} materials={} textures={} nodes={} "
                   "skins={} anims={} morph_prims={} morph_targets={} ({:.1f} ms)\n",
                   path, (long long)gpu.prims.size(), (long long)gpu.mats.size(),
                   (long long)gpu.textures.size(), (long long)gpu.nodes.size(),
                   (long long)gpu.skins.size(), (long long)gpu.anims.size(),
                   (long long)n_morph, (long long)n_targets, ms);
        if (n_morph > 0)
            std::print("runtime: gltf morph '{}' primitives={} targets={}\n",
                       path, (long long)n_morph, (long long)n_targets);
    }
    gltf_models[path] = std::move(gpu);
    return &gltf_models[path];
}

/* ---- glTF 片段 / 蒙皮 / morph 解析与采样（plans/22 §6）----
 * 资产是派生文本 JSON（.skel.json / .clip.json），按 VFS 路径缓存；失败也缓存（failed=true），
 * 成功/失败都只告警一次。装配期预解析（setup_gltf_assets）→ 模拟帧内只读缓存。 */

/* 读组件里的 string[] 字段（反射 container 算子；用于 scn.AnimationPlayer.clips）。 */
static std::vector<std::string> read_string_array(const mq_field_desc* f, void* field) {
    std::vector<std::string> out;
    if (!f || f->kind != MQ_TK_ARRAY_DYN || !f->container_size || !f->container_elem) return out;
    const uint32_t n = f->container_size(field);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const std::string* s = (const std::string*)f->container_elem(field, i);
        if (s && !s->empty()) out.push_back(*s);
    }
    return out;
}

bool Engine::load_skeleton(const std::string& path) {
    auto it = skeletons.find(path);
    if (it != skeletons.end()) return !it->second.failed;
    Skeleton sk;
    sk.name = path;
    sk.failed = true;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data) {
        std::print(stderr, "runtime: skeleton 未找到 '{}'\n", path);
        skeletons[path] = std::move(sk);
        return false;
    }
    mq_json* dom = mq_json_parse((const char*)data, n);
    mq_vfs_free(data);
    if (!dom) {
        std::print(stderr, "runtime: skeleton json 解析失败 '{}'\n", path);
        skeletons[path] = std::move(sk);
        return false;
    }
    const mq_json* jn = mq_json_get(dom, "joint_names");
    const mq_json* jb = mq_json_get(dom, "inverse_bind");
    const uint32_t nj = mq_json_len(jn);
    const uint32_t nb = mq_json_len(jb);
    bool ok = nj > 0 && nj == nb;
    if (!ok)
        std::print(stderr, "runtime: skeleton '{}' 无效（joint_names={} inverse_bind={}）\n", path, nj, nb);
    if (ok) {
        sk.joint_names.reserve(nj);
        for (uint32_t i = 0; i < nj; ++i) {
            const char* nm = mq_json_str(mq_json_at(jn, i), "");
            sk.joint_names.emplace_back(nm ? nm : "");
        }
        sk.inverse_bind.resize((size_t)nj * 16);
        for (uint32_t i = 0; i < nj && ok; ++i) {
            const mq_json* row = mq_json_at(jb, i);
            if (mq_json_len(row) != 16) { ok = false; break; }
            for (uint32_t k = 0; k < 16; ++k)
                sk.inverse_bind[(size_t)i * 16 + k] = (float)mq_json_num(mq_json_at(row, k), 0.0);
        }
        if (!ok) sk.inverse_bind.clear();
    }
    const char* nm = mq_json_str(mq_json_get(dom, "name"), nullptr);
    if (nm && *nm) sk.name = nm;
    mq_json_free(dom);
    if (!ok) {
        skeletons[path] = std::move(sk);
        return false;
    }
    sk.failed = false;
    const size_t joints = sk.joint_names.size();
    skeletons[path] = std::move(sk);
    if (render3d_info)
        std::print("runtime: skeleton '{}' joints={}\n", path, (long long)joints);
    return true;
}

bool Engine::load_gclip(const std::string& path) {
    auto it = gclips.find(path);
    if (it != gclips.end()) return !it->second.failed;
    GClip clip;
    clip.name = path;
    clip.failed = true;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data) {
        std::print(stderr, "runtime: clip 未找到 '{}'\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    mq_json* dom = mq_json_parse((const char*)data, n);
    mq_vfs_free(data);
    if (!dom) {
        std::print(stderr, "runtime: clip json 解析失败 '{}'\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    clip.duration = (float)mq_json_num(mq_json_get(dom, "duration"), 0.0);
    const mq_json* jtracks = mq_json_get(dom, "tracks");
    const uint32_t nt = mq_json_len(jtracks);
    float max_time = 0.0F;
    for (uint32_t ti = 0; ti < nt; ++ti) {
        const mq_json* jt = mq_json_at(jtracks, ti);
        const char* node = mq_json_str(mq_json_get(jt, "node"), "");
        const char* pathname = mq_json_str(mq_json_get(jt, "path"), "");
        if (!node || !*node || !pathname) continue;
        int tp = -1;
        if (std::strcmp(pathname, "translation") == 0) tp = 0;
        else if (std::strcmp(pathname, "rotation") == 0) tp = 1;
        else if (std::strcmp(pathname, "scale") == 0) tp = 2;
        else if (std::strcmp(pathname, "weights") == 0) tp = 3;
        if (tp < 0) continue;                          /* 未知通道（pointer 等）跳过 */
        GClipTrack tr;
        tr.node = node;
        tr.path = tp;
        const char* interp = mq_json_str(mq_json_get(jt, "interpolation"), "LINEAR");
        tr.step = interp && std::strcmp(interp, "STEP") == 0;
        const mq_json* jtimes = mq_json_get(jt, "times");
        const mq_json* jvals = mq_json_get(jt, "values");
        const uint32_t nk = mq_json_len(jtimes);
        const uint32_t nv = mq_json_len(jvals);
        if (nk == 0 || nk != nv) {                     /* 关键帧/值数量不一致：跳过该轨道 */
            std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 关键帧数不符（{} vs {}），跳过\n",
                       path, tr.node, pathname, nk, nv);
            continue;
        }
        const uint32_t comps = mq_json_len(mq_json_at(jvals, 0));
        if (comps == 0 || (tp == 1 && comps != 4) ||
            ((tp == 0 || tp == 2) && comps != 3)) {
            std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 分量数 {} 不符，跳过\n",
                       path, tr.node, pathname, comps);
            continue;
        }
        tr.comps = comps;
        tr.times.resize(nk);
        tr.values.resize((size_t)nk * comps);
        bool ok = true;
        for (uint32_t k = 0; k < nk && ok; ++k) {
            tr.times[k] = (float)mq_json_num(mq_json_at(jtimes, k), 0.0);
            const mq_json* row = mq_json_at(jvals, k);
            if (mq_json_len(row) != comps) { ok = false; break; }
            for (uint32_t c = 0; c < comps; ++c)
                tr.values[(size_t)k * comps + c] = (float)mq_json_num(mq_json_at(row, c), 0.0);
        }
        if (!ok) { std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 值行宽不符，跳过\n", path, tr.node, pathname); continue; }
        if (!tr.times.empty() && tr.times.back() > max_time) max_time = tr.times.back();
        clip.tracks.push_back(std::move(tr));
    }
    const char* nm = mq_json_str(mq_json_get(dom, "name"), nullptr);
    if (nm && *nm) clip.name = nm;
    mq_json_free(dom);
    if (clip.tracks.empty()) {
        std::print(stderr, "runtime: clip '{}' 无可采样轨道\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    if (clip.duration <= 0.0F) clip.duration = max_time;   /* 缺省由关键帧推导 */
    clip.failed = false;
    const int ntracks = (int)clip.tracks.size();
    const float dur = clip.duration;
    gclips[path] = std::move(clip);
    if (render3d_info)
        std::print("runtime: clip '{}' tracks={} duration={:.3f}\n", path, ntracks, (double)dur);
    return true;
}

/* 采样一条轨道：STEP 取前关键帧；LINEAR 线性（rotation 用四元数 slerp）。 */
static void sample_gclip_track(const GClipTrack& tr, float time, std::vector<float>& out) {
    out.assign(tr.comps, 0.0F);
    const uint32_t nk = (uint32_t)tr.times.size();
    if (nk == 0 || tr.comps == 0) return;
    if (time <= tr.times[0]) {
        std::memcpy(out.data(), tr.values.data(), sizeof(float) * tr.comps);
        return;
    }
    if (time >= tr.times[nk - 1]) {
        std::memcpy(out.data(), tr.values.data() + (size_t)(nk - 1) * tr.comps,
                    sizeof(float) * tr.comps);
        return;
    }
    uint32_t k = 0;
    while (k + 1 < nk && tr.times[k + 1] <= time) ++k;
    const float* a = tr.values.data() + (size_t)k * tr.comps;
    if (tr.step) {
        std::memcpy(out.data(), a, sizeof(float) * tr.comps);
        return;
    }
    const float* b = tr.values.data() + (size_t)(k + 1) * tr.comps;
    const float span = tr.times[k + 1] - tr.times[k];
    float alpha = span > 0.0F ? (time - tr.times[k]) / span : 0.0F;
    if (alpha < 0.0F) alpha = 0.0F;
    if (alpha > 1.0F) alpha = 1.0F;
    if (tr.path == 1 && tr.comps == 4) {
        /* quat：dot<0 先翻转；dot>0.9995 用 nlerp（近似共线避免除零）；否则 slerp */
        float qb[4] = { b[0], b[1], b[2], b[3] };
        float dot = a[0] * qb[0] + a[1] * qb[1] + a[2] * qb[2] + a[3] * qb[3];
        if (dot < 0.0F) { dot = -dot; for (int i = 0; i < 4; ++i) qb[i] = -qb[i]; }
        float q[4];
        if (dot > 0.9995F) {
            for (int i = 0; i < 4; ++i) q[i] = a[i] + (qb[i] - a[i]) * alpha;
        } else {
            const float theta = std::acos(std::min(1.0F, dot));
            const float s = std::sin(theta);
            const float wa = std::sin((1.0F - alpha) * theta) / s;
            const float wb = std::sin(alpha * theta) / s;
            for (int i = 0; i < 4; ++i) q[i] = a[i] * wa + qb[i] * wb;
        }
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (ql > 0.0F) { const float inv = 1.0F / ql; for (int i = 0; i < 4; ++i) q[i] *= inv; }
        std::memcpy(out.data(), q, sizeof q);
        return;
    }
    for (uint32_t c = 0; c < tr.comps; ++c) out[c] = a[c] + (b[c] - a[c]) * alpha;
}

/* 按权重 CPU 重算 morph 顶点并上传（rapi->update_mesh）。权重没变不应调用（见 extract_3d）。 */
bool Engine::apply_morph(const std::string& path, const std::vector<float>& weights) {
    auto it = morph_meshes.find(path);
    if (it == morph_meshes.end()) return false;
    MorphMesh& mm = it->second;
    if (mm.mesh < 0 || mm.targets.empty() || mm.base.empty()) return false;
    if (!rapi || !rself || !rapi->update_mesh) {
        if (!mm.warned_no_update) {
            mm.warned_no_update = true;
            std::print(stderr, "runtime: 渲染器不支持 update_mesh，morph 跳过 '{}'\n", path);
        }
        return false;
    }
    std::vector<uint8_t> out = mm.base;
    for (uint32_t vi = 0; vi < mm.vertex_count; ++vi) {
        float* pos = (float*)(out.data() + (size_t)vi * mm.stride);
        float* nrm = pos + 3;
        bool normal_touched = false;
        for (size_t ti = 0; ti < mm.targets.size(); ++ti) {
            const float w = ti < weights.size() ? weights[ti] : 0.0F;
            if (w == 0.0F) continue;
            const MorphTarget& tgt = mm.targets[ti];
            const size_t o = (size_t)vi * 3;
            if (o + 2 < tgt.positions.size()) {
                pos[0] += w * tgt.positions[o];
                pos[1] += w * tgt.positions[o + 1];
                pos[2] += w * tgt.positions[o + 2];
            }
            if (o + 2 < tgt.normals.size()) {
                nrm[0] += w * tgt.normals[o];
                nrm[1] += w * tgt.normals[o + 1];
                nrm[2] += w * tgt.normals[o + 2];
                normal_touched = true;
            }
        }
        if (normal_touched) {
            const float len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            if (len2 > 1.0e-12F) {
                const float inv = 1.0F / std::sqrt(len2);
                nrm[0] *= inv; nrm[1] *= inv; nrm[2] *= inv;
            }
        }
    }
    if (rapi->update_mesh(rself, mm.mesh, out.data(), (uint64_t)out.size()) != 0) {
        if (!mm.warned_no_update) {
            mm.warned_no_update = true;
            std::print(stderr, "runtime: update_mesh 失败，morph 跳过 '{}'\n", path);
        }
        return false;
    }
    mm.applied_weights = weights;
    return true;
}

/* 装配期预解析 glTF 运行时资源（plans/22 §6.1）：实体名表 → 骨架/片段/morph 网格。
 * 装配期做 IO 合法（不在模拟帧内，docs/10 D7）；失败只告警一次（缓存 failed）。 */
void Engine::setup_gltf_assets() {
    read_gltf_env();
    entity_by_name.clear();
    entity_morph_mesh.clear();
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    /* 名字表：唯一名 → 最小 id（重名只告警一次；动画轨道/骨架关节按名字引用实体） */
    int dup = 0;
    std::string dup_example;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e || e->name.empty()) continue;
        auto [nit, inserted] = entity_by_name.emplace(e->name, id);
        (void)nit;
        if (!inserted) { ++dup; if (dup_example.empty()) dup_example = e->name; }
    }
    if (dup > 0)
        std::print(stderr, "runtime: 实体重名 {} 处（例如 '{}'）；按名字解析取最小 id\n",
                   dup, dup_example);
    int n_skin = 0, n_clip = 0, n_morph = 0, n_gltf = 0;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* dmesh = nullptr;
        if (auto* mb = comp_by_name(*this, id, "render.MeshRenderer", &dmesh)) {
            const std::string* mp = read_str(dmesh, mb->data(), "mesh_path");
            if (mp && !mp->empty()) {
                if (gltf::is_gltf_path(*mp)) {
                    if (auto* gm = load_gltf_model(*mp); gm && !gm->failed) {
                        gltf_state_ensure(id, *mp, *gm);
                        ++n_gltf;
                    }
                } else {
                    (void)load_mesh(*mp);                     /* 缓存；失败只告警一次 */
                    auto mit = morph_meshes.find(*mp);
                    if (mit != morph_meshes.end() && mit->second.mesh >= 0 && !mit->second.targets.empty()) {
                        entity_morph_mesh[id] = *mp;
                        ++n_morph;
                    }
                }
            }
            const std::string* sp = read_str(dmesh, mb->data(), "skin_path");
            if (sp && !sp->empty()) {
                ++n_skin;
                (void)load_skeleton(*sp);
            }
        }
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            const auto* ap = (d && (d->role & MQ_ROLE_ANIM_PLAYER))
                                 ? as_component<scn::AnimationPlayer>(d, blob.data()) : nullptr;
            if (!ap) continue;
            for (const std::string& cp : ap->clips) {
                ++n_clip;
                (void)load_gclip(cp);
            }
        }
    }
    gltf_dynamic = false;
    for (const auto& [gp, gm] : gltf_models) {
        (void)gp;
        if (gm.failed) continue;
        if (!gm.anims.empty() || !gm.skins.empty()) gltf_dynamic = true;
        for (const auto& pr : gm.prims)
            if (!pr.morph_targets.empty()) gltf_dynamic = true;
    }
    if (!gltf_states.empty()) gltf_anim_step(0.0F);
    if (render3d_info || n_skin > 0 || n_clip > 0 || n_morph > 0 || n_gltf > 0)
        std::print("runtime: gltf assets skins={} clips={} morph_meshes={} runtime_gltf={} names={}\n",
                   n_skin, n_clip, n_morph, n_gltf, (long long)entity_by_name.size());
}

void Engine::read_gltf_env() {
    if (gltf_env_read) return;
    gltf_env_read = true;
    auto flag = [](const char* name, bool def) {
        const char* v = std::getenv(name);
        if (!v || !*v) return def;
        return !(v[0] == '0' || v[0] == 'o' || v[0] == 'O' ||
                 v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
    };
    anim_enabled = flag("MAQUE_ANIM", true);
    skin_enabled = flag("MAQUE_SKIN", true);
    morph_enabled = flag("MAQUE_MORPH", true);
    render3d_info = std::getenv("MAQUE_RENDER3D_INFO") != nullptr;
}

/* glTF 片段采样（plans/22 §6.2）：仅处理 clips 非空的 scn.AnimationPlayer（旧 clip 通路不变）。
 * 推进播放头 → 采样（纯函数）→ 写 Engine::anim_pose（pos/rot/scale）与 morph 权重覆盖。 */
void Engine::anim_step_gclip(int entity, const mq_type_desc* d, std::vector<uint8_t>& blob, float dt) {
    auto* ap = as_component_mut<scn::AnimationPlayer>(d, blob.data());
    if (!ap || ap->clips.empty()) return;
    const int32_t ci = ap->clip_index;
    const std::string& clip_path = ap->clips[(ci >= 0 && (size_t)ci < ap->clips.size()) ? (size_t)ci : 0];
    if (!load_gclip(clip_path)) return;
    auto cit = gclips.find(clip_path);
    if (cit == gclips.end() || cit->second.failed || cit->second.tracks.empty()) return;
    const GClip& clip = cit->second;
    const bool playing = ap->playing;
    const bool loop = ap->loop;
    const float speed = ap->speed;
    float time = ap->time;
    if (playing) time += dt * speed;
    if (clip.duration > 0.0F) {
        if (loop) {
            /* 取模（fmod 而非反复相减：退化片段 duration 极小时不会卡住；确定性纯函数） */
            time = std::fmod(time, clip.duration);
            if (time < 0.0F) time += clip.duration;
        } else if (time > clip.duration) {
            time = clip.duration;
        } else if (time < 0.0F) {
            time = 0.0F;
        }
    }
    ap->time = time;
    ap->duration = clip.duration;
    if (!anim_enabled) return;                    /* MAQUE_ANIM=0：保持 bind pose（不写派生姿态） */
    std::vector<float> v;
    for (const GClipTrack& tr : clip.tracks) {
        auto hit = entity_by_name.find(tr.node);
        if (hit == entity_by_name.end()) continue;
        const int te = hit->second;
        sample_gclip_track(tr, time, v);
        if (tr.path == 0 && v.size() >= 3) {
            AnimPose& p = anim_pose[te];
            p.has_pos = true;
            p.pos = math::Vec3{ v[0], v[1], v[2] };
        } else if (tr.path == 1 && v.size() >= 4) {
            AnimPose& p = anim_pose[te];
            p.has_rot = true;
            std::memcpy(p.rot, v.data(), sizeof p.rot);
        } else if (tr.path == 2 && v.size() >= 3) {
            AnimPose& p = anim_pose[te];
            p.has_scale = true;
            p.scale = math::Vec3{ v[0], v[1], v[2] };
        } else if (tr.path == 3) {
            /* weights：写到该节点（含整棵子树）下所有 morph mesh 实体的权重覆盖 */
            std::vector<int> stack2{ te };
            while (!stack2.empty()) {
                const int cur = stack2.back();
                stack2.pop_back();
                if (entity_morph_mesh.find(cur) != entity_morph_mesh.end())
                    morph_weight_override[cur] = v;
                if (SceneNode* cn = scene->at(cur))
                    for (int k2 : cn->children) stack2.push_back(k2);
            }
        }
    }
    if (render3d_info && !gltf_anim_info) {
        gltf_anim_info = true;
        std::print("runtime: anim entity={} clip='{}' tracks={} duration={:.3f} time={:.3f} pose={}\n",
                   entity, clip_path, (int)clip.tracks.size(), (double)clip.duration,
                   (double)time, (int)anim_pose.size());
    }
}

/* 材质资源：builtin:/ 或 assets（JSON，反射反序列化 scn.SpriteMaterial）。 */
bool Engine::load_material(const std::string& ref, scn::SpriteMaterial& out) {
    if (ref.rfind("builtin:", 0) == 0) {
        out.base_color = math::Vec4{ 1, 1, 1, 1 };
        out.texture = (ref == "builtin:/materials/checker") ? "builtin:/textures/checker"
                                                            : "builtin:/textures/white";
        return true;
    }
    void* d = nullptr; size_t n = 0;
    std::string path = "/" + ref;
    if (mq_vfs_read(vfs, path.c_str(), &d, &n) != 0) { std::print("runtime: material not found '{}'\n", ref); return false; }
    mq_json* dom = mq_json_parse((const char*)d, n);
    mq_vfs_free(d);
    if (!dom) return false;
    const mq_type_desc* mt = mq_reflect_type_by_name(reflect, "scn.SpriteMaterial");
    int rc = mt ? mq_persist_apply_json(reflect, mt, dom, &out) : 1;
    mq_json_free(dom);
    return rc == 0;
}

/* 世界 → 快照（反射驱动；进快照池避免抖动，docs/07 §8、docs/10 §4）。 */
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
/* ============================================================================
 * M2 3D 基础（plans/18-pbr-3d.md M2）：render.* 组件 → mq_renderer_frame3d
 *   · 矩阵列主序；右手系、Y 向上、相机看 -Z；WebGPU NDC z∈[0,1]
 *   · M2 光照 = 方向光 Lambert + 环境色（PBR 在 M3）；所有 mesh 共用内建 cube（M4 换资产）
 *   · 提取只读（渲染不回写模拟，ADR-0005）
 * ==========================================================================*/
static void m4_identity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0F;
}
/* 局部 TRS（Euler XYZ 弧度）→ 列主序 mat4；R = Rz * Ry * Rx（与 docs/components/02 约定一致）。 */
static void m4_from_trs(const math::Transform3D& t, float* m) {
    const float cx = std::cos(t.rotation.x), sx = std::sin(t.rotation.x);
    const float cy = std::cos(t.rotation.y), sy = std::sin(t.rotation.y);
    const float cz = std::cos(t.rotation.z), sz = std::sin(t.rotation.z);
    const float r00 = cy * cz, r10 = cy * sz, r20 = -sy;
    const float r01 = sx * sy * cz - cx * sz, r11 = sx * sy * sz + cx * cz, r21 = sx * cy;
    const float r02 = cx * sy * cz + sx * sz, r12 = cx * sy * sz - sx * cz, r22 = cx * cy;
    m[0] = r00 * t.scale.x; m[1] = r10 * t.scale.x; m[2] = r20 * t.scale.x; m[3] = 0.0F;
    m[4] = r01 * t.scale.y; m[5] = r11 * t.scale.y; m[6] = r21 * t.scale.y; m[7] = 0.0F;
    m[8] = r02 * t.scale.z; m[9] = r12 * t.scale.z; m[10] = r22 * t.scale.z; m[11] = 0.0F;
    m[12] = t.position.x; m[13] = t.position.y; m[14] = t.position.z; m[15] = 1.0F;
}
/* 透视投影（右手系，WebGPU NDC z∈[0,1]）。 */
static void m4_perspective(float fovy_rad, float aspect, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    const float f = 1.0F / std::tan(fovy_rad * 0.5F);
    m[0] = f / aspect; m[5] = f;
    m[10] = zf / (zn - zf); m[11] = -1.0F; m[14] = (zn * zf) / (zn - zf);
}
/* 正交投影（ortho_height = 垂直可见范围；右手系，WebGPU z∈[0,1]）。 */
static void m4_orthographic(float height, float aspect, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    const float hh = std::max(height * 0.5F, 1e-5F);
    const float hw = std::max(hh * aspect, 1e-5F);
    m[0] = 1.0F / hw;
    m[5] = 1.0F / hh;
    m[10] = 1.0F / (zn - zf);
    m[14] = zn / (zn - zf);
    m[15] = 1.0F;
}
/* 世界矩阵的旋转轴（列 c）：单位化后返回。列长≈1 时**不做除法** ——
 * 保证平级场景（world_m = m4_from_trs(local)，scale=1）与旧的 Euler 旋转逐字节一致；
 * 真正带缩放的节点（列长明显偏离 1）才归一化（相机/灯光忽略缩放）。 */
static void m4_axis(const float* m, int c, float out[3]) {
    float x = m[c * 4], y = m[c * 4 + 1], z = m[c * 4 + 2];
    const float len2 = x * x + y * y + z * z;
    if (std::fabs(std::sqrt(len2) - 1.0F) > 1.0e-4F) {
        const float inv = len2 > 0.0F ? 1.0F / std::sqrt(len2) : 1.0F;
        x *= inv; y *= inv; z *= inv;
    }
    out[0] = x; out[1] = y; out[2] = z;
}
/* 相机世界矩阵 → view（R^T * T(-pos)）；平级场景（scale=1）与旧 m4_view(trs) 逐字节一致。 */
static void m4_view_from_m(const float* wm, float* m) {
    float R[16];
    for (int c = 0; c < 3; ++c) m4_axis(wm, c, R + c * 4);
    R[3] = R[7] = R[11] = 0.0F;
    R[12] = R[13] = R[14] = 0.0F; R[15] = 1.0F;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m[c * 4 + r] = R[r * 4 + c];
    m[3] = 0.0F; m[7] = 0.0F; m[11] = 0.0F; m[15] = 1.0F;
    const float px = wm[12], py = wm[13], pz = wm[14];
    m[12] = -(m[0] * px + m[4] * py + m[8] * pz);
    m[13] = -(m[1] * px + m[5] * py + m[9] * pz);
    m[14] = -(m[2] * px + m[6] * py + m[10] * pz);
}
/* 四元数 (x,y,z,w) + 平移/缩放 → 列主序 mat4（glTF rotation 轨道；无万向锁）。 */
static void m4_from_quat_trs(const math::Vec3& pos, const float q_in[4], const math::Vec3& scale, float* m) {
    float q[4] = { q_in[0], q_in[1], q_in[2], q_in[3] };
    const float qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (qlen > 0.0F) { const float inv = 1.0F / qlen; for (int i = 0; i < 4; ++i) q[i] *= inv; }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1.0F - 2.0F * (yy + zz)) * scale.x; m[1] = (2.0F * (xy + wz)) * scale.x;
    m[2] = (2.0F * (xz - wy)) * scale.x; m[3] = 0.0F;
    m[4] = (2.0F * (xy - wz)) * scale.y; m[5] = (1.0F - 2.0F * (xx + zz)) * scale.y;
    m[6] = (2.0F * (yz + wx)) * scale.y; m[7] = 0.0F;
    m[8] = (2.0F * (xz + wy)) * scale.z; m[9] = (2.0F * (yz - wx)) * scale.z;
    m[10] = (1.0F - 2.0F * (xx + yy)) * scale.z; m[11] = 0.0F;
    m[12] = pos.x; m[13] = pos.y; m[14] = pos.z; m[15] = 1.0F;
}
static void m4_mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                           a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

/* glTF 节点局部 TRS（四元数）→ 列主序 mat4；T * R * S（glTF 规范）。 */
static void m4_from_qtrs(const float t[3], const float q[4], const float s[3], float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1.0F - 2.0F * (yy + zz)) * s[0];
    m[1] = (2.0F * (xy + wz)) * s[0];
    m[2] = (2.0F * (xz - wy)) * s[0];
    m[3] = 0.0F;
    m[4] = (2.0F * (xy - wz)) * s[1];
    m[5] = (1.0F - 2.0F * (xx + zz)) * s[1];
    m[6] = (2.0F * (yz + wx)) * s[1];
    m[7] = 0.0F;
    m[8] = (2.0F * (xz + wy)) * s[2];
    m[9] = (2.0F * (yz - wx)) * s[2];
    m[10] = (1.0F - 2.0F * (xx + yy)) * s[2];
    m[11] = 0.0F;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0F;
}

/* 动画通道采样：out 长度 = ch.comps（weights 通道可 >4）。 */
template <typename Ch>
static void sample_gltf_channel(const Ch& ch, float time, std::vector<float>& out) {
    const size_t n = ch.times.size();
    out.assign((size_t)ch.comps, 0.0F);
    if (n == 0 || ch.comps <= 0 || ch.values.empty()) return;
    const int comps = ch.comps;
    const int groups = ch.interp == 2 ? 3 : 1;
    const size_t fcount = (size_t)comps * (size_t)groups;
    auto val = [&](size_t key, int grp, int c) -> float {
        const size_t o = key * fcount + (size_t)grp * (size_t)comps + (size_t)c;
        return o < ch.values.size() ? ch.values[o] : 0.0F;
    };
    size_t i = 0;
    if (time <= ch.times[0]) {
        i = 0;
        if (ch.interp == 0 || n == 1) {
            for (int c = 0; c < comps; ++c) out[c] = val(0, ch.interp == 2 ? 1 : 0, c);
            return;
        }
    } else if (time >= ch.times[n - 1]) {
        i = n - 1;
        if (ch.interp == 0 || n == 1) {
            for (int c = 0; c < comps; ++c) out[c] = val(i, ch.interp == 2 ? 1 : 0, c);
            return;
        }
    } else {
        while (i + 1 < n && ch.times[i + 1] <= time) ++i;
    }
    if (ch.interp == 0) {
        for (int c = 0; c < comps; ++c) out[c] = val(i, 0, c);
        return;
    }
    const size_t j = i + 1 < n ? i + 1 : i;
    const float t0 = ch.times[i];
    const float t1 = ch.times[j];
    const float dt = t1 - t0;
    const float u = dt > 0.0F ? std::clamp((time - t0) / dt, 0.0F, 1.0F) : 0.0F;
    if (ch.interp == 1) {
        for (int c = 0; c < comps; ++c)
            out[c] = val(i, 0, c) + (val(j, 0, c) - val(i, 0, c)) * u;
        return;
    }
    /* CUBICSPLINE：key = (in-tangent, value, out-tangent)，切线按秒缩放。 */
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2.0F * u3 - 3.0F * u2 + 1.0F;
    const float h10 = u3 - 2.0F * u2 + u;
    const float h01 = -2.0F * u3 + 3.0F * u2;
    const float h11 = u3 - u2;
    for (int c = 0; c < comps; ++c) {
        const float p0 = val(i, 1, c);
        const float p1 = val(j, 1, c);
        const float m0 = dt * val(i, 2, c);
        const float m1 = dt * val(j, 0, c);
        out[c] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
    }
}

bool Engine::apply_gltf_morph(int entity, GltfGpuModel& gm, size_t prim_idx,
                              const std::vector<float>& weights) {
    (void)entity;
    if (prim_idx >= gm.prims.size()) return false;
    GltfGpuModel::Prim& pr = gm.prims[prim_idx];
    if (pr.mesh < 0 || pr.morph_targets.empty() || pr.base.empty()) return false;
    if (!rapi || !rself || !rapi->update_mesh) {
        if (!pr.warned_update) {
            pr.warned_update = true;
            std::print(stderr, "runtime: 渲染器不支持 update_mesh，glTF morph 跳过\n");
        }
        return false;
    }
    std::vector<uint8_t> out = pr.base;
    const uint32_t row_floats = pr.layout >= 2 ? (pr.layout == 3 ? 14u : 12u)
                                               : (pr.layout == 1 ? 14u : 12u);
    for (uint32_t vi = 0; vi < pr.vertex_count; ++vi) {
        float* pos = (float*)(out.data() + (size_t)vi * pr.stride);
        if ((size_t)(vi * row_floats + 2) >= pr.base.size() / sizeof(float)) break;
        float* nrm = pos + 3;
        bool normal_touched = false;
        for (size_t ti = 0; ti < pr.morph_targets.size(); ++ti) {
            const float w = ti < weights.size() ? weights[ti] : 0.0F;
            if (w == 0.0F) continue;
            const GltfGpuModel::MorphTarget& tgt = pr.morph_targets[ti];
            const size_t o = (size_t)vi * 3;
            if (o + 2 < tgt.positions.size()) {
                pos[0] += w * tgt.positions[o];
                pos[1] += w * tgt.positions[o + 1];
                pos[2] += w * tgt.positions[o + 2];
            }
            if (o + 2 < tgt.normals.size()) {
                nrm[0] += w * tgt.normals[o];
                nrm[1] += w * tgt.normals[o + 1];
                nrm[2] += w * tgt.normals[o + 2];
                normal_touched = true;
            }
        }
        if (normal_touched) {
            const float len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            if (len2 > 1.0e-12F) {
                const float inv = 1.0F / std::sqrt(len2);
                nrm[0] *= inv; nrm[1] *= inv; nrm[2] *= inv;
            }
        }
    }
    if (rapi->update_mesh(rself, pr.mesh, out.data(), (uint64_t)out.size()) != 0) {
        if (!pr.warned_update) {
            pr.warned_update = true;
            std::print(stderr, "runtime: glTF update_mesh 失败，morph 跳过\n");
        }
        return false;
    }
    pr.applied_weights = weights;
    return true;
}

void Engine::gltf_state_ensure(int entity, const std::string& path, const GltfGpuModel& gm) {
    if (gltf_states.find(entity) != gltf_states.end()) return;
    GltfEntityState st;
    st.path = path;
    const size_t n = gm.nodes.size();
    st.node_local.assign(n * 10, 0.0F);
    st.node_animated.assign(n, 0);
    st.node_world.assign(n * 16, 0.0F);
    st.node_weights.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& nd = gm.nodes[i];
        float* L = &st.node_local[i * 10];
        for (int k = 0; k < 3; ++k) { L[k] = nd.t[k]; L[7 + k] = nd.s[k]; }
        for (int k = 0; k < 4; ++k) L[3 + k] = nd.r[k];
        float lm[16];
        if (nd.has_matrix) std::memcpy(lm, nd.m, sizeof lm);
        else m4_from_qtrs(nd.t, nd.r, nd.s, lm);
        if (nd.parent >= 0 && (size_t)nd.parent < i)
            m4_mul(&st.node_world[(size_t)nd.parent * 16], lm, &st.node_world[i * 16]);
        else
            std::memcpy(&st.node_world[i * 16], lm, sizeof lm);
    }
    st.palettes.resize(gm.skins.size());
    for (size_t si = 0; si < gm.skins.size(); ++si) {
        const auto& sk = gm.skins[si];
        st.palettes[si].assign(sk.joints.size() * 16, 0.0F);
        for (size_t ji = 0; ji < sk.joints.size(); ++ji) {
            const int jn = sk.joints[ji];
            if (jn < 0 || (size_t)jn >= n) continue;
            const float* ibm = sk.inverse_bind.data() + ji * 16;
            m4_mul(&st.node_world[(size_t)jn * 16], ibm, &st.palettes[si][ji * 16]);
        }
    }
    st.morph_weights.resize(gm.prims.size());
    for (size_t pi = 0; pi < gm.prims.size(); ++pi)
        st.morph_weights[pi] = gm.prims[pi].default_weights;
    if (!gm.anims.empty()) {
        st.duration = gm.anims.front().duration;
        st.clip = 0;
    }
    st.inited = true;
    gltf_states[entity] = std::move(st);
}

void Engine::gltf_anim_step(float dt) {
    for (auto& kv : gltf_states) {
        const int entity = kv.first;
        GltfEntityState& st = kv.second;
        auto mit = gltf_models.find(st.path);
        if (mit == gltf_models.end() || mit->second.failed) continue;
        GltfGpuModel& gm = mit->second;
        const mq_type_desc* md = nullptr;
        std::vector<uint8_t>* mb = comp_by_name(*this, entity, "render.MeshRenderer", &md);
        void* mbase = mb ? mb->data() : nullptr;
        if (md && mbase) {
            const std::string* clip_name = read_str(md, mbase, "clip");
            if (clip_name && !clip_name->empty()) {
                for (size_t ai = 0; ai < gm.anims.size(); ++ai) {
                    if (gm.anims[ai].name == *clip_name) { st.clip = (int)ai; break; }
                }
            }
            const float speed = read_num(md, mbase, "speed", 1.0F);
            const bool playing = read_num(md, mbase, "playing", 1.0F) >= 0.5F;
            const bool loop = read_num(md, mbase, "loop", 1.0F) >= 0.5F;
            if (!gm.anims.empty() && st.clip >= 0 && (size_t)st.clip < gm.anims.size())
                st.duration = gm.anims[(size_t)st.clip].duration;
            if (playing && !gm.anims.empty()) {
                st.time += dt * speed;
                if (loop && st.duration > 0.0F) {
                    st.time = std::fmod(st.time, st.duration);
                    if (st.time < 0.0F) st.time += st.duration;
                } else if (st.time < 0.0F) {
                    st.time = 0.0F;
                } else if (st.duration > 0.0F && st.time > st.duration) {
                    st.time = st.duration;
                }
            }
            write_num(md, mbase, "time", (double)st.time);
            write_num(md, mbase, "duration", (double)st.duration);
        }
        const size_t n = gm.nodes.size();
        const bool anim_on = anim_enabled && !gm.anims.empty() && st.clip >= 0 &&
                             (size_t)st.clip < gm.anims.size();
        auto apply_pointer = [](GltfGpuModel& model, int mi, int field,
                                const std::vector<float>& v) {
            if (mi < 0 || (size_t)mi >= model.mats.size() || v.empty()) return;
            auto& m = model.mats[(size_t)mi];
            const float a = v[0];
            const float b = v.size() > 1 ? v[1] : 0.0F;
            const float c = v.size() > 2 ? v[2] : 0.0F;
            const float d = v.size() > 3 ? v[3] : 0.0F;
            switch (field) {
            case gltf::PF_BASE_COLOR:
                m.base_color[0] = a; m.base_color[1] = b; m.base_color[2] = c; m.base_color[3] = d;
                break;
            case gltf::PF_METALLIC: m.metallic = a; break;
            case gltf::PF_ROUGHNESS: m.roughness = a; break;
            case gltf::PF_EMISSIVE: m.emissive[0] = a; m.emissive[1] = b; m.emissive[2] = c; break;
            case gltf::PF_EMISSIVE_STRENGTH: m.emissive_intensity = a; break;
            case gltf::PF_ALPHA_CUTOFF: m.alpha_cutoff = a; break;
            case gltf::PF_CLEARCOAT: m.clearcoat = a; break;
            case gltf::PF_CLEARCOAT_ROUGHNESS: m.clearcoat_roughness = a; break;
            case gltf::PF_SHEEN_COLOR: m.sheen_color[0] = a; m.sheen_color[1] = b; m.sheen_color[2] = c; break;
            case gltf::PF_SHEEN_ROUGHNESS: m.sheen_roughness = a; break;
            case gltf::PF_IOR: m.ior = a; break;
            case gltf::PF_TRANSMISSION:
                m.anisotropy_rotation = a;
                if (a > 0.001F) m.flags2 |= 0x100000;
                break;
            case gltf::PF_VOLUME_THICKNESS: m.volume_thickness = a; break;
            case gltf::PF_ATTENUATION_DISTANCE: m.attenuation_distance = a; break;
            case gltf::PF_DIFFUSE_TRANSMISSION:
                m.diffuse_transmission_factor = a;
                if (a > 0.001F) m.flags2 |= 0x1000000;
                break;
            case gltf::PF_SPECULAR: m.specular_intensity = a; break;
            case gltf::PF_IRIDESCENCE: m.iridescence = a; break;
            case gltf::PF_ANISOTROPY: m.anisotropy = a; break;
            case gltf::PF_DISPERSION: m.dispersion = a; break;
            default: break;
            }
        };
        for (size_t i = 0; i < n; ++i) {
            const auto& nd = gm.nodes[i];
            float* L = &st.node_local[i * 10];
            for (int k = 0; k < 3; ++k) { L[k] = nd.t[k]; L[7 + k] = nd.s[k]; }
            for (int k = 0; k < 4; ++k) L[k + 3] = nd.r[k];
            st.node_animated[i] = 0;
            st.node_weights[i].clear();
        }
        if (anim_on) {
            const auto& anim = gm.anims[(size_t)st.clip];
            std::vector<float> vals;
            for (const auto& ch : anim.channels) {
                if (ch.path == 4) {
                    sample_gltf_channel(ch, st.time, vals);
                    apply_pointer(gm, ch.pointer_material, ch.pointer_field, vals);
                    continue;
                }
                if (ch.node < 0 || (size_t)ch.node >= n) continue;
                float* L = &st.node_local[(size_t)ch.node * 10];
                if (ch.path == 0 || ch.path == 1 || ch.path == 2) {
                    sample_gltf_channel(ch, st.time, vals);
                    if (ch.path == 0) {
                        for (int k = 0; k < 3 && k < ch.comps; ++k) L[k] = vals[(size_t)k];
                    } else if (ch.path == 1) {
                        for (int k = 0; k < 4 && k < ch.comps; ++k) L[k + 3] = vals[(size_t)k];
                        const float qx = L[3], qy = L[4], qz = L[5], qw = L[6];
                        const float ql = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
                        if (ql > 1.0e-8F) { L[3] /= ql; L[4] /= ql; L[5] /= ql; L[6] /= ql; }
                    } else {
                        for (int k = 0; k < 3 && k < ch.comps; ++k) L[7 + k] = vals[(size_t)k];
                    }
                    st.node_animated[(size_t)ch.node] = 1;
                } else if (ch.path == 3) {
                    sample_gltf_channel(ch, st.time, st.node_weights[(size_t)ch.node]);
                }
            }
        }
        for (int i : gm.order) {
            if (i < 0 || (size_t)i >= n) continue;
            const auto& nd = gm.nodes[(size_t)i];
            float lm[16];
            if (nd.has_matrix) std::memcpy(lm, nd.m, sizeof lm);
            else m4_from_qtrs(&st.node_local[(size_t)i * 10], &st.node_local[(size_t)i * 10 + 3],
                              &st.node_local[(size_t)i * 10 + 7], lm);
            if (nd.parent >= 0)
                m4_mul(&st.node_world[(size_t)nd.parent * 16], lm, &st.node_world[(size_t)i * 16]);
            else
                std::memcpy(&st.node_world[(size_t)i * 16], lm, sizeof lm);
        }
        if (skin_enabled) {
            for (size_t si = 0; si < gm.skins.size(); ++si) {
                const auto& sk = gm.skins[si];
                auto& pal = st.palettes[si];
                pal.assign(sk.joints.size() * 16, 0.0F);
                for (size_t ji = 0; ji < sk.joints.size(); ++ji) {
                    const int jn = sk.joints[ji];
                    if (jn < 0 || (size_t)jn >= n) continue;
                    m4_mul(&st.node_world[(size_t)jn * 16], sk.inverse_bind.data() + ji * 16,
                           &pal[ji * 16]);
                }
            }
        } else {
            for (auto& pal : st.palettes) {
                for (size_t ji = 0; ji + 16 <= pal.size(); ji += 16) {
                    m4_identity(&pal[ji]);
                }
            }
        }
        if (morph_enabled) {
            for (size_t pi = 0; pi < gm.prims.size(); ++pi) {
                auto& pr = gm.prims[pi];
                if (pr.morph_targets.empty()) continue;
                const auto& nw = st.node_weights[(size_t)std::max(pr.node, 0)];
                std::vector<float> w = nw.size() == pr.morph_targets.size() ? nw : pr.default_weights;
                if (w == pr.applied_weights) {
                    st.morph_weights[pi] = w;
                    continue;
                }
                if (pr.morph_owner < 0) pr.morph_owner = entity;
                if (pr.morph_owner != entity) {
                    if (!pr.warned_shared) {
                        pr.warned_shared = true;
                        std::print(stderr, "runtime: glTF morph 网格被多实体共享，仅首实体 '{}' 驱动\n",
                                   pr.morph_owner);
                    }
                    continue;
                }
                apply_gltf_morph(entity, gm, pi, w);
                st.morph_weights[pi] = w;
            }
        }
        if (render3d_info && !st.logged) {
            st.logged = true;
            std::print("runtime: gltf anim entity={} '{}' clip={} time={:.3f}/{:.3f} "
                       "channels={} skins={} morph={}\n",
                       entity, st.path, st.clip, (double)st.time, (double)st.duration,
                       gm.anims.empty() ? 0 : (long long)gm.anims[(size_t)std::clamp(st.clip, 0, (int)gm.anims.size() - 1)].channels.size(),
                       (long long)gm.skins.size(),
                       (long long)std::count_if(gm.prims.begin(), gm.prims.end(),
                                                [](const GltfGpuModel::Prim& p) { return !p.morph_targets.empty(); }));
        }
    }
}
static void read_color(const mq_type_desc* d, const void* base, const char* field, math::Color* out) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (f) std::memcpy(out, field_ptr(f, (const uint8_t*)base), sizeof *out);
}

/* 内建 cube：24 顶点（每面 4 个，法线独立）/ 36 索引；48B 规范布局（pos3|normal3|tangent4|uv2）。
 * M2 用它验证 depth/相机/提取/直呈链路；M4 由 glTF mesh 资产取代。 */
static void build_cube(std::vector<float>& verts, std::vector<uint32_t>& indices) {
    const float s = 0.6F;
    struct Face { float n[3]; float p[4][3]; };
    static const Face faces[6] = {
        { { 0, 0, 1 }, { { -s, -s, s }, { s, -s, s }, { s, s, s }, { -s, s, s } } },
        { { 0, 0, -1 }, { { s, -s, -s }, { -s, -s, -s }, { -s, s, -s }, { s, s, -s } } },
        { { 1, 0, 0 }, { { s, -s, s }, { s, -s, -s }, { s, s, -s }, { s, s, s } } },
        { { -1, 0, 0 }, { { -s, -s, -s }, { -s, -s, s }, { -s, s, s }, { -s, s, -s } } },
        { { 0, 1, 0 }, { { -s, s, s }, { s, s, s }, { s, s, -s }, { -s, s, -s } } },
        { { 0, -1, 0 }, { { -s, -s, -s }, { s, -s, -s }, { s, -s, s }, { -s, -s, s } } },
    };
    verts.clear();
    indices.clear();
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = (uint32_t)verts.size() / 12;
        for (int i = 0; i < 4; ++i) {
            /* 每面 4 角 UV：0=(-s,-s) 1=(s,-s) 2=(s,s) 3=(-s,s)（贴图 PBR 用；M5 法线贴图同 UV） */
            const float u = (i == 0 || i == 3) ? 0.0F : 1.0F;
            const float v = (i >= 2) ? 1.0F : 0.0F;
            verts.insert(verts.end(), { faces[f].p[i][0], faces[f].p[i][1], faces[f].p[i][2],
                                        faces[f].n[0], faces[f].n[1], faces[f].n[2],
                                        1.0F, 0.0F, 0.0F, 1.0F, u, v });
        }
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

Engine::RenderTextureState* Engine::render_texture_for(int entity) {
    for (auto& rt : render_textures) if (rt.entity == entity) return &rt;
    return nullptr;
}
const Engine::RenderTextureState* Engine::render_texture_for(int entity) const {
    for (const auto& rt : render_textures) if (rt.entity == entity) return &rt;
    return nullptr;
}

/* MSAA 样本数解析：主目标与每张相机离屏纹理走同一条链，所以"整帧开 4x"与"只有小地图开 4x"
 * 是同一套语义，不存在第二套优先级。
 *   · 更专门的开关赢：RT 自己看 opt_rt_msaa，再退到全局 opt_msaa / MAQUE_MSAA；
 *   · 显式覆盖之下，才是目标自己的字段（render.RenderTexture.msaa_samples）与场景字段；
 *   · 返回 1 或 4；gfx 侧 4x 附件创建失败会自行回退 1x（打印一次，不静默）。 */
int Engine::resolve_msaa(int target_override, bool is_render_texture) {
    if (!msaa_env_loaded) {
        msaa_env_loaded = true;
        if (const char* msaa_env = std::getenv("MAQUE_MSAA")) {
            msaa_env_set = true;
            msaa_env_value = std::atoi(msaa_env);
        }
    }
    int samples = scene ? scene->desc.msaa_samples : 0;
    if (target_override > 0) samples = target_override;
    if (msaa_env_set) samples = msaa_env_value;
    if (opt_msaa > 0) samples = opt_msaa;
    if (is_render_texture && opt_rt_msaa > 0) samples = opt_rt_msaa;
    return samples >= 4 ? 4 : 1;
}

void Engine::destroy_render_textures() {
    for (auto& rt : render_textures) {
        auto erase_key = [&](const std::string& key) {
            auto it = tex_cache.find(key);
            if (it != tex_cache.end() && it->second == rt.handle) tex_cache.erase(it);
        };
        erase_key(rt.uri);
        const int owner = entity_instance.count(rt.entity) ? entity_instance[rt.entity] : 0;
        if (owner == 0) erase_key("rendertexture://" + rt.name);
        if (rt.handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt.handle);
    }
    render_textures.clear();
    camera_captures.clear();
}

void Engine::setup_render_textures() {
    destroy_render_textures();
    sync_render_textures();
}

void Engine::sync_render_textures() {
    if (!scene || !reflect) return;
    const mq_type_desc* rtd = mq_reflect_type_by_name(reflect, "render.RenderTexture");
    if (!rtd) return;
    const bool supported = MQ_RENDERER_HAS(rapi, create_render_texture) &&
                           MQ_RENDERER_HAS(rapi, configure_render_texture) &&
                           MQ_RENDERER_HAS(rapi, render_to_texture) &&
                           MQ_RENDERER_HAS(rapi, read_texture);
    if (!supported) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::print(stderr, "runtime: renderer 不支持 RenderTexture ABI（需要 create/configure/render/read）\n");
        }
    }
    std::vector<int> ids;
    for (const auto& [id, e] : scene->nodes)
        if (e.comps.find(rtd->type_id) != e.comps.end()) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::unordered_set<int> live(ids.begin(), ids.end());

    /* 先清理已销毁节点/已移除组件，且从 tex_cache 擦除别名，避免句柄重复释放。 */
    for (size_t i = 0; i < render_textures.size();) {
        RenderTextureState& rt = render_textures[i];
        if (live.count(rt.entity)) { ++i; continue; }
        auto erase_key = [&](const std::string& key) {
            auto it = tex_cache.find(key);
            if (it != tex_cache.end() && it->second == rt.handle) tex_cache.erase(it);
        };
        erase_key(rt.uri);
        const int owner = entity_instance.count(rt.entity) ? entity_instance[rt.entity] : 0;
        if (owner == 0) erase_key("rendertexture://" + rt.name);
        if (rt.handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt.handle);
        render_textures.erase(render_textures.begin() + (ptrdiff_t)i);
    }

    std::unordered_set<std::string> used;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        std::vector<uint8_t>& blob = e->comps[rtd->type_id];
        const mq_type_desc* dummy = nullptr;
        const bool has3d_camera = comp_by_name(*this, id, "render.Camera3D", &dummy) != nullptr;
        const bool has2d_camera = comp_by_name(*this, id, "render.Camera2D", &dummy) != nullptr ||
                                  comp_by_name(*this, id, "scn.Camera", &dummy) != nullptr;
        auto* rtc = as_component_mut<render::RenderTexture>(rtd, blob.data());
        if (!rtc) continue;
        std::string name = rtc->name.empty() ? e->name : rtc->name;
        if (name.empty()) name = "camera-" + std::to_string(id);
        const int owner = entity_instance.count(id) ? entity_instance[id] : 0;
        const std::string uri = "rendertexture://" + std::to_string(owner) + "/" + name;
        const bool valid_target = (has3d_camera || has2d_camera) && supported;
        const bool duplicate = valid_target && !used.insert(uri).second;
        RenderTextureState* rt = render_texture_for(id);

        auto write_transients = [&](const std::string& texture, bool ready, uint64_t rendered) {
            rtc->texture = texture;
            rtc->ready = ready;
            rtc->last_frame = (uint32_t)rendered;
        };
        if (duplicate || (!has3d_camera && !has2d_camera) || !supported) {
            if (duplicate)
                std::print(stderr, "runtime: RenderTexture '{}' 在实例 {} 内重名（entity={} 跳过）\n", name, owner, id);
            else if (!has3d_camera && !has2d_camera)
                std::print(stderr, "runtime: RenderTexture entity={} 必须与 scn.Camera/render.Camera2D/Camera3D 同节点\n", id);
            if (rt) {
                auto erase_key = [&](const std::string& key) {
                    auto it = tex_cache.find(key);
                    if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                };
                erase_key(rt->uri);
                erase_key("rendertexture://" + rt->name);
                if (rt->handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt->handle);
                render_textures.erase(std::remove_if(render_textures.begin(), render_textures.end(),
                                                     [&](const RenderTextureState& s) { return s.entity == id; }),
                                      render_textures.end());
            }
            write_transients("", false, 0);
            continue;
        }

        const int32_t requested_w = (int32_t)rtc->width;
        const int32_t requested_h = (int32_t)rtc->height;
        const int32_t width = std::max(1, requested_w > 0 ? requested_w : W);
        const int32_t height = std::max(1, requested_h > 0 ? requested_h : H);
        const int32_t filter = (int32_t)rtc->filter;
        const math::Color cc = rtc->clear_color;
        mq_renderer_render_texture_desc desc{};
        desc.struct_size = sizeof desc;
        desc.width = width; desc.height = height; desc.pixel_format = 0;
        desc.filter = filter; desc.wrap_u = 1; desc.wrap_v = 1;
        desc.clear_color[0] = cc.r; desc.clear_color[1] = cc.g;
        desc.clear_color[2] = cc.b; desc.clear_color[3] = cc.a;

        if (!rt) {
            int32_t handle = -1;
            if (rapi->create_render_texture(rself, &desc, &handle) != 0 || handle < 0) {
                std::print(stderr, "runtime: RenderTexture '{}' 创建失败（{}x{}）\n", uri, width, height);
                write_transients("", false, 0);
                continue;
            }
            RenderTextureState state;
            state.entity = id; state.handle = handle; state.width = width; state.height = height;
            state.filter = filter; state.name = name; state.uri = uri; state.is_3d = has3d_camera;
            render_textures.push_back(std::move(state));
            rt = &render_textures.back();
            std::print("runtime: RenderTexture '{}' {}x{} handle={} camera={}\n",
                       uri, width, height, (long long)handle, has3d_camera ? "3d" : "2d");
        } else {
            if (rt->uri != uri) {
                auto it = tex_cache.find(rt->uri);
                if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                if (owner == 0) {
                    const std::string old_short = "rendertexture://" + rt->name;
                    it = tex_cache.find(old_short);
                    if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                }
                rt->uri = uri; rt->name = name;
            }
            if (rt->width != width || rt->height != height || rt->filter != filter) {
                if (rapi->configure_render_texture(rself, rt->handle, &desc) == 0) {
                    rt->width = width; rt->height = height; rt->filter = filter;
                    rt->dirty = true; rt->ready = false; rt->capture_pixels.clear();
                } else {
                    std::print(stderr, "runtime: RenderTexture '{}' 重配 {}x{} 失败，保留旧目标 {}x{}\n",
                               uri, width, height, rt->width, rt->height);
                }
            }
            if (rt->is_3d != has3d_camera) rt->dirty = true;
            rt->is_3d = has3d_camera;
        }
        rt->update_mode = (int32_t)rtc->update_mode;
        /* 目标自己的 MSAA 变化不改纹理本身（多采样只是 pass 的临时附件），
         * 但要重画一次，否则 OnDemand 目标会留着旧样本数的画面。 */
        const int32_t rt_msaa = (int32_t)rtc->msaa_samples;
        if (rt->msaa_samples != rt_msaa) rt->dirty = true;
        rt->msaa_samples = rt_msaa;
        const bool include_ui = rtc->include_ui;
        const bool enabled = rtc->enabled;
        if (rt->include_ui != include_ui || (!rt->enabled && enabled)) rt->dirty = true;
        rt->include_ui = include_ui;
        rt->enabled = enabled;
        tex_cache[rt->uri] = rt->handle;
        if (owner == 0) tex_cache["rendertexture://" + rt->name] = rt->handle;
        write_transients(rt->uri, rt->ready, rt->last_frame);
    }
    std::sort(render_textures.begin(), render_textures.end(),
              [](const RenderTextureState& a, const RenderTextureState& b) { return a.entity < b.entity; });
}

void Engine::setup_3d() {
    has_3d = false;
    if (!scene) return;
    int n_cam = 0, n_mesh = 0, n_light = 0;
    for (auto& kv : scene->nodes) {
        const mq_type_desc* d = nullptr;
        if (comp_by_name(*this, kv.first, "render.Camera3D", &d)) ++n_cam;
        if (comp_by_name(*this, kv.first, "render.MeshRenderer", &d)) ++n_mesh;
        if (comp_by_name(*this, kv.first, "render.Light", &d)) ++n_light;
    }
    if (!n_cam && !n_mesh) return;
    has_3d = true;
    if (cube_mesh < 0 && rapi && rself && rapi->create_mesh) {
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        build_cube(verts, indices);
        mq_renderer_mesh_gpu md{};
        md.vertices = verts.data();
        md.vertex_bytes = (uint64_t)verts.size() * sizeof(float);
        md.vertex_stride = 48;
        md.vertex_layout = 0;
        md.indices = indices.data();
        md.index_count = (uint32_t)indices.size();
        md.bounds_min[0] = md.bounds_min[1] = md.bounds_min[2] = -0.6F;
        md.bounds_max[0] = md.bounds_max[1] = md.bounds_max[2] = 0.6F;
        int32_t id = -1;
        if (rapi->create_mesh(rself, &md, &id) == 0) {
            cube_mesh = id;
            mesh_stats[id] = { (uint32_t)(verts.size() / 12), (uint32_t)indices.size() };
        }
    }
    std::print("runtime: 3d scene cams={} meshes={} lights={} cube_mesh={}\n",
               n_cam, n_mesh, n_light, (long long)cube_mesh);
    /* plans/22 §6.1：实体名表 / 骨架 / 片段 / morph 网格预解析（装配期 IO，不在模拟帧内） */
    setup_gltf_assets();
}

bool Engine::make_camera_view2d(int entity, int width, int height, mq_renderer_view& view,
                                uint32_t* layer_mask) {
    SceneNode* e = scene ? scene->at(entity) : nullptr;
    if (!e || !node_active_effective(*this, entity)) return false;
    if (layer_mask) *layer_mask = 0xFFFFFFFFu;
    view = mq_renderer_view{};
    view.vw = (float)width; view.vh = (float)height;
    const mq_type_desc* d = nullptr;
    if (auto* b = comp_by_name(*this, entity, "render.Camera2D", &d); b && d) {
        const auto* cam = as_component<render::Camera2D>(d, b->data());
        if (!cam || !cam->active) return false;
        const math::Vec2 zoom = cam->zoom;
        const math::Vec2 offset = cam->offset;
        const math::Rect viewport = cam->viewport;
        if (layer_mask) *layer_mask = cam->layer_mask;
        const float zx = std::max(zoom.x, 1e-5F);
        const float zy = std::max(zoom.y, 1e-5F);
        const float nx = std::clamp(viewport.x, 0.0F, 1.0F);
        const float ny = std::clamp(viewport.y, 0.0F, 1.0F);
        const float nw = std::clamp(viewport.width, 0.0F, 1.0F - nx);
        const float nh = std::clamp(viewport.height, 0.0F, 1.0F - ny);
        if (nw <= 0.0F || nh <= 0.0F) return false;
        view.vx = nx * width; view.vy = ny * height;
        view.vw = std::max(1.0F, nw * width); view.vh = std::max(1.0F, nh * height);
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, entity, angle, scale, opacity);
        (void)scale; (void)opacity;
        const float camera_x = e->world_x + offset.x;
        const float camera_y = (float)H - (e->world_y + offset.y);
        view.cx = camera_x;
        view.cy = camera_y;
        view.pan = 0.0F;
        view.eye = 0.0F;
        view.zoom = zx;
        view.zoom_y = zy;
        view.rot = angle * 3.14159265358979323846F / 180.0F;
        return true;
    }
    if (auto* b = comp_by_name(*this, entity, "scn.Camera", &d); b && d) {
        if (read_num(d, b->data(), "active", 1.0F) < 0.5F) return false;
        const float x = read_num(d, b->data(), "x", e->world_x);
        const float y = read_num(d, b->data(), "y", e->world_y);
        view.cx = x;
        view.cy = (float)H - y;
        view.zoom = std::max(read_num(d, b->data(), "zoom", 1.0F), 1e-5F);
        view.zoom_y = view.zoom;
        view.rot = read_num(d, b->data(), "rotation", 0.0F);
        return true;
    }
    return false;
}

void Engine::fill_frame_environment(mq_renderer_frame3d& fr) {
    fr.env = mq_renderer_environment{};
    fr.env.sky_texture = -1;
    fr.env.env_texture = -1;
    fr.env.env_irradiance_texture = -1;
    fr.env.env_cubemap = -1;
    fr.env.env_cubemap_irradiance = -1;
    fr.env.sky_mode = 1;
    if (!scene) return;
    float env_col[3] = { scene->desc.env_color.r, scene->desc.env_color.g, scene->desc.env_color.b };
    float sky_col[3] = { scene->desc.env_sky_color.r, scene->desc.env_sky_color.g, scene->desc.env_sky_color.b };
    float ground_col[3] = { scene->desc.env_ground_color.r, scene->desc.env_ground_color.g, scene->desc.env_ground_color.b };
    const bool sky_zero = sky_col[0] == 0.0F && sky_col[1] == 0.0F && sky_col[2] == 0.0F;
    const bool ground_zero = ground_col[0] == 0.0F && ground_col[1] == 0.0F && ground_col[2] == 0.0F;
    for (int i = 0; i < 3; ++i) {
        if (sky_zero) sky_col[i] = env_col[i] * 1.15F;
        if (ground_zero) ground_col[i] = env_col[i] * 0.45F;
        fr.env.env_color[i] = env_col[i];
        fr.env.env_sky_color[i] = sky_col[i];
        fr.env.env_ground_color[i] = ground_col[i];
    }
    fr.env.env_color[3] = fr.env.env_sky_color[3] = fr.env.env_ground_color[3] = 1.0F;
    fr.env.intensity = scene->desc.env_intensity;
    fr.env.env_highlight = scene->desc.env_highlight;
    fr.env.env_rotation = scene->desc.env_rotation * 3.14159265F / 180.0F;

    std::string env_path = scene->desc.env_texture;
    const char* env_force = std::getenv("MAQUE_ENV_IBL");
    if (env_force && env_force[0] == '1') env_path = "builtin:/textures/studio_env";
    if (env_force && env_force[0] == '0') env_path.clear();
    if (!env_path.empty()) load_env_textures(env_path, fr.env.env_texture, fr.env.env_irradiance_texture);
    if (!scene->desc.env_cubemap.empty())
        load_env_cubemap(scene->desc.env_cubemap, fr.env.env_cubemap, fr.env.env_cubemap_irradiance);
    if (fr.env.env_cubemap >= 0) fr.env.sky_mode = scene->desc.env_background ? 2 : 1;
    else if (fr.env.env_texture >= 0 && scene->desc.env_background) fr.env.sky_mode = 3;
}

bool Engine::read_camera_texture(RenderTextureState& rt, bool refresh) {
    if (!rt.ready || rt.handle < 0 || !MQ_RENDERER_HAS(rapi, read_texture)) return false;
    if (!refresh && !rt.capture_pixels.empty()) return true;
    rt.capture_pixels.assign((size_t)rt.width * (size_t)rt.height * 4u, 0);
    const int32_t rc = rapi->read_texture(rself, rt.handle, rt.capture_pixels.data(),
                                          (uint32_t)rt.capture_pixels.size());
    if (rc != 0) { rt.capture_pixels.clear(); return false; }
    rt.capture_frame = rt.last_frame;
    return true;
}

bool Engine::save_camera_texture(int entity, const std::string& path) {
    RenderTextureState* rt = render_texture_for(entity);
    if (!rt || !rt->ready || rt->handle < 0 || path.empty() || !MQ_RENDERER_HAS(rapi, read_texture)) return false;
    std::vector<uint8_t> pixels((size_t)rt->width * (size_t)rt->height * 4u, 0);
    if (rapi->read_texture(rself, rt->handle, pixels.data(), (uint32_t)pixels.size()) != 0) return false;
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    const int rc = png ? mq_image_write_png(path.c_str(), (uint32_t)rt->width, (uint32_t)rt->height,
                                             pixels.data(), (uint32_t)rt->width * 4u)
                       : (write_ppm(path, rt->width, rt->height, pixels) ? 0 : 1);
    if (rc == 0)
        std::print("runtime: camera shot '{}' {}x{} camera={} frame={}\n",
                   path, rt->width, rt->height, entity, (unsigned long long)rt->last_frame);
    else
        std::print(stderr, "runtime: camera shot 写入失败 '{}' camera={}\n", path, entity);
    return rc == 0;
}

uint64_t Engine::compute_scene_visual_hash() const {
    if (!scene) return 0;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        const SceneNode* e = scene->at(id);
        if (!e) continue;
        mix(&id, sizeof id);
        mix(&e->parent, sizeof e->parent);
        mix(&e->active, sizeof e->active);
        mix(&e->scale, sizeof e->scale);
        mix(&e->angle, sizeof e->angle);
        mix(&e->opacity, sizeof e->opacity);
        mix(&e->z_index, sizeof e->z_index);
        mix(&e->world_x, sizeof e->world_x);
        mix(&e->world_y, sizeof e->world_y);
        mix(e->world_m, sizeof e->world_m);
        mix(e->name.data(), e->name.size());
        std::vector<uint32_t> tids;
        tids.reserve(e->comps.size());
        for (const auto& [tid, blob] : e->comps) { (void)blob; tids.push_back(tid); }
        std::sort(tids.begin(), tids.end());
        for (uint32_t tid : tids) {
            mix(&tid, sizeof tid);
            auto it = e->comps.find(tid);
            if (it != e->comps.end()) {
                mix(it->second.data(), it->second.size());
            }
        }
    }
    return h;
}

void Engine::render_camera_textures(const std::vector<mq_renderer_item>& draws,
                                    const std::vector<mq_renderer_mesh>& meshes) {
    if (render_textures.empty() || !MQ_RENDERER_HAS(rapi, render_to_texture)) return;
    /* 截图请求会强制 OnDemand 目标在本帧更新。 */
    for (const auto& req : camera_captures)
        if (RenderTextureState* rt = render_texture_for(req.entity)) rt->dirty = true;
    /* 静态场景：所有 RT 都干净时，整帧 RT pass（含各自的 3D 提取/深度/命令缓冲）直接跳过。 */
    if (scene_visual_unchanged) {
        bool any_rt_dirty = false;
        for (const auto& rt : render_textures)
            if (rt.enabled && (rt.dirty || !rt.ready)) { any_rt_dirty = true; break; }
        if (!any_rt_dirty) return;
    }

    /* 依赖排序：A 的可见 2D 输入采样 B，则 B 先于 A。同一环内按实体 id 继续，语义为
     * “较早目标读环中较晚目标的上一帧，较晚目标可读较早目标的本帧”，确定且无同-pass 反馈。 */
    std::unordered_map<int32_t, size_t> by_handle;
    for (size_t i = 0; i < render_textures.size(); ++i) by_handle[render_textures[i].handle] = i;
    std::vector<std::vector<size_t>> edges(render_textures.size());
    std::vector<int> indegree(render_textures.size(), 0);
    for (size_t i = 0; i < render_textures.size(); ++i) {
        std::unordered_set<size_t> deps;
        uint32_t dep_2d_mask = 0xFFFFFFFFu;
        uint32_t dep_3d_mask = 0xFFFFFFFFu;
        if (!render_textures[i].is_3d) {
            const mq_type_desc* camera_desc = nullptr;
            if (auto* camera = comp_by_name(*this, render_textures[i].entity, "render.Camera2D", &camera_desc);
                camera && camera_desc) {
                if (const auto* cam = as_component<render::Camera2D>(camera_desc, camera->data()))
                    dep_2d_mask = cam->layer_mask;
            }
        } else {
            const mq_type_desc* camera_desc = nullptr;
            if (auto* camera = comp_by_name(*this, render_textures[i].entity, "render.Camera3D", &camera_desc);
                camera && camera_desc) {
                if (const auto* cam = as_component<render::Camera3D>(camera_desc, camera->data()))
                    dep_3d_mask = cam->culling_mask;
            }
        }
        auto consider = [&](int32_t texture, int layer) {
            if (texture < 0 || layer < 0 || layer > (render_textures[i].include_ui ? 1 : 0)) return;
            if (!render_textures[i].is_3d && layer < 32 && (dep_2d_mask & (1u << layer)) == 0) return;
            auto it = by_handle.find(texture);
            if (it != by_handle.end() && it->second != i) deps.insert(it->second);
        };
        if (!render_textures[i].is_3d) {
            for (const auto& item : draws) consider(item.texture, item.layer);
            for (const auto& mesh : meshes) consider(mesh.texture, mesh.layer);
        } else {
            if (render_textures[i].include_ui) {
                for (const auto& item : draws) if (item.layer == 1) consider(item.texture, item.layer);
                for (const auto& mesh : meshes) if (mesh.layer == 1) consider(mesh.texture, mesh.layer);
            }
            /* 3D 材质在 per-camera extract_3d 中生成；这里直接扫描 PBR 的字符串贴图槽，
             * 把 rendertexture:// 引用提前加入 pass 依赖。 */
            for (const auto& [eid, ent] : scene->nodes) for (const auto& [tid, blob] : ent.comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || std::strcmp(d->name, "asset.PbrMaterial") != 0) continue;
                const mq_type_desc* mesh_desc = nullptr;
                auto* mesh_blob = comp_by_name(*this, eid, "render.MeshRenderer", &mesh_desc);
                const auto* mesh = (mesh_blob && mesh_desc)
                    ? as_component<render::MeshRenderer>(mesh_desc, mesh_blob->data()) : nullptr;
                if (!mesh || !node_active_effective(*this, eid) || !mesh->visible) continue;
                const uint32_t mesh_mask = mesh->culling_mask;
                if ((mesh_mask & dep_3d_mask) == 0) continue;
                for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                    const mq_field_desc& f = d->fields[fi];
                    if (f.kind != MQ_TK_STRING || std::string(f.name).find("texture") == std::string::npos) continue;
                    const std::string& ref = *(const std::string*)(blob.data() + f.offset);
                    auto hit = tex_cache.find(scoped_texture_ref(eid, ref));
                    if (hit != tex_cache.end()) consider(hit->second, 0);
                }
            }
        }
        for (size_t dep : deps) { edges[dep].push_back(i); ++indegree[i]; }
    }
    std::vector<size_t> order;
    for (size_t produced = 0; produced < render_textures.size();) {
        size_t pick = render_textures.size();
        for (size_t i = 0; i < render_textures.size(); ++i)
            if (indegree[i] == 0 && std::find(order.begin(), order.end(), i) == order.end() &&
                (pick == render_textures.size() || render_textures[i].entity < render_textures[pick].entity)) pick = i;
        if (pick == render_textures.size()) break;
        order.push_back(pick); ++produced; indegree[pick] = -1;
        for (size_t to : edges[pick]) --indegree[to];
    }
    if (order.size() != render_textures.size()) {
        static bool warned_cycle = false;
        if (!warned_cycle) {
            warned_cycle = true;
            std::print(stderr, "runtime: RenderTexture 依赖含环 → 环内按相机 entity id 使用确定的上一帧/本帧顺序\n");
        }
        std::vector<size_t> rest;
        for (size_t i = 0; i < render_textures.size(); ++i)
            if (std::find(order.begin(), order.end(), i) == order.end()) rest.push_back(i);
        std::sort(rest.begin(), rest.end(), [&](size_t a, size_t b) {
            return render_textures[a].entity < render_textures[b].entity;
        });
        order.insert(order.end(), rest.begin(), rest.end());
    }

    for (size_t oi : order) {
        RenderTextureState& rt = render_textures[oi];
        const bool capture_due = std::any_of(camera_captures.begin(), camera_captures.end(),
                                             [&](const CameraCaptureRequest& r) { return r.entity == rt.entity; });
        if ((!rt.enabled && !capture_due) || !node_active_effective(*this, rt.entity)) continue;
        if (rt.update_mode != 0 && !rt.dirty) continue;
        const mq_type_desc* rtd = nullptr;
        auto* rtb = comp_by_name(*this, rt.entity, "render.RenderTexture", &rtd);
        if (!rtb || !rtd) continue;
        mq_renderer_frame3d fr{};
        fr.width = rt.width; fr.height = rt.height;
        fr.env.sky_texture = fr.env.env_texture = fr.env.env_irradiance_texture = -1;
        fr.env.env_cubemap = fr.env.env_cubemap_irradiance = -1;
        /* MSAA 逐目标解析（Engine::resolve_msaa）：F4「RenderTexture MSAA」> F4「MSAA」>
         * MAQUE_MSAA > render.RenderTexture.msaa_samples > scn.Scene.msaa_samples。
         * 2D 分支会退回 1x：多采样只作用于 3D pass，2D 图元层恒单采样（见下）。 */
        const int rt_msaa = resolve_msaa(rt.msaa_samples, true);
        fr.msaa_samples = rt_msaa;
        math::Color cc{};
        read_color(rtd, rtb->data(), "clear_color", &cc);
        fr.bg[0] = cc.r; fr.bg[1] = cc.g; fr.bg[2] = cc.b; fr.bg[3] = cc.a;

        std::vector<mq_renderer_item> target_items;
        std::vector<mq_renderer_mesh> target_meshes;
        auto keep_layer = [&](int layer) { return layer == 0 || (rt.include_ui && layer == 1); };
        if (!rt.is_3d) {
            if (rt_msaa > 1) {
                /* 别让人以为 2D 目标的 msaa 生效了：2D 图元走 render_impl，管线 sampleCount 恒 1，
                 * 边缘质量由纹理过滤决定，MSAA 对它没有任何作用（只打印一次，不逐帧刷屏）。 */
                static bool warned_2d_msaa = false;
                if (!warned_2d_msaa) {
                    warned_2d_msaa = true;
                    std::print(stderr, "runtime: RenderTexture '{}' 是 2D 相机目标 → MSAA 无效"
                                       "（多采样只作用于 3D pass；本提示只打印一次）\n", rt.uri);
                }
                fr.msaa_samples = 1;
            }
            const mq_type_desc* camera2d_desc = nullptr;
            if (auto* camera2d = comp_by_name(*this, rt.entity, "render.Camera2D", &camera2d_desc);
                camera2d && camera2d_desc) {
                math::Color camera_clear{};
                read_color(camera2d_desc, camera2d->data(), "clear_color", &camera_clear);
                fr.bg[0] = camera_clear.r; fr.bg[1] = camera_clear.g;
                fr.bg[2] = camera_clear.b; fr.bg[3] = camera_clear.a;
            }
            mq_renderer_view view{};
            uint32_t camera_mask = 0xFFFFFFFFu;
            if (!make_camera_view2d(rt.entity, rt.width, rt.height, view, &camera_mask)) continue;
            auto keep_2d = [&](int layer) {
                return keep_layer(layer) && layer >= 0 && layer < 32 && (camera_mask & (1u << layer)) != 0;
            };
            for (const auto& item : draws) if (keep_2d(item.layer)) target_items.push_back(item);
            for (const auto& mesh : meshes) if (keep_2d(mesh.layer)) target_meshes.push_back(mesh);
            /* shader 精灵是 2D 图元（layer 0）：2D 相机的 RT 也要捕获它们（docs/13）。
             * 3D RT 不捕获（它只按 include_ui 叠 UI 层；精灵属世界层 2D）。 */
            std::vector<mq_renderer_shader_sprite> target_sprites;
            extract_shader_sprites(target_sprites);
            fr.views2d = &view; fr.view2d_count = 1;
            fr.items = target_items.data(); fr.item_count = (uint32_t)target_items.size();
            fr.meshes = target_meshes.data(); fr.mesh_count = (uint32_t)target_meshes.size();
            fr.shader_sprites = target_sprites.data();
            fr.shader_sprite_count = (uint32_t)target_sprites.size();
            const int32_t rc = rapi->render_to_texture(rself, rt.handle, &fr);
            if (rc != 0) { std::print(stderr, "runtime: RenderTexture '{}' 2D render rc={}\n", rt.uri, rc); continue; }
        } else {
            std::vector<mq_renderer_view3d> v3;
            std::vector<mq_renderer_light> l3;
            std::vector<mq_renderer_pbr_material> m3;
            std::vector<mq_renderer_draw3d> d3;
            std::vector<mq_renderer_skin> s3;
            extract_3d(rt.width, rt.height, v3, l3, m3, d3, s3, rt.entity, false);
            if (v3.empty()) continue;
            if (rt.include_ui) {
                for (const auto& item : draws) if (item.layer == 1) target_items.push_back(item);
                for (const auto& mesh : meshes) if (mesh.layer == 1) target_meshes.push_back(mesh);
            }
            std::vector<mq_renderer_shader_pass> passes;
            extract_shader_passes(passes);
            fr.views = v3.data(); fr.view_count = (uint32_t)v3.size();
            fr.lights = l3.data(); fr.light_count = (uint32_t)l3.size();
            fr.materials = m3.data(); fr.material_count = (uint32_t)m3.size();
            fr.draws = d3.data(); fr.draw_count = (uint32_t)d3.size();
            fr.skins = s3.data(); fr.skin_count = (uint32_t)s3.size();
            fr.items = target_items.data(); fr.item_count = (uint32_t)target_items.size();
            fr.meshes = target_meshes.data(); fr.mesh_count = (uint32_t)target_meshes.size();
            fr.views2d = nullptr; fr.view2d_count = 0;
            fr.passes = passes.data(); fr.pass_count = (uint32_t)passes.size();
            fill_frame_environment(fr);
            const int32_t rc = rapi->render_to_texture(rself, rt.handle, &fr);
            if (rc != 0) { std::print(stderr, "runtime: RenderTexture '{}' 3D render rc={}\n", rt.uri, rc); continue; }
        }
        rt.ready = true;
        rt.dirty = false;
        rt.last_frame = frame;
        if (auto* rtc = as_component_mut<render::RenderTexture>(rtd, rtb->data())) {
            rtc->ready = true;
            rtc->last_frame = (uint32_t)frame;
        }
    }
    for (const auto& req : camera_captures) {
        RenderTextureState* rt = render_texture_for(req.entity);
        if (rt && rt->ready && rt->last_frame == frame) {
            (void)save_camera_texture(req.entity, req.path);
        } else {
            std::print(stderr, "runtime: camera shot 跳过 '{}' camera={}（本帧 RenderTexture 更新失败）\n",
                       req.path, req.entity);
        }
    }
    camera_captures.clear();
}

/* ==========================================================================
 * 场景资源全屏 shader pass（docs/13）
 *   · 资源是场景目录里的 .wgsl（相对路径、VFS 根 = 场景目录）——**不是** builtin shader；
 *   · 装配期加载/编译一次；每帧只推进 time（模拟 dt，确定性）并提交给渲染器；
 *   · 失败（文件缺失/编译错）打印明确原因并跳过，场景其余部分照常运行。
 * ==========================================================================*/
/* `render.ShaderSprite.shader` / `render.ShaderGraph.program` 是否指向多阶段 program：
 * 约定按扩展名 `.json` 判定（.wgsl = 单 pass 契约；program JSON = 多阶段图）。 */
static bool is_shader_graph_program(const std::string& path) {
    if (path.size() < 5) return false;
    std::string ext = path.substr(path.size() - 5);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".json";
}

void Engine::destroy_shader_passes() {
    if (rapi && rself && rapi->destroy_shader_pass)
        for (auto& p : loaded_shader_passes)
            if (p.handle >= 0) rapi->destroy_shader_pass(rself, p.handle);
    loaded_shader_passes.clear();
    has_shader_pass = false;
}

void Engine::setup_shader_passes() {
    destroy_shader_passes();    if (!scene || !vfs || !rapi || !rself || !rapi->create_shader_pass) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    /* 两种组件共用同一套加载/编译路径：全屏 pass 与 shader 精灵只是在 gfx 侧的绘制方式不同。 */
    auto load = [&](const char* type, bool sprite) {
        for (int id : ids) {
            const mq_type_desc* d = nullptr;
            auto* blob = comp_by_name(*this, id, type, &d);
            if (!blob || !d) continue;
            const std::string* path = read_str(d, blob->data(), "shader");
            if (!path || path->empty()) continue;
            /* `render.ShaderSprite.shader` 指向 program JSON 时是多阶段 shader 图（由
             * setup_shader_graphs 加载），不能当 WGSL 编译。 */
            if (sprite && is_shader_graph_program(*path)) continue;
            void* src = nullptr;
            size_t n = 0;
            const std::string vpath = "/" + *path;
            if (mq_vfs_read(vfs, vpath.c_str(), &src, &n) != MQ_VFS_OK || !src || n == 0) {
                std::print(stderr, "runtime: {} '{}' 未找到（相对场景目录，经 VFS 读取）\n",
                           sprite ? "shader sprite" : "shader pass", *path);
                if (src) mq_vfs_free(src);
                continue;
            }
            std::string wgsl((const char*)src, n);
            mq_vfs_free(src);
            int32_t handle = -1;
            const int32_t rc = rapi->create_shader_pass(rself, wgsl.c_str(), &handle);
            if (rc != 0 || handle < 0) {
                std::print(stderr, "runtime: {} '{}' 编译失败 rc={}"
                                   "（需 @fragment fs_main + group0 binding0 的 32B uniform，见 docs/13）\n",
                           sprite ? "shader sprite" : "shader pass", *path, (int)rc);
                continue;
            }
            loaded_shader_passes.push_back({ id, handle, sprite });
            std::print("runtime: {} '{}' loaded (entity={}, id={})\n",
                       sprite ? "shader sprite" : "shader pass", *path, id, (long long)handle);
        }
    };
    load("render.ShaderPass", false);
    load("render.ShaderSprite", true);
    has_shader_pass = !loaded_shader_passes.empty();
}

void Engine::shader_pass_step(float dt) {
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        for (const char* type : { "render.ShaderPass", "render.ShaderSprite", "render.ShaderGraph" }) {
            const mq_type_desc* d = nullptr;
            auto* blob = comp_by_name(*this, id, type, &d);
            if (!blob || !d) continue;
            if (read_num(d, blob->data(), "paused", 0.0F) >= 0.5F) continue;
            const float speed = read_num(d, blob->data(), "speed", 1.0F);
            if (speed == 0.0F) continue;
            const float time = read_num(d, blob->data(), "time", 0.0F) + dt * speed;
            if (const mq_field_desc* f = mq_reflect_field(d, "time"))
                field_set_f32(f, blob->data(), time);
        }
    }
}

void Engine::extract_shader_passes(std::vector<mq_renderer_shader_pass>& out) {
    out.clear();
    if (!scene || loaded_shader_passes.empty()) return;
    /* 指针：世界设计坐标 → 屏幕 Canvas → 渲染目标像素。 */
    const float rx = canvas.window_w > 0.0F ? (float)TW / canvas.window_w : render_scale_req;
    const float ry = canvas.window_h > 0.0F ? (float)TH / canvas.window_h : render_scale_req;
    const float mx = (canvas.offset_x + mouse_x * canvas.scale_x) * rx;
    const float my = (canvas.offset_y + ((float)H - mouse_y) * canvas.scale_y) * ry;
    for (const auto& p : loaded_shader_passes) {
        if (p.sprite) continue;                       /* 精灵走 extract_shader_sprites（2D 图层） */
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, p.entity, "render.ShaderPass", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        mq_renderer_shader_pass sp{};
        sp.shader = p.handle;
        sp.time = read_num(d, blob->data(), "time", 0.0F);
        sp.frame = (float)frame;
        sp.mouse[0] = mx;
        sp.mouse[1] = my;
        out.push_back(sp);
    }
}

/* ==========================================================================
 * 场景资源多通道 shader 图（render.ShaderGraph；docs/13 §多通道 shader）
 *
 * program JSON（路径相对场景目录，经 VFS 读取）：
 *   {
 *     "buffers": [ { "name": "a", "size": 540 }, ... ],          // RGBA32F 中间缓冲
 *     "passes": [
 *       { "name": "buffer_a", "shader": "buffer_a.wgsl", "common": "common.wgsl",
 *         "target": "a", "channels": ["self", "", "", ""] },
 *       ...,
 *       { "name": "image", "shader": "image.wgsl", "common": "common.wgsl",
 *         "target": "", "channels": ["b", "c", "", ""] }          // "" / "screen" = 主目标
 *     ]
 *   }
 *
 * 语义：
 *   · 每个 buffer 两张纹理（ping-pong）：本帧写 tex[write]、上一帧内容在 tex[1-write]；
 *     所有 pass 都写完后下一帧再交换，所以 "self"/"prev" 永远读到上一帧（自反馈）。
 *   · 同一帧内引用某个 buffer 时读它**本帧最新**的内容（前面 pass 刚写的），因此可串联；
 *     引用本 pass 的目标缓冲时读到的是上一帧内容（ping-pong 保证不会同 pass 读自己）。
 *   · target 为空 / "screen" / "main" = 主渲染目标（背景层，先于 3D/2D 绘制）。
 *   · common 非空时把 common 源码拼在每个 pass 源码之前（共享函数/常量库）。
 * ==========================================================================*/

void Engine::shader_graph_swap_targets() {
    for (auto& g : loaded_shader_graphs)
        for (auto& b : g.buffers)
            b.write ^= 1;
}

/* `render.ShaderSprite.shader` / `render.ShaderGraph.program` 是否指向多阶段 program：
 * 约定按扩展名 `.json` 判定（.wgsl = 单 pass 契约；program JSON = 多阶段图）。 */

void Engine::destroy_shader_graphs() {
    if (rapi && rself) {
        if (rapi->destroy_shader_graph_pass)
            for (auto& g : loaded_shader_graphs)
                for (auto& p : g.passes)
                    if (p.shader >= 0) rapi->destroy_shader_graph_pass(rself, p.shader);
        if (rapi->destroy_texture)
            for (auto& g : loaded_shader_graphs)
                for (auto& b : g.buffers)
                    for (int k = 0; k < 2; ++k)
                        if (b.tex[k] >= 0) rapi->destroy_texture(rself, b.tex[k]);
    }
    loaded_shader_graphs.clear();
    has_shader_graph = false;
    shader_graph_swap_frame = ~0ull;
}

void Engine::setup_shader_graphs() {
    destroy_shader_graphs();
    if (!scene || !vfs || !rapi || !rself) return;
    if (!rapi->create_shader_graph_pass || !rapi->create_shader_graph_target) return;

    auto read_text = [&](const std::string& rel, std::string& out) -> bool {
        void* src = nullptr;
        size_t n = 0;
        const std::string vpath = "/" + rel;
        if (mq_vfs_read(vfs, vpath.c_str(), &src, &n) != MQ_VFS_OK || !src || n == 0) {
            if (src) mq_vfs_free(src);
            return false;
        }
        out.assign((const char*)src, n);
        mq_vfs_free(src);
        return true;
    };

    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 加载一份 program：背景形态（render.ShaderGraph）与精灵形态（render.ShaderSprite + .json）共用。 */
    auto load_program = [&](int id, const std::string& program_path, bool sprite) {
        const std::string* path = &program_path;
        std::string text;
        if (!read_text(*path, text)) {
            std::print(stderr, "runtime: shader graph program '{}' 未找到（相对场景目录，经 VFS 读取）\n", *path);
            return;
        }
        mq_json* dom = mq_json_parse(text.data(), text.size());
        if (!dom) {
            std::print(stderr, "runtime: shader graph program '{}' JSON 解析失败\n", *path);
            return;
        }

        LoadedShaderGraph g;
        g.entity = id;
        g.program_path = *path;
        g.sprite = sprite;
        g.start_frame = frame;          /* 阶段的 frame 从 0 起算：重载后自反馈缓冲会重新初始化 */
        /* pass/common 的路径相对 **program 文件所在目录**（program 自身相对场景目录）。 */
        std::string base_dir;
        {
            const size_t slash = path->find_last_of('/');
            if (slash != std::string::npos) base_dir = path->substr(0, slash + 1);
        }
        auto resolve = [&](const std::string& rel) -> std::string {
            if (rel.empty()) return rel;
            if (rel[0] == '/') return rel.substr(1);
            return base_dir + rel;
        };

        /* 中间缓冲：每个两张纹理（ping-pong）。`size` = 正方形边长；也可以写 `width`+`height`
         * 要非方形缓冲（例如按屏幕比例的画面缓冲、光照单元网格），`size` 仍然有效且优先。 */
        const mq_json* jbufs = mq_json_get(dom, "buffers");
        const uint32_t nbuf = mq_json_len(jbufs);
        for (uint32_t i = 0; i < nbuf; ++i) {
            const mq_json* jb = mq_json_at(jbufs, i);
            const char* name = mq_json_str(mq_json_get(jb, "name"), "");
            const int32_t size = (int32_t)mq_json_num(mq_json_get(jb, "size"), 0);
            int32_t bw = size, bh = size;
            if (size <= 0) {
                bw = (int32_t)mq_json_num(mq_json_get(jb, "width"), 0);
                bh = (int32_t)mq_json_num(mq_json_get(jb, "height"), 0);
            }
            if (!name || !*name || bw <= 0 || bh <= 0) {
                std::print(stderr, "runtime: shader graph '{}' buffers[{}] 需要 name + "
                                   "(size>0 或 width>0 且 height>0)，已跳过\n", *path, i);
                continue;
            }
            ShaderGraphBuffer b;
            b.name = name;
            b.w = bw;
            b.h = bh;
            bool ok = true;
            for (int k = 0; k < 2 && ok; ++k) {
                int32_t tid = -1;
                if (rapi->create_shader_graph_target(rself, b.w, b.h, &tid) != 0 || tid < 0) {
                    std::print(stderr, "runtime: shader graph '{}' 缓冲 '{}' 创建失败（{}x{}）\n",
                               *path, b.name, b.w, b.h);
                    ok = false;
                } else {
                    b.tex[k] = tid;
                }
            }
            if (!ok) {
                for (int k = 0; k < 2; ++k)
                    if (b.tex[k] >= 0) rapi->destroy_texture(rself, b.tex[k]);
                continue;
            }
            g.buffers.push_back(std::move(b));
        }

        auto buffer_index = [&](const char* name) -> int32_t {
            if (!name || !*name) return -1;
            for (size_t bi = 0; bi < g.buffers.size(); ++bi)
                if (g.buffers[bi].name == name) return (int32_t)bi;
            return -1;
        };

        /* 阶段：shader（+ 可选 common）→ 编译；target/channels 解析成缓冲下标。 */
        const mq_json* jpasses = mq_json_get(dom, "passes");
        const uint32_t npass = mq_json_len(jpasses);
        for (uint32_t i = 0; i < npass; ++i) {
            const mq_json* jp = mq_json_at(jpasses, i);
            const char* shader = mq_json_str(mq_json_get(jp, "shader"), "");
            const char* common = mq_json_str(mq_json_get(jp, "common"), "");
            const char* target = mq_json_str(mq_json_get(jp, "target"), "");
            if (!shader || !*shader) {
                std::print(stderr, "runtime: shader graph '{}' passes[{}] 缺 shader，已跳过\n", *path, i);
                continue;
            }
            std::string src;
            if (common && *common) {
                if (!read_text(resolve(common), src)) {
                    std::print(stderr, "runtime: shader graph '{}' common '{}' 未找到\n", *path, common);
                    continue;
                }
                src += "\n";
            }
            std::string body;
            if (!read_text(resolve(shader), body)) {
                std::print(stderr, "runtime: shader graph '{}' pass '{}' 未找到\n", *path, shader);
                continue;
            }
            src += body;
            int32_t handle = -1;
            if (rapi->create_shader_graph_pass(rself, src.c_str(), &handle) != 0 || handle < 0) {
                std::print(stderr, "runtime: shader graph '{}' pass '{}' 编译失败"
                                   "（common='{}'；契约见 docs/13 §多通道 shader）\n",
                           *path, shader, (common && *common) ? common : "");
                continue;
            }
            ShaderGraphPass p;
            p.path = shader;
            p.name = mq_json_str(mq_json_get(jp, "name"), shader);
            p.shader = handle;
            if (target && *target && std::strcmp(target, "screen") != 0 && std::strcmp(target, "main") != 0) {
                p.target_buffer = buffer_index(target);
                if (p.target_buffer < 0) {
                    std::print(stderr, "runtime: shader graph '{}' pass '{}' 的输出缓冲 '{}' 不存在，已跳过\n",
                               *path, shader, target);
                    rapi->destroy_shader_graph_pass(rself, handle);
                    continue;
                }
            }
            const mq_json* jch = mq_json_get(jp, "channels");
            for (int k = 0; k < 4; ++k) {
                const char* cn = mq_json_str(mq_json_at(jch, (uint32_t)k), "");
                if (!cn || !*cn) continue;
                if (std::strcmp(cn, "self") == 0 || std::strcmp(cn, "prev") == 0) {
                    p.channels[k] = p.target_buffer;          /* 自反馈：上一帧内容（ping-pong） */
                } else {
                    p.channels[k] = buffer_index(cn);
                    if (p.channels[k] < 0)
                        std::print(stderr, "runtime: shader graph '{}' pass '{}' 的 iChannel{} '{}' 不存在（按未绑定处理）\n",
                                   *path, shader, k, cn);
                }
            }
            g.passes.push_back(std::move(p));
        }
        mq_json_free(dom);

        if (g.passes.empty()) {
            /* 全部失败：回收缓冲，保留上面的清晰报错（场景其余部分照常运行）。 */
            for (auto& b : g.buffers)
                for (int k = 0; k < 2; ++k)
                    if (b.tex[k] >= 0) rapi->destroy_texture(rself, b.tex[k]);
            return;
        }
        std::print("runtime: {} '{}' loaded (entity={}, passes={}, buffers={})\n",
                   sprite ? "shader sprite graph" : "shader graph",
                   *path, id, (long long)g.passes.size(), (long long)g.buffers.size());
        loaded_shader_graphs.push_back(std::move(g));
    };

    /* 背景形态：render.ShaderGraph.program */
    for (int id : ids) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, id, "render.ShaderGraph", &d);
        if (!blob || !d) continue;
        const std::string* path = read_str(d, blob->data(), "program");
        if (!path || path->empty()) continue;
        load_program(id, *path, false);
    }
    /* 精灵形态：render.ShaderSprite.shader 指向 program JSON（与 .wgsl 共用同一个字段）。 */
    for (int id : ids) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, id, "render.ShaderSprite", &d);
        if (!blob || !d) continue;
        const std::string* path = read_str(d, blob->data(), "shader");
        if (!path || path->empty() || !is_shader_graph_program(*path)) continue;
        load_program(id, *path, true);
    }
    has_shader_graph = !loaded_shader_graphs.empty();
}

/* 一条阶段链的提取（背景形态与精灵形态共用）：缓冲的最新纹理 + 每个阶段的目标/通道/尺寸/输入。
 * screen_w/h = 该图的"屏幕"空间（背景 = 渲染目标像素；精灵 = 精灵矩形设计像素），
 * 主目标阶段的分辨率与 screen_resolution 都取它，mouse 也必须在同一空间。 */
void Engine::build_shader_graph_stages(const LoadedShaderGraph& g, float time, float frame_value,
                                       float mx, float my, bool pressed, const float keys[4],
                                       float screen_w, float screen_h,
                                       std::vector<mq_renderer_shader_graph_pass>& out) {
    /* 帧初：每个 buffer 的最新内容 = 上一帧写的那张（tex[1-write]）。 */
    std::vector<int32_t> latest(g.buffers.size());
    for (size_t i = 0; i < g.buffers.size(); ++i)
        latest[i] = g.buffers[i].tex[1 - g.buffers[i].write];
    for (const auto& p : g.passes) {
        mq_renderer_shader_graph_pass rp{};
        rp.shader = p.shader;
        rp.frame = frame_value;
        rp.time = time;
        rp.mouse[0] = mx;
        rp.mouse[1] = my;
        rp.mouse[2] = pressed ? 1.0F : 0.0F;
        rp.mouse[3] = 0.0F;
        for (int k = 0; k < 4; ++k) rp.keys[k] = keys[k];
        rp.screen_resolution[0] = screen_w;
        rp.screen_resolution[1] = screen_h;
        if (p.target_buffer >= 0) {
            const ShaderGraphBuffer& b = g.buffers[(size_t)p.target_buffer];
            rp.target = b.tex[b.write];
            rp.resolution[0] = (float)b.w;
            rp.resolution[1] = (float)b.h;
        } else {
            rp.target = -1;
            rp.resolution[0] = screen_w;
            rp.resolution[1] = screen_h;
        }
        for (int k = 0; k < 4; ++k) {
            const int32_t bi = p.channels[k];
            rp.channels[k] = (bi >= 0 && bi < (int32_t)latest.size()) ? latest[(size_t)bi] : -1;
        }
        /* 保护：同一 pass 不能读写同一张纹理（WebGPU 校验会拒绝）。正常声明不会触发
         *（"self" 走 ping-pong 的另一张）；这里兜住"同一帧内重复写同一个缓冲"的写法。 */
        for (int k = 0; k < 4; ++k) {
            if (rp.channels[k] >= 0 && rp.channels[k] == rp.target) {
                const int32_t bi = p.channels[k];
                rp.channels[k] = g.buffers[(size_t)bi].tex[1 - g.buffers[(size_t)bi].write];
            }
        }
        out.push_back(rp);
        if (p.target_buffer >= 0) latest[(size_t)p.target_buffer] = rp.target;
    }
}

/* 每帧提取（背景形态：render.ShaderGraph.program）。 */
void Engine::extract_shader_graphs(std::vector<mq_renderer_shader_graph_pass>& out) {
    out.clear();
    if (!scene || loaded_shader_graphs.empty() || !rapi || !rself) return;
    /* ping-pong 每**模拟帧**只切换一次：同一模拟帧可能多次成帧（主目标 + RenderTexture）。 */
    if (frame != shader_graph_swap_frame) {
        shader_graph_swap_frame = frame;
        shader_graph_swap_targets();
    }
    /* 指针：世界设计坐标 → 屏幕 Canvas → 渲染目标像素（与 render.ShaderPass 同一条映射）。 */
    const float rx = canvas.window_w > 0.0F ? (float)TW / canvas.window_w : render_scale_req;
    const float ry = canvas.window_h > 0.0F ? (float)TH / canvas.window_h : render_scale_req;
    const float mx = (canvas.offset_x + mouse_x * canvas.scale_x) * rx;
    const float my = (canvas.offset_y + ((float)H - mouse_y) * canvas.scale_y) * ry;
    const bool pressed = input && mq_input_pointer_down(input, 0) != 0;
    float keys[4] = { 0.0F, 0.0F, 0.0F, 0.0F };
    if (input) {
        keys[0] = mq_input_key_down(input, MQ_KEY_BACKSPACE) ? 1.0F : 0.0F;
        keys[1] = shift_down ? 1.0F : 0.0F;
        keys[2] = mq_input_key_down(input, MQ_KEY_ENTER) ? 1.0F : 0.0F;
    }
    for (auto& g : loaded_shader_graphs) {
        if (g.sprite) continue;                  /* 精灵形态 → extract_shader_graph_sprites */
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, g.entity, "render.ShaderGraph", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        const float time = read_num(d, blob->data(), "time", 0.0F);
        build_shader_graph_stages(g, time, (float)(frame - g.start_frame),
                                  mx, my, pressed, keys, (float)TW, (float)TH, out);
    }
}

/* 多通道 shader 图的精灵形态（render.ShaderGraph 且 width/height>0）：
 * 位置/尺寸/旋转/透明度语义与 render.ShaderSprite 一致；每个精灵自带完整阶段链，
 * 主目标阶段先渲染到该精灵的离屏纹理，再由 gfx 当普通 2D 图元合成。 */
void Engine::extract_shader_graph_sprites(std::vector<mq_renderer_shader_graph_sprite>& out) {
    out.clear();
    graph_sprite_stage_scratch.clear();
    if (!scene || loaded_shader_graphs.empty() || !rapi || !rself) return;
    /* F4「2D/UI 图层」开关只隐藏游戏层（时间仍照常推进）。 */
    if (!opt_2d_enabled) return;
    if (frame != shader_graph_swap_frame) {
        shader_graph_swap_frame = frame;
        shader_graph_swap_targets();
    }
    const bool pressed = input && mq_input_pointer_down(input, 0) != 0;
    float keys[4] = { 0.0F, 0.0F, 0.0F, 0.0F };
    if (input) {
        keys[0] = mq_input_key_down(input, MQ_KEY_BACKSPACE) ? 1.0F : 0.0F;
        keys[1] = shift_down ? 1.0F : 0.0F;
        keys[2] = mq_input_key_down(input, MQ_KEY_ENTER) ? 1.0F : 0.0F;
    }
    std::vector<size_t> offsets;                  /* 每个精灵的阶段在 scratch 里的起点 */
    for (auto& g : loaded_shader_graphs) {
        if (!g.sprite) continue;                  /* 背景形态 → extract_shader_graphs */
        SceneNode* e = scene->at(g.entity);
        if (!e) continue;
        /* 精灵形态的参数（width/height/enabled/time）在 render.ShaderSprite 上。 */
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, g.entity, "render.ShaderSprite", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        float w = 0.0F, h = 0.0F;
        read_f32(d, blob->data(), "width", &w);
        read_f32(d, blob->data(), "height", &h);
        if (w <= 0.0F || h <= 0.0F) continue;
        if (!node_active_effective(*this, g.entity)) continue;
        float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
        accumulate_node_attrs(*this, g.entity, node_angle, node_scale, node_opacity);
        const float half_w = w * 0.5F * node_scale;
        const float half_h = h * 0.5F * node_scale;
        const float alpha = std::clamp(node_opacity, 0.0F, 1.0F);
        if (half_w <= 0.0F || half_h <= 0.0F || alpha <= 0.0F) continue;
        /* 指针：世界坐标 → 精灵内局部**设计像素**（原点左上）；在矩形外 → 不按下、坐标 0。 */
        const float lx = mouse_x - (e->world_x - half_w);
        const float ly = (e->world_y + half_h) - mouse_y;
        const bool inside = lx >= 0.0F && ly >= 0.0F && lx <= 2.0F * half_w && ly <= 2.0F * half_h;
        const float mx = inside ? lx : 0.0F;
        const float my = inside ? ly : 0.0F;
        const bool sprite_pressed = pressed && inside;
        const float time = read_num(d, blob->data(), "time", 0.0F);

        const size_t begin = graph_sprite_stage_scratch.size();
        build_shader_graph_stages(g, time, (float)(frame - g.start_frame),
                                  mx, my, sprite_pressed, keys,
                                  2.0F * half_w, 2.0F * half_h, graph_sprite_stage_scratch);

        mq_renderer_shader_graph_sprite s{};
        s.z = g.entity;                              /* 稳定序（与 2D 图元的 z = 实体 id 一致） */
        s.x = e->world_x;
        s.y = (float)H - e->world_y;                 /* 世界 y 向上 → 屏幕 y 向下 */
        s.half_w = half_w;
        s.half_h = half_h;
        s.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
        s.a = alpha;
        s.passes = nullptr;                          /* 阶段全部追加完后再取指针（vector 可能扩容） */
        s.pass_count = (uint32_t)(graph_sprite_stage_scratch.size() - begin);
        if (s.pass_count == 0) continue;
        out.push_back(s);
        offsets.push_back(begin);
    }
    for (size_t i = 0; i < out.size(); ++i)
        out[i].passes = graph_sprite_stage_scratch.data() + offsets[i];
}

/* shader 精灵 → 2D 图元式记录（位置/尺寸/旋转/透明度语义与 scn.Sprite 一致）。
 * 位置是**设计坐标**（中心；y 已翻到屏幕向下），尺寸含节点 scale；gfx 据此先渲染离屏纹理、
 * 再当普通带纹理矩形合成，因此画布缩放/letterbox/旋转/图层顺序都走既有 2D 通路。 */
void Engine::extract_shader_sprites(std::vector<mq_renderer_shader_sprite>& out) {
    out.clear();
    if (!scene || loaded_shader_passes.empty()) return;
    /* F4「2D/UI 图层」开关只隐藏游戏层：shader 精灵是 2D 图元，一并隐藏（时间仍照常推进）。 */
    if (!opt_2d_enabled) return;
    for (const auto& p : loaded_shader_passes) {
        if (!p.sprite) continue;
        SceneNode* e = scene->at(p.entity);
        if (!e) continue;
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, p.entity, "render.ShaderSprite", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        if (!node_active_effective(*this, p.entity)) continue;
        float w = 64.0F, h = 64.0F;
        read_f32(d, blob->data(), "width", &w);
        read_f32(d, blob->data(), "height", &h);
        if (w <= 0.0F || h <= 0.0F) continue;
        float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
        accumulate_node_attrs(*this, p.entity, node_angle, node_scale, node_opacity);
        const float half_w = w * 0.5F * node_scale;
        const float half_h = h * 0.5F * node_scale;
        const float alpha = std::clamp(node_opacity, 0.0F, 1.0F);
        if (alpha <= 0.0F) continue;
        mq_renderer_shader_sprite s{};
        s.shader = p.handle;
        s.z = p.entity;                              /* 稳定序（与 2D 图元的 z = 实体 id 一致） */
        s.x = e->world_x;
        s.y = (float)H - e->world_y;                 /* 世界 y 向上 → 屏幕 y 向下 */
        s.half_w = half_w;
        s.half_h = half_h;
        s.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
        s.a = alpha;
        s.time = read_num(d, blob->data(), "time", 0.0F);
        s.frame = (float)frame;
        /* 指针：世界坐标 → 精灵内局部**设计像素**（原点左上）。在矩形外 → 0（shader 的鼠标语义）。 */
        const float lx = mouse_x - (e->world_x - half_w);
        const float ly = (e->world_y + half_h) - mouse_y;
        if (lx >= 0.0F && ly >= 0.0F && lx <= 2.0F * half_w && ly <= 2.0F * half_h) {
            s.mouse[0] = lx;
            s.mouse[1] = ly;
        }
        out.push_back(s);
    }
}

void Engine::extract_3d(int32_t tw, int32_t th,
                        std::vector<mq_renderer_view3d>& out_views,
                        std::vector<mq_renderer_light>& out_lights,
                        std::vector<mq_renderer_pbr_material>& out_mats,
                        std::vector<mq_renderer_draw3d>& out_draws,
                        std::vector<mq_renderer_skin>& out_skins,
                        int camera_entity, bool allow_orbit) {
    out_views.clear(); out_lights.clear(); out_mats.clear(); out_draws.clear();
    out_skins.clear();
    skin_palettes.clear();                       /* 上一帧调色板已随上一帧渲染用完 */
    if (!gltf_env_read) read_gltf_env();
    if (!scene || tw <= 0 || th <= 0) return;
    /* plans/22 §6.3：每帧先由节点树 + 片段派生姿态算世界矩阵（extract 只读派生数据）。
     * 纯 shader-pass 场景（has_3d=false）没有 3D 组件，不必付这次遍历。 */
    if (has_3d) scene->update_world3d(*this);
    /* 实体 id 排序：unordered_map 迭代顺序不可依赖（确定性） */
    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 材质：M3 从**同节点的 asset.PbrMaterial 组件**读取（M4 换成共享 .material.json 资产）；
     * 无材质组件的节点用默认灰（金属度 0 / 粗糙度 0.6）。 */
    math::Transform3D cam_trs{};
    cam_trs.position = math::Vec3{ 0.0F, 1.6F, 5.0F };
    cam_trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
    float cam_wm[16];                            /* 相机世界矩阵（plans/22：由 world_m 派生 view） */
    m4_identity(cam_wm);
    bool has_cam_wm = false;
    bool has_cam = false;
    bool gltf_lights_added = false;
    bool gltf_light_logged = false;
    float cam_priority = -1.0e9F;
    float fov_y = 60.0F, ortho_height = 2.0F, near_z = 0.1F, far_z = 100.0F, exposure = 1.0F;
    int32_t projection = 0;
    int32_t clear_mode = 1;
    int32_t camera_mask = -1;
    math::Rect camera_viewport{ 0.0F, 0.0F, 1.0F, 1.0F };
    math::Color clear{ 0.06F, 0.06F, 0.09F, 1.0F };

    for (int idx : ids) {
        auto it = scene->nodes.find(idx);
        if (it == scene->nodes.end()) continue;
        const float* wm = it->second.world_m;    /* update_world3d 的派生结果（列主序） */
        const mq_type_desc* dt = nullptr;
        const mq_type_desc* dcam = nullptr;
        const mq_type_desc* dmesh = nullptr;
        const mq_type_desc* dlight = nullptr;
        auto* tb = comp_by_name(*this, idx, "scene.Transform3D", &dt);
        math::Transform3D trs{};
        trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
        if (auto* t3 = tb ? as_component<scene::Transform3D>(dt, tb->data()) : nullptr)
            trs = t3->local;
        auto* cb = comp_by_name(*this, idx, "render.Camera3D", &dcam);
        const bool rt_camera = comp_by_name(*this, idx, "render.RenderTexture", nullptr) != nullptr;
        if (auto* cam = (cb && dcam) ? as_component<render::Camera3D>(dcam, cb->data()) : nullptr;
            cam && (camera_entity < 0 || idx == camera_entity) &&
            !(camera_entity < 0 && rt_camera)) {
            if (cam->active && node_active_effective(*this, idx) &&
                (!has_cam || cam->priority > cam_priority)) {
                has_cam = true;
                cam_priority = cam->priority;
                cam_trs = trs;
                std::memcpy(cam_wm, wm, sizeof cam_wm);   /* 相机用世界矩阵（平级场景 == 旧 trs） */
                has_cam_wm = true;
                projection = (int32_t)cam->projection;
                fov_y = cam->fov_y;
                ortho_height = cam->ortho_height;
                near_z = cam->near;
                far_z = cam->far;
                exposure = cam->exposure;
                clear_mode = (int32_t)cam->clear_mode;
                camera_mask = (int32_t)cam->culling_mask;
                camera_viewport = cam->viewport;
                clear = cam->clear_color;
            }
        }
        auto* lb = comp_by_name(*this, idx, "render.Light", &dlight);
        if (auto* light = (lb && dlight) ? as_component<render::Light>(dlight, lb->data()) : nullptr) {
            /* M5b：Directional / Point / Spot 全类型（后两种用节点世界位置；方向 = 节点 -Z 轴） */
            mq_renderer_light L{};
            L.type = (int)light->type;
            L.color[0] = light->color.r; L.color[1] = light->color.g; L.color[2] = light->color.b;
            L.intensity = light->intensity;
            L.range = light->range;
            constexpr float kDeg = 3.14159265F / 180.0F;
            L.inner_cos = std::cos(light->inner_angle * kDeg);
            L.outer_cos = std::cos(light->outer_angle * kDeg);
            /* plans/22 §6.3：位置/朝向取节点世界矩阵（平级场景与旧 trs 逐字节一致） */
            float az[3];
            m4_axis(wm, 2, az);
            L.pos[0] = wm[12]; L.pos[1] = wm[13]; L.pos[2] = wm[14];
            L.dir[0] = -az[0]; L.dir[1] = -az[1]; L.dir[2] = -az[2];
            out_lights.push_back(L);
        }
        auto* mb = comp_by_name(*this, idx, "render.MeshRenderer", &dmesh);
        const auto* mesh = (mb && dmesh) ? as_component<render::MeshRenderer>(dmesh, mb->data()) : nullptr;
        const std::string* gmp = mesh ? &mesh->mesh_path : nullptr;
        if (mesh && gmp && !gmp->empty() && gltf::is_gltf_path(*gmp)) {
            if (!mesh->visible) continue;
            GltfGpuModel* gm = load_gltf_model(*gmp);
            if (gm && !gm->failed) {
                gltf_state_ensure(idx, *gmp, *gm);
                GltfEntityState& gst = gltf_states[idx];
                /* plans/24 P6：KHR_materials_variants 选择（组件 variant 名称）。 */
                int selected_variant = -1;
                if (!gm->variants.empty()) {
                    const std::string* vn = read_str(dmesh, mb->data(), "variant");
                    if (vn && !vn->empty()) {
                        for (size_t vi = 0; vi < gm->variants.size(); ++vi)
                            if (gm->variants[vi] == *vn) { selected_variant = (int)vi; break; }
                    }
                }
                /* plans/24 P5：KHR_lights_punctual → frame3d 灯光（按模型尺度归一化）。 */
                for (const GltfGpuModel::Light& gl : gm->lights) {
                    if (gl.node < 0) continue;
                    const size_t nw = (size_t)gl.node * 16u;
                    if (nw + 16u > gst.node_world.size()) continue;
                    float lm[16];
                    m4_mul(wm, &gst.node_world[nw], lm);
                    mq_renderer_light L{};
                    L.type = gl.type;
                    L.pos[0] = lm[12]; L.pos[1] = lm[13]; L.pos[2] = lm[14];
                    float laz[3];
                    m4_axis(lm, 2, laz);
                    L.dir[0] = -laz[0]; L.dir[1] = -laz[1]; L.dir[2] = -laz[2];
                    for (int k = 0; k < 3; ++k) L.color[k] = gl.color[k];
                    if (gl.type == 0) {
                        L.intensity = std::clamp(2.6F * gl.intensity, 0.0F, 20.0F);
                    } else {
                        const float dref = std::max(0.5F * gm->radius, 1e-3F);
                        L.intensity = std::clamp(2.6F * (dref * dref + 1.0F) * gl.intensity,
                                                 0.0F, 2000.0F);
                    }
                    L.range = gl.type == 0 ? 0.0F : gl.range;
                    L.inner_cos = gl.inner_cos;
                    L.outer_cos = gl.outer_cos;
                    out_lights.push_back(L);
                    gltf_lights_added = true;
                    if (render3d_info && !gltf_light_logged) {
                        gltf_light_logged = true;
                        std::print("runtime: gltf light on entity={} node={} type={} intensity={:.3f}\n",
                                   idx, gl.node, gl.type, (double)L.intensity);
                    }
                }
                if (!has_cam && !has_cam_wm && !gm->cameras.empty()) {
                    for (const auto& gc : gm->cameras) {
                        if (!gc.perspective || gc.node < 0) continue;
                        const size_t nw = (size_t)gc.node * 16u;
                        if (nw + 16u > gst.node_world.size()) continue;
                        m4_mul(wm, &gst.node_world[nw], cam_wm);
                        has_cam_wm = true;
                        fov_y = gc.yfov * 57.2957795F;
                        near_z = std::max(gc.znear, 1e-4F);
                        far_z = std::max(gc.zfar, near_z + 1.0F);
                        if (render3d_info)
                            std::print("runtime: gltf camera entity={} node={} fov={:.2f} near={:.3f} far={:.3f}\n",
                                       idx, gc.node, (double)fov_y, (double)near_z, (double)far_z);
                        break;
                    }
                }
                for (size_t pi = 0; pi < gm->prims.size(); ++pi) {
                    const GltfGpuModel::Prim& gp = gm->prims[pi];
                    int mat_index = gp.material;
                    if (selected_variant >= 0 &&
                        (size_t)selected_variant < gp.variant_materials.size() &&
                        gp.variant_materials[(size_t)selected_variant] >= 0)
                        mat_index = gp.variant_materials[(size_t)selected_variant];
                    if (gp.mesh < 0 || mat_index < 0 || (size_t)mat_index >= gm->mats.size())
                        continue;
                    mq_renderer_draw3d dr{};
                    dr.mesh = gp.mesh;
                    dr.material = (int32_t)out_mats.size();
                    out_mats.push_back(gm->mats[(size_t)mat_index]);
                    dr.skin = -1;
                    int skin_index = -1;
                    const bool skinned = gp.skin >= 0 && (size_t)gp.skin < gst.palettes.size() &&
                                         !gst.palettes[(size_t)gp.skin].empty();
                    if (skinned) {
                        const auto& pal = gst.palettes[(size_t)gp.skin];
                        skin_palettes.push_back(pal);
                        mq_renderer_skin rs{};
                        rs.palette = skin_palettes.back().data();
                        rs.joint_count = (uint32_t)(pal.size() / 16);
                        out_skins.push_back(rs);
                        skin_index = (int32_t)out_skins.size() - 1;
                        std::memcpy(dr.model, wm, sizeof dr.model);
                    } else if (gp.node >= 0 && (size_t)gp.node * 16u + 16u <= gst.node_world.size()) {
                        m4_mul(wm, &gst.node_world[(size_t)gp.node * 16u], dr.model);
                    } else {
                        std::memcpy(dr.model, wm, sizeof dr.model);
                    }
                    dr.skin = skin_index;
                    dr.flags |= 0x40000000;            /* draw.layer 是有效 culling mask（旧调用方无此位） */
                    dr.layer = -1;
                    if (const auto* m = as_component<render::MeshRenderer>(dmesh, mb->data()))
                        dr.layer = (int32_t)m->culling_mask;
                    dr.sort_key = (uint64_t)idx;
                    out_draws.push_back(dr);
                }
            }
            continue;
        }
        if (mb && dmesh && cube_mesh >= 0) {
            if (read_num(dmesh, mb->data(), "visible", 1.0F) < 0.5F) continue;
            /* M3 材质：同节点的 asset.PbrMaterial 组件（M4 起由共享 .material.json 资产引用） */
            mq_renderer_pbr_material mat{};
            for (int ti = 0; ti < 5; ++ti) mat.textures[ti] = -1;   /* -1 = 未给贴图 → 渲染器用 1×1 白 */
            for (int ti = 0; ti < 3; ++ti) mat.textures2[ti] = -1;  /* matcap / gradient / clearcoat-normal */
            for (int ti = 0; ti < 7; ++ti) mat.textures3[ti] = -1;  /* KHR 扩展 7 槽（clearcoat/sheen/iridescence/anisotropy） */
            for (int ti = 0; ti < 2; ++ti) mat.textures4[ti] = -1;  /* KHR_materials_specular 2 槽（factor/color） */
            mat.textures5[0] = -1;                                  /* KHR_materials_transmission 贴图槽（R=透射率掩码） */
            for (int ti = 0; ti < 2; ++ti) mat.textures6[ti] = -1;  /* diffuse transmission（A 乘子 / RGB sRGB 色） */
            mat.textures7[0] = -1;                                  /* KHR_materials_volume thicknessTexture.G */
            for (int ti = 0; ti < 2; ++ti) {
                mat.uv_scale[ti] = 1.0F; mat.uv_offset[ti] = 0.0F;       /* KHR_texture_transform 缺省 */
                mat.normal_uv_scale[ti] = 1.0F; mat.normal_uv_offset[ti] = 0.0F;
            }
            mat.shading_model = 0;                                  /* Standard（与旧 PbrMaterial 语义一致） */
            mat.side = 2;                                           /* Double：兼容旧 cullMode=None */
            mat.opacity = 1.0F;
            mat.emissive_intensity = 1.0F;
            mat.env_intensity = 1.0F;
            mat.normal_scale_y = 1.0F;
            mat.shininess = 30.0F;
            mat.specular_intensity = 1.0F;
            mat.ior = 1.5F;
            mat.specular[0] = mat.specular[1] = mat.specular[2] = 1.0F;
            mat.sheen_roughness = 0.5F;
            mat.iridescence_ior = 1.3F;
            mat.iridescence_thickness_min = 100.0F;
            mat.iridescence_thickness_max = 400.0F;
            mat.clearcoat_normal_scale = 1.0F;
            mat.toon_steps = 3;
            mat.depth_packing = 0;
            mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = mat.base_color[3] = 1.0F;
            mat.metallic = 0.0F;
            mat.roughness = 0.6F;
            mat.normal_scale = 1.0F;
            mat.occlusion_strength = 1.0F;
            const mq_type_desc* dmat = nullptr;
            auto* matb = comp_by_name(*this, idx, "asset.PbrMaterial", &dmat);
            if (const auto* pm = (matb && dmat)
                    ? as_component<asset::PbrMaterial>(dmat, matb->data()) : nullptr) {
                mat.base_color[0] = pm->base_color.r; mat.base_color[1] = pm->base_color.g;
                mat.base_color[2] = pm->base_color.b; mat.base_color[3] = pm->base_color.a;
                mat.emissive[0] = pm->emissive.x; mat.emissive[1] = pm->emissive.y;
                mat.emissive[2] = pm->emissive.z;
                mat.metallic = pm->metallic;
                mat.roughness = pm->roughness;
                mat.normal_scale = pm->normal_scale;
                mat.occlusion_strength = pm->occlusion_strength;
                mat.alpha_cutoff = pm->alpha_cutoff;
                mat.alpha_mode = (int32_t)pm->alpha_mode;
    /* transmission/diffuse 近似把 MASK 改成 BLEND 时保留 alpha 镂空（shader 用 flags2 bit19） */
    if (pm->alpha_mask_in_blend) mat.flags2 |= 0x80000;

                mat.flags = pm->double_sided ? 1 : 0;
                mat.clearcoat = pm->clearcoat;
                mat.clearcoat_roughness = pm->clearcoat_roughness;
                /* plans/19：多着色模型参数（缺省 = Standard 观感，旧场景零变化） */
                mat.shading_model = (int32_t)pm->shading_model;
                mat.side = (int32_t)pm->side;
                mat.opacity = pm->opacity;
                mat.emissive_intensity = pm->emissive_intensity;
                mat.env_intensity = pm->env_intensity;
                mat.normal_scale_y = pm->normal_scale_y;
                mat.specular[0] = pm->specular.x; mat.specular[1] = pm->specular.y;
                mat.specular[2] = pm->specular.z;
                mat.shininess = pm->shininess;
                mat.specular_intensity = pm->specular_intensity;
                mat.ior = pm->ior;
                mat.sheen = pm->sheen;
                mat.sheen_roughness = pm->sheen_roughness;
                mat.sheen_color[0] = pm->sheen_color.x; mat.sheen_color[1] = pm->sheen_color.y;
                mat.sheen_color[2] = pm->sheen_color.z;
                mat.iridescence = pm->iridescence;
                mat.iridescence_ior = pm->iridescence_ior;
                mat.iridescence_thickness_min = pm->iridescence_thickness_min;
                mat.iridescence_thickness_max = pm->iridescence_thickness_max;
                mat.anisotropy = pm->anisotropy;
                mat.anisotropy_rotation = pm->anisotropy_rotation;
                /* KHR_materials_transmission 近似参数：shader 用 Fresnel 视角 alpha、gfx 画两遍（背面→正面）。
                 * Draw 的 anisotropy_rotation 槽 shader 未使用，复用它传 transmission factor（避免扩 uniform）。
                 * ⚠️ 必须写在上面读取材质自身 anisotropy_rotation **之后**：否则会被无条件覆盖为 0，
                 *    shader 收到的 Fresnel 权重恒 0（玻璃退化为均匀半透"幽灵"，CompareDispersion 即此坑）。 */
                const float transmission_factor = pm->transmission_factor;
                if (transmission_factor > 0.001F) {
                    mat.anisotropy_rotation = transmission_factor;
                    mat.flags2 |= 0x100000;             /* bit20 = transmission approx */
                }
                /* KHR_materials_volume / KHR_materials_dispersion：折射厚度与色散近似参数
                 * （gfx 打包进 Draw.clearcoat.z / Draw.alpha.w，材质 uniform 仍是 288B 不扩）。
                 * 缺省 0 = 无体积折射/色散（旧材质零行为变化）。 */
                mat.volume_thickness = pm->volume_thickness;
                mat.dispersion = pm->dispersion;
                /* KHR_materials_volume：thicknessFactor/thicknessTexture 给的是 **mesh 空间** 厚度，
                 * 规范要求随节点变换缩放；attenuationDistance 是**世界空间**。gfx/shader 把它当世界
                 * 空间厚度用（Beer-Lambert 路径长 + 折射偏移），这里先乘节点世界缩放（非均匀用几何均值
                 * 近似路径长）。漏乘时小尺度资产（USDShaderBall 节点 scale=0.01）光学厚度 ×100 → 发黑。 */
                if (mat.volume_thickness > 0.0F) {
                    const float vscale = std::cbrt(std::abs(trs.scale.x * trs.scale.y * trs.scale.z));
                    if (vscale > 1.0e-6F) mat.volume_thickness *= vscale;
                }
                /* KHR_materials_diffuse_transmission：**不透明**薄壁透光（不再 alpha BLEND，
                 * 否则陶瓷/叶片会变幽灵玻璃）。factor → shader 背光透射项；flags2 bit24 标记。 */
                const float diffuse_transmission_factor = pm->diffuse_transmission_factor;
                if (diffuse_transmission_factor > 0.001F) {
                    mat.diffuse_transmission_factor = diffuse_transmission_factor;
                    mat.diffuse_transmission_color[0] = pm->diffuse_transmission_color.x;
                    mat.diffuse_transmission_color[1] = pm->diffuse_transmission_color.y;
                    mat.diffuse_transmission_color[2] = pm->diffuse_transmission_color.z;
                    mat.flags2 |= 0x1000000;            /* bit24 = diffuse transmission approx */
                }
                /* KHR_materials_volume（第 24 轮）：Beer-Lambert 吸收（attenuationColor/Distance）+
                 * thicknessTexture.G（厚度乘子）+ volume_scatter 的 multiscatterColorFactor。 */
                mat.attenuation_color[0] = pm->attenuation_color.x;
                mat.attenuation_color[1] = pm->attenuation_color.y;
                mat.attenuation_color[2] = pm->attenuation_color.z;
                mat.attenuation_distance = pm->attenuation_distance;
                mat.multiscatter_color[0] = pm->multiscatter_color.x;
                mat.multiscatter_color[1] = pm->multiscatter_color.y;
                mat.multiscatter_color[2] = pm->multiscatter_color.z;
                mat.clearcoat_normal_scale = pm->clearcoat_normal_scale;
                mat.toon_steps = pm->toon_steps;
                mat.depth_packing = (int32_t)pm->depth_packing;
                if (pm->flat_shading) mat.flags2 |= 1;
                /* KHR_texture_transform（plans/19）：基础贴图用 uv_scale/uv_offset，法线单独一组 */
                mat.uv_scale[0] = pm->uv_scale.x; mat.uv_scale[1] = pm->uv_scale.y;
                mat.uv_offset[0] = pm->uv_offset.x; mat.uv_offset[1] = pm->uv_offset.y;
                mat.normal_uv_scale[0] = pm->normal_uv_scale.x;
                mat.normal_uv_scale[1] = pm->normal_uv_scale.y;
                mat.normal_uv_offset[0] = pm->normal_uv_offset.x;
                mat.normal_uv_offset[1] = pm->normal_uv_offset.y;
                /* glTF TEXCOORD_1（多 UV 集）：uv1_mask bit0..7 = 8 个**基础**贴图槽是否用第二套 UV；
                 * 折进 flags2 bit4..11（uv1_sel），bit12..18 留给 KHR 扩展贴图 presence（见下），
                 * 故这里只取低 8 位（0xFF）——用 0x1FF 会占用 bit12。
                 * 只有 vertex_layout=1 的网格才有有效 uv1；layout0 的 vs 写 uv1=0，mask=0 时无影响。 */
                {
                    const int32_t uv1_mask = (int32_t)pm->uv1_mask;
                    mat.flags2 |= (uv1_mask & 0xFF) << 4;
                }
                /* M4：PBR 贴图槽（albedo / metallic-roughness / occlusion）；normal 在 M5（需要切线空间） */
                if (const std::string* texref = &pm->base_color_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[0] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr albedo texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->metallic_roughness_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[1] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr mr texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->occlusion_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[2] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr ao texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->normal_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[3] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr normal texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->emissive_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[4] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr emissive texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* plans/19：matcap / toon 渐变 / clearcoat 法线三个新贴图槽（flags2 位告知 shader 是否有图） */
                /* KHR_texture_transform.rotation（弧度；glTF T*R*S 顺序，shader 里先缩放再旋转再平移） */
    mat.uv_rotation = pm->uv_rotation;
    mat.normal_uv_rotation = pm->normal_uv_rotation;
    if (const std::string* texref = &pm->matcap_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[0] = tid; mat.flags2 |= 2; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: matcap texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->gradient_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_ref(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[1] = tid; mat.flags2 |= 4; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: toon gradient texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->clearcoat_normal_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[2] = tid; mat.flags2 |= 8; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: clearcoat normal texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* KHR 扩展材质贴图槽（plans/20）：7 槽 → mat.textures3[]，存在项置 flags2 presence bit12..18。
                 * 槽序（与 gfx binding 10..16 / shader 一致）：
                 *   0 clearcoat(R)          1 clearcoat_roughness(G)  2 sheen_color(RGB, sRGB)
                 *   3 sheen_roughness(A)    4 iridescence(R)          5 iridescence_thickness(G)
                 *   6 anisotropy(RG=方向, B=强度)
                 * 除 sheen_color 是颜色贴图（sRGB）外，其余都是线性数据贴图（G/A/R 通道语义由 shader 解释）。
                 * 缺失/空 → textures3 保持 -1、presence=0：shader 走旧路径（旧材质零行为变化）。 */
                {
                    auto load_ext_tex = [&](const std::string& texref, int slot, bool srgb, const char* name) {
                        if (texref.empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, texref)) : load_texture_mips(scoped_texture_ref(idx, texref));
                        if (tid >= 0) {
                            mat.textures3[slot] = tid;
                            mat.flags2 |= 1 << (12 + slot);          /* presence：bit12..18 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr ext texture {} '{}' -> {}\n", name, texref, tid);
                    };
                    load_ext_tex(pm->clearcoat_texture, 0, false, "clearcoat_texture");
                    load_ext_tex(pm->clearcoat_roughness_texture, 1, false, "clearcoat_roughness_texture");
                    load_ext_tex(pm->sheen_color_texture, 2, true, "sheen_color_texture");
                    load_ext_tex(pm->sheen_roughness_texture, 3, false, "sheen_roughness_texture");
                    load_ext_tex(pm->iridescence_texture, 4, false, "iridescence_texture");
                    load_ext_tex(pm->iridescence_thickness_texture, 5, false, "iridescence_thickness_texture");
                    load_ext_tex(pm->anisotropy_texture, 6, false, "anisotropy_texture");
                }
                /* KHR_materials_specular 两槽（flags2 bit21/22）：specularTexture.A=specularFactor（线性）、
                 * specularColorTexture.RGB=specularColorFactor（sRGB）；shader 只作用于电介质 F0。 */
                {
                    auto load_spec_tex = [&](const std::string& texref, int slot, bool srgb, const char* name) {
                        if (texref.empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, texref)) : load_texture_mips(scoped_texture_ref(idx, texref));
                        if (tid >= 0) {
                            mat.textures4[slot] = tid;
                            mat.flags2 |= 1 << (21 + slot);          /* presence：bit21/22 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr specular texture {} '{}' -> {}\n", name, texref, tid);
                    };
                    load_spec_tex(pm->specular_texture, 0, false, "specular_texture");
                    load_spec_tex(pm->specular_color_texture, 1, true, "specular_color_texture");
                }
                /* KHR_materials_transmission 贴图槽（尾部追加）：R = 透射率掩码（0=不透、1=全透）。
                 * 线性数据贴图 → load_texture_mips（不是 sRGB）；句柄放 textures5[0]，
                 * 存在时置 flags2 bit23（presence；gfx 在 group1 新 binding 绑定）。空/失败保持 -1、presence=0。 */
                if (const std::string* texref = &pm->transmission_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures5[0] = tid; mat.flags2 |= 0x800000; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: transmission texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* KHR_materials_diffuse_transmission 两槽（尾部追加）：
                 * diffuseTransmissionTexture.A = 透射率乘子（线性数据，load_texture_mips）；
                 * diffuseTransmissionColorTexture.RGB = 透射色（sRGB）。presence = flags2 bit25/26，
                 * gfx 绑定在 group1 binding 20/21；空/失败保持 -1、presence=0。 */
                {
                    auto load_dt_tex = [&](const std::string& texref, int slot, bool srgb, const char* name) {
                        if (texref.empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, texref)) : load_texture_mips(scoped_texture_ref(idx, texref));
                        if (tid >= 0) {
                            mat.textures6[slot] = tid;
                            mat.flags2 |= 1 << (25 + slot);          /* presence：bit25/26 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr diffuse-transmission texture {} '{}' -> {}\n",
                                       name, texref, tid);
                    };
                    load_dt_tex(pm->diffuse_transmission_texture, 0, false, "diffuse_transmission_texture");
                    load_dt_tex(pm->diffuse_transmission_color_texture, 1, true, "diffuse_transmission_color_texture");
                }
                /* KHR_materials_volume thicknessTexture.G（线性数据；flags2 bit27 presence，
                 * gfx 绑定在 group1 binding 22）。 */
                if (const std::string* texref = &pm->volume_thickness_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures7[0] = tid; mat.flags2 |= 0x8000000; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: volume thickness texture '{}' -> {}\n", *texref, tid);
                    }
                }
            }
            if (std::getenv("MAQUE_RENDER3D_INFO"))
                std::print("runtime: 3d material node={} model={} side={} flags2={} tex2=({},{},{})\n",
                           idx, mat.shading_model, mat.side, mat.flags2,
                           mat.textures2[0], mat.textures2[1], mat.textures2[2]);
            const int32_t mat_index = (int32_t)out_mats.size();
            out_mats.push_back(mat);
            /* M4c：mesh_path 非空时用离线导入网格（glTF → .mesh.json），否则内建 cube */
            int32_t mesh_id = cube_mesh;
            std::string mesh_path_str;
            if (const std::string* mp = read_str(dmesh, mb->data(), "mesh_path")) {
                if (!mp->empty()) {
                    mesh_path_str = *mp;
                    const int32_t loaded = load_mesh(*mp);
                    if (loaded >= 0) mesh_id = loaded;
                    if (std::getenv("MAQUE_RENDER3D_INFO"))
                        std::print("runtime: mesh renderer node={} path='{}' → mesh={}\n", idx, *mp, loaded);
                }
            }
            /* ---- 蒙皮（plans/22 §6.3）：调色板 palette[j] = world_m(joint) * inverse_bind[j] ----
             * 蒙皮网格忽略节点变换（glTF 规范）→ dr.model = identity、dr.skin = frame3d.skins 下标。
             * 解析失败 → 告警一次并回退静态（dr.model = world_m、skin = -1）；
             * MAQUE_SKIN=0 → 调色板填 identity（bind pose 不变量，用于 e2e A/B 对照）。 */
            int32_t skin_index = -1;
            float model_m[16];
            std::memcpy(model_m, wm, sizeof model_m);
            const std::string* skin_path = read_str(dmesh, mb->data(), "skin_path");
            if (skin_path && !skin_path->empty()) {
                auto lay_it = mesh_layout.find(mesh_path_str);
                const bool skinned_ok = !mesh_path_str.empty() && lay_it != mesh_layout.end() &&
                                        lay_it->second >= 2;
                if (!skinned_ok) {
                    if (!gltf_warned.count("skin-layout:" + *skin_path)) {
                        gltf_warned.insert("skin-layout:" + *skin_path);
                        std::print(stderr, "runtime: 蒙皮网格 '{}/{}' 布局缺失或载入失败，回退静态\n",
                                   mesh_path_str, *skin_path);
                    }
                } else {
                    const bool sk_loaded = load_skeleton(*skin_path);
                    auto sit = skeletons.find(*skin_path);
                    const Skeleton* sk = (sk_loaded && sit != skeletons.end() && !sit->second.failed)
                                             ? &sit->second : nullptr;
                    const size_t nj = sk ? sk->joint_names.size() : 0;
                    const bool sk_valid = sk && nj > 0 && sk->inverse_bind.size() == nj * 16;
                    bool built = false;
                    if (sk_valid) {
                        std::vector<float> pal(nj * 16);
                        if (!skin_enabled) {
                            /* MAQUE_SKIN=0：identity 调色板（joint_count 保留）→ bind pose 不变量 */
                            for (size_t j = 0; j < nj; ++j) m4_identity(pal.data() + j * 16);
                            built = true;
                        } else if (!entity_by_name.empty()) {
                            built = true;
                            for (size_t j = 0; j < nj; ++j) {
                                auto jhit = entity_by_name.find(sk->joint_names[j]);
                                const SceneNode* je = jhit == entity_by_name.end()
                                                       ? nullptr : scene->at(jhit->second);
                                if (!je) { built = false; break; }
                                m4_mul(je->world_m, sk->inverse_bind.data() + j * 16,
                                       pal.data() + j * 16);
                            }
                        }
                        if (built) {
                            skin_palettes.push_back(std::move(pal));
                            mq_renderer_skin rs{};
                            rs.palette = skin_palettes.back().data();
                            rs.joint_count = (uint32_t)nj;
                            out_skins.push_back(rs);
                            skin_index = (int32_t)out_skins.size() - 1;
                            m4_identity(model_m);            /* 蒙皮网格忽略节点世界变换 */
                        }
                    }
                    if (!built && !gltf_warned.count("skin-joint:" + *skin_path)) {
                        gltf_warned.insert("skin-joint:" + *skin_path);
                        std::print(stderr, "runtime: 蒙皮 '{}/{}' 关节解析失败，回退静态\n",
                                   mesh_path_str, *skin_path);
                    }
                }
            }
            /* ---- CPU morph（plans/22 §6.4）：权重 = 动画覆盖 > 默认；权变才重算 + update_mesh ---- */
            if (!mesh_path_str.empty()) {
                auto mit = morph_meshes.find(mesh_path_str);
                if (mit != morph_meshes.end() && mit->second.mesh >= 0 && !mit->second.targets.empty()) {
                    MorphMesh& mm = mit->second;
                    auto oit = morph_weight_override.find(idx);
                    const std::vector<float>& mw = (oit != morph_weight_override.end())
                                                       ? oit->second : mm.default_weights;
                    if (mm.owner_entity < 0) mm.owner_entity = idx;
                    if (mm.owner_entity == idx) {
                        if (mm.applied_weights != mw) (void)apply_morph(mesh_path_str, mw);
                    } else if (!mm.warned_shared) {
                        mm.warned_shared = true;
                        std::print(stderr, "runtime: mesh '{}' 被多个实体共享，morph 只取首实体 entity={}\n",
                                   mesh_path_str, mm.owner_entity);
                    }
                }
            }
            mq_renderer_draw3d dr{};
            dr.mesh = mesh_id;
            dr.material = mat_index;
            std::memcpy(dr.model, model_m, sizeof dr.model);
            dr.skin = skin_index;
            dr.flags |= 0x40000000;            /* draw.layer 是有效 culling mask（旧调用方无此位） */
            dr.layer = -1;
            if (const auto* m = as_component<render::MeshRenderer>(dmesh, mb->data()))
                dr.layer = (int32_t)m->culling_mask;
            dr.sort_key = (uint64_t)idx;
            out_draws.push_back(dr);
        }
    }

    /* OrbitControls：仅主相机绕世界原点环绕；RenderTexture 相机保持节点自身变换。 */
    if (allow_orbit) if (const char* oo = std::getenv("MAQUE_ORBIT")) {
        float oy = 0.0F, op = 0.3F, od = 1.0F;
        if (std::sscanf(oo, "%f,%f,%f", &oy, &op, &od) >= 1) {
            orbit_yaw = oy; orbit_pitch = op; orbit_dist = od > 0.0F ? od : 1.0F;
            orbit_enabled = true; orbit_inited = true;
        }
    }
    if (allow_orbit && orbit_enabled) {
        if (!orbit_inited) {
            const float ex = cam_trs.position.x, ey = cam_trs.position.y, ez = cam_trs.position.z;
            float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (d < 1e-4F) d = 1.0F;
            orbit_dist = d;
            orbit_pitch = std::asin(std::max(-1.0F, std::min(1.0F, ey / d)));
            orbit_yaw = std::atan2(ex, ez);
            orbit_inited = true;
        }
        const float cp = std::cos(orbit_pitch), sp = std::sin(orbit_pitch);
        const float sy = std::sin(orbit_yaw), cy = std::cos(orbit_yaw);
        const float px = sy * cp * orbit_dist, py = sp * orbit_dist, pz = cy * cp * orbit_dist;
        cam_trs.position = math::Vec3{ px, py, pz };
        cam_trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
        const float fl = std::sqrt(px * px + py * py + pz * pz);
        if (fl > 1e-6F) {
            const float fx = -px / fl, fy = -py / fl, fz = -pz / fl;
            cam_trs.rotation = math::Vec3{ std::asin(std::max(-1.0F, std::min(1.0F, fy))),
                                           std::atan2(-fx, -fz), 0.0F };
        }
        m4_from_trs(cam_trs, cam_wm);        /* Orbit 覆盖相机：世界矩阵 = 绕世界原点环绕 */
        has_cam_wm = true;
        (void)has_cam;
    }
    if (gltf_lights_added) {
        std::stable_partition(out_lights.begin(), out_lights.end(),
                              [](const mq_renderer_light& L) { return L.type == 0; });
        if (out_lights.size() > 8) out_lights.resize(8);
    }
    opt_last_sun = false;
    if (out_lights.empty()) {                              /* 无方向光：给一盏默认主光，避免全黑 */
        /* F4 引擎选项：默认太阳光可关 / 调强度 / 调方向（仅"场景没有任何光源"这条回退路径）。 */
        if (opt_sun_enabled) {
            mq_renderer_light L{};
            L.type = 0;
            /* 方向全 0 会让 shader 的 normalize 退化 → 给一个合法方向（面板已夹，环境变量入口也兜住） */
            float dx = opt_sun_dir[0], dy = opt_sun_dir[1], dz = opt_sun_dir[2];
            if (std::fabs(dx) + std::fabs(dy) + std::fabs(dz) < 1e-4F) dz = -1.0F;
            L.dir[0] = dx; L.dir[1] = dy; L.dir[2] = dz;
            L.color[0] = L.color[1] = L.color[2] = 1.0F;
            L.intensity = opt_sun_intensity;
            out_lights.push_back(L);
            opt_last_sun = true;
        }
    }

    /* F4 引擎选项：主相机 near/far 覆盖（不写场景组件）。
     * 覆盖关闭时把**场景实际值**回填进选项字段 —— 面板里一勾选"覆盖"就等同当前画面，不跳变。 */
    if (allow_orbit && opt_cam_override) {
        near_z = std::max(opt_cam_near, 1e-4F);
        far_z = std::max(opt_cam_far, near_z + 1e-3F);
    } else if (allow_orbit) {
        opt_cam_near = near_z;
        opt_cam_far = far_z;
    }
    if (allow_orbit) {
        opt_last_near = near_z;
        opt_last_far = far_z;
    }

    mq_renderer_view3d v{};
    if (camera_entity >= 0 && !has_cam) return;       /* RenderTexture 必须由同节点的有效 Camera3D 驱动 */
    /* 纯 shader-pass / 纯 2D 场景（无 Camera3D / MeshRenderer）**不要合成默认相机 view**：
     * gfx 的 3D pass 会按 view 的 clear_color 覆盖自己的 viewport —— 对纯 shader 场景就是把
     * 全屏 `render.ShaderPass` 背景整块擦成背景色（表现为 shader 只剩画布外一圈、或整屏纯背景色）。
     * 不产出 view（view_count=0）时 gfx 直接跳过 3D pass，scene shader pass 就是整幅画面。 */
    if (camera_entity < 0 && !has_3d) return;
    /* 相机 view 由**世界矩阵**派生（plans/22 §6.3）：默认相机 / 平级场景与旧 m4_view(trs) 逐字节一致 */
    if (!has_cam_wm) m4_from_trs(cam_trs, cam_wm);
    float view[16], proj[16];
    m4_view_from_m(cam_wm, view);
    const float vx = std::clamp(camera_viewport.x, 0.0F, 1.0F);
    const float vy = std::clamp(camera_viewport.y, 0.0F, 1.0F);
    const float vw = std::clamp(camera_viewport.width, 0.0F, 1.0F - vx);
    const float vh = std::clamp(camera_viewport.height, 0.0F, 1.0F - vy);
    float root_x = 0.0F, root_y = 0.0F, root_w = (float)tw, root_h = (float)th;
    if (camera_entity < 0 && canvas.window_w > 0.0F && canvas.window_h > 0.0F) {
        const float rx = (float)tw / canvas.window_w;
        const float ry = (float)th / canvas.window_h;
        root_x = canvas.offset_x * rx;
        root_y = canvas.offset_y * ry;
        root_w = canvas.viewport_w * rx;
        root_h = canvas.viewport_h * ry;
    }
    const float pixel_w = std::max(1.0F, vw * root_w);
    const float pixel_h = std::max(1.0F, vh * root_h);
    const float aspect = pixel_w / pixel_h;
    if (projection == 1) m4_orthographic(ortho_height, aspect, near_z, far_z, proj);
    else m4_perspective(fov_y * 3.14159265F / 180.0F, aspect, near_z, far_z, proj);
    std::memcpy(v.view, view, sizeof view);
    std::memcpy(v.proj, proj, sizeof proj);
    v.eye[0] = cam_wm[12]; v.eye[1] = cam_wm[13]; v.eye[2] = cam_wm[14];
    v.viewport[0] = root_x + vx * root_w;
    v.viewport[1] = root_y + vy * root_h;
    v.viewport[2] = pixel_w;
    v.viewport[3] = pixel_h;
    v.exposure = exposure;
    v.clear_mode = clear_mode;
    v.clear_color[0] = clear.r; v.clear_color[1] = clear.g;
    v.clear_color[2] = clear.b; v.clear_color[3] = clear.a;
    v.layer_mask = camera_mask;
    out_views.push_back(v);
    if (render3d_info && !gltf_frame_info) {
        gltf_frame_info = true;
        std::print("runtime: 3d frame0 draws={} skins={} morph={} anim_pose={} names={} "
                   "lights={} mats={}\n",
                   (long long)out_draws.size(), (long long)out_skins.size(),
                   (long long)morph_meshes.size(), (long long)anim_pose.size(),
                   (long long)entity_by_name.size(), (long long)out_lights.size(),
                   (long long)out_mats.size());
    }
}

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

/* ---- 动画（docs/15 §2）：关键帧在模拟步内采样并写回反射字段 ---- */

/* 路径解析：先按组件类型名定位（"scn.Sprite.radius"），再按叶子字段名定位（"radius"）。
 * 返回目标类型/字段与所在实体；全部按 role 与反射，不硬编码组件类型。 */
bool resolve_field_path(Engine& eng, int entity, const std::string& path,
                        const mq_type_desc** out_type, const mq_field_desc** out_field, void** out_base) {
    SceneNode* e = eng.scene->at(entity);
    if (!e) return false;
    const size_t dot = path.rfind('.');
    const std::string type_part = dot == std::string::npos ? std::string() : path.substr(0, dot);
    const std::string field_part = dot == std::string::npos ? path : path.substr(dot + 1);
    /* 允许 "Type.field"（跨组件）或裸 "field"（本实体任一组件的字段） */
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
        if (!type_part.empty() && type_part != d->name) continue;
        const mq_field_desc* f = mq_reflect_field(d, field_part.c_str());
        if (!f) continue;
        *out_type = d; *out_field = f; *out_base = blob.data();
        return true;
    }
    return false;
}

bool Engine::load_clip(const std::string& ref) {
    if (ref.empty() || find_clip(ref) >= 0) return find_clip(ref) >= 0;
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + ref;
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK) {
        std::print(stderr, "runtime: clip not found '{}'\n", ref);
        return false;
    }
    const mq_type_desc* ct = mq_reflect_type_by_name(reflect, "asset.AnimationClip");
    if (!ct) { mq_vfs_free(d); return false; }
    Clip clip;
    clip.blob.assign((uint8_t*)d, (uint8_t*)d + ct->size);
    mq_vfs_free(d);
    std::string text;
    { void* t = nullptr; size_t tn = 0;
      if (mq_vfs_read(vfs, vpath.c_str(), &t, &tn) == MQ_VFS_OK) { text.assign((char*)t, tn); mq_vfs_free(t); } }
    if (text.empty()) return false;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    construct_blob(*this, ct, clip.blob);
    const int rc = mq_persist_apply_json(reflect, ct, dom, clip.blob.data());
    mq_json_free(dom);
    if (rc != 0) return false;
    const asset::AnimationClip* ac = (const asset::AnimationClip*)clip.blob.data();
    clip.name = ref;
    clip.length = ac->length;
    clip.loop = ac->loop == anim::LoopMode::Loop;
    if (ac->tracks.empty()) { std::print("runtime: clip '{}' has no tracks\n", ref); return false; }
    /* 校验每条轨道至少能解析（角色/实体在采样时确定） */
    for (auto& t : ac->tracks) clip.paths.push_back(t.field_path);
    clips.push_back(std::move(clip));
    std::print("runtime: clip '{}' loaded ({} track(s), length={:.2f}s)\n",
               ref, (int)ac->tracks.size(), (double)ac->length);
    return true;
}

void Engine::anim_step(float dt) {
    if (!scene) return;                                   // 片段首次使用时按需载入（见下）
    if (!gltf_env_read) read_gltf_env();
    gltf_anim_step(dt);                                   /* plans/24：直读 glTF 的实体级动画/蒙皮/morph */
    /* plans/22 §6.2：派生姿态表每帧重建（纯函数 = (time, clip)；MAQUE_ANIM=0 时保持空表 = bind pose）。 */
    anim_pose.clear();
    morph_weight_override.clear();
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_ANIM_PLAYER)) continue;
            auto* ap = as_component_mut<scn::AnimationPlayer>(d, blob.data());
            if (!ap) continue;
            /* glTF 片段通路：clips 非空 → 只走新逻辑（旧 clip 通路完全不变） */
            if (!ap->clips.empty()) {
                anim_step_gclip(id, d, blob, dt);
                continue;
            }
            if (!ap->playing || ap->clip.empty()) continue;
            if (find_clip(ap->clip) < 0 && !load_clip(ap->clip)) continue;
            const int ci = find_clip(ap->clip);
            if (ci < 0) continue;
            const asset::AnimationClip* ac = (const asset::AnimationClip*)clips[ci].blob.data();
            const float length = ac->length;
            /* 播放头推进（确定性：dt 来自固定步长，无墙钟，docs/10 D1/D3） */
            ap->time += dt * ap->speed;
            const bool loop = clips[ci].loop;
            if (length > 0.0F) {
                if (loop) { while (ap->time >= length) ap->time -= length; }
                else if (ap->time > length) ap->time = length;
            }
            const float time = ap->time;
            ap->duration = length;
            /* 采样 + 写回：对每条轨道做线性插值（值型 Vec4 前 n 分量按字段类型写标量） */
            for (auto& track : ac->tracks) {
                if (track.keys.empty() || track.field_path.empty()) continue;
                const mq_type_desc* tt = nullptr; const mq_field_desc* tf = nullptr; void* base = nullptr;
                if (!resolve_field_path(*this, id, track.field_path, &tt, &tf, &base)) continue;
                /* 定位关键帧区间 */
                size_t k = 0;
                while (k + 1 < track.keys.size() && track.keys[k + 1].time <= time) ++k;
                const asset::Key& a = track.keys[k];
                const asset::Key& b = track.keys[k + 1 < track.keys.size() ? k + 1 : k];
                float alpha = 0.0F;
                if (b.time > a.time) alpha = (time - a.time) / (b.time - a.time);
                if (alpha < 0.0F) alpha = 0.0F;
                if (alpha > 1.0F) alpha = 1.0F;
                const float v[4] = {
                    a.value.x + (b.value.x - a.value.x) * alpha,
                    a.value.y + (b.value.y - a.value.y) * alpha,
                    a.value.z + (b.value.z - a.value.z) * alpha,
                    a.value.w + (b.value.w - a.value.w) * alpha,
                };
                void* fp = (uint8_t*)base + tf->offset;
                if (tt == nullptr || tf == nullptr) continue;
                if (tf->kind == MQ_TK_F32) { *(float*)fp = v[0]; continue; }
                if (tf->kind == MQ_TK_STRUCT) {
                    const mq_type_desc* vt = mq_reflect_type_by_id(reflect, tf->type_id);
                    if (!vt) continue;
                    for (uint32_t fi = 0, vi = 0; fi < vt->field_count && vi < 4; ++fi, ++vi) {
                        const mq_field_desc* vf = &vt->fields[fi];
                        if (vf->kind == MQ_TK_F32) field_set_f32(vf, (uint8_t*)fp, v[vi]);
                    }
                }
            }
        }
    }
}

/* 音频组件（docs/30 §6）：AudioSource/AudioListener 按 role 扫描（不硬编码类型），
 * 所有字段走反射；只写 transient（state/playhead/voice），不改配置字段。 */
void Engine::audio_step(float dt) {
    (void)dt;
    if (!audio || !scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 听者：每 instance 取稳定排序后的第一个 active */
    const mq_type_desc* listener_desc = nullptr;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        auto* lb = comp_by_role(*this, id, MQ_ROLE_AUDIO_LISTENER);
        if (!lb) continue;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (d && (d->role & MQ_ROLE_AUDIO_LISTENER)) { listener_desc = d; break; }
        }
        const auto* lis = listener_desc ? as_component<audio::AudioListener>(listener_desc, lb->data()) : nullptr;
        if (!lis || !lis->active) continue;
        const float rad = e->angle * 3.14159265358979F / 180.0F;
        mq_audio_listener l{};
        l.struct_size = sizeof l;
        l.x = e->world_x;
        l.y = e->world_y;
        l.z = 0.0F;
        l.dir_x = std::cos(rad);
        l.dir_y = std::sin(rad);
        l.dir_z = 0.0F;
        mq_audio_set_listener(audio, &l);
        const float master = lis->master_volume;
        if (master != last_listener_master) {
            mq_audio_set_master(audio, master, 0);
            last_listener_master = master;
        }
        break;
    }

    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        auto* sb = comp_by_role(*this, id, MQ_ROLE_AUDIO_SOURCE);
        if (!sb) continue;
        const mq_type_desc* d = nullptr;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* t = mq_reflect_type_by_id(reflect, tid);
            if (t && (t->role & MQ_ROLE_AUDIO_SOURCE)) { d = t; break; }
        }
        auto* src = d ? as_component_mut<audio::AudioSource>(d, sb->data()) : nullptr;
        if (!src) continue;
        uint64_t voice = src->voice;
        const std::string* clip_path = &src->clip;
        if (clip_path->empty()) {
            if (voice) {
                mq_audio_stop(audio, voice);
                src->voice = 0;
            }
            continue;
        }
        const bool autoplay = src->autoplay;
        const bool loop = src->loop;
        const float volume = src->volume;
        const float pitch = src->pitch;
        const float min_d = src->min_distance;
        const float max_d = src->max_distance;
        const uint32_t panning = (uint32_t)src->panning;
        const uint32_t attenuation = (uint32_t)src->attenuation;
        const std::string* bus = &src->bus;

        if (voice == 0) {
            if (!autoplay) continue;
            if (std::getenv("MAQUE_TRACE_AUDIO"))
                std::print("[audio] start id={} type={} clip='{}'\n", id, d->name, *clip_path);
            mq_audio_clip* clip = audio_clip(*clip_path);
            if (!clip) continue;
            mq_audio_voice_desc vd{};
            vd.struct_size = sizeof vd;
            vd.clip = clip;
            vd.volume = volume;
            vd.pitch = pitch;
            vd.loop = loop ? 1u : 0u;
            vd.bus = (bus && !bus->empty()) ? bus->c_str() : "sfx";
            vd.panning = panning;
            vd.attenuation = attenuation;
            vd.min_distance = min_d;
            vd.max_distance = max_d;
            vd.x = e->world_x;
            vd.y = e->world_y;
            vd.z = 0.0F;
            voice = mq_audio_play(audio, &vd);
            if (std::getenv("MAQUE_TRACE_AUDIO")) {
                uint32_t fr = 0, rt = 0, chn = 0;
                mq_audio_clip_info(clip, &fr, &rt, &chn);
                std::print("[audio] start voice={} frames={} rate={} ch={}\n", voice, fr, rt, chn);
            }
            if (!voice) continue;
            src->voice = voice;
        }
        /* 每帧同步可调参数（脚本可用 node_set_number 改 volume 等） */
        mq_audio_set_volume(audio, voice, volume);
        mq_audio_set_pitch(audio, voice, pitch);
        mq_audio_set_spatial(audio, voice, e->world_x, e->world_y, 0.0F, panning, attenuation,
                             min_d, max_d);
        uint32_t st = 0;
        float head = 0.0F;
        if (mq_audio_voice_state(audio, voice, &st, &head) == MQ_AUDIO_OK) {
            src->state = (audio::AudioPlayState)st;
            src->playhead = head;
        }
    }
}

/* ---- 粒子（docs/15 §4）：CPU 确定性粒子；池上限 + 引擎 RNG（参与回放） ---- */
void Engine::particles_step(float dt) {
    if (!scene) return;
    /* 推进已有粒子（按生成顺序；与实体无关，顺序稳定） */
    size_t w = 0;
    for (size_t i = 0; i < particles.size(); ++i) {
        Particle& p = particles[i];
        p.life += dt;
        if (p.life >= p.max_life) continue;
        p.vy += 0.0F;                                  // 重力在下面按发射器参数施加
        p.x += p.vx * dt; p.y += p.vy * dt;
        particles[w++] = p;
    }
    particles.resize(w);

    /* 发射（按实体 id 升序 → 确定性；RNG 为引擎 RNG） */
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            const auto* pe = (d && (d->role & MQ_ROLE_PARTICLE))
                                 ? as_component<scn::ParticleEmitter>(d, blob.data()) : nullptr;
            if (!pe || !pe->enabled) continue;
            const float rate = pe->rate, life = pe->lifetime, speed = pe->speed;
            const float spread = pe->spread, gravity = pe->gravity, size = pe->size;
            const float r = pe->r, g = pe->g, b = pe->b;
            const uint32_t max_count = pe->max_count;
            /* 其它粒子的重力（按各自发射器；用 owner 记录） */
            for (auto& p : particles) if (p.owner == id) p.vy += gravity * dt;
            /* 累加器（每实体一份，存在 emitter_accum；按 id 定位） */
            while ((int)emitter_accum.size() <= id) emitter_accum.push_back(0);
            emitter_accum[id] += (int)(rate * dt * 1024.0F);        // 定点累加（确定性，避免浮点漂移）
            const int per_emit = emitter_accum[id] / 1024;
            emitter_accum[id] -= per_emit * 1024;
            for (int i = 0; i < per_emit; ++i) {
                if (particles.size() >= max_particles) break;
                uint32_t alive_owner = 0;
                for (auto& p : particles) if (p.owner == id) ++alive_owner;
                if (alive_owner >= max_count) break;
                const float ang = (next_rng() / 4294967296.0F) * 6.28318530718F;
                const float mag = speed * (1.0F - spread * (next_rng() / 4294967296.0F));
                Particle p;
                p.owner = id;
                p.x = e->world_x; p.y = e->world_y;
                p.vx = std::cos(ang) * mag;
                p.vy = std::sin(ang) * mag;
                p.max_life = life > 0 ? life : 1.0F;
                p.size = size; p.r = r; p.g = g; p.b = b;
                particles.push_back(p);
            }
        }
    }
}

/* ---- 导航（docs/15 导航）：A* 在帧内确定性执行，代理沿路径追踪 ---- */
void Engine::nav_step() {
    if (!scene || !nav) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            auto* na = (d && (d->role & MQ_ROLE_NAV_AGENT))
                           ? as_component_mut<scn::NavAgent>(d, blob.data()) : nullptr;
            if (!na) continue;
            if (!na->has_target) { na->state = 0; continue; }
            const float tx = na->tx, ty = na->ty, speed = na->speed, arrive = na->arrive_dist;
            const bool repath = na->repath;

            AgentPath& ap = agent_paths[id];
            /* 路径缺失/目标变化 → 重新寻路（确定性：与实体 id 顺序无关的单点查询） */
            const bool need_path = ap.path.empty() || ap.index >= ap.path.size() / 2;
            if (need_path || (repath && !ap.path.empty())) {
                const uint32_t idx_last = ap.path.empty() ? 0 : (uint32_t)(ap.path.size() / 2 - 1);
                const float goal_x = ap.path.empty() ? tx : ap.path[idx_last * 2];
                const float goal_y = ap.path.empty() ? ty : ap.path[idx_last * 2 + 1];
                if (need_path || (repath && (goal_x != tx || goal_y != ty))) {
                    std::vector<float> buf(2 * 4096);
                    uint32_t n = 0, need = 0;
                    const int rc = mq_nav_find_path(nav, e->world_x, e->world_y, tx, ty, buf.data(),
                                                   (uint32_t)buf.size() / 2, &n, &need);
                    if (rc == MQ_NAV_ERR_CAPACITY) {
                        buf.resize(need);
                        mq_nav_find_path(nav, e->world_x, e->world_y, tx, ty, buf.data(), need, &n, &need);
                    }
                    if (rc == MQ_NAV_OK) {
                        n = mq_nav_smooth(nav, buf.data(), n);
                        ap.path.assign(buf.begin(), buf.begin() + (size_t)n * 2);
                        ap.index = 0;
                        na->state = 1;   // Moving
                    } else {
                        ap.path.clear();
                        ap.index = 0;
                        na->state = 3;   // NoPath
                        continue;
                    }
                }
            }
            if (ap.path.empty()) continue;
            float x = e->world_x, y = e->world_y;
            const float dt = 1.0F / 60.0F;
            const int rc = mq_nav_step_towards(nav, &x, &y, ap.path.data(),
                                               (uint32_t)(ap.path.size() / 2), &ap.index,
                                               speed, dt, arrive);
            host::entity_set_pos(this, (float)id, x, y);
            e->world_x = x; e->world_y = y;
            if (rc == 1) na->state = 2;    // Arrived
        }
    }
}

/* 装配期预解析（docs/13 §8：渲染/提取只读）：把所有实体的纹理与材质引用解析进缓存。
 * 引擎在装配阶段做 IO（合法：不在模拟帧内，docs/10 D7）。 */
/* 切换场景时**必须**清空资源缓存（docs/32 §5.1 资源基准）：缓存键是组件里的**相对路径**
 *（如 khronos 每个模型的 "textures/image0.png"），而 VFS 根挂载点已换成新场景目录 ——
 * 不清会直接命中上一个场景的纹理/网格，表现为“换了场景但贴图还是上一个模型”。 */
void Engine::clear_asset_caches() {
    destroy_render_textures();   /* 先擦除 tex_cache 中的虚拟 URI/别名并释放一次，避免下方重复销毁 */
    const size_t nt = tex_cache.size(), nm = mesh_cache.size(), ne = env_tex_cache.size();
    const size_t nec = env_cube_cache.size();
    const size_t ng = gclips.size(), ns = skeletons.size(), nmo = morph_meshes.size();
    const size_t ngl = gltf_models.size();
    if (rapi && rself) {
        if (rapi->destroy_texture) {
            for (auto& [k, id] : tex_cache) if (id >= 0) rapi->destroy_texture(rself, id);
            for (auto& [k, pair] : env_tex_cache) {
                if (pair.first >= 0) rapi->destroy_texture(rself, pair.first);
                if (pair.second >= 0) rapi->destroy_texture(rself, pair.second);
            }
            for (auto& [k, pair] : env_cube_cache) {
                if (pair.first >= 0) rapi->destroy_texture(rself, pair.first);
                if (pair.second >= 0) rapi->destroy_texture(rself, pair.second);
            }
            for (auto& [k, gm] : gltf_models)
                for (int32_t t : gm.textures)
                    if (t >= 0) rapi->destroy_texture(rself, t);
        }
        if (rapi->destroy_mesh) {
            for (auto& [k, id] : mesh_cache) if (id >= 0) rapi->destroy_mesh(rself, id);
            for (auto& [k, gm] : gltf_models)
                for (const auto& pr : gm.prims)
                    if (pr.mesh >= 0) rapi->destroy_mesh(rself, pr.mesh);
        }
    }
    for (const auto& [k, id] : mesh_cache) mesh_stats.erase(id);
    for (const auto& [k, gm] : gltf_models)
        for (const auto& pr : gm.prims) mesh_stats.erase(pr.mesh);
    /* cube_mesh 跨场景保留，其 stats 也保留；其余句柄随缓存销毁同步失效。 */
    tex_cache.clear();
    material_cache.clear();
    mesh_cache.clear();
    env_tex_cache.clear();
    env_cube_cache.clear();
    gltf_models.clear();
    gltf_states.clear();
    /* plans/22 §6.5：glTF 动画/蒙皮/morph 缓存与名字表也按**相对路径/场景**失效 */
    gclips.clear();
    skeletons.clear();
    morph_meshes.clear();
    mesh_layout.clear();
    anim_pose.clear();
    morph_weight_override.clear();
    entity_morph_mesh.clear();
    entity_by_name.clear();
    skin_palettes.clear();
    gltf_warned.clear();
    gltf_frame_info = false;
    gltf_anim_info = false;
    if (nt + nm + ne + nec + ng + ns + nmo + ngl > 0)
        std::print("runtime: asset caches cleared on scene switch ({} textures, {} meshes, {} env, {} env_cube, "
                   "{} clips, {} skeletons, {} morph, {} gltf)\n",
                   nt, nm, ne, nec, ng, ns, nmo, ngl);
}

void Engine::preload_scene_assets() {
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d) continue;
            if (d->role & MQ_ROLE_SPRITE) {
                if (const std::string* tp = read_str(d, blob.data(), "texture"); tp && !tp->empty())
                    load_texture_ref(scoped_texture_ref(id, *tp));
                if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty()) {
                    scn::SpriteMaterial m{};
                    if (load_material(*mp, m)) {
                        material_cache[*mp] = m;
                        if (!m.texture.empty()) load_texture_ref(scoped_texture_ref(id, m.texture));
                    }
                }
            }
        }
    }
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
void Engine::text_setup() {
    if (font) return;
    const char* env = std::getenv("MAQUE_FONT");
    if (env && *env) {
        font = mq_font_load_file(env);
        if (font) font_path = env;
    }
    /* 项目字体：scenes/fonts/ 下按文件名排序取第一个 .ttf。
     * 搜索根顺序 = 场景目录 → CWD（与组件级 font 字段的解析规则一致）。 */
    if (!font) {
        std::vector<std::filesystem::path> roots;
        if (!scn_dir.empty()) roots.emplace_back(std::filesystem::path(scn_dir) / "fonts");
        roots.emplace_back(std::filesystem::path("scenes") / "fonts");
        roots.emplace_back(std::filesystem::path("fonts"));
        for (const auto& dir : roots) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            std::vector<std::filesystem::path> ttfs;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                const auto& p = it->path();
                if (p.extension() == ".ttf" || p.extension() == ".otf") ttfs.push_back(p);
            }
            std::sort(ttfs.begin(), ttfs.end());          // 稳定：同目录多字体时取文件名最小者
            for (const auto& t : ttfs) {
                font = mq_font_load_file(t.string().c_str());
                if (font) { font_path = t.string(); break; }
            }
            if (font) break;
        }
    }
    if (!font) {
        static const char* kCandidates[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/lato/Lato-Medium.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/tuffy/tuffy_bold.ttf",
            /* Windows 常见系统字体（TTC 也可被 FreeType 打开） */
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/msyh.ttc",
        };
        for (const char* c : kCandidates) {
            font = mq_font_load_file(c);
            if (font) { font_path = c; break; }
        }
    }
    if (!font) { font = mq_font_builtin(); font_path = "(builtin 5x7)"; }
    fonts.push_back({ font_path, font });
    std::print("runtime: text font '{}'\n", font_path);
}

/* 字体选择：标签的 font 字段（相对路径按 CWD 与场景目录解析）→ 缓存 */
mq_font* Engine::text_font_for(const std::string& path) {
    if (path.empty()) return font;
    for (const auto& fe : fonts) if (fe.path == path && fe.font) return fe.font;
    std::vector<std::string> tries = { path, scn_dir + "/" + path };
    for (const auto& t : tries) {
        if (mq_font* f = mq_font_load_file(t.c_str())) {
            fonts.push_back({ path, f });
            std::print("runtime: label font '{}'\n", t);
            return f;
        }
    }
    std::print(stderr, "runtime: label font '{}' not found (fallback: {})\n", path, font_path);
    fonts.push_back({ path, nullptr });
    return font;
}

/* 调试：导出图集为 PGM/PPM（MAQUE_TEXT_DUMP=<前缀>）——核查字形位图内容 */
static void dump_atlas(const char* prefix, float sz, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    const std::string path = std::string(prefix) + "_atlas_" + std::to_string((int)sz) + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::print(f, "P6\n{} {}\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint8_t a = rgba[i * 4 + 3];
        const uint8_t px[3] = { a, a, a };
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
    std::print("runtime: 图集已导出 {}\n", path);
}

/* 字形图集纹理：按 (字体, 字号) 缓存；图集增长（generation 变化）→ 重建纹理并释放旧的 */
int Engine::glyph_texture(const mq_font* f, float size) {
    const float sz = size > 0.0F ? size : 16.0F;
    if (!f || !rapi || !rself) return -1;
    const uint32_t gen = mq_font_atlas_generation(const_cast<mq_font*>(f), sz);
    for (auto& a : font_atlases) {
        if (a.font != f || a.size != sz) continue;
        if (a.generation == gen && a.texture >= 0) return a.texture;      /* 未变化 → 复用 */
        uint32_t w = 0, h = 0;
        if (mq_font_atlas(const_cast<mq_font*>(f), sz, nullptr, 0, &w, &h) != MQ_TEXT_ERR_CAP) return a.texture;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        if (mq_font_atlas(const_cast<mq_font*>(f), sz, rgba.data(), rgba.size(), &w, &h) != MQ_TEXT_OK) return a.texture;
        const int tex = rapi->create_texture(rself, rgba.data(), (int32_t)w, (int32_t)h);
        if (tex < 0) return a.texture;
        if (const char* dp = std::getenv("MAQUE_TEXT_DUMP"); dp && *dp) dump_atlas(dp, sz, rgba, w, h);
        if (a.texture >= 0) retired_textures.push_back(a.texture);      // 延迟释放（见成员注释）
        a.texture = tex; a.w = w; a.h = h; a.generation = gen;
        return tex;
    }
    uint32_t w = 0, h = 0;
    if (mq_font_atlas(const_cast<mq_font*>(f), sz, nullptr, 0, &w, &h) != MQ_TEXT_ERR_CAP) return -1;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    if (mq_font_atlas(const_cast<mq_font*>(f), sz, rgba.data(), rgba.size(), &w, &h) != MQ_TEXT_OK) return -1;
    if (const char* dp = std::getenv("MAQUE_TEXT_DUMP"); dp && *dp) dump_atlas(dp, sz, rgba, w, h);
    const int tex = rapi->create_texture(rself, rgba.data(), (int32_t)w, (int32_t)h);
    if (tex < 0) return -1;
    font_atlases.push_back({ f, sz, tex, w, h, gen });
    return tex;
}

/* 文本提取（docs/13 §8：只读）：排版后逐字形提交 quad。
 * 支持 scn.Label（扁平字段）与 ui.TextLabel / ui.RichText（字段）。 */
void Engine::extract_text(Engine& eng, int idx, SceneNode& e, const mq_type_desc* d,
                          std::vector<mq_renderer_item>& out, float inherit_alpha) {
    std::string text, font_path_comp;
    float size = 16.0F, r = 1, g = 1, b = 1, a = 1, max_w = 0, max_h = 0, line_spacing = 0;
    int align = 0, wrap = 0, overflow = 0;
    bool bold = false, italic = false;
    float ox = 0.0F, oy = 0.0F;
    float outline_size = 0.0F, orr = 0.0F, ogg = 0.0F, obb = 0.0F, oaa = 1.0F;
    float shx = 0.0F, shy = 0.0F, shr = 0.0F, shg = 0.0F, shb = 0.0F, sha = 0.0F;
    const uint8_t* tb = e.comps[d->type_id].data();
    if (const auto* l = as_component<scn::Label>(d, tb)) {
        text = l->text; font_path_comp = l->font;
        size = l->font_size; max_w = l->max_width; max_h = l->max_height;
        line_spacing = l->line_spacing;
        r = l->r; g = l->g; b = l->b; a = l->a;
        align = (int)l->align; wrap = (int)l->wrap; overflow = (int)l->overflow;
        bold = l->bold; italic = l->italic;
        outline_size = l->outline_size;
        orr = l->outline_r; ogg = l->outline_g; obb = l->outline_b; oaa = l->outline_a;
        shx = l->shadow_x; shy = l->shadow_y;
        shr = l->shadow_r; shg = l->shadow_g; shb = l->shadow_b; sha = l->shadow_a;
        ox = l->x; oy = l->y;
    } else if (const auto* tl = as_component<ui::TextLabel>(d, tb)) {
        text = tl->text; size = tl->font_size; line_spacing = tl->line_spacing;
        r = tl->color.x; g = tl->color.y; b = tl->color.z; a = tl->color.w;
        align = (int)tl->align_h; wrap = (int)tl->wrap_mode;
        outline_size = tl->outline_size;
        orr = tl->outline_color.x; ogg = tl->outline_color.y;
        obb = tl->outline_color.z; oaa = tl->outline_color.w;
    } else if (const auto* rt = as_component<render::RichText>(d, tb)) {
        text = rt->text; size = rt->font_size; max_w = rt->max_width;
        line_spacing = rt->line_spacing;
        r = rt->color.r; g = rt->color.g; b = rt->color.b; a = rt->color.a;
        align = (int)rt->align_h; wrap = (int)rt->wrap_mode;
        outline_size = rt->outline_size;
        orr = rt->outline_color.r; ogg = rt->outline_color.g;
        obb = rt->outline_color.b; oaa = rt->outline_color.a;
        if (!rt->parse_tags) {
            std::string esc;
            for (char c : text) { if (c == '<') esc += "&lt;"; else esc.push_back(c); }
            text = esc;
        }
    } else {
        return;                                          // role 文本组件必须是三个内置类型之一
    }
    if (text.empty()) return;

    mq_text_style st{};
    st.struct_size = (uint32_t)sizeof st;
    st.font_size = size;
    /* 设备缩放：窗口像素 / 目标像素（HiDPI 时按设备字号栅格化 → 1:1 呈现，不做二次缩放） */
    /* 字形栅格化倍率 = **渲染目标像素 / 设计像素**（=1：目标就是设计分辨率）。
     * 注意不是画布的窗口缩放：窗口放大由 present 做（整倍 → 最近邻，保持锐利）；
     * 若按窗口缩放栅格化（如 3x 出 36px 字形），再画进 320x240 的目标就是**降采样 3 倍** →
     * 笔画丢失、文字发虚/乱码（曾如此）。超采样目标（目标 > 设计）时才 > 1。 */
    st.device_scale = eng.ss;   /* 设计单位 → 当前渲染目标像素，仅影响字形栅格化 */
    st.pixel_snap = 1;                                  /* pixel-perfect：字形对齐整数像素 */
    st.line_spacing = line_spacing;
    st.wrap = (uint32_t)(wrap < 0 ? 0 : wrap);
    st.align = (uint32_t)(align < 0 ? 0 : align);
    st.max_width = max_w;
    st.bold = bold ? 1 : 0;
    st.italic = italic ? 1 : 0;
    st.r = r; st.g = g; st.b = b; st.a = a * inherit_alpha;

    /* 字体：标签的 font 字段优先（相对路径按 CWD/场景目录解析），否则默认字体 */
    mq_font* label_font = eng.text_font_for(font_path_comp);
    /* 排版度量为**设计像素**（text 模块内部与窗口/渲染倍率无关）；inv 只用于把字形位图
     * 的设备像素尺寸换算回设计像素（位图分辨率随窗口变化，布局不变）。 */
    const float inv = st.device_scale > 0.0F ? 1.0F / st.device_scale : 1.0F;
    const float req_size = size;

    /* 高度限制 + Shrink：度量已是设计像素，直接与 max_h 比较。
     * 先试请求字号；装不下则**二分**找「能装下的最大字号」——高度随字号总体单调增
     * （字号变小 → 每行更宽、折行更少 → 高度下降），比单步比例缩放更接近恰好装下
     * （比例缩放会因折行数突变而过缩，文字小到看不清）。16 轮二分 = 0.0004px 精度。 */
    const bool shrink_mode = max_h > 0.0F && overflow == (int)ui::TextOverflow::Shrink;
    mq_text_metrics m{};
    if (shrink_mode) {
        st.font_size = size;
        const bool ok0 = mq_text_measure(label_font, &st, text.c_str(), &m) == MQ_TEXT_OK;
        if (!ok0 || m.height > max_h + 0.01F) {
            float lo = 1.0F, hi = size;             /* lo = 假定装得下，hi = 装不下 */
            for (int it = 0; it < 16; ++it) {
                const float mid = (lo + hi) * 0.5F;
                st.font_size = mid;
                if (mq_text_measure(label_font, &st, text.c_str(), &m) != MQ_TEXT_OK) break;
                if (m.height <= max_h + 0.01F) lo = mid; else hi = mid;
            }
            size = std::floor(lo * 100.0F) / 100.0F;
            if (size < 1.0F) size = 1.0F;
        }
    }
    st.font_size = size;

    /* 先测量（写回 transient 字段，供脚本/控制面读取），再排版 */
    const int mrc = mq_text_measure(label_font, &st, text.c_str(), &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text] id={} type={} len={} size={:.1f}(req {:.1f}) maxw={:.0f} maxh={:.0f} overflow={} wrap={} rc={} lines={}\n",
                   idx, d->name, text.size(), st.font_size, req_size, max_w, max_h, overflow, st.wrap, mrc,
                   m.line_count);
    if (mrc != MQ_TEXT_OK) return;
    std::vector<mq_text_glyph> glyphs(m.glyph_count ? m.glyph_count : 1);
    uint32_t n = 0;
    const int lrc = mq_text_layout(label_font, &st, text.c_str(), glyphs.data(), (uint32_t)glyphs.size(), &n, &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text]   layout rc={} glyphs={} pos=({:.0f},{:.0f}) tex={} first=({:.1f},{:.1f},{:.1f}x{:.1f})\n",
                   lrc, n, e.world_x, e.world_y, n ? glyph_texture(label_font, glyphs[0].device_size) : -1,
                   n ? glyphs[0].x : 0.0F, n ? glyphs[0].y : 0.0F,
                   n ? glyphs[0].w : 0.0F, n ? glyphs[0].h : 0.0F);
    if (lrc != MQ_TEXT_OK) return;
    /* 度量写回：text 模块现在直接返回**设计像素**度量（与窗口/渲染倍率无关）。
     * 宽度/高度限制时夹到限制值（Clip 语义：盒子就是限制尺寸；Shrink 时通常已装下）。 */
    const float measured_w = m.width, measured_h = m.height;
    const float shown_w = max_w > 0.0F ? std::min(measured_w, max_w) : measured_w;
    const float shown_h = max_h > 0.0F ? std::min(measured_h, max_h) : measured_h;
    if (auto* l = as_component_mut<scn::Label>(d, e.comps[d->type_id].data())) {
        l->text_w = shown_w; l->text_h = shown_h;
        l->line_count = (int32_t)m.line_count;
        l->fit_font_size = shrink_mode ? st.font_size : 0.0F;
    } else if (auto* tl = as_component_mut<ui::TextLabel>(d, e.comps[d->type_id].data())) {
        tl->text_size = math::Vec2{ shown_w, shown_h };
    } else if (auto* rt = as_component_mut<render::RichText>(d, e.comps[d->type_id].data())) {
        rt->content_size = math::Vec2{ shown_w, shown_h };
        rt->line_count = (int32_t)m.line_count;
    }

    /* 锚点：scn.Label 的 x/y 为左上角偏移；ui.* 以节点位置为左上角 */

    /* 节点旋转（node_set_angle + 父链累积）：轴心 = **节点位置**（与 Sprite 一致，
     * x/y 偏移只做平移）。世界 y 向上、屏幕 y 向下 → 取负；旋转量同时作用于字形位置与
     * 字形自身朝向（整块文本刚体旋转）。angle=0 时 cos=1/sin=0 精确，走旧路径逐字节不变。 */
    float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
    accumulate_node_attrs(eng, idx, node_angle, node_scale, node_opacity);
    (void)node_scale; (void)node_opacity;
    const float rot = -node_angle * 3.14159265358979323846F / 180.0F;
    const float rot_cs = std::cos(rot), rot_sn = std::sin(rot);
    const bool rotated = rot_sn != 0.0F || rot_cs != 1.0F;
    const float pivot_x = e.world_x;
    const float pivot_y = (float)eng.H - e.world_y;
    /* 阶段 1：只收集（纹理与 UV 留到 text_flush，按**最终**图集计算）。
     * 每个字形按 阴影 → 描边 → 字形本体 的顺序入队（同 z，靠原始下标稳定排序）。 */
    const float base_a = st.a;                          /* 组件 alpha × 继承 alpha */
    auto push_quad = [&](const mq_text_glyph& gl, float dx, float dy,
                         float cr, float cg, float cb, float ca, int zi) {
        if (gl.w <= 0.0F || gl.h <= 0.0F) return;
        /* x0/y0 已是**设计像素**（text 模块布局空间）；只有字形位图 w/h 是设备像素
         * （按渲染倍率栅格化），换算回设计像素后落位 —— 布局与窗口大小无关。 */
        const float half_w = gl.w * 0.5F * inv, half_h = gl.h * 0.5F * inv;
        PendingQuad q{};
        q.font = label_font; q.size = gl.device_size;
        q.ax = gl.ax; q.ay = gl.ay;
        q.tw = (int)gl.w; q.th = (int)gl.h;
        q.cp = gl.codepoint;
        /* 锚点 = 位图左上角 (x0,y0)；排版 y 向下、屏幕 y 向下 → 基准翻转后相加 */
        const float lx = ox + gl.x0 + half_w + dx;
        const float ly = -oy + gl.y0 + half_h + dy;
        q.x = pivot_x + (rotated ? lx * rot_cs - ly * rot_sn : lx);
        q.y = pivot_y + (rotated ? lx * rot_sn + ly * rot_cs : ly);
        q.rotation = rot;
        q.snap = rotated ? 0 : 1;                        /* 旋转后不能再按轴对齐左上角吸附 */
        q.half_w = half_w; q.half_h = half_h;
        q.r = cr; q.g = cg; q.b = cb; q.a = ca;
        q.z = 100 + zi;
        eng.pending_quads.push_back(q);
    };
    /* 描边：8 方向 × rings 环（ring 半径按 outline_size 均分；上限 8 环防止极端参数爆量）。
     * 用同一字形位图多次偏移绘制（描边色只取 alpha），确定性且与图集无关。 */
    const int outline_rings = outline_size > 0.0F
        ? std::clamp((int)std::ceil(outline_size), 1, 8) : 0;
    const bool has_shadow = sha > 0.0F && (shx != 0.0F || shy != 0.0F);
    for (uint32_t i = 0; i < n; ++i) {
        const mq_text_glyph& gl = glyphs[i];
        if (gl.w <= 0.0F || gl.h <= 0.0F) continue;
        /* 裁剪（设计像素）：
         *   · wrap=None + max_width：按宽度限制裁剪（文档语义：盒子就是限制尺寸）；
         *   · wrap=Word/Char：折行已由排版保证不越界；呈现用设备整数 advance 累计，
         *     可能比设计宽度多出 <1 设备像素 —— 不能逐字形裁掉，否则窗口非整数倍缩放时
         *     行尾字形会被误剪。高度超出 max_height 仍然整字形裁剪（Clip 语义）。 */
        const float gx = gl.x0, gy = gl.y0, gw = gl.w * inv, gh = gl.h * inv;
        if (max_w > 0.0F && wrap == 0 && gx + gw > max_w + 1.0F) continue;
        if (max_h > 0.0F && gy + gh > max_h + 0.5F) continue;
        if (has_shadow) push_quad(gl, shx, shy, shr, shg, shb, sha * base_a, (int)i);
        if (outline_rings > 0) {
            const float oa = std::clamp(oaa * base_a, 0.0F, 1.0F);
            for (int rr = 1; rr <= outline_rings; ++rr) {
                const float rad = outline_size * (float)rr / (float)outline_rings;
                for (int k = 0; k < 8; ++k) {
                    constexpr float kPi4 = 0.7853981633974483F;
                    const float ang = kPi4 * (float)k;
                    push_quad(gl, std::cos(ang) * rad, std::sin(ang) * rad, orr, ogg, obb, oa, (int)i);
                }
            }
        }
        push_quad(gl, 0.0F, 0.0F, gl.r, gl.g, gl.b, gl.a, (int)i);
    }
}

/* 阶段 2：刷新图集纹理（按最终尺寸）→ 计算 UV → 提交图元。
 * 必须在**所有**标签排版完成后调用（否则 UV 指向的图集尺寸会过期）。 */
void Engine::text_flush(std::vector<mq_renderer_item>& out) {
    if (pending_quads.empty()) return;
    for (const PendingQuad& q : pending_quads) {
        const int tex = glyph_texture(q.font, q.size);
        if (tex < 0) continue;
        /* 取该 (字体,字号) 的图集尺寸（= 纹理尺寸）→ 按最终尺寸换算 UV */
        uint32_t aw = 1, ah = 1;
        for (const auto& a : font_atlases)
            if (a.font == q.font && a.size == q.size) { aw = a.w ? a.w : 1; ah = a.h ? a.h : 1; break; }
        mq_renderer_item it{};
        it.x = q.x; it.y = q.y;
        it.half_w = q.half_w; it.half_h = q.half_h;
        it.size = q.half_w > q.half_h ? q.half_w : q.half_h;   /* 兼容字段 */
        it.filter = 1;                                          /* 1 纹素 = 1 像素 */
        it.rotation = q.rotation;
        it.snap = q.snap;                                       /* 未旋转 → 左上角吸附整数像素 */
        it.r = q.r; it.g = q.g; it.b = q.b; it.a = q.a;
        it.texture = tex;
        it.uv0 = (float)q.ax / (float)aw;
        it.uv1 = (float)q.ay / (float)ah;
        it.uv2 = (float)(q.ax + q.tw) / (float)aw;
        it.uv3 = (float)(q.ay + q.th) / (float)ah;
        it.layer = 1; it.z = q.z;
        out.push_back(it);
    }
    int traced = 0;
    if (std::getenv("MAQUE_TRACE_TEXT")) {
      for (const PendingQuad& tq : pending_quads) {
        if (tq.tw <= 0 || traced >= 80) continue;
        ++traced;
        uint32_t tw_ = 1, th_ = 1;
        for (const auto& fa : font_atlases)
            if (fa.font == tq.font && fa.size == tq.size) { tw_ = fa.w; th_ = fa.h; break; }
        /* 纹理实际尺寸 vs 图集当前尺寸：不一致 → UV 会指向纹理之外（表现为字形错乱） */
        uint32_t aw_now = 0, ah_now = 0;
        mq_font_atlas(const_cast<mq_font*>(tq.font), tq.size, nullptr, 0, &aw_now, &ah_now);
        std::print("[text]   quad U+{:04X} 纹理={}x{} 图集现={}x{} texel=({},{}) {}x{} uv=({:.4f},{:.4f},{:.4f},{:.4f})\n",
                   tq.cp, tw_, th_, aw_now, ah_now, tq.ax, tq.ay, tq.tw, tq.th,
                   (double)tq.ax / tw_, (double)tq.ay / th_,
                   (double)(tq.ax + tq.tw) / tw_, (double)(tq.ay + tq.th) / th_);
      }
    }
    if (std::getenv("MAQUE_TRACE_TEXT")) {
        const PendingQuad& q0 = pending_quads.front();
        uint32_t aw0 = 1, ah0 = 1;
        for (const auto& a : font_atlases)
            if (a.font == q0.font && a.size == q0.size) { aw0 = a.w; ah0 = a.h; break; }
        std::print("[text]   flush {} quad(s) 图集={}x{} 首个 texel=({},{}) {}x{} → uv=({:.4f},{:.4f},{:.4f},{:.4f})\n",
                   pending_quads.size(), aw0, ah0, q0.ax, q0.ay, q0.tw, q0.th,
                   (double)q0.ax / aw0, (double)q0.ay / ah0,
                   (double)(q0.ax + q0.tw) / aw0, (double)(q0.ay + q0.th) / ah0);
    }
    pending_quads.clear();
}


/* 读模拟器场景：先 VFS（资产路径，'/' 前缀兼容），失败再按宿主文件路径读（开发/CI 便利）。
 * 场景文件是**开发工具输入**而非游戏资产，因此允许宿主路径（docs/19）。 */
bool read_sim_scenario(mq_vfs* vfs, const char* path, std::string& out) {
    if (!path || !*path) return false;
    void* data = nullptr; size_t n = 0;
    std::string vp = path;
    if (vp[0] != '/') vp = "/" + vp;
    if (vfs && mq_vfs_read(vfs, vp.c_str(), &data, &n) == MQ_VFS_OK) {
        out.assign((const char*)data, n);
        mq_vfs_free(data);
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !out.empty();
}

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

/* ---- Spine 2D（docs/27）：装配期建实例、帧内步进、提取期出网格图元 ----
 *
 * 分层与数据流（每一层只做一件事）：
 *   VFS 读 .skel/.atlas ──► mq_spine_data_create（骨架 + 图集，页纹理走回调）
 *   纹理回调：页名 → VFS 路径 → image 解码 → rapi->create_texture → 回填非零句柄
 *   每帧：mq_spine_instance_step(dt*speed) → mq_spine_instance_draw(CPU 顶点/UV/颜色/索引/批次)
 *         → 顶点做"骨架空间 → 渲染目标像素空间"映射（y 向上翻成 y 向下，见 spine_extract 注释）
 *   提取：每个批次 = 一条 mq_renderer_mesh（同一图集页 + 同一混合模式）
 *
 * ⚠️ 实测事实（决定了下面的变换怎么写，别再凭印象改）：
 *   spine 的绘制顶点在**骨架子空间**里 y **向上**（spineboy 站立姿势包围盒 y ∈ [-8, 654]，
 *   脚在 y≈0、头在正 y；spine 编辑器与大多数 2D 运行时一致）。所以映射到引擎的
 *   "屏幕像素、y 向下"必须**翻一次 y**（H - world_y - local_y），而不是"直接用"。 */

/* 页纹理回调：spine 把页名交给我们，我们按 (atlas 目录 + 页名) 去 VFS 读 → 解码 → 上传。
 * 句柄约定：0 = 未回填（spine 侧语义），而 gfx 的纹理 id 从 0 开始 → 句柄 = id + 1。 */
static int spine_texture_cb(void* user, const char* page_name, const void* /*file_data*/,
                            size_t /*size*/, uint64_t* out_texture) {
    auto* cx = static_cast<SpineTexCtx*>(user);
    if (!cx || !cx->eng || !page_name || !*page_name || !out_texture) return MQ_SPINE_ERR_ARG;
    *out_texture = 0;
    const std::string path = cx->dir + page_name;
    const int tex = cx->eng->load_texture(path);
    if (tex < 0) {
        std::print(stderr, "runtime: spine atlas page read failed '{}'\n", path);
        return MQ_SPINE_ERR_PAGE;
    }
    *out_texture = (uint64_t)tex + 1u;      // 非零（0 = 未回填）
    return MQ_SPINE_OK;
}

/* 路径归一：组件里的资源引用可以是 "/art/x.skel" 或 "art/x.skel" → VFS 一律要前导 '/'。 */
static std::string vfs_path_of(const std::string& ref) {
    return (!ref.empty() && ref[0] == '/') ? ref : ("/" + ref);
}
static std::string dir_of(const std::string& path) {          // "/art/x.atlas" → "/art/"
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string("/") : path.substr(0, slash + 1);
}
static std::string stem_of(const std::string& path) {         // "/art/x.skel" → "/art/x"
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

Engine::Spine* Engine::spine_of(int entity) {
    for (auto& sp : spines) if (sp.entity == entity) return &sp;
    return nullptr;
}

/* 事件回调（spine2d → 引擎）：把 spine 的动画事件**翻译成引擎的脚本事件**。
 *
 * 为什么走这条路（而不是让脚本轮询 ABI）：docs/14 的事件总线是引擎唯一的脚本回调路径，
 * 按**处理器名**路由 → JS/Lua 语言无关、投递顺序确定、错误隔离都在总线里做好了。
 * 同一份事实还留了 pull 面（spine_event_count/…），push 与 pull 是同一次记录（有单测对拍）。
 *
 * 命名约定（与官方 demo 的 5 类事件一一对应，多一个用户事件）：
 *   start→"spine_start" interrupt→"spine_interrupt" end→"spine_end"
 *   complete→"spine_complete" dispose→"spine_dispose" event→"spine_event"
 * 处理器值 = 轨下标；用户事件的细节（名字/int/float/string）用 spine_event_* 取。
 *
 * ⚠️ 单线程、帧内同步调用（spine 的 AnimationState 就是这样），无线程、无异步 → 确定性不变（docs/10）。 */
static void spine_event_cb(void* user, mq_spine_instance* /*inst*/, const mq_spine_event* ev) {
    auto* sp = static_cast<Engine::Spine*>(user);
    if (!sp || !sp->eng || !ev) return;
    Engine& eng = *sp->eng;
    const char* name = nullptr;
    switch (ev->type) {
    case MQ_SPINE_EVENT_START:     name = "spine_start"; break;
    case MQ_SPINE_EVENT_INTERRUPT: name = "spine_interrupt"; break;
    case MQ_SPINE_EVENT_END:       name = "spine_end"; break;
    case MQ_SPINE_EVENT_COMPLETE:  name = "spine_complete"; break;
    case MQ_SPINE_EVENT_DISPOSE:   name = "spine_dispose"; break;
    case MQ_SPINE_EVENT_EVENT:     name = "spine_event"; break;
    default: return;
    }
    /* 值语义：用户事件带 int 值（官方 demo 的 "%d event: %s, %d, %f, %s" 里那个 int），
     * 其余事件带 track —— 都只是"标量提示"，精确字段由 pull 面给（docs/14 载荷只有标量）。 */
    const float value = (ev->type == MQ_SPINE_EVENT_EVENT) ? (float)ev->int_value : (float)ev->track;
    /* 重入护栏：脚本处理器里再调 spine_play 会同步触发新事件 → 再投递 → 若处理器又 play，
     * 就会无限递归到爆栈。正常链式播放只需要 1~2 层，故限深 8；超限只跳过**推送**
     *（事件本身仍进 pull 记录，数据不丢），并明确报一次。 */
    if (eng.spine_event_depth >= 8) {
        static bool warned = false;
        if (!warned) { warned = true;
            std::print(stderr, "runtime: spine 事件推送重入超过 8 层 → 后续只记录不推送（脚本处理器疑似自递归）\n"); }
        return;
    }
    ++eng.spine_event_depth;
    host::node_emit_number(&eng, (float)sp->entity, name, value);
    --eng.spine_event_depth;
}

void Engine::spine_release() {
    for (auto& sp : spines) {
        if (sp.inst) { mq_spine_instance_destroy(sp.inst); sp.inst = nullptr; }
        if (sp.data) { mq_spine_data_destroy(sp.data); sp.data = nullptr; }   // 页纹理归引擎（tex_cache），此处不释放
        /* 运行时合成的皮肤：本引擎是分配者 → 由本引擎释放（ABI 的"分配者释放"约定） */
        for (mq_spine_skin* sk : sp.own_skins) mq_spine_skin_destroy(sk);
        sp.own_skins.clear();
        sp.composed = nullptr;
    }
    spines.clear();
}

/* 帧边界：清空所有实例的事件记录。
 * 位置很关键 —— 必须在**脚本 update 之后、spine_step 之前**：
 *   · spine_step 在本帧脚本之后跑 → 它产生的事件留到下一帧脚本，才 pull 得到；
 *   · 脚本自己调 spine_play 产生的 start 也在本帧脚本之后才被清 → 下一帧同样 pull 得到。
 * 若放在脚本 update **之前**清（踩过）：上一帧 step 的事件会在脚本读之前被抹掉，
 * spine_event_count 永远是 0（push 面照常工作，但 pull 面瞎了）。 */
void Engine::spine_clear_events() {
    for (auto& sp : spines) {
        if (!sp.inst) continue;
        sp.events_last = mq_spine_instance_event_count(sp.inst);
        mq_spine_instance_clear_events(sp.inst);
    }
}

void Engine::spine_setup() {
    if (!scene || !vfs || !rapi) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* d = nullptr;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* dd = mq_reflect_type_by_id(reflect, tid);
            if (dd && (dd->role & MQ_ROLE_SPINE)) { d = dd; break; }
        }
        if (!d) continue;
        auto& blob = e->comps[d->type_id];
        const std::string* skelref = read_str(d, blob.data(), "skeleton");
        if (!skelref || skelref->empty()) {
            std::print(stderr, "runtime: spine entity {} missing 'skeleton' ref\n", id);
            continue;
        }
        Spine sp;
        sp.entity = id;
        sp.comp = d->type_id;
        sp.skel_path = vfs_path_of(*skelref);
        /* 图集路径：① 组件的 atlas 字段（显式）；② material 字段（spine 约定：指向 .atlas）；
         * ③ 与骨架同名的 .atlas（约定，最省事） */
        if (const std::string* ap = read_str(d, blob.data(), "atlas"); ap && !ap->empty())
            sp.atlas_path = vfs_path_of(*ap);
        else if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty())
            sp.atlas_path = vfs_path_of(*mp);
        else
            sp.atlas_path = stem_of(sp.skel_path) + ".atlas";
        sp.tex_dir = dir_of(sp.atlas_path);

        /* 骨架：从内存进（.skel = 二进制，.json = 文本；docs/27 的实测约定） */
        std::string ext;
        { const size_t dot = sp.skel_path.rfind('.');
          if (dot != std::string::npos) ext = sp.skel_path.substr(dot); }
        const int is_binary = (ext == ".json") ? 0 : 1;
        void* sd = nullptr; size_t sn = 0;
        if (mq_vfs_read(vfs, sp.skel_path.c_str(), &sd, &sn) != MQ_VFS_OK || !sd || sn == 0) {
            std::print(stderr, "runtime: spine skeleton read failed '{}'\n", sp.skel_path);
            continue;
        }
        std::vector<char> skel((char*)sd, (char*)sd + sn);
        mq_vfs_free(sd);
        void* ad = nullptr; size_t an = 0;
        if (mq_vfs_read(vfs, sp.atlas_path.c_str(), &ad, &an) != MQ_VFS_OK || !ad || an == 0) {
            std::print(stderr, "runtime: spine atlas read failed '{}'"
                               " (set the component's material field to the .atlas path)\n", sp.atlas_path);
            continue;
        }
        std::string atlas_text((char*)ad, an);
        mq_vfs_free(ad);
        atlas_text.push_back('\0');                            // spine 内部按 C 字符串解析

        sp.tex_ctx.eng = this;
        sp.tex_ctx.dir = sp.tex_dir;
        sp.data = mq_spine_data_create(skel.data(), sn, is_binary, atlas_text.c_str(), "",
                                       spine_texture_cb, &sp.tex_ctx);
        if (!sp.data) {
            std::print(stderr, "runtime: spine parse failed '{}' (binary={})\n", sp.skel_path, is_binary);
            continue;
        }
        sp.inst = mq_spine_instance_create(sp.data);
        if (!sp.inst) { mq_spine_data_destroy(sp.data); sp.data = nullptr; continue; }

        const int n_anim = mq_spine_data_animation_count(sp.data);
        const int n_skin = mq_spine_data_skin_count(sp.data);
        const int n_page = mq_spine_data_page_count(sp.data);
        for (int i = 0; i < n_anim; ++i) {
            const char* nm = mq_spine_data_animation_name(sp.data, i);
            sp.anim_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_skin; ++i) {
            const char* nm = mq_spine_data_skin_name(sp.data, i);
            sp.skin_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_page; ++i) {
            mq_spine_page pg{};
            if (mq_spine_data_page(sp.data, i, &pg) != MQ_SPINE_OK) continue;
            Spine::Page p;
            p.name = pg.name ? pg.name : "";
            p.width = pg.width; p.height = pg.height; p.handle = pg.texture; p.pma = pg.pma;
            sp.pages.push_back(std::move(p));
        }
        /* 初值：皮肤 / 动画 / 循环（组件字段是稳定路径，见 schema/world/anim.yaml） */
        int start_anim = (int)read_num(d, blob.data(), "animation", 0.0F);
        if (start_anim < 0 || start_anim >= n_anim) start_anim = 0;
        const int skin_idx = (int)read_num(d, blob.data(), "skin", 0.0F);
        if (skin_idx > 0 && skin_idx < n_skin)
            mq_spine_instance_set_skin(sp.inst, sp.skin_names[(size_t)skin_idx].c_str());
        /* 混合/时间缩放/槽区间：组件字段 → 实例（脚本可随时覆盖；见 host.yaml 的 spine_* 段） */
        mq_spine_instance_set_default_mix(sp.inst, read_num(d, blob.data(), "default_mix", 0.0F));
        mq_spine_instance_set_time_scale(sp.inst, read_num(d, blob.data(), "time_scale", 1.0F));
        const int sf = (int)read_num(d, blob.data(), "slots_from", -1.0F);
        const int st = (int)read_num(d, blob.data(), "slots_to", -1.0F);
        if (sf >= 0) mq_spine_instance_set_slots_range(sp.inst, sf, st);
        const bool autoplay = read_num(d, blob.data(), "autoplay", 1.0F) != 0.0F;
        if (autoplay && n_anim > 0) {
            const bool loop = read_num(d, blob.data(), "play_mode", 1.0F) == 1.0F;   // PlayMode::Loop = 1
            if (mq_spine_instance_play(sp.inst, sp.anim_names[(size_t)start_anim].c_str(), loop ? 1 : 0, 0) == MQ_SPINE_OK)
                sp.last_anim = start_anim;
        }
        /* 绘制缓冲：按"顶点数 ≈ 附件数 × 段数"的规模预留（骨架 4k 顶点足够；不足时下一帧扩容重试） */
        sp.verts.resize(8192); sp.uvs.resize(8192); sp.cols.resize(16384); sp.darks.resize(16384);
        sp.tverts.resize(8192);
        sp.idxs.resize(32768); sp.batches.resize(512);
        sp.z = (int)read_num(d, blob.data(), "z_index", 0.0F) * 100000 + id;
        /* instance 字段（transient）：给自省/脚本一个"实例存在"的证据（下标 + 1，0 = 无） */
        if (const mq_field_desc* f = mq_reflect_field(d, "instance"))
            *(uint64_t*)(blob.data() + f->offset) = (uint64_t)(spines.size() + 1);
        std::print("runtime: spine entity {} '{}' skel='{}' atlas='{}' anims={} skins={} pages={}\n",
                   id, e->name, sp.skel_path, sp.atlas_path, n_anim, n_skin, n_page);
        if (!sp.pages.empty()) {
            std::print("runtime: spine pages:");
            for (const auto& p : sp.pages)
                std::print(" [{} {}x{} tex={} pma={}]", p.name, p.width, p.height, (long long)p.handle, p.pma);
            std::print("\n");
        }
        spines.push_back(std::move(sp));
    }
    spines.shrink_to_fit();          // 之后不再增删 → tex_ctx / eng / 事件回调 user 的地址稳定
    /* 事件回调：**必须在向量定型之后**装（user = &spines[i]，push_back 会让旧地址失效）。
     * 装完即生效：之后任何 play/addAnimation/step 触发的动画事件都会同步投给脚本总线。 */
    for (auto& sp : spines) {
        sp.eng = this;
        mq_spine_instance_set_event_cb(sp.inst, spine_event_cb, &sp);
    }
    std::print("runtime: spine instances={}\n", spines.size());
}

void Engine::spine_step(float dt) {
    if (!scene) return;
    for (auto& sp : spines) {
        if (!sp.inst || !sp.data) continue;
        SceneNode* e = scene->at(sp.entity);
        const mq_type_desc* d = mq_reflect_type_by_id(reflect, sp.comp);
        if (!e || !d) continue;
        auto it = e->comps.find(sp.comp);
        if (it == e->comps.end() || it->second.empty()) continue;
        std::vector<uint8_t>& blob = it->second;

        /* 组件字段 → 语义（脚本改这些字段即生效；无需额外 API 也能驱动动画） */
        const int n_anim = (int)sp.anim_names.size();
        int want = (int)read_num(d, blob.data(), "animation", 0.0F);
        if (want < 0 || want >= n_anim) want = sp.last_anim;
        const bool loop = read_num(d, blob.data(), "play_mode", 1.0F) == 1.0F;
        if (want != sp.last_anim && want >= 0) {                 // 组件里换了动画 → 真的去播
            /* 但若该轨**已经在播**这个动画（例如 add_animation 排队到点自己切过去了），
             * 只同步 last_anim，不要重启（重启会白白打断播放头/触发一次 start+interrupt）。 */
            const char* cur = mq_spine_instance_track_animation(sp.inst, 0);
            if (cur && sp.anim_names[(size_t)want] == cur) {
                sp.last_anim = want;
            } else if (mq_spine_instance_play(sp.inst, sp.anim_names[(size_t)want].c_str(), loop ? 1 : 0, 0) == MQ_SPINE_OK) {
                sp.last_anim = want;
            }
        }
        const float speed = read_num(d, blob.data(), "speed", 1.0F);
        /* 物理时间轴：SpinePhysTimestep::NoPhysics(2) → 只应用已有物理姿势（Physics_Pose），
         * 其余（Inherit/Fixed）→ 正常更新（Physics_Update）。忽略这个字段会让"冻结物理"失效。 */
        const int phys_mode = (int)read_num(d, blob.data(), "phys_timestep", 0.0F);
        const int physics = (phys_mode == 2) ? MQ_SPINE_PHYSICS_POSE : MQ_SPINE_PHYSICS_UPDATE;
        mq_spine_instance_step_ex(sp.inst, dt * speed, physics);  // 固定步长（确定性，docs/10 D1）

        /* 变换：模块负责 缩放/翻转（骨架单位 → 屏幕单位），旋转/平移留给提取期（见 spine_extract） */
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, sp.entity, angle, scale, opacity);
        const int flip = (int)read_num(d, blob.data(), "flip", 0.0F);       // FlipMode：1=水平 2=垂直 3=两者
        mq_spine_instance_set_transform(sp.inst, 0.0F, 0.0F, scale,
                                        (flip & 1) ? 1 : 0, (flip & 2) ? 1 : 0);
        float cr = 1, cg = 1, cb = 1, ca = 1;
        if (const mq_field_desc* f = mq_reflect_field(d, "color"); f && f->kind == MQ_TK_STRUCT) {
            const mq_type_desc* ct = mq_reflect_type_by_id(reflect, f->type_id);
            const uint8_t* cp = blob.data() + f->offset;
            if (ct) {
                read_f32(ct, cp, "r", &cr); read_f32(ct, cp, "g", &cg);
                read_f32(ct, cp, "b", &cb); read_f32(ct, cp, "a", &ca);
            }
        }
        mq_spine_instance_set_color(sp.inst, cr, cg, cb, ca * opacity);

        /* 绘制数据（CPU 侧；只读导出，不改模拟状态） */
        mq_spine_draw out{};
        const int rc = mq_spine_instance_draw(sp.inst, sp.verts.data(), sp.uvs.data(), sp.cols.data(),
                                              sp.darks.data(), sp.idxs.data(), sp.batches.data(),
                                              (int)(sp.verts.size() / 2), (int)sp.idxs.size(),
                                              (int)sp.batches.size(), &out);
        if (rc != MQ_SPINE_OK) { sp.draw = mq_spine_draw{}; continue; }
        if (out.vertex_count >= (int)(sp.verts.size() / 2) || out.index_count >= (int)sp.idxs.size()) {
            /* 缓冲满 = 几何被截断（会少画附件）→ 翻倍后下一帧重试，并明确报一次 */
            sp.verts.resize(sp.verts.size() * 2); sp.uvs.resize(sp.uvs.size() * 2);
            sp.cols.resize(sp.cols.size() * 2); sp.darks.resize(sp.darks.size() * 2);
            sp.tverts.resize(sp.tverts.size() * 2);
            sp.idxs.resize(sp.idxs.size() * 2); sp.batches.resize(sp.batches.size() * 2);
            static bool warned = false;
            if (!warned) { warned = true;
                std::print(stderr, "runtime: spine draw buffers full -> growing"
                                   " (this frame's geometry may be truncated)\n"); }
        }
        sp.draw = out;
        sp.z = (int)read_num(d, blob.data(), "z_index", 0.0F) * 100000 + sp.entity;

        /* 骨架空间 → 渲染目标像素空间：
         *   · spine 顶点 y **向上**（实测；见本节顶部注释），引擎渲染目标 y 向下 → 翻一次；
         *   · 累计节点角度绕实体原点旋转，再平移到实体世界位置；
         *   · world 是 y 向上的世界坐标，屏幕 y = H - world_y。 */
        const float rad = angle * 3.14159265358979323846F / 180.0F;
        const float cs = std::cos(rad), sn = std::sin(rad);
        const float ox = e->world_x, oy = (float)H - e->world_y;
        const int vc = out.vertex_count;
        if ((int)(sp.tverts.size() / 2) < vc) sp.tverts.resize((size_t)vc * 2);
        for (int i = 0; i < vc; ++i) {
            const float lx = sp.verts[(size_t)i * 2], ly = sp.verts[(size_t)i * 2 + 1];
            const float rx = lx * cs - ly * sn;
            const float ry = lx * sn + ly * cs;
            sp.tverts[(size_t)i * 2] = ox + rx;
            sp.tverts[(size_t)i * 2 + 1] = oy - ry;
        }
        /* 诊断（MAQUE_TRACE_SPINE=1）：绘制数据规模 / 变换前后包围盒 / UV 统计 —— 排查"看不见"的第一手证据
         * （实测靠它定位过两个坑：mesh.vertex_count 忘填 → 渲染器静默不画；UV 取错 accessor → 全采样到空白） */
        if (std::getenv("MAQUE_TRACE_SPINE") && (frame % 20 == 0)) {
            float minx = 0, maxx = 0, miny = 0, maxy = 0;
            int uv_zero = 0; float u0 = 2, u1 = -1, v0 = 2, v1 = -1;
            for (int i = 0; i < vc; ++i) {
                const float x = sp.tverts[(size_t)i * 2], y = sp.tverts[(size_t)i * 2 + 1];
                if (i == 0) { minx = maxx = x; miny = maxy = y; }
                else { minx = std::min(minx, x); maxx = std::max(maxx, x);
                       miny = std::min(miny, y); maxy = std::max(maxy, y); }
                const float uu = sp.uvs[(size_t)i * 2], vv = sp.uvs[(size_t)i * 2 + 1];
                if (uu == 0.0F && vv == 0.0F) ++uv_zero;
                u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                v0 = std::min(v0, vv); v1 = std::max(v1, vv);
            }
            std::print("[spine] frame={} entity={} verts={} idx={} batches={} local_bbox=({:.1f},{:.1f},{:.1f},{:.1f})"
                       " screen_bbox=({:.1f},{:.1f})-({:.1f},{:.1f}) scale={:.2f} angle={:.1f} origin=({:.1f},{:.1f})"
                       " pages={} uv_zero={}/{} uv_span=u[{:.4f}..{:.4f}]v[{:.4f}..{:.4f}] events={}\n",
                       (long long)frame, sp.entity, out.vertex_count, out.index_count, out.batch_count,
                       out.bounds_x, out.bounds_y, out.bounds_w, out.bounds_h,
                       minx, miny, maxx, maxy, scale, angle, ox, oy, sp.pages.size(),
                       uv_zero, vc, u0, u1, v0, v1, sp.events_last);
        }
        /* transient 回写：自省/脚本能读到"正在播什么、播到哪"（不进快照/校验和） */
        if (const mq_field_desc* f = mq_reflect_field(d, "state"); f && f->kind == MQ_TK_STRUCT) {
            const mq_type_desc* st = mq_reflect_type_by_id(reflect, f->type_id);
            uint8_t* base = blob.data() + f->offset;
            if (st) {
                if (const mq_field_desc* ft = mq_reflect_field(st, "time"))
                    field_set_f32(ft, base, mq_spine_instance_time(sp.inst, 0));
                if (const mq_field_desc* ft = mq_reflect_field(st, "track"))
                    field_set_i32(ft, base, 0);
                if (const mq_field_desc* ft = mq_reflect_field(st, "loop"))
                    field_set_bool(ft, base, loop);
            }
        }
    }
}

void Engine::spine_extract(std::vector<mq_renderer_mesh>& out) {
    for (auto& sp : spines) {
        if (!sp.inst || sp.draw.vertex_count <= 0 || sp.draw.index_count <= 0) continue;
        out.reserve(out.size() + (size_t)sp.draw.batch_count);        for (int b = 0; b < sp.draw.batch_count; ++b) {
            const mq_spine_batch& sb = sp.batches[(size_t)b];
            mq_renderer_mesh m{};
            m.vertices = sp.tverts.data();
            m.uvs = sp.uvs.data();
            m.colors = sp.cols.data();
            m.dark_colors = sp.darks.data();     // 两色着色（TintBlack）暗色通道；长度同 colors
            m.indices = sp.idxs.data();
            m.vertex_count = (uint32_t)sp.draw.vertex_count;     // 顶点数（少了它 = 渲染器不画，且是静默的）
            m.first_index = sb.first_index;
            m.index_count = sb.index_count;
            m.base_vertex = 0;
            m.blend = sb.blend;
            m.layer = 0;
            m.z = sp.z;
            m.texture = -1;
            if (sb.page >= 0 && sb.page < (int)sp.pages.size()) {
                const Spine::Page& p = sp.pages[(size_t)sb.page];
                m.pma = p.pma;
                /* 句柄 = gfx 纹理 id + 1（0 表示未回填）→ 还原成引擎纹理 id 交给渲染器 */
                if (p.handle > 0) {
                    const int tex = (int)(p.handle - 1);
                    if (rapi && rapi->has_texture && rapi->has_texture(rself, tex)) m.texture = tex;
                }
            }
            out.push_back(m);
        }
    }
}

/* 批量截图落地：模板 → 文件名 → 写图；并判断"是不是最后一张"（是则退出）。
 * 模板里的 `{}` 换成帧号；没有 `{}` 就自动插到扩展名前（`out.png` → `out_0042.png`）。
 * 格式仍按扩展名（.png → PNG，其余 → PPM）。 */
void Engine::write_seq_shot(const std::vector<uint8_t>& px, int tw, int th) {
    std::string path = shot_seq;
    char num[32];
    std::snprintf(num, sizeof num, "%d", (int)frame);
    const size_t brace = path.find("{}");
    if (brace != std::string::npos) {
        path.replace(brace, 2, num);
    } else {
        const size_t dot = path.find_last_of('.');
        char pad[32];
        std::snprintf(pad, sizeof pad, "_%04d", (int)frame);
        if (dot == std::string::npos) path += pad;
        else path.insert(dot, pad);
    }
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    const int rc = png ? mq_image_write_png(path.c_str(), (uint32_t)tw, (uint32_t)th,
                                            px.data(), (uint32_t)tw * 4u)
                       : (write_ppm(path, tw, th, px) ? 0 : 1);
    if (rc == 0) std::print("runtime: shot[{}] '{}' {}x{} frame={}\n",
                            shots_taken + 1, path, tw, th, (int)frame);
    else std::print(stderr, "runtime: shot 写入失败 '{}'\n", path);
    ++shots_taken;
    /* 收尾条件：显式帧号列表 → 拍完最大帧即退；every 模式 → 拍够 count（count=0 → 由
     * MAQUE_MAX_FRAMES 控制，不自己退）。 */
    if (!shot_list.empty()) {
        const int last = *std::max_element(shot_list.begin(), shot_list.end());
        if ((int)frame >= last) quit = true;
    } else if (shot_count > 0 && shots_taken >= shot_count) {
        quit = true;
    }
}

/* ---- ImGui 调试面板（docs/27 §ImGui）------------------------------------------------
 *   ① **不进确定性**：面板是视图层 → 不写任何实体/组件、不进快照/校验和（docs/10）。
 *   ② **画在所有引擎图层之上**：网格 layer = MQ_IMGUI_LAYER（引擎 UI 是 1），
 *      在**引擎渲染完（含 ui pass）、present 之前**追加 → 截图里也有面板（可自证）。
 *   ③ **不点穿**：ImGui 想要鼠标/键盘时，清空本帧设备事件，世界输入即被阻断。
 *
 * 未启用时（默认）**一次都不碰** ImGui 上下文 —— tank/flappy 等程序零开销。
 */
/* 固定 UI 度量（**一次算定**）：字号/面板尺寸只由**屏幕逻辑分辨率 + DPI + 渲染倍率**
 * 决定，与游戏窗口大小、场景分辨率、视图数无关。
 *
 * 为什么不用窗口/渲染目标比（旧做法 `ps = win_w/TW`）：同一个面板在不同窗口里会
 * 一会儿过大一会儿过小（用户反馈"标题太大、有些窗口又偏小"）。SDK 侧 SDL 对
 * DPI-unaware 进程报的是**逻辑分辨率**（2560x1600 @150% → 1707x1067），
 * 所以 `屏幕高/1080` 这一条公式同时覆盖了"4K 屏要放大"和"150% 缩放要放大"。
 * 可用 `MAQUE_IMGUI_SCALE` 显式覆盖（1.0 = 默认）。
 * 字号：正文 15 逻辑像素（可用 `MAQUE_IMGUI_FONT_SIZE` 覆盖）、标题 = 正文 × 4/3
 * —— 标题只比正文大一档，不会像之前 26px:15px 那样压过内容。 */
void Engine::imgui_resolve_metrics() {
    imgui_screen_w = imgui_screen_h = 0;
    /* 只有**真实窗口**才取屏幕尺寸；headless（dummy 驱动）没有有意义的"屏幕"，
     * 保持 ui_scale=1.0（截图/CI 的基准）。 */
    if (window && platform) mq_platform_screen_size(platform, &imgui_screen_w, &imgui_screen_h);
    float ui = 1.0F;
    if (imgui_screen_h > 0) ui = (float)imgui_screen_h / 1080.0F;
    if (const char* e = std::getenv("MAQUE_IMGUI_SCALE"); e && *e) {
        const float v = (float)std::atof(e);
        if (v > 0.0F) ui = v;                        // 显式覆盖（相对 1080p 的倍率）
    }
    if (ui < 0.7F) ui = 0.7F;
    if (ui > 2.5F) ui = 2.5F;
    imgui_ui_scale = ui;
    float body = 15.0F;
    if (const char* e = std::getenv("MAQUE_IMGUI_FONT_SIZE"); e && *e) {
        const float v = (float)std::atof(e);
        if (v > 0.0F) body = v;                      // 正文字号（逻辑像素）
    }
    imgui_body_size = body;
    imgui_title_size = std::round(body * 4.0F / 3.0F);
}

void Engine::imgui_setup() {
    if (imgui_inited) return;
    /* 字体：复用文本模块已解析的路径（MAQUE_FONT → scenes/fonts/*.ttf → 系统）。
     * **必须给 TTF**：ImGui 默认字体只有 ASCII，中文会渲染成 '?'（实测）。 */
    text_setup();                                // 确保 font_path 已解析（幂等）
    imgui_resolve_metrics();                     // 屏幕分辨率 → 固定 UI 度量（一次）
    /* 窗口 绘制区/逻辑区 比：HiDPI 下 ImGui 显示空间 = 设备像素（见 imgui_frame_begin），
     * 字号/样式与输入坐标都按这个比例换算。 */
    imgui_content_scale = (window && platform) ? mq_platform_content_scale(platform) : 1.0F;
    if (imgui_content_scale <= 0.0F) imgui_content_scale = 1.0F;
    /* 呈现通道 = SDL_Renderer（游戏帧与面板共用同一个）：能拿到 renderer 就把面板交给
     * **官方后端**（它自己管坐标/裁剪/字体纹理/多窗口，拖动/缩放是官方行为），
     * 与引擎帧一起在**同一次 present** 上屏（见 step_frame 呈现段）。
     * 拿不到 renderer（headless / MAQUE_PRESENT=off / 无窗口）→ 自研网格链，
     * 面板直接写进引擎像素缓冲（--shot 可验证）。
     * ⚠️ 必须在 init 之前声明 backend 模式：否则会先走静态图集，再切官方后端会撞
     *    "Build() before RendererHasTextures" 断言（实测）。 */
    void* pre_ren = (window && platform && !direct_present) ? mq_platform_ensure_renderer(platform) : nullptr;
    /* 直接呈现（plans/18 M1）：窗口归 wgpu 交换链所有，不能再建 SDL_Renderer（SDL2 不允许混用）→
     * ImGui 走 CPU 像素 → 2D 网格的既有回退链，随 render_frame3d 的 meshes 提交。 */
    if (pre_ren) mq_imgui_set_backend_mode(1);
    /* 像素倍率：官方后端工作在 drawable 像素；网格链还需乘主输出 render_scale。 */
    const float ps = imgui_ui_scale * imgui_content_scale *
                     (pre_ren ? 1.0F : std::max(render_scale_req, 0.25F));
    imgui_pixel_scale = ps > 0.0F ? ps : 1.0F;
    if (mq_imgui_init_with_fonts(font_path.empty() ? nullptr : font_path.c_str(),
                                 imgui_title_size, imgui_body_size, imgui_pixel_scale) != 0) {
        std::print(stderr, "runtime: imgui init failed\n");
        imgui_on = false;
        return;
    }
    if (pre_ren) {
        if (mq_imgui_backend_init(pre_ren) == 0) {
            imgui_backend = true;
            std::print("runtime: imgui uses official SDL_Renderer backend (shared present)\n");
        } else {
            /* 官方后端初始化失败 → 回退网格链：把模式复位，让图集走静态 Build 路径。
             * （字体已按"窗口像素"倍率烘好；网格链下略小于目标空间比例，可接受——
             *  这条路径只在 renderer 存在但官方后端初始化失败时触发。） */
            mq_imgui_set_backend_mode(0);
            imgui_backend = false;
            std::print(stderr, "runtime: imgui official backend init failed → mesh-chain fallback\n");
        }
    }
    imgui_inited = true;
    imgui_tex = -1;
    imgui_tex_gen = 0;
    stat_window_sized = false;       // 新上下文：stats 窗口按固定度量重新定位/定尺寸
    /* 面板固定尺寸：正文 16 字宽 × 10 行高（含标题/内边距）——屏幕比例固定，不随窗口变。 */
    imgui_panel_w = std::round(imgui_body_size * 16.0F * imgui_pixel_scale);
    imgui_panel_h = std::round(imgui_body_size * 10.0F * imgui_pixel_scale);
    std::print("runtime: imgui ui scale={:.2f} screen={}x{} title={:.0f} body={:.0f} "
               "pixel={:.2f} panel={:.0f}x{:.0f}\n",
               imgui_ui_scale, imgui_screen_w, imgui_screen_h,
               imgui_title_size, imgui_body_size, imgui_pixel_scale, imgui_panel_w, imgui_panel_h);
}

/* 开关面板：关掉时释放上下文 + 字体纹理（不占 GPU/输入）；再开会重新初始化。 */
void Engine::imgui_enable(bool on, bool show_stats) {
    imgui_stats = on && show_stats;                 // F4 可复用 ImGui 上下文而不显示 F2 stats
    if (on == imgui_on && (on ? imgui_inited : !imgui_inited)) return;   // 幂等
    if (!on) {
        /* F2/脚本关掉整个 ImGui 时**同时收起 F4 面板状态**：
         * 否则"F4 开着 → F2 关 → F2 再开"会把引擎选项面板又带回来（F2、F4 互相干扰）。 */
        const bool had_opts = opt_panel;
        opt_panel = false;
        opt_opened_imgui = false;
        if (imgui_inited) {
            mq_imgui_shutdown();
            if (imgui_tex >= 0 && rapi && rself && rapi->destroy_texture)
                rapi->destroy_texture(rself, imgui_tex);
            imgui_tex = -1;
            imgui_tex_gen = 0;
            imgui_inited = false;
            imgui_frame_active = false;
            imgui_meshes.clear();
        }
        imgui_on = false;
        std::print("runtime: imgui panel OFF\n");
        if (had_opts) std::print("runtime: engine options panel OFF\n");
        return;
    }
    imgui_on = true;
    imgui_setup();
    std::print("runtime: imgui panel ON\n");
}

/* ImGui 的显示空间尺寸（= `mq_imgui_begin_frame` 的 show_w/show_h，也是导出顶点所在空间）。
 * 必须与**实际画到哪里**一致，否则鼠标命中会整体偏移（docs/bugs/010）：
 *   · 官方 SDL_Renderer 后端：绘制区**设备像素**（renderer 后台缓冲；HiDPI 下大于逻辑尺寸）；
 *   · 网格链（WGPU 直呈 / 无窗口 / present off）：渲染目标像素 TW×TH
 *     （gfx 对 MQ_IMGUI_LAYER 用恒等视图，顶点逐像素落在目标上）。 */
void Engine::imgui_show_size(float* w, float* h) const {
    float sw = (float)TW, sh = (float)TH;
    if (imgui_backend) {
        int dw = 0, dh = 0;
        if (platform) mq_platform_drawable_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) { sw = (float)dw; sh = (float)dh; }
        else if (win_w > 0 && win_h > 0) { sw = (float)win_w; sh = (float)win_h; }
    }
    if (w) *w = sw;
    if (h) *h = sh;
}

/* 窗口**逻辑**像素 → ImGui 显示空间 的倍率（等比）。
 * 无窗口（headless/截图）时指针本来就是目标像素 → 1。
 * ⚠️ 与 Canvas 的 scale/offset 无关：面板是屏幕空间覆盖层，不参与"设计坐标 → 目标"映射
 *    （旧实现按画布反算 → WGPU 直呈下点击/拖动整体偏移，见 docs/bugs/010）。 */
float Engine::imgui_show_scale() const {
    if (!window || !platform) return 1.0F;
    int ww = 0, wh = 0;
    mq_platform_window_size(platform, &ww, &wh);
    float sw = 0.0F, sh = 0.0F;
    imgui_show_size(&sw, &sh);
    if (ww <= 0 || !(sw > 0.0F)) return 1.0F;
    return sw / (float)ww;
}

/* 把本帧设备事件转发给 ImGui（**窗口逻辑像素坐标**；调用点在转成世界坐标**之前**）。
 * ImGui 的显示空间 = 渲染目标像素（网格链）或绘制区设备像素（官方后端），两者与窗口逻辑
 * 像素只差一个等比倍率 → 用 `imgui_show_scale()` 换算。**不能**再按 Canvas 的 offset/scale
 * 反算：面板是屏幕空间覆盖层，不跟随游戏视图的 zoom/平移（docs/bugs/010）。 */
void Engine::imgui_input_feed(const mq_platform_pointer_event* ptrs, uint32_t np,
                              const mq_platform_key_event* keys, uint32_t nk) {
    if (!imgui_on || !imgui_inited) return;
    float wheel = 0.0F;
    for (uint32_t i = 0; i < np; ++i) {
        const mq_platform_pointer_event& p = ptrs[i];
        imgui_ptr_x = p.x;
        imgui_ptr_y = p.y;
        if (p.type == MQ_PLATFORM_POINTER_DOWN && p.button == 0) imgui_ptr_down = true;
        else if (p.type == MQ_PLATFORM_POINTER_UP && p.button == 0) imgui_ptr_down = false;
        else if (p.type == MQ_PLATFORM_POINTER_WHEEL) wheel += p.scroll_y != 0.0F ? p.scroll_y : p.dy;
    }
    const float k = imgui_show_scale();
    mq_imgui_input_pointer(imgui_ptr_x * k, imgui_ptr_y * k, imgui_ptr_down ? 1 : 0, wheel);
    for (uint32_t i = 0; i < nk; ++i) {
        const mq_platform_key_event& k = keys[i];
        mq_imgui_input_key((int)k.key, k.down ? 1 : 0);
        if (k.down && k.text[0]) mq_imgui_input_chars(k.text);
    }
}

/* CI/无真鼠标：把 MAQUE_IMGUI_POINTER 声明的**合成指针事件**压进本帧设备队列。
 * 走与真实鼠标完全相同的路径（iframe → imgui_input_feed → ImGui），用于 e2e 验证拖动。
 * 格式："frame:x:y:type[;frame:x:y:type...]"，type 0=move 1=down 2=up。 */
void Engine::imgui_fake_input(mq_platform_pointer_event* ptrs, uint32_t& count, uint32_t cap) {
    if (!imgui_fake_parsed) {
        imgui_fake_parsed = true;
        const char* spec = std::getenv("MAQUE_IMGUI_POINTER");
        if (spec && *spec) {
            std::string s(spec), item;
            for (size_t i = 0; i <= s.size(); ++i) {
                const char c = i < s.size() ? s[i] : ';';
                if (c != ';') { if (c != ' ') item.push_back(c); continue; }
                int fr = 0, ty = 0; float x = 0, y = 0;
                if (item.size() > 0 && std::sscanf(item.c_str(), "%d:%f:%f:%d", &fr, &x, &y, &ty) == 4)
                    imgui_fake_ptr.push_back({ fr, x, y, ty });
                item.clear();
            }
            std::print("runtime: imgui fake pointer events={}\n", imgui_fake_ptr.size());
        }
    }
    for (const ImguiFakePointer& ev : imgui_fake_ptr) {
        if ((uint64_t)ev.frame != frame || count >= cap) continue;
        mq_platform_pointer_event& e = ptrs[count++];
        std::memset(&e, 0, sizeof e);
        e.pointer_id = 0;
        e.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
        e.button = 0;
        e.type = ev.type == 1 ? MQ_PLATFORM_POINTER_DOWN
               : ev.type == 2 ? MQ_PLATFORM_POINTER_UP
                              : MQ_PLATFORM_POINTER_MOVE;
        e.x = ev.x;
        e.y = ev.y;
        e.pressure = ev.type == 1 ? 1.0F : 0.0F;
    }
}

void Engine::imgui_frame_begin(float dt) {
    /* 首次调用时决定初始开关：**默认关**（ImGui 是开发工具，不默认覆盖游戏画面）。
     * 打开：启动期 `MAQUE_IMGUI=1`（含 headless/截图），或运行时 F2 / 脚本
     * `imgui_set_enabled(1)`；`MAQUE_IMGUI=0` 只是显式初值（F2 仍可再开）。 */
    if (!imgui_env_checked) {
        imgui_env_checked = true;
        const char* e = std::getenv("MAQUE_IMGUI");
        if (e && *e && std::strcmp(e, "0") != 0) {
            imgui_on = true;
            imgui_stats = true;
            std::print("runtime: imgui on (MAQUE_IMGUI) (F2 toggles)\n");
        }
    }
    if (!imgui_on) return;
    if (!imgui_inited) imgui_setup();
    if (!imgui_inited) return;
    const int target_w = TW;
    const int target_h = TH;
    if (target_w <= 0 || target_h <= 0) return;

    /* 字体图集（**静态路径**）：必须在 `mq_imgui_begin_frame` **之前** Build + 上传。
     * 为什么顺序反了就不行：ImGui 的 legacy 后端路径要求"NewFrame 时图集已 Build"
     * （imgui_draw.cpp: `IM_ASSERT_USER_ERROR(atlas->TexIsBuilt, ...)`），
     * 而 `mq_imgui_font_pixels()` 正是做 Build 的那一步。所以：
     *   ① 首次：取像素（内部 Build）→ 上传纹理 → `SetTexID` 绑定；
     *   ② 之后：图集内容不再变（静态），直接跳过。 */
    if (!imgui_backend && (imgui_tex < 0 || mq_imgui_font_dirty())) {
        int fw = 0, fh = 0;
        const uint8_t* px = mq_imgui_font_pixels(&fw, &fh);
        if (px && fw > 0 && fh > 0 && rapi && rself && rapi->create_texture) {
            if (imgui_tex >= 0 && rapi->destroy_texture) rapi->destroy_texture(rself, imgui_tex);
            imgui_tex = rapi->create_texture(rself, px, fw, fh);
            if (imgui_tex >= 0) mq_imgui_set_font_texture((uint64_t)(imgui_tex + 1));
            std::print("runtime: imgui font atlas {}x{} → texture {}\n", fw, fh, imgui_tex);
            /* MAQUE_IMGUI_DUMP_ATLAS=<file.raw>：把实际传给 GPU 的图集落盘（RGBA32），
             * 用来区分"图集里没字形"和"采不到字形"。 */
            if (const char* d = std::getenv("MAQUE_IMGUI_DUMP_ATLAS"); d && *d) {
                std::ofstream f(d, std::ios::binary);
                f.write(reinterpret_cast<const char*>(px), (std::streamsize)fw * fh * 4);
                std::print("runtime: imgui atlas dumped '{}' {}x{}\n", d, fw, fh);
            }
        }
    }

    /* 显示空间（docs/bugs/010）：见 `imgui_show_size()` —— 官方后端 = 绘制区设备像素，
     * 网格链 = 渲染目标像素。字号/面板尺寸都是**固定 UI 度量**（imgui_resolve_metrics），
     * 与窗口/目标大小无关。 */
    float show_w = 0.0F, show_h = 0.0F;
    imgui_show_size(&show_w, &show_h);
    const float scale = 1.0F, off_x = 0.0F, off_y = 0.0F;
    const float dpi = 1.0F;
    if (imgui_backend) mq_imgui_backend_new_frame();   // 官方后端的 NewFrame（须在 ImGui::NewFrame 之前）
    mq_imgui_begin_frame(show_w, show_h, (float)target_w, (float)target_h,
                        scale, off_x, off_y, dt, dpi);
    imgui_frame_active = true;

    /* ---- stats 面板（打开时）：所有游戏通用，左上角，无需脚本参与 ----
     * · 尺寸/字号 = **屏幕分辨率派生的固定值**（`imgui_panel_w/h`）——同一块屏上
     *   无论游戏窗口多大、场景分辨率多少，面板看起来都一样大（用户反馈"标题太大/
     *   有些窗口又偏小"的修复点）；只有窗口小到放不下时才按比例收缩。
     * · 位置/尺寸只在**首帧**设一次（FirstUseEver）——之后用户拖动/缩放不被复位。 */
    if (mq_imgui_ready()) {
        if (imgui_stats) {
            if (!stat_window_sized) {
                const float pad = 8.0F * imgui_pixel_scale;
                const float min_w = 80.0F * imgui_pixel_scale;
                const float min_h = 60.0F * imgui_pixel_scale;
                const float w = std::min(imgui_panel_w, show_w * 0.85F);
                const float h = std::min(imgui_panel_h, show_h * 0.75F);
                mq_imgui_set_pos(pad, pad);
                mq_imgui_set_size(w > min_w ? w : min_w, h > min_h ? h : min_h);
                stat_window_sized = true;
            }
            if (mq_imgui_begin_overlay("stats")) {
                mq_imgui_push_small_font();     // 内容用正文字号（图集烘了标题/正文两档）
                char line[160];
                std::snprintf(line, sizeof line, "FPS %.0f  (%.1f ms)",
                              stat_fps, stat_fps > 0 ? 1000.0F / stat_fps : 0.0F);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "logic %.1f ms  render %.1f ms", stat_logic_ms, stat_render_ms);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "UI draw %d  3D draw %d", stat_ui_calls, stat_3d_calls);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "tris %d  verts %d", stat_tris, stat_verts);
                mq_imgui_text(line);
                if (stat_vram_bytes < 1024u * 1024u)
                    std::snprintf(line, sizeof line, "VRAM ~%.1f KiB", stat_vram_bytes / 1024.0);
                else
                    std::snprintf(line, sizeof line, "VRAM ~%.2f MiB", stat_vram_bytes / (1024.0 * 1024.0));
                mq_imgui_text(line);
                /* 内存域（docs/29 §10）：明细一行 + 分区（冷暖）一行；关闭时显示 "off"。 */
                {
                    char memline[224];
                    if (maque::tracy::memory_available() && maque::tracy::memory_enabled()) {
                        maque::tracy::memory_format(memline, sizeof memline);
                        std::snprintf(line, sizeof line, "MEM %s", memline);
                        mq_imgui_text(line);
                        maque::tracy::memory_area_format(memline, sizeof memline);
                        std::snprintf(line, sizeof line, "MEM %s", memline);
                        mq_imgui_text(line);
                    } else {
                        mq_imgui_text("MEM off");
                    }
                }
                std::snprintf(line, sizeof line, "frame %llu  target %dx%d",
                              (unsigned long long)frame, target_w, target_h);
                mq_imgui_text(line);
                mq_imgui_pop_small_font();
            }
            mq_imgui_end();
        }
        /* F4 引擎选项面板可独立显示；只有 F2 / 脚本 / MAQUE_IMGUI 才打开 stats。 */
        if (opt_panel) engine_options_panel(show_w, show_h);
    }

    /* 注：③ "不点穿"（清空本帧设备事件）放在调用点 —— iframe 是 step_frame 的局部量。 */
}

/* ---- F4 引擎选项（docs/28）----------------------------------------------------
 * 「引擎选项」面板：渲染模式 / 游戏 3D、2D/UI 图层 / MSAA / 默认太阳光 / 主相机 near·far /
 * 后处理 PostEffect。
 *   · 纯视图层：不写场景组件、不进快照/校验和、不参与模拟（docs/10 确定性不受影响）；
 *   · 值在 extract_3d / 成帧时读取（Engine 的 opt_* 字段），切场景后仍保留；
 *   · 启动初值可用 `MAQUE_ENGINE_OPTIONS`（CI/截图/演示不必依赖鼠标拖控件）。
 * 环境变量格式（逗号分隔；向量分量用 ':'）：
 *   MAQUE_ENGINE_OPTIONS="mode=textured,3d=1,2d=0,msaa=4,rt_msaa=4,sun=0,near=0.01,far=500"
 * 键：mode(rendered|textured|solid|wireframe) / 3d(0|1) / 2d(0|1) / msaa(0|1|4) /
 * rt_msaa(0|1|4 = 相机离屏纹理的全局 MSAA，只作用于 3D 相机目标) /
 * sun(0|1) / sun_intensity / sun_dir(x:y:z) / near / far /
 * post(0|1) / tonemap(none|aces) / exposure / bloom(0|1) / bloom_intensity / bloom_threshold /
 * bloom_radius / vignette(0|1) / vignette_intensity / chromatic(0|1) / chromatic_strength /
 * grade(0|1) / saturation / contrast / gray / post_2d(0|1)
 * near 或 far 任一出场即启用主相机 near/far 覆盖；出现任何后处理键会自动打开总开关
 * （显式写 post=0 则在最后被尊重）。后处理**默认只叠加 3D 层**；post_2d=1 才含 2D/UI。 */

/* 后处理效果位（与 mq_renderer_post.effects / post/shader.wgsl 一致）。 */
enum : uint32_t {
    MQ_POST_BLOOM     = 1u << 0,
    MQ_POST_VIGNETTE  = 1u << 1,
    MQ_POST_CHROMATIC = 1u << 2,
    MQ_POST_GRADE     = 1u << 3,
};

/* opt_post_* → frame3d.post。关闭时只填 enabled=0（其余留 0）→ gfx 不走后处理通路。 */
void Engine::engine_options_fill_post(mq_renderer_post& out) const {
    std::memset(&out, 0, sizeof out);
    out.exposure = 1.0F;
    if (!opt_post_enabled) return;
    out.enabled = 1;
    out.tonemap = opt_post_tonemap;
    out.exposure = opt_post_exposure;
    out.effects = (opt_post_bloom ? MQ_POST_BLOOM : 0u) |
                  (opt_post_vignette ? MQ_POST_VIGNETTE : 0u) |
                  (opt_post_chromatic ? MQ_POST_CHROMATIC : 0u) |
                  (opt_post_grade ? MQ_POST_GRADE : 0u);
    out.bloom_intensity = opt_post_bloom_intensity;
    out.bloom_threshold = opt_post_bloom_threshold;
    out.bloom_radius = opt_post_bloom_radius;
    out.vignette = opt_post_vignette_intensity;
    out.aberration = opt_post_chromatic_strength;
    out.saturation = opt_post_saturation;
    out.contrast = opt_post_contrast;
    out.grayscale = opt_post_grayscale;
    out.include_2d = opt_post_include_2d ? 1 : 0;
}

/* 后处理状态变化时打印一行（面板调参/环境初值都可见，CI 可断言）。 */
void Engine::engine_options_log_post() {
    std::string fx;
    auto add = [&fx](bool on, const char* name) {
        if (!on) return;
        if (!fx.empty()) fx.push_back(',');
        fx += name;
    };
    add(opt_post_bloom, "bloom");
    add(opt_post_vignette, "vignette");
    add(opt_post_chromatic, "chromatic");
    add(opt_post_grade, "grade");
    std::print("runtime: engine option post={} effects={} exposure={:.2f} tonemap={} scope={}\n",
               opt_post_enabled ? "on" : "off", fx.empty() ? "-" : fx.c_str(),
               (double)opt_post_exposure,
               opt_post_tonemap ? "aces" : "none",
               opt_post_include_2d ? "all" : "3d");
}

void Engine::engine_options_parse_env() {
    if (opt_env_parsed) return;
    opt_env_parsed = true;
    const char* e = std::getenv("MAQUE_ENGINE_OPTIONS");
    if (!e || !*e) return;
    /* 出现任何后处理键 → 自动打开总开关（只要没显式写 post=0）。 */
    bool post_touched = false;
    bool post_explicit = false;
    std::string s(e), item;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = i < s.size() ? s[i] : ',';
        if (c != ',') { if (c != ' ' && c != '\t') item.push_back(c); continue; }
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        const std::string key = eq == std::string::npos ? item : item.substr(0, eq);
        const std::string val = eq == std::string::npos ? std::string() : item.substr(eq + 1);
        const bool on = !(val == "0" || val == "off" || val == "false");
        if (key == "mode") {
            opt_render_mode = val == "wireframe" ? 2 : (val == "solid" ? 1 : (val == "textured" ? 3 : 0));
        } else if (key == "3d") {
            opt_3d_enabled = on;
        } else if (key == "2d") {
            opt_2d_enabled = on;
        } else if (key == "msaa") {
            const int m = std::atoi(val.c_str());
            opt_msaa = m >= 4 ? 4 : (m == 1 ? 1 : 0);
        } else if (key == "rt_msaa") {
            /* 相机离屏纹理的全局 MSAA 覆盖（只作用于 3D 相机目标，不动主输出）。 */
            const int m = std::atoi(val.c_str());
            opt_rt_msaa = m >= 4 ? 4 : (m == 1 ? 1 : 0);
        } else if (key == "sun") {
            opt_sun_enabled = on;
        } else if (key == "sun_intensity") {
            opt_sun_intensity = std::clamp((float)std::atof(val.c_str()), 0.0F, 20.0F);
        } else if (key == "sun_dir") {
            float x = 0.0F, y = 0.0F, z = 0.0F;
            if (std::sscanf(val.c_str(), "%f:%f:%f", &x, &y, &z) == 3) {
                opt_sun_dir[0] = x; opt_sun_dir[1] = y; opt_sun_dir[2] = z;
            }
        } else if (key == "near") {
            opt_cam_near = std::max((float)std::atof(val.c_str()), 1e-4F);
            opt_cam_override = true;
        } else if (key == "far") {
            opt_cam_far = std::max((float)std::atof(val.c_str()), 1e-3F);
            opt_cam_override = true;
        } else if (key == "post") {
            opt_post_enabled = on;
            post_explicit = true;
        } else if (key == "tonemap") {
            opt_post_tonemap = (val == "aces") ? 1 : 0;
            post_touched = true;
        } else if (key == "exposure") {
            opt_post_exposure = std::clamp((float)std::atof(val.c_str()), 0.05F, 8.0F);
            post_touched = true;
        } else if (key == "bloom") {
            opt_post_bloom = on;
            post_touched = true;
        } else if (key == "bloom_intensity") {
            opt_post_bloom_intensity = std::clamp((float)std::atof(val.c_str()), 0.0F, 4.0F);
            post_touched = true;
        } else if (key == "bloom_threshold") {
            opt_post_bloom_threshold = std::clamp((float)std::atof(val.c_str()), 0.0F, 1.0F);
            post_touched = true;
        } else if (key == "bloom_radius") {
            opt_post_bloom_radius = std::clamp((float)std::atof(val.c_str()), 0.001F, 0.15F);
            post_touched = true;
        } else if (key == "vignette") {
            opt_post_vignette = on;
            post_touched = true;
        } else if (key == "vignette_intensity") {
            opt_post_vignette_intensity = std::clamp((float)std::atof(val.c_str()), 0.0F, 1.0F);
            post_touched = true;
        } else if (key == "chromatic") {
            opt_post_chromatic = on;
            post_touched = true;
        } else if (key == "chromatic_strength") {
            opt_post_chromatic_strength = std::clamp((float)std::atof(val.c_str()), 0.0F, 0.05F);
            post_touched = true;
        } else if (key == "grade") {
            opt_post_grade = on;
            post_touched = true;
        } else if (key == "saturation") {
            opt_post_saturation = std::clamp((float)std::atof(val.c_str()), 0.0F, 3.0F);
            post_touched = true;
        } else if (key == "contrast") {
            opt_post_contrast = std::clamp((float)std::atof(val.c_str()), 0.0F, 3.0F);
            post_touched = true;
        } else if (key == "gray") {
            opt_post_grayscale = std::clamp((float)std::atof(val.c_str()), 0.0F, 1.0F);
            post_touched = true;
        } else if (key == "post_2d") {
            /* 显式把 2D/UI 也纳入后处理（默认只叠加 3D 层）。 */
            opt_post_include_2d = on;
            post_touched = true;
        }
        item.clear();
    }
    if (post_touched && !post_explicit) opt_post_enabled = true;
    static const char* const kModeNames[4] = { "rendered", "solid", "wireframe", "textured" };
    std::print("runtime: engine options (env) mode={} 3d={} 2d={} msaa={} rt_msaa={} sun={} intensity={:.2f} cam={}",
               kModeNames[opt_render_mode], opt_3d_enabled ? "on" : "off", opt_2d_enabled ? "on" : "off",
               opt_msaa == 0 ? "scene" : (opt_msaa == 4 ? "4x" : "off"),
               opt_rt_msaa == 0 ? "scene" : (opt_rt_msaa == 4 ? "4x" : "off"),
               opt_sun_enabled ? "on" : "off", (double)opt_sun_intensity,
               opt_cam_override ? "override" : "scene");
    if (opt_cam_override)
        std::print(" near={:.3f} far={:.3f}", (double)opt_cam_near, (double)opt_cam_far);
    std::print("\n");
    engine_options_log_post();
}

/* F4 面板内容。调用方保证本帧 ImGui 已 begin（`imgui_frame_begin`）。
 * 布局：默认贴在 stats 面板右侧；窗口太窄时夹紧在窗口内。
 * 位置/尺寸用 FirstUseEver 语义（`mq_imgui_begin_at`）→ 用户拖动/缩放后不会被每帧复位。 */
void Engine::engine_options_panel(float show_w, float show_h) {
    const float pad = 8.0F * imgui_pixel_scale;
    const float gap = 8.0F * imgui_pixel_scale;
    const float w = std::min(imgui_panel_w * 1.30F, std::max(show_w * 0.90F, 120.0F));
    /* 高度：常用项 + 后处理折叠头一行；展开后靠 ImGui 滚动条（窗口太矮时夹到 85%）。 */
    const float h = std::min(imgui_panel_h * 4.05F, std::max(show_h * 0.85F, 100.0F));
    float x = imgui_panel_w > 0.0F ? pad + imgui_panel_w + gap : pad;
    if (x + w > show_w - pad) x = std::max(pad, show_w - w - pad);
    if (!mq_imgui_begin_at("engine options", x, pad, w, h)) { mq_imgui_end(); return; }
    mq_imgui_push_small_font();
    mq_imgui_push_item_width(std::min(w * 0.50F, 150.0F * imgui_pixel_scale));

    mq_imgui_text("选项：只影响本机画面，不改场景");
    mq_imgui_text_disabled("F4 开关本面板 / F2 开关 stats");
    mq_imgui_separator();

    /* ① 调试渲染与游戏图层：只改提交路径，不隐藏 F4 面板自身。 */
    const int mode_sel = opt_render_mode == 2 ? 0
                       : opt_render_mode == 1 ? 1
                       : opt_render_mode == 3 ? 2 : 3;
    const int new_mode_sel = (int)mq_imgui_combo("渲染模式", (float)mode_sel,
                                                  "wireframe,solid,textured,rendered");
    const int new_mode = new_mode_sel == 0 ? 2
                       : new_mode_sel == 1 ? 1
                       : new_mode_sel == 2 ? 3 : 0;
    if (new_mode != opt_render_mode) {
        opt_render_mode = new_mode;
        static const char* const kModeNames[4] = { "rendered", "solid", "wireframe", "textured" };
        std::print("runtime: engine option mode={}\n", kModeNames[opt_render_mode]);
    }
    const bool old_3d = opt_3d_enabled, old_2d = opt_2d_enabled;
    opt_3d_enabled = mq_imgui_checkbox("3D", opt_3d_enabled ? 1.0F : 0.0F) != 0.0F;
    opt_2d_enabled = mq_imgui_checkbox("2D / UI", opt_2d_enabled ? 1.0F : 0.0F) != 0.0F;
    if (old_3d != opt_3d_enabled)
        std::print("runtime: engine option 3d={}\n", opt_3d_enabled ? "on" : "off");
    if (old_2d != opt_2d_enabled)
        std::print("runtime: engine option 2d={}\n", opt_2d_enabled ? "on" : "off");
    mq_imgui_separator();

    /* ② MSAA（**主输出**）：0=跟随场景（与 MAQUE_MSAA），1=关，4=4x */
    const int msaa_sel = opt_msaa == 4 ? 2 : (opt_msaa == 1 ? 1 : 0);
    const int new_sel = (int)mq_imgui_combo("MSAA", (float)msaa_sel, "跟随场景,关闭,4x");
    const int new_msaa = new_sel == 2 ? 4 : (new_sel == 1 ? 1 : 0);
    if (new_msaa != opt_msaa) {
        opt_msaa = new_msaa;
        std::print("runtime: engine option msaa={}\n",
                   opt_msaa == 4 ? "4x" : (opt_msaa == 1 ? "off" : "scene"));
    }
    if (opt_last_msaa >= 4) mq_imgui_text("实际：4x MSAA");
    else mq_imgui_text("实际：关闭（1x）");
    /* ②b RenderTexture MSAA：只管**相机离屏目标**（render.RenderTexture），不动上面那个主输出开关。
     * 「跟随」= 交给 MAQUE_MSAA / 各目标的 msaa_samples 字段 / 场景字段决定。
     * 更专门的开关赢：这里选了 关/4x，就覆盖 ②的选择与场景字段（面板上直接说清楚，避免两处对着猜）。 */
    const int rt_msaa_sel = opt_rt_msaa == 4 ? 2 : (opt_rt_msaa == 1 ? 1 : 0);
    const int new_rt_sel = (int)mq_imgui_combo("RenderTexture MSAA", (float)rt_msaa_sel, "跟随,关闭,4x");
    const int new_rt_msaa = new_rt_sel == 2 ? 4 : (new_rt_sel == 1 ? 1 : 0);
    if (new_rt_msaa != opt_rt_msaa) {
        opt_rt_msaa = new_rt_msaa;
        std::print("runtime: engine option rt_msaa={}\n",
                   opt_rt_msaa == 4 ? "4x" : (opt_rt_msaa == 1 ? "off" : "scene"));
    }
    {
        char line[160];
        if (opt_last_rt_total <= 0)
            std::snprintf(line, sizeof line, "实际：无 3D 相机离屏目标（2D 目标不吃 MSAA）");
        else
            std::snprintf(line, sizeof line, "实际：%d/%d 张相机纹理 4x",
                          opt_last_rt_msaa4, opt_last_rt_total);
        mq_imgui_text(line);
    }
    mq_imgui_text_disabled("多采样只作用于 3D pass；离屏纹理本体仍单采样（pass 末 resolve）");
    mq_imgui_separator();

    /* ③ 默认太阳光：**只在场景没有任何光源时**补光（老场景的兜底行为可开关/调参） */
    mq_imgui_text("默认太阳光（场景无光源时补光）");
    opt_sun_enabled = mq_imgui_checkbox("启用", opt_sun_enabled ? 1.0F : 0.0F) != 0.0F;
    opt_sun_intensity = mq_imgui_slider_float("强度", opt_sun_intensity, 0.0F, 4.0F);
    for (int i = 0; i < 3; ++i) {
        /* 光的**传播方向**（shader 里 normalize(-dir) 得到入射方向）；分量为 0 会退化 → 夹一下 */
        static const char* kAxis[3] = { "方向 X", "方向 Y", "方向 Z" };
        opt_sun_dir[i] = std::clamp(mq_imgui_drag_float(kAxis[i], opt_sun_dir[i], 0.01F), -4.0F, 4.0F);
    }
    {
        const float len = std::fabs(opt_sun_dir[0]) + std::fabs(opt_sun_dir[1]) + std::fabs(opt_sun_dir[2]);
        if (len < 1e-4F) opt_sun_dir[2] = -1.0F;      // 全 0 → 给一个合法方向（避免 normalize(0)）
    }
    if (opt_last_sun) mq_imgui_text("实际：使用默认太阳光");
    else mq_imgui_text_disabled("实际：未使用（场景已有光源或已关闭）");
    mq_imgui_separator();

    /* ④ 主相机 near/far 覆盖（覆盖关闭时字段跟随场景实际值，勾选后不跳变） */
    mq_imgui_text("主相机 near / far");
    opt_cam_override = mq_imgui_checkbox("覆盖场景相机", opt_cam_override ? 1.0F : 0.0F) != 0.0F;
    if (opt_cam_override) {
        opt_cam_near = std::max(mq_imgui_drag_float("near", opt_cam_near, 0.005F), 1e-4F);
        opt_cam_far = mq_imgui_drag_float("far", opt_cam_far, 0.5F);
        if (opt_cam_far < opt_cam_near * 1.001F) opt_cam_far = opt_cam_near * 1.001F;
    } else {
        mq_imgui_text_disabled("跟随场景相机的 near/far");
    }
    {
        char line[160];
        std::snprintf(line, sizeof line, "实际：near=%.4f far=%.2f",
                      (double)opt_last_near, (double)opt_last_far);
        mq_imgui_text(line);
    }
    mq_imgui_separator();

    /* ⑤ 后处理 PostEffect（docs/13 §后处理）——视图层覆盖，不改场景。
     * 折叠头（默认收起）：面板高度留给常用项，展开后调参；总开关在折叠头内。
     * 默认只叠加 3D 层（2D/UI 不受影响），勾「包含 2D / UI」才整帧。
     * 执行顺序 = 采样（含色差）→ 曝光 → 色调映射 → Bloom → 暗角 → 色彩分级。 */
    if (mq_imgui_collapsing_header("后处理 PostEffect")) {
        const bool old_enabled = opt_post_enabled;
        /* label 带 ##post：与「默认太阳光」的「启用」同名会触发 ImGui ID 冲突（显示仍是「启用」）。 */
        opt_post_enabled = mq_imgui_checkbox("启用##post", opt_post_enabled ? 1.0F : 0.0F) != 0.0F;
        if (old_enabled != opt_post_enabled) engine_options_log_post();
        if (opt_post_enabled) {
            /* 作用范围：默认**只叠加 3D 层**（2D/UI 原样叠在 post 结果之上）。 */
            {
                const bool old = opt_post_include_2d;
                opt_post_include_2d = mq_imgui_checkbox("包含 2D / UI", opt_post_include_2d ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_include_2d) engine_options_log_post();
            }
            if (!opt_post_include_2d)
                mq_imgui_text_disabled("默认只叠加 3D 层（2D / UI 不受影响）");
            /* 曝光 + 色调映射 */
            {
                const int sel = opt_post_tonemap ? 1 : 0;
                const int nsel = (int)mq_imgui_combo("色调映射", (float)sel, "无,ACES");
                const int ntm = nsel == 1 ? 1 : 0;
                if (ntm != opt_post_tonemap) { opt_post_tonemap = ntm; engine_options_log_post(); }
                const float nexp = mq_imgui_slider_float("曝光", opt_post_exposure, 0.10F, 4.0F);
                if (nexp != opt_post_exposure) { opt_post_exposure = nexp; engine_options_log_post(); }
            }
            /* Bloom 泛光 */
            {
                const bool old = opt_post_bloom;
                opt_post_bloom = mq_imgui_checkbox("Bloom 泛光", opt_post_bloom ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_bloom) engine_options_log_post();
                if (opt_post_bloom) {
                    const float ni = mq_imgui_slider_float("强度##bloom", opt_post_bloom_intensity, 0.0F, 3.0F);
                    if (ni != opt_post_bloom_intensity) { opt_post_bloom_intensity = ni; engine_options_log_post(); }
                    const float nt = mq_imgui_slider_float("阈值##bloom", opt_post_bloom_threshold, 0.0F, 1.0F);
                    if (nt != opt_post_bloom_threshold) { opt_post_bloom_threshold = nt; engine_options_log_post(); }
                    const float nr = mq_imgui_drag_float("半径##bloom", opt_post_bloom_radius, 0.001F);
                    const float clamped = std::clamp(nr, 0.001F, 0.15F);
                    if (clamped != opt_post_bloom_radius) { opt_post_bloom_radius = clamped; engine_options_log_post(); }
                }
            }
            /* 暗角 */
            {
                const bool old = opt_post_vignette;
                opt_post_vignette = mq_imgui_checkbox("暗角", opt_post_vignette ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_vignette) engine_options_log_post();
                if (opt_post_vignette) {
                    const float nv = mq_imgui_slider_float("强度##vignette", opt_post_vignette_intensity, 0.0F, 1.0F);
                    if (nv != opt_post_vignette_intensity) { opt_post_vignette_intensity = nv; engine_options_log_post(); }
                }
            }
            /* 色差 */
            {
                const bool old = opt_post_chromatic;
                opt_post_chromatic = mq_imgui_checkbox("色差", opt_post_chromatic ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_chromatic) engine_options_log_post();
                if (opt_post_chromatic) {
                    const float na = mq_imgui_slider_float("强度##chromatic", opt_post_chromatic_strength, 0.0F, 0.02F);
                    if (na != opt_post_chromatic_strength) { opt_post_chromatic_strength = na; engine_options_log_post(); }
                }
            }
            /* 色彩分级（饱和度 / 对比度 / 灰度） */
            {
                const bool old = opt_post_grade;
                opt_post_grade = mq_imgui_checkbox("色彩分级", opt_post_grade ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_grade) engine_options_log_post();
                if (opt_post_grade) {
                    const float ns = mq_imgui_slider_float("饱和度", opt_post_saturation, 0.0F, 2.0F);
                    if (ns != opt_post_saturation) { opt_post_saturation = ns; engine_options_log_post(); }
                    const float nc = mq_imgui_slider_float("对比度", opt_post_contrast, 0.25F, 2.0F);
                    if (nc != opt_post_contrast) { opt_post_contrast = nc; engine_options_log_post(); }
                    const float ng = mq_imgui_slider_float("灰度", opt_post_grayscale, 0.0F, 1.0F);
                    if (ng != opt_post_grayscale) { opt_post_grayscale = ng; engine_options_log_post(); }
                }
            }
        } else {
            mq_imgui_text_disabled("关闭（不改画面；开启后效果叠加在 3D 上层）");
        }
    }
    mq_imgui_separator();

    if (mq_imgui_button("重置为默认")) {
        opt_render_mode = 0;
        opt_3d_enabled = true;
        opt_2d_enabled = true;
        opt_msaa = 0;
        opt_rt_msaa = 0;
        opt_sun_enabled = true;
        opt_sun_intensity = 1.1F;
        opt_sun_dir[0] = 0.35F; opt_sun_dir[1] = -0.85F; opt_sun_dir[2] = -0.4F;
        opt_cam_override = false;            // near/far 下一帧自动跟随场景
        opt_post_enabled = false;
        opt_post_tonemap = 0;
        opt_post_exposure = 1.0F;
        opt_post_bloom = false;
        opt_post_bloom_intensity = 0.85F;
        opt_post_bloom_threshold = 0.62F;
        opt_post_bloom_radius = 0.018F;
        opt_post_vignette = false;
        opt_post_vignette_intensity = 0.55F;
        opt_post_chromatic = false;
        opt_post_chromatic_strength = 0.0022F;
        opt_post_grade = false;
        opt_post_saturation = 1.0F;
        opt_post_contrast = 1.0F;
        opt_post_grayscale = 0.0F;
        opt_post_include_2d = false;
        std::print("runtime: engine options reset\n");
    }

    mq_imgui_pop_item_width();
    mq_imgui_pop_small_font();
    mq_imgui_end();
}

/* 结束帧并把命令转成引擎网格图元（在 text_flush 之后、渲染之前调用）。
 * 几何拷进 Engine 自己的缓冲：C ABI 的指针只在"本帧 end_frame → 下帧 begin_frame"有效，
 * 而网格要到渲染时才被消费 —— 拷一份最稳，且避免跨模块生命周期耦合。 */
void Engine::imgui_extract(std::vector<mq_renderer_mesh>& out) {
    if (!imgui_on || !imgui_inited || !imgui_frame_active) return;
    imgui_frame_active = false;
    if (imgui_backend) { (void)out; return; }   // 官方后端：结束后由 mq_imgui_backend_render 画                  // 一帧只 end 一次（防脚本误关导致状态错乱）
    mq_imgui_frame f{};
    if (mq_imgui_end_frame(&f) != 0 || f.cmd_count == 0 || !f.cmds) {
        imgui_last_cmds = 0;
        return;
    }
    imgui_last_cmds = (int)f.cmd_count;
    ++imgui_frames;

    /* 一次算出总顶点/索引量 → reserve（避免边写边扩容） */
    size_t nv = 0, ni = 0;
    for (uint32_t i = 0; i < f.cmd_count; ++i) { nv += f.cmds[i].vertex_count; ni += f.cmds[i].index_count; }
    imgui_pos.clear(); imgui_uv.clear(); imgui_col.clear(); imgui_idx.clear();
    imgui_pos.reserve(nv * 2); imgui_uv.reserve(nv * 2); imgui_col.reserve(nv * 4);
    imgui_idx.reserve(ni);
    out.reserve(out.size() + f.cmd_count);

    for (uint32_t ci = 0; ci < f.cmd_count; ++ci) {
        const mq_imgui_cmd& c = f.cmds[ci];
        if (c.vertex_count == 0 || c.index_count == 0) continue;
        const uint32_t base = (uint32_t)(imgui_pos.size() / 2);
        imgui_pos.insert(imgui_pos.end(), c.positions, c.positions + (size_t)c.vertex_count * 2);
        imgui_uv.insert(imgui_uv.end(), c.uvs, c.uvs + (size_t)c.vertex_count * 2);
        imgui_col.insert(imgui_col.end(), c.colors, c.colors + (size_t)c.vertex_count * 4);
        const size_t idx_off = imgui_idx.size();
        /* 引用已是命令内 0 基；**不能**再加 base：gfx 画时会叠 baseVertex（=本网格顶点起点），
         * 再加一次就是双偏移 → 三角形拿到别的图元顶点（画面拉伸/模糊）。 */
        for (uint32_t k = 0; k < c.index_count; ++k) imgui_idx.push_back(c.indices[k]);

        /* 纹理：命令里的句柄 = gfx 纹理 id + 1（字体图集）。未上传 → -1（渲染器跳过该批）。
         * ⚠️ 判据是 `>= 1` 不是 `> 1`：gfx 的纹理 id 从 **0** 开始，
         * 句柄 1 对应 id 0（场景里没有别的纹理时字体图集正好是 0 号）——
         * 写成 `> 1` 会把**第一张纹理**当成"未上传"，面板整批被跳过。 */
        int tex = -1;
        if (c.texture >= 1) {
            const int tid = (int)(c.texture - 1);
            if (rapi && rapi->has_texture && rapi->has_texture(rself, tid)) tex = tid;
        }
        mq_renderer_mesh m{};
        m.vertices = imgui_pos.data() + (size_t)base * 2;
        m.uvs = imgui_uv.data() + (size_t)base * 2;
        m.colors = imgui_col.data() + (size_t)base * 4;
        m.dark_colors = nullptr;                 // 面板不参与 TintBlack（nullptr = 等价默认真黑）
        m.indices = imgui_idx.data() + idx_off;
        m.vertex_count = c.vertex_count;
        m.first_index = 0;                       // 本命令自带独立索引段
        m.index_count = c.index_count;
        m.base_vertex = 0;                       // 索引已含全局 base（上面的 +base）
        m.texture = tex;
        m.blend = 0;                             // normal（ImGui 默认 alpha 混合）
        m.pma = 0;                               // 图集是直通 RGBA（非预乘）
        m.layer = MQ_IMGUI_LAYER;                // ★ 在所有引擎图层之上
        m.z = (int32_t)ci;                       // 命令序 = ImGui 自身的绘制顺序
        /* 裁剪矩形：ImGui 的 scissor（子窗口/文本裁剪/滑块内部都靠它）。 */
        m.clip_x = c.clip_x;
        m.clip_y = c.clip_y;
        m.clip_w = c.clip_w;
        m.clip_h = c.clip_h;
        out.push_back(m);
    }
}

/* ---- Tracy 性能分析（docs/29）------------------------------------------------------
 * 设计：客户端**手动启动**（F3 / MAQUE_TRACY=1）；未启动时所有宏 no-op。
 * 基础数据：frame/logic/render/present/FPS/draw/实体（每步）+ CPU%/内存（1Hz）。
 * F3 还会拉起 tracy-profiler GUI（独立进程；退出游戏后 trace 仍可看）。
 */
static std::string find_tracy_profiler() {
    std::error_code ec;
#if defined(_WIN32)
    const char* exe_name = "tracy-profiler.exe";
#elif defined(__APPLE__)
    const char* exe_name = "Tracy.app/Contents/MacOS/Tracy";
#else
    const char* exe_name = "tracy-profiler";
#endif
    std::vector<fs::path> cands;
    if (const char* e = std::getenv("MAQUE_TRACY_PROFILER"); e && *e) cands.emplace_back(fs::path(e));
    /* 从 CWD 逐级向上找 `.deps/tracy/<exe>`（典型：仓库根 或 build-win/samples/player 启动）。 */
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 6 && !dir.empty() && !ec; ++i) {
        cands.push_back(dir / ".deps" / "tracy" / exe_name);
        cands.push_back(dir / "tools" / "tracy" / exe_name);
        dir = dir.parent_path();
    }
    for (const auto& c : cands)
        if (fs::exists(c, ec) && !ec) return c.string();
    return {};
}

void Engine::tracy_start(const char* why) {
    if (maque::tracy::started()) return;
    if (!maque::tracy::startup()) {
        std::print(stderr, "runtime: [tracy] 本构建未编入 Tracy（MAQUE_TRACY=OFF）\n");
        return;
    }
    /* 基础数据曲线：颜色与模块/分区色一致（frametime=System 蓝 / logic=Script 绿 /
     * render=3D 红 / present=System 紫 …；内存域曲线在 memory_publish 首次上报时配置）。 */
    MQ_TRACY_PLOT_CONFIG("frametime", 0, false, true, maque::tracy::color(maque::tracy::Module::runtime));
    MQ_TRACY_PLOT_CONFIG("logic", 0, false, true, maque::tracy::color(maque::tracy::Module::script));
    MQ_TRACY_PLOT_CONFIG("render", 0, false, true, maque::tracy::color(maque::tracy::Module::render));
    MQ_TRACY_PLOT_CONFIG("present", 0, false, true, maque::tracy::color(maque::tracy::Module::platform));
    MQ_TRACY_PLOT_CONFIG("fps", 0, false, true, maque::tracy::color(maque::tracy::Module::ui));
    MQ_TRACY_PLOT_CONFIG("draws", 0, false, true, 0x9E9E9E);
    MQ_TRACY_PLOT_CONFIG("entities", 0, false, true, maque::tracy::color(maque::tracy::Mem::node));
    MQ_TRACY_PLOT_CONFIG("cpu", 2 /*percentage*/, false, true, maque::tracy::color(maque::tracy::Module::input));
    MQ_TRACY_PLOT_CONFIG("memory", 1 /*memory*/, false, true, maque::tracy::area_color(maque::tracy::Area::system));
    maque::tracy::thread_name("main");
    MQ_TRACY_MESSAGE("Tracy client started");
    const char* port = std::getenv("MAQUE_TRACY_PORT");
    std::print("runtime: [tracy] client started ({}) — listening on 127.0.0.1:{}\n", why,
               port && *port ? port : "8086");
}

void Engine::tracy_launch_gui() {
    if (tracy_gui_launched) return;
    if (!maque::tracy::started()) return;          // 未编入/启动失败：不弹 GUI
    const std::string exe = find_tracy_profiler();
    if (exe.empty()) {
        std::print(stderr,
                   "runtime: [tracy] 未找到 tracy-profiler；Windows 跑 tools/tracy/get-profiler.ps1 下载、"
                   "Linux/macOS 跑 tools/tracy/build-profiler.sh 构建 GUI，"
                   "或设 MAQUE_TRACY_PROFILER=<path>\n");
        return;
    }
    const char* env_port = std::getenv("MAQUE_TRACY_PORT");
    const std::string port = (env_port && *env_port) ? env_port : "8086";
    const std::string args = "-a 127.0.0.1 -p " + port;
    const int rc = platform ? mq_platform_spawn_detached(platform, exe.c_str(), args.c_str()) : 1;
    if (rc == 0) {
        tracy_gui_launched = true;
        std::print("runtime: [tracy] launched profiler '{} {}'\n", exe, args);
    } else {
        std::print(stderr, "runtime: [tracy] 启动 profiler 失败（rc={}）: '{}'\n", rc, exe);
    }
}

void Engine::tracy_tick() {
    if (!tracy_env_checked) {
        tracy_env_checked = true;
        const char* e = std::getenv("MAQUE_TRACY");
        if (e && *e && std::strcmp(e, "0") != 0) tracy_start("MAQUE_TRACY");
    }
    if (!maque::tracy::started()) return;
    /* 1Hz：进程 CPU%/常驻内存（Tracy 基础数据）。 */
    if (frame % 60 != 0 || !platform) return;
    double cpu = 0.0;
    uint64_t mem = 0;
    if (mq_platform_process_stats(platform, &cpu, &mem) == 0) {
        MQ_TRACY_PLOT("cpu", cpu);
        MQ_TRACY_PLOT("memory", (double)mem);
    }
}

/* ---- Tracy 内存域采样（docs/29 §10）----
 * 采样型域每 MAQUE_TRACY_MEM_INTERVAL（默认 1s）刷新一次；每个域只有一个写入者：
 *   · node    = 场景实体表 + 组件 blob + 名字/树/实例元数据（**估算**：map 桶开销按 3 指针/节点计）
 *   · texture = 渲染器 texture_bytes（含字形/ImGui 图集、RenderTexture、env cube）
 *   · gpu     = 渲染器 buffer_bytes（网格顶点/索引；旧插件只有总量时回退到 gpu）
 *   · font    = 文本模块 mq_font_memory_bytes（CPU 侧图集/字形表）
 * delta 型域（script / heap）由各自的分配器钩子维护，这里不碰。 */
void Engine::memory_sample() {
    /* ---- 节点树 ---- */
    uint64_t node = 0;
    if (scene) {
        node += sizeof(Scene);
        node += (uint64_t)scene->desc.title.capacity() + scene->desc.script.capacity() +
                scene->desc.overlay_script.capacity() + scene->desc.global_script.capacity() +
                scene->desc.env_texture.capacity() + scene->desc.env_cubemap.capacity();
        node += (uint64_t)scene->desc.scripts.capacity() * sizeof(std::string);
        for (const auto& s : scene->desc.scripts) node += (uint64_t)s.capacity();
        node += (uint64_t)scene->text_.capacity();
        node += (uint64_t)scene->nodes.size() * (sizeof(void*) * 3 + sizeof(SceneNode));   // 桶+链表节点估算
        for (const auto& [id, e] : scene->nodes) {
            (void)id;
            node += (uint64_t)e.name.capacity();
            node += (uint64_t)e.comps.size() *
                    (sizeof(uint32_t) + sizeof(std::vector<uint8_t>) + sizeof(void*) * 2);
            for (const auto& [tid, blob] : e.comps) { (void)tid; node += (uint64_t)blob.capacity(); }
        }
    }
    for (const auto& [iid, inst] : instances) {
        (void)iid;
        if (!inst) continue;
        node += sizeof(SceneInstanceRuntime);
        node += (uint64_t)inst->source.capacity() + inst->base.capacity() + inst->params.capacity() +
                inst->error.capacity();
        node += (uint64_t)inst->roots.capacity() * sizeof(int) +
                (uint64_t)inst->children.capacity() * sizeof(int);
        node += (uint64_t)inst->scripts.capacity() * sizeof(std::string) +
                (uint64_t)inst->script_hashes.capacity() * sizeof(uint64_t);
        for (const auto& s : inst->scripts) node += (uint64_t)s.capacity();
    }
    maque::tracy::memory_set(maque::tracy::Mem::node, node);

    /* ---- 字体/图集（CPU 侧）---- */
    uint64_t font = 0;
    for (const auto& fe : fonts) font += mq_font_memory_bytes(fe.font);
    font += mq_imgui_font_atlas_bytes();    /* 调试面板的静态图集（未打开/官方后端自管 → 0） */
    maque::tracy::memory_set(maque::tracy::Mem::font, font);

    /* ---- GPU：纹理 / 缓冲（渲染器是唯一真相）---- */
    uint64_t tex = 0, buf = 0;
    bool split = false;
    if (rapi && rself && MQ_RENDERER_HAS(rapi, resource_bytes2)) {
        mq_renderer_resource_usage u{};
        u.struct_size = (uint32_t)sizeof u;
        if (rapi->resource_bytes2(rself, &u) == 0) {
            tex = u.texture_bytes;
            buf = u.buffer_bytes + u.other_bytes;
            split = true;
        }
    }
    if (!split) buf = stat_vram_bytes;     /* 旧插件：只有总量 → 记到 gpu（texture 保持 0） */
    maque::tracy::memory_set(maque::tracy::Mem::texture, tex);
    maque::tracy::memory_set(maque::tracy::Mem::gpu, buf);

    /* ---- 控制面快照（Perf.get / CI 泄漏门）----
     * 与 Tracy 内存域同一份真相：采样型域用刚算出的值，delta 型域（script/heap）读分配器账本。
     * 键名带 `mem.` 前缀，避免与既有 metrics（entities/draws/...）冲突；值是字节数。
     * 这是**当前值 + 峰值 + 分配/释放次数**，够做"跑 N 帧后回到基线"的断言；需要调用栈/逐块
     * 归因时仍走 Tracy（MAQUE_TRACY=1 + Memory 视图）。 */
    auto gauge = [&](const char* name, uint64_t value) {
        mq_obs_gauge_set(obs, name, (double)value);
    };
    auto gauge_domain = [&](const char* name, maque::tracy::Mem m) {
        const maque::tracy::MemInfo info = maque::tracy::memory_info(m);
        char key[64];
        std::snprintf(key, sizeof key, "mem.%s", name);
        gauge(key, info.bytes);
        std::snprintf(key, sizeof key, "mem.peak.%s", name);
        gauge(key, info.peak);
        std::snprintf(key, sizeof key, "mem.%s.allocs", name);
        gauge(key, info.allocs);
        std::snprintf(key, sizeof key, "mem.%s.frees", name);
        gauge(key, info.frees);
    };
    gauge_domain("script", maque::tracy::Mem::script);
    gauge_domain("node", maque::tracy::Mem::node);
    gauge_domain("texture", maque::tracy::Mem::texture);
    gauge_domain("gpu", maque::tracy::Mem::gpu);
    gauge_domain("font", maque::tracy::Mem::font);
    gauge_domain("heap", maque::tracy::Mem::heap);
    gauge("mem.tracked", maque::tracy::memory_total());
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
