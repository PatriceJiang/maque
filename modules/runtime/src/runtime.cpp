/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */
#include "runtime/mq_runtime.h"

#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "core/script/mq_script.h"
#include "core/handle/mq_handle.h"
#include "core/vfs/mq_vfs.h"
#include "core/image/mq_image.h"
#include "core/observability/mq_obs.h"
#include "core/control/mq_control.h"
#include "core/api/mq_api.h"
#include "maque_api.h"                 // 生成：host:: 声明（api_list）
#include "maque_ifaces.h"                 // 生成：mq_renderer_v1（插件接口 ABI）
#include "core/module/mq_module.h"          // mq_host（dlopen 加载插件）
#include "maque_types.h"

#include <print>
#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

struct Entity {
    std::string name;
    int parent = -1;
    float x = 0, y = 0;                                    // 世界（每帧计算）
    std::unordered_map<uint32_t, std::vector<uint8_t>> comps;   // type_id → blob
};

struct Engine;
struct Scene;

struct Engine {
    mq_host*            host = nullptr;                   // 模块宿主（加载渲染插件）
    const mq_renderer_v1* rapi = nullptr;                 // 渲染器插件接口
    void*               rself = nullptr;                  // 渲染器实例
    mq_vfs*             vfs = nullptr;
    mq_script_vm*       vm = nullptr;
    mq_reflect_ctx*     reflect = nullptr;                 // per-instance
    mq_obs_ctx*         obs = nullptr;                     // 可观测（事件/指标）
    mq_control*         control = nullptr;                 // 控制平面（JSON-RPC）
    uint64_t            last_checksum = 0;
    const mq_type_desc* scene_type = nullptr;
    Scene*              scene = nullptr;

    std::unordered_map<std::string, int> tex_cache;
    uint32_t rng = 0;
    bool     quit = false;
    float    bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    int W = 0, H = 0, nviews = 1, TW = 0;
    std::vector<mq_renderer_view> views;

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
    int load_texture(const std::string& path);                 // assets（VFS 编码文件）
    int load_texture_ref(const std::string& ref);              // builtin: 或 assets
    bool load_material(const std::string& ref, scn::SpriteMaterial& out);
    void main_loop(const std::string& frames_dir, const std::string& name);
};

struct Scene {
    scn::Scene desc;
    std::string text_;
    mq_handle_table* table = nullptr;
    std::unordered_map<int, Entity> ents;

    Scene() { table = mq_handle_table_create(sizeof(uint8_t), 1); }
    ~Scene() { if (table) mq_handle_table_destroy(table); }
    Scene(const Scene&) = delete; Scene& operator=(const Scene&) = delete;

    bool deserialize(Engine& eng, const std::string& text);
    void instantiate(Engine& eng);
    int  spawn();
    Entity* at(int index);
    void remove(int index);
    int  count() const { return (int)ents.size(); }
    void compute_world(Engine& eng);
    uint64_t checksum(Engine& eng);     // 世界状态校验和（确定性；反射驱动）
    void extract(Engine& eng, std::vector<mq_renderer_item>& out);
};

/* ---- 反射读取辅助（不依赖具体类型，仅按字段名） ---- */
bool read_f32(const mq_type_desc* d, const void* blob, const char* name, float* out) {
    const mq_field_desc* f = mq_reflect_field(d, name);
    if (!f || f->kind != MQ_TK_F32) return false;
    *out = *(const float*)((const uint8_t*)blob + f->offset);
    return true;
}
const std::string* read_str(const mq_type_desc* d, const void* blob, const char* name) {
    const mq_field_desc* f = mq_reflect_field(d, name);
    if (!f || f->kind != MQ_TK_STRING) return nullptr;
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
int Engine::load_texture(const std::string& path) {
    auto it = tex_cache.find(path);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    int rc = mq_image_decode(data, n, &rgba, &w, &h);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, mq_image_last_error()); return -1; }
    int id = rapi->create_texture(rself, rgba, (int)w, (int)h);
    mq_image_free(rgba);
    tex_cache[path] = id;
    return id;
}

/* builtin:/ → 引擎内建（此处程序化生成）；否则 assets（VFS）。 */
int Engine::load_texture_ref(const std::string& ref) {
    if (ref.rfind("builtin:", 0) == 0) {
        auto it = tex_cache.find(ref);
        if (it != tex_cache.end()) return it->second;
        int w = 1, h = 1;
        std::vector<uint8_t> px;
        if (ref == "builtin:/textures/checker") {
            w = h = 16; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                bool alt = (((x / 4) + (y / 4)) & 1) != 0;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = alt ? 255 : 230; c[1] = alt ? 255 : 140; c[2] = alt ? 255 : 60; c[3] = 255;
            }
        } else { px = { 255, 255, 255, 255 }; }
        int id = rapi->create_texture(rself, px.data(), w, h);
        tex_cache[ref] = id;
        return id;
    }
    return load_texture(ref);   // assets
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

void Engine::main_loop(const std::string& frames_dir, const std::string& name) {
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);
    const float dt = 1.0F / 60.0F;
    const int   cap = 60 * 60;
    std::vector<uint8_t> pixels((size_t)TW * H * 4);
    std::vector<mq_renderer_item> draws;
    uint64_t trail = 1469598103934665603ull;
    mq_obs_event(obs, "runtime", 1, "loop_start");
    int frame = 0;
    for (; frame < cap && !quit; ++frame) {
        draws.clear();
        if (scene) {
            if (mq_script_has(vm, "update")) mq_script_call1(vm, "update", dt, nullptr);
            mq_script_gc(vm);
            scene->compute_world(*this);
            scene->extract(*this, draws);
        }
        rapi->render_views(rself, TW, H, views.data(), (uint32_t)views.size(),
                           draws.data(), (uint32_t)draws.size(), bg, pixels.data(), (uint32_t)pixels.size());
        if (!frames_dir.empty()) {
            char fn[64]; std::snprintf(fn, sizeof(fn), "/frame_%04d.ppm", frame);
            write_ppm(frames_dir + fn, TW, H, pixels);
        }
    }
    last_checksum = trail;
    mq_obs_gauge_set(obs, "frames_total", (double)frame);
    mq_obs_event(obs, "runtime", 1, quit ? "quit" : "timeout");
    std::print("runtime: '{}' exited after {} frames (quit={}) world_trail=0x{:016x}\n",
               name, frame, quit ? 1 : 0, trail);
}

/* ---- Scene ---- */
int Scene::spawn() {
    void* p = nullptr;
    mq_handle h = mq_handle_table_alloc(table, &p);
    if (h.index == MQ_HANDLE_INDEX_NONE) return -1;
    ents.emplace((int)h.index, Entity{});
    return (int)h.index;
}
Entity* Scene::at(int index) {
    if (index < 0) return nullptr;
    auto it = ents.find(index);
    return it == ents.end() ? nullptr : &it->second;
}
void Scene::remove(int index) {
    ents.erase(index);
    mq_handle_table_free(table, mq_handle_table_handle_at(table, (uint32_t)index));
}

bool Scene::deserialize(Engine& eng, const std::string& text) {
    text_ = text;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    int rc = mq_persist_apply_json(eng.reflect, eng.scene_type, dom, &desc);
    mq_json_free(dom);
    return rc == 0;
}

static int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent) {
    int idx = sc.spawn();
    if (idx < 0) return -1;
    Entity* ent = sc.at(idx);
    ent->parent = parent;
    const char* nm = mq_json_str(mq_json_get(e, "name"), nullptr);
    if (nm) ent->name = nm;

    const mq_json* comps = mq_json_get(e, "components");
    uint32_t cn = mq_json_len(comps);
    for (uint32_t i = 0; i < cn; ++i) {
        const mq_json* c = mq_json_at(comps, i);
        const char* tname = mq_json_str(mq_json_get(c, "type"), nullptr);
        if (!tname) continue;
        const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, tname);
        if (!d) { std::print("runtime: unknown component '{}'\n", tname); continue; }
        std::vector<uint8_t>& blob = ent->comps[d->type_id];
        blob.resize(d->size);
        mq_persist_apply_json(eng.reflect, d, c, blob.data());
    }
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
    for (uint32_t i = 0; i < kn; ++i) load_entity(eng, sc, mq_json_at(ch, i), idx);
    return idx;
}

void Scene::instantiate(Engine& eng) {
    mq_json* dom = mq_json_parse(text_.c_str(), text_.size());
    if (!dom) return;
    const mq_json* ents_json = mq_json_get(dom, "entities");
    uint32_t n = mq_json_len(ents_json);
    for (uint32_t i = 0; i < n; ++i) load_entity(eng, *this, mq_json_at(ents_json, i), -1);
    mq_json_free(dom);
    std::print("runtime: instantiated {} root entities\n", n);
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
    for (auto& [k, v] : ents) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    for (int k : idx) {
        Entity& e = ents[k];
        mix(&k, sizeof k); mix(&e.parent, sizeof e.parent); mix(&e.x, sizeof e.x); mix(&e.y, sizeof e.y);
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

/* 组件语义由 roles 决定：transform2d / sprite —— 引擎不认具体组件类型。 */
void Scene::compute_world(Engine& eng) {
    for (auto& [idx, e] : ents) {
        float lx = 0, ly = 0;
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (d && (d->role & MQ_ROLE_TRANSFORM2D)) {
                read_f32(d, blob.data(), "x", &lx);
                read_f32(d, blob.data(), "y", &ly);
            }
        }
        if (e.parent >= 0) {
            if (Entity* pe = at(e.parent)) { e.x = pe->x + lx; e.y = pe->y + ly; continue; }
        }
        e.x = lx; e.y = ly;
    }
}

void Scene::extract(Engine& eng, std::vector<mq_renderer_item>& out) {
    for (auto& [idx, e] : ents) {
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (!d || !(d->role & MQ_ROLE_SPRITE)) continue;
            float radius = 8, r = 1, g = 1, b = 1;
            read_f32(d, blob.data(), "radius", &radius);
            read_f32(d, blob.data(), "r", &r);
            read_f32(d, blob.data(), "g", &g);
            read_f32(d, blob.data(), "b", &b);
            std::string texref;
            if (const std::string* tp = read_str(d, blob.data(), "texture")) texref = *tp;
            // 材质资源覆盖颜色/贴图（builtin:/ 或 assets）
            if (const std::string* mp = read_str(d, blob.data(), "material"))
                if (!mp->empty()) {
                    scn::SpriteMaterial m{};
                    if (eng.load_material(*mp, m)) {
                        r = m.base_color.x; g = m.base_color.y; b = m.base_color.z;
                        if (!m.texture.empty()) texref = m.texture;
                    }
                }
            int tex = -1;
            if (!texref.empty()) tex = eng.load_texture_ref(texref);
            out.push_back(mq_renderer_item{ e.x, (float)eng.H - e.y, radius, r, g, b, 1.0F, tex });
        }
    }
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

} // namespace

/* ---------------- 宿主 API 实现（声明来自生成的 maque_api.h） ---------------- */
namespace host {

float screen_w(void* ctx) { return (float)((Engine*)ctx)->W; }
float screen_h(void* ctx) { return (float)((Engine*)ctx)->H; }
float rng(void* ctx) { return (float)((Engine*)ctx)->next_rng() / 4294967296.0f; }
void  quit(void* ctx) { ((Engine*)ctx)->quit = true; }
void  clear(void* ctx, float r, float g, float b) {
    auto* e = (Engine*)ctx; e->bg[0] = r; e->bg[1] = g; e->bg[2] = b;
}
void  log(void*, const char* msg) { std::print("[game] {}\n", msg ? msg : ""); }
float cfg(void* ctx, const char* path) { return (float)cfg_field((Engine*)ctx, path); }
float load_texture(void* ctx, const char* path) {
    auto* e = (Engine*)ctx;
    std::string p = (path && path[0] == '/') ? path : ("/" + std::string(path ? path : ""));
    return (float)e->load_texture(p);
}
float entity_spawn(void* ctx) { return (float)((Engine*)ctx)->scene->spawn(); }
void  entity_set_pos(void* ctx, float id, float x, float y) {
    auto* e = (Engine*)ctx;
    if (Entity* ent = e->scene->at((int)id)) {
        // 写回 transform2d 组件（按角色）
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            if (d && (d->role & MQ_ROLE_TRANSFORM2D)) {
                if (const mq_field_desc* fx = mq_reflect_field(d, "x")) *(float*)(blob.data() + fx->offset) = x;
                if (const mq_field_desc* fy = mq_reflect_field(d, "y")) *(float*)(blob.data() + fy->offset) = y;
            }
        }
        ent->x = x; ent->y = y;
    }
}
float entity_x(void* ctx, float id) { Entity* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->x : 0.0f; }
float entity_y(void* ctx, float id) { Entity* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->y : 0.0f; }
float entity_count(void* ctx) { return (float)((Engine*)ctx)->scene->count(); }
void  entity_destroy(void* ctx, float id) { ((Engine*)ctx)->scene->remove((int)id); }

} // namespace host

/* ---- 控制平面方法（引擎能力，docs/11） ---- */
static int h_perf(void* u, const char*, const mq_json*, char* out, size_t cap) {
    std::snprintf(out, cap, "%s", mq_obs_dump_json(((Engine*)u)->obs));
    return 0;
}
static int h_scene(void* u, const char*, const mq_json*, char* out, size_t cap) {
    auto& d = ((Engine*)u)->scene->desc;
    std::snprintf(out, cap, "{\"title\":\"%s\",\"script\":\"%s\",\"width\":%d,\"height\":%d,\"views\":%d}",
                  d.title.c_str(), d.script.c_str(), d.width, d.height, d.views);
    return 0;
}
static int h_checksum(void* u, const char*, const mq_json*, char* out, size_t cap) {
    std::snprintf(out, cap, "{\"trail\":\"%llu\"}", (unsigned long long)((Engine*)u)->last_checksum);
    return 0;
}

extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;
    std::filesystem::path scnp(scn_path);
    std::string scn_dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();

    Engine eng;
    eng.vfs = mq_vfs_create();
    mq_vfs_mount(eng.vfs, "/", ("file://" + scn_dir).c_str());

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(eng.vfs, ("/" + scnp.filename().string()).c_str(), &d, &n) == 0) { text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) { std::print("runtime: cannot read scene '{}'\n", scn_path); mq_vfs_destroy(eng.vfs); return 1; }

    // 反射 ctx（per-instance）：载入 schema
    eng.reflect = mq_reflect_create();
    mq_reflect_load_schema(eng.reflect);
    eng.scene_type = mq_reflect_type_by_name(eng.reflect, "scn.Scene");
    if (!eng.scene_type) { std::print("runtime: no scn.Scene type\n"); return 1; }

    const char* plugin = std::getenv("MAQUE_RENDERER");
#ifdef MQ_RENDERER_DEFAULT
    if (!plugin) plugin = MQ_RENDERER_DEFAULT;
#endif
    if (!plugin) plugin = "libmaque_gfx.so";
    eng.host = mq_host_create();
    mq_loaded_module* rmod = nullptr;
    if (mq_host_load(eng.host, plugin, &rmod) != 0 ||
        !(eng.rapi = (const mq_renderer_v1*)mq_host_query(eng.host, MQ_RENDERER_API_NAME, 1))) {
        std::print("runtime: cannot load renderer plugin '{}'\n", plugin);
        mq_host_destroy(eng.host); mq_vfs_destroy(eng.vfs); return 2;
    }
    const char* backend = nullptr;
    eng.rself = eng.rapi->create(&backend);
    if (!eng.rself) { std::print("runtime: renderer create failed\n"); return 2; }
    std::print("runtime: renderer plugin '{}' backend='{}'\n", plugin, backend ? backend : "?");

    Scene scene;
    eng.scene = &scene;
    if (!scene.deserialize(eng, text)) { std::print("runtime: failed to deserialize scene\n"); return 1; }
    scene.instantiate(eng);
    scene.compute_world(eng);

    eng.W = scene.desc.width > 0 ? scene.desc.width : 320;
    eng.H = scene.desc.height > 0 ? scene.desc.height : 240;
    eng.nviews = scene.desc.views > 0 ? scene.desc.views : 1;
    eng.TW = eng.W * eng.nviews;
    eng.views.resize(eng.nviews);
    for (int i = 0; i < eng.nviews; ++i)
        eng.views[i] = mq_renderer_view{ (float)(i * eng.W), 0, (float)eng.W, (float)eng.H,
                                         (float)(i * eng.W), (i - (eng.nviews - 1) * 0.5F) * scene.desc.eye_sep };
    eng.rng = scene.desc.seed * 2654435761u + 12345u;
    std::print("runtime: scene '{}' title='{}' script='{}' {}x{} views={}\n",
               scn_path, scene.desc.title, scene.desc.script, eng.W, eng.H, eng.nviews);

    eng.obs = mq_obs_create();
    eng.vm = mq_script_vm_create();
    mq_bind_generated(eng.vm, &eng);               // 生成绑定（math.* / host.*），注入引擎 ctx
    const std::string& script_path = scene.desc.script;
    if (!script_path.empty()) {
        std::string js;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(eng.vfs, ("/" + script_path).c_str(), &d, &n) == 0) { js.assign((char*)d, n); mq_vfs_free(d); } }
        if (js.empty()) { std::print("runtime: cannot read script '{}'\n", script_path); return 1; }
        if (mq_script_eval(eng.vm, js.c_str(), script_path.c_str()) != 0) { std::print("runtime: script error\n"); return 1; }
    }
    if (mq_script_has(eng.vm, "on_start")) mq_script_call0(eng.vm, "on_start", nullptr);

    // 控制平面：注册方法；若无显示/AI 驱动，可 headless 服务请求
    eng.control = mq_control_create();
    mq_control_register(eng.control, "Perf.get", h_perf, &eng);
    mq_control_register(eng.control, "Scene.info", h_scene, &eng);
    mq_control_register(eng.control, "World.checksum", h_checksum, &eng);
    if (const char* req = std::getenv("MAQUE_CONTROL")) {
        auto serve = [&](const char* r) { std::print("{}\n", mq_control_dispatch(eng.control, r)); };
        if (std::strcmp(req, "-") == 0) {
            char line[4096];
            while (std::fgets(line, sizeof line, stdin)) { line[std::strcspn(line, "\n")] = 0; if (*line) serve(line); }
        } else serve(req);
        mq_control_destroy(eng.control);
        mq_script_vm_destroy(eng.vm); mq_obs_destroy(eng.obs); mq_reflect_destroy(eng.reflect);
        if (eng.rapi && eng.rself) eng.rapi->destroy(eng.rself);
        mq_host_destroy(eng.host); mq_vfs_destroy(eng.vfs);
        return 0;
    }

    eng.main_loop(frames_dir ? frames_dir : "", scnp.filename().string());

    mq_control_destroy(eng.control);
    mq_script_vm_destroy(eng.vm);
    mq_obs_destroy(eng.obs);
    mq_reflect_destroy(eng.reflect);
    if (eng.rapi && eng.rself) eng.rapi->destroy(eng.rself);
    mq_host_destroy(eng.host);
    mq_vfs_destroy(eng.vfs);
    return eng.quit ? 0 : 1;
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
