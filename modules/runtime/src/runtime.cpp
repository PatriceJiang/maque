/* maque 运行时：
 *   Engine（"浏览器"）：设备(Via Renderer 接口) / VFS / 脚本 VM / 纹理 / 视图。
 *   Scene （游戏）    ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 若干带 type 的组件；组件由反射按类型名反序列化（docs/12/17）。 */
#include "runtime/mq_runtime.h"

#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "core/script/mq_script.h"
#include "core/handle/mq_handle.h"
#include "core/vfs/mq_vfs.h"
#include "core/image/mq_image.h"
#include "core/api/mq_api.h"
#include "render/renderer/mq_renderer.h"
#include "maque_types.h"

#include <print>
#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <filesystem>

namespace {

/* ---------------- 实体 = name + 组件映射(type_id → blob) ---------------- */
struct Entity {
    std::string name;
    int   parent = -1;
    int   tex = -1;                              // 解析自 Sprite 组件
    float lx = 0, ly = 0;                        // 局部变换（Transform2D 组件）
    float x = 0, y = 0;                          // 世界（每帧计算）
    std::unordered_map<uint32_t, std::vector<uint8_t>> comps;
};

struct Engine;
struct Scene;

/* ---------------- Engine：通用运行时 ---------------- */
struct Engine {
    maque::render::Renderer* renderer = nullptr;
    mq_vfs*             vfs = nullptr;
    mq_script_vm*       vm = nullptr;
    const mq_type_desc* scene_type = nullptr;
    uint32_t            tid_transform = 0, tid_sprite = 0;   // 组件类型 id
    Scene*              scene = nullptr;

    std::vector<std::string> tex_paths;
    uint32_t rng = 0;
    bool     quit = false;
    float    bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    int W = 0, H = 0, nviews = 1, TW = 0;
    std::vector<maque::render::View> views;

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
    int  load_texture(const char* path);
    void bind();
    void main_loop(const std::string& frames_dir, const std::string& name);
};

/* ---------------- Scene：实体/组件世界 ---------------- */
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

    int   spawn();
    Entity* at(int index);
    void  remove(int index);
    int   count() const { return (int)ents.size(); }
    void  compute_world();
    void  extract(Engine& eng, std::vector<maque::render::DrawItem>& out);
};

/* ---------------- 工具 ---------------- */
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
        t = mq_reflect_type_by_id(f->type_id);
        if (!t) return 0.0;
        seg = dot + 1;
    }
    double v = 0.0; mq_reflect_get_f64(parent, f, &v); return v;
}

/* ---------------- Engine 实现 ---------------- */
int Engine::load_texture(const char* path) {
    for (size_t i = 0; i < tex_paths.size(); ++i)
        if (tex_paths[i] == path) return (int)i;
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, path, &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    int rc = mq_image_decode(data, n, &rgba, &w, &h);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, mq_image_last_error()); return -1; }
    int id = renderer->create_texture(rgba, (int)w, (int)h);
    mq_image_free(rgba);
    if (id < 0) return -1;
    tex_paths.emplace_back(path);
    return id;
}

void Engine::main_loop(const std::string& frames_dir, const std::string& name) {
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);
    const float dt = 1.0F / 60.0F;
    const int   cap = 60 * 60;
    std::vector<uint8_t> pixels;
    std::vector<maque::render::DrawItem> draws;
    int frame = 0;
    for (; frame < cap && !quit; ++frame) {
        draws.clear();
        if (scene) {
            if (mq_script_has(vm, "update")) mq_script_call1(vm, "update", dt, nullptr);  // dispatch
            mq_script_gc(vm);
            scene->compute_world();
            scene->extract(*this, draws);
        }
        renderer->render_views(TW, H, views.data(), views.size(), draws.data(), draws.size(), bg, pixels);
        if (!frames_dir.empty()) {
            char fn[64]; std::snprintf(fn, sizeof(fn), "/frame_%04d.ppm", frame);
            write_ppm(frames_dir + fn, TW, H, pixels);
        }
    }
    std::print("runtime: '{}' exited after {} frames (quit={})\n", name, frame, quit ? 1 : 0);
}

/* ---------------- Scene 实现 ---------------- */
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
    int rc = mq_persist_apply_json(eng.scene_type, dom, &desc);   // 类型化 Scene 字段
    mq_json_free(dom);
    return rc == 0;
}

/* 实例化：遍历通用节点（name/children/components），组件按 type 反射反序列化。 */
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
        const mq_type_desc* d = mq_reflect_type_by_name(tname);   // ← 按 type 判别组件
        if (!d) { std::print("runtime: unknown component type '{}'\n", tname); continue; }
        std::vector<uint8_t>& blob = ent->comps[d->type_id];
        blob.resize(d->size);
        mq_persist_apply_json(d, c, blob.data());
        if (d->type_id == eng.tid_transform) {
            auto* tr = reinterpret_cast<scn::Transform2D*>(blob.data());
            ent->lx = tr->x; ent->ly = tr->y;
        } else if (d->type_id == eng.tid_sprite) {
            auto* sp = reinterpret_cast<scn::Sprite*>(blob.data());
            if (!sp->texture.empty()) ent->tex = eng.load_texture(("/" + sp->texture).c_str());
        }
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

void Scene::compute_world() {
    for (auto& [idx, e] : ents) {
        if (e.parent >= 0) {
            if (Entity* pe = at(e.parent)) { e.x = pe->x + e.lx; e.y = pe->y + e.ly; continue; }
        }
        e.x = e.lx; e.y = e.ly;
    }
}

void Scene::extract(Engine& eng, std::vector<maque::render::DrawItem>& out) {
    for (auto& [idx, e] : ents) {
        auto it = e.comps.find(eng.tid_sprite);
        if (it == e.comps.end()) continue;          // 无 Sprite 组件 → 不渲染
        auto* sp = reinterpret_cast<scn::Sprite*>(it->second.data());
        maque::render::DrawItem d{ e.x, (float)eng.H - e.y, sp->radius,
                                   sp->r, sp->g, sp->b, 1.0F, e.tex };
        out.push_back(d);
    }
}

/* ---------------- 宿主 API ---------------- */
int fn_log(void*, const char* s, double*) { std::print("[game] {}\n", s); return 0; }
int fn_cfg(void* u, const char* path, double* ret) { *ret = cfg_field((Engine*)u, path); return 0; }
int fn_screen_w(void* u, int, const double*, double* ret) { *ret = (double)((Engine*)u)->W; return 0; }
int fn_screen_h(void* u, int, const double*, double* ret) { *ret = (double)((Engine*)u)->H; return 0; }
int fn_rng(void* u, int, const double*, double* ret) { *ret = (double)((Engine*)u)->next_rng() / 4294967296.0; return 0; }
int fn_quit(void* u, int, const double*, double*) { ((Engine*)u)->quit = true; return 0; }
int fn_clear(void* u, int argc, const double* a, double*) {
    auto* e = (Engine*)u;
    if (argc >= 3) { e->bg[0] = (float)a[0]; e->bg[1] = (float)a[1]; e->bg[2] = (float)a[2]; }
    return 0;
}
int fn_draw(void*, int, const double*, double*) { return 0; }
int fn_entity_spawn(void* u, int, const double*, double* ret) { *ret = (double)((Engine*)u)->scene->spawn(); return 0; }
int fn_entity_set_pos(void* u, int argc, const double* a, double*) {
    auto* e = (Engine*)u; if (argc < 3 || !e->scene) return 0;
    if (Entity* ent = e->scene->at((int)a[0])) { ent->lx = (float)a[1]; ent->ly = (float)a[2]; }
    return 0;
}
int fn_entity_x(void* u, int argc, const double* a, double* ret) {
    Entity* ent = (argc >= 1 && ((Engine*)u)->scene) ? ((Engine*)u)->scene->at((int)a[0]) : nullptr;
    *ret = ent ? ent->x : 0.0; return 0;
}
int fn_entity_y(void* u, int argc, const double* a, double* ret) {
    Entity* ent = (argc >= 1 && ((Engine*)u)->scene) ? ((Engine*)u)->scene->at((int)a[0]) : nullptr;
    *ret = ent ? ent->y : 0.0; return 0;
}
int fn_entity_count(void* u, int, const double*, double* ret) { *ret = (double)((Engine*)u)->scene->count(); return 0; }
int fn_entity_destroy(void* u, int argc, const double* a, double*) {
    auto* e = (Engine*)u; if (argc >= 1) e->scene->remove((int)a[0]); return 0;
}
int fn_load_texture(void* u, const char* path, double* ret) {
    std::string p = (path && path[0] == '/') ? std::string(path) : ("/" + std::string(path ? path : ""));
    *ret = (double)((Engine*)u)->load_texture(p.c_str()); return 0;
}

void Engine::bind() {
    mq_bind_generated(vm);
    mq_script_register_str(vm, "log", fn_log, this);
    mq_script_register_str(vm, "cfg", fn_cfg, this);
    mq_script_register_str(vm, "load_texture", fn_load_texture, this);
    mq_script_register_num(vm, "screen_w", fn_screen_w, this);
    mq_script_register_num(vm, "screen_h", fn_screen_h, this);
    mq_script_register_num(vm, "rng", fn_rng, this);
    mq_script_register_num(vm, "quit", fn_quit, this);
    mq_script_register_num(vm, "clear", fn_clear, this);
    mq_script_register_num(vm, "draw_sprite", fn_draw, this);
    mq_script_register_num(vm, "entity_spawn", fn_entity_spawn, this);
    mq_script_register_num(vm, "entity_set_pos", fn_entity_set_pos, this);
    mq_script_register_num(vm, "entity_x", fn_entity_x, this);
    mq_script_register_num(vm, "entity_y", fn_entity_y, this);
    mq_script_register_num(vm, "entity_count", fn_entity_count, this);
    mq_script_register_num(vm, "entity_destroy", fn_entity_destroy, this);
}

} // namespace

/* ---------------- C ABI 入口 ---------------- */
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
    mq_reflect_init();
    eng.scene_type = mq_reflect_type_by_name("scn.Scene");
    const mq_type_desc* tt = mq_reflect_type_by_name("scn.Transform2D");
    const mq_type_desc* st = mq_reflect_type_by_name("scn.Sprite");
    eng.tid_transform = tt ? tt->type_id : 0;
    eng.tid_sprite = st ? st->type_id : 0;

    std::string backend;
    eng.renderer = maque::render::create_renderer(&backend);
    if (!eng.renderer) { std::print("runtime: no WebGPU device\n"); mq_vfs_destroy(eng.vfs); return 2; }
    std::print("runtime: WebGPU backend '{}'\n", backend);

    Scene scene;
    eng.scene = &scene;
    if (!scene.deserialize(eng, text)) { std::print("runtime: failed to deserialize scene\n"); return 1; }
    scene.instantiate(eng);
    scene.compute_world();                          // 先算世界变换，脚本才能读到正确位置

    eng.W = scene.desc.width > 0 ? scene.desc.width : 320;
    eng.H = scene.desc.height > 0 ? scene.desc.height : 240;
    eng.nviews = scene.desc.views > 0 ? scene.desc.views : 1;
    eng.TW = eng.W * eng.nviews;
    eng.views.resize(eng.nviews);
    for (int i = 0; i < eng.nviews; ++i)
        eng.views[i] = maque::render::View{ (float)(i * eng.W), 0, (float)eng.W, (float)eng.H,
                                            (float)(i * eng.W), (i - (eng.nviews - 1) * 0.5F) * scene.desc.eye_sep };
    eng.rng = scene.desc.seed * 2654435761u + 12345u;
    std::print("runtime: scene '{}' title='{}' script='{}' {}x{} views={}\n",
               scn_path, scene.desc.title, scene.desc.script, eng.W, eng.H, eng.nviews);

    eng.vm = mq_script_vm_create();
    eng.bind();
    const std::string& script_path = scene.desc.script;
    if (!script_path.empty()) {
        std::string js;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(eng.vfs, ("/" + script_path).c_str(), &d, &n) == 0) { js.assign((char*)d, n); mq_vfs_free(d); } }
        if (js.empty()) { std::print("runtime: cannot read script '{}'\n", script_path); return 1; }
        if (mq_script_eval(eng.vm, js.c_str(), script_path.c_str()) != 0) { std::print("runtime: script error\n"); return 1; }
    }
    if (mq_script_has(eng.vm, "on_start")) mq_script_call0(eng.vm, "on_start", nullptr);

    eng.main_loop(frames_dir ? frames_dir : "", scnp.filename().string());

    mq_script_vm_destroy(eng.vm);
    delete eng.renderer;
    mq_vfs_destroy(eng.vfs);
    return eng.quit ? 0 : 1;
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
