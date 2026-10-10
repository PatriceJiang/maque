/* maque 运行时实现（重构版）：
 *   Engine（"浏览器"）：设备 / VFS / 脚本 VM / 纹理 / 视图 —— 跨场景的通用能力。
 *   Scene （游戏）    ：反序列化后的场景描述 + 实体/组件 —— deserialize / dispatch / extract。
 *   主循环            ：Engine::main_loop() —— 每帧 dispatch(scene) → extract → render。
 * mq_run_scene_ex 只是薄组合：建 Engine → Scene::deserialize → main_loop → 析构。 */
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
#include <filesystem>

namespace {

/* ---------------- 组件（引擎世界模型，docs/12/17） ---------------- */
struct Entity {
    float lx = 0, ly = 0;        // 局部（相对父）
    float x = 0, y = 0;          // 世界（每帧由父子计算）
    float radius = 8;
    float r = 0.4f, g = 0.8f, b = 1.0f;
    bool  has_sprite = false;
    int   parent = -1;           // 父实体 index（-1 = 根）
    int   tex = -1;              // 纹理 id（-1 = 无）
};

struct Engine;
struct Scene;

/* ---------------- Engine：通用运行时（"浏览器"） ---------------- */
struct Engine {
    maque::render::Renderer* renderer = nullptr;  // 后端无关渲染器（docs/13）
    mq_vfs*             vfs = nullptr;
    mq_script_vm*       vm = nullptr;
    const mq_type_desc* scene_type = nullptr;
    Scene*              scene = nullptr;          // 当前场景

    std::vector<std::string> tex_paths;           // 路径→纹理 id 缓存（纹理归 renderer）
    uint32_t rng = 0;
    bool     quit = false;
    float    bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };

    // 视图（VR/多屏）
    int W = 0, H = 0, nviews = 1, TW = 0;
    std::vector<maque::render::View> views;

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
    int load_texture(const char* path);
    void bind();                                   // 注册宿主 API 到脚本
    void main_loop(const std::string& frames_dir, const std::string& name);
};

/* ---------------- Scene：游戏世界（实体/组件） ---------------- */
struct Scene {
    scn::Scene desc;                               // 反序列化数据
    mq_handle_table* entities = nullptr;

    Scene() { entities = mq_handle_table_create(sizeof(Entity), alignof(Entity)); }
    ~Scene() { if (entities) mq_handle_table_destroy(entities); }
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    bool deserialize(Engine& eng, const std::string& json_text);   // 解析场景 + 实体树
    int  spawn(float x, float y, float radius, float r, float g, float b);
    Entity* at(int index);

    void compute_world();                          // 父子世界变换
    void extract(Engine& eng, std::vector<maque::render::DrawItem>& out);  // 渲染层从组件提取（只读）
};

/* ---------------- 小工具 ---------------- */
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
    const mq_field_desc* f = nullptr;
    void* parent = obj;
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
        renderer->render_views(TW, H, views.data(), views.size(),
                               draws.data(), draws.size(), bg, pixels);
        if (!frames_dir.empty()) {
            char fn[64]; std::snprintf(fn, sizeof(fn), "/frame_%04d.ppm", frame);
            write_ppm(frames_dir + fn, TW, H, pixels);
        }
    }
    std::print("runtime: '{}' exited after {} frames (quit={})\n", name, frame, quit ? 1 : 0);
}

/* ---------------- Scene 实现 ---------------- */
int Scene::spawn(float x, float y, float radius, float r, float g, float b) {
    void* p = nullptr;
    mq_handle h = mq_handle_table_alloc(entities, &p);
    if (h.index == MQ_HANDLE_INDEX_NONE) return -1;
    auto* e = (Entity*)p;
    e->has_sprite = true;
    e->lx = x; e->ly = y; e->x = x; e->y = y;
    e->radius = radius; e->r = r; e->g = g; e->b = b;
    return (int)h.index;
}

Entity* Scene::at(int index) {
    if (index < 0) return nullptr;
    return (Entity*)mq_handle_table_get(entities, mq_handle_table_handle_at(entities, (uint32_t)index));
}

/* 递归：从 JSON 节点建实体（含嵌套 children + 纹理）。 */
static int load_node(Engine& eng, Scene& sc, const mq_json* e, int parent) {
    float lx = (float)mq_json_num(mq_json_get(e, "x"), 0.0);
    float ly = (float)mq_json_num(mq_json_get(e, "y"), 0.0);
    float radius = (float)mq_json_num(mq_json_get(e, "radius"), 8.0);
    float r = (float)mq_json_num(mq_json_get(e, "r"), 0.4);
    float g = (float)mq_json_num(mq_json_get(e, "g"), 0.8);
    float b = (float)mq_json_num(mq_json_get(e, "b"), 1.0);
    const char* texpath = mq_json_str(mq_json_get(e, "texture"), nullptr);
    int tex = texpath ? eng.load_texture(("/" + std::string(texpath)).c_str()) : -1;

    int idx = sc.spawn(lx, ly, radius, r, g, b);
    if (idx < 0) return -1;
    Entity* ent = sc.at(idx);
    ent->parent = parent;
    ent->tex = tex;
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t cn = mq_json_len(ch);
    for (uint32_t i = 0; i < cn; ++i) load_node(eng, sc, mq_json_at(ch, i), idx);
    return idx;
}

bool Scene::deserialize(Engine& eng, const std::string& text) {
    mq_json* doc = mq_json_parse(text.c_str(), text.size());
    if (!doc) return false;
    // 1) 类型化字段（title/script/width/height/seed/views/eye_sep）
    if (mq_persist_load_json(eng.scene_type, text.c_str(), text.size(), &desc) != 0) {
        mq_json_free(doc); return false;
    }
    // 2) 实体树（通用 JSON 结构）
    const mq_json* ents = mq_json_get(doc, "entities");
    uint32_t n = mq_json_len(ents);
    for (uint32_t i = 0; i < n; ++i) load_node(eng, *this, mq_json_at(ents, i), -1);
    mq_json_free(doc);
    if (n) std::print("runtime: loaded {} root entities\n", n);
    return true;
}

void Scene::compute_world() {
    uint32_t cap = mq_handle_table_capacity(entities);
    for (uint32_t i = 0; i < cap; ++i) {
        auto* e = (Entity*)mq_handle_table_get(entities, mq_handle_table_handle_at(entities, i));
        if (!e) continue;
        if (e->parent >= 0) {
            auto* pe = (Entity*)mq_handle_table_get(entities, mq_handle_table_handle_at(entities, (uint32_t)e->parent));
            if (pe) { e->x = pe->x + e->lx; e->y = pe->y + e->ly; continue; }
        }
        e->x = e->lx; e->y = e->ly;
    }
}

void Scene::extract(Engine& eng, std::vector<maque::render::DrawItem>& out) {
    uint32_t cap = mq_handle_table_capacity(entities);
    for (uint32_t i = 0; i < cap; ++i) {
        auto* e = (Entity*)mq_handle_table_get(entities, mq_handle_table_handle_at(entities, i));
        if (!e || !e->has_sprite) continue;
        maque::render::DrawItem d{ e->x, (float)eng.H - e->y, e->radius, e->r, e->g, e->b, 1.0F, e->tex };
        out.push_back(d);
    }
}

/* ---------------- 宿主 API（引擎能力，非游戏） ---------------- */
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
int fn_draw(void* u, int argc, const double* a, double*) {
    (void)u; (void)argc; (void)a; return 0;   // 通用直绘（预留；当前经实体组件提取）
}
int fn_entity_spawn(void* u, int argc, const double* a, double* ret) {
    auto* e = (Engine*)u;
    float x = argc >= 1 ? (float)a[0] : 0.0f, y = argc >= 2 ? (float)a[1] : 0.0f;
    float radius = argc >= 3 ? (float)a[2] : 8.0f;
    float r = argc >= 6 ? (float)a[3] : 0.4f, g = argc >= 6 ? (float)a[4] : 0.8f, b = argc >= 6 ? (float)a[5] : 1.0f;
    *ret = (double)e->scene->spawn(x, y, radius, r, g, b);
    return 0;
}
int fn_entity_set_pos(void* u, int argc, const double* a, double*) {
    auto* e = (Engine*)u; if (argc < 3 || !e->scene) return 0;
    if (Entity* ent = e->scene->at((int)a[0])) { ent->lx = (float)a[1]; ent->ly = (float)a[2]; }
    return 0;
}
int fn_entity_x(void* u, int argc, const double* a, double* ret) {
    auto* e = (Engine*)u; Entity* ent = (argc >= 1 && e->scene) ? e->scene->at((int)a[0]) : nullptr;
    *ret = ent ? ent->x : 0.0; return 0;
}
int fn_entity_y(void* u, int argc, const double* a, double* ret) {
    auto* e = (Engine*)u; Entity* ent = (argc >= 1 && e->scene) ? e->scene->at((int)a[0]) : nullptr;
    *ret = ent ? ent->y : 0.0; return 0;
}
int fn_entity_count(void* u, int, const double*, double* ret) {
    *ret = (double)mq_handle_table_count(((Engine*)u)->scene->entities); return 0;
}
int fn_entity_destroy(void* u, int argc, const double* a, double*) {
    auto* e = (Engine*)u; if (argc >= 1)
        mq_handle_table_free(e->scene->entities, mq_handle_table_handle_at(e->scene->entities, (uint32_t)a[0]));
    return 0;
}
int fn_load_texture(void* u, const char* path, double* ret) {
    std::string p = (path && path[0] == '/') ? std::string(path) : ("/" + std::string(path ? path : ""));
    *ret = (double)((Engine*)u)->load_texture(p.c_str());
    return 0;
}

void Engine::bind() {
    mq_bind_generated(vm);                          // 声明式引擎 API（math.* 等）
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

/* ---------------- C ABI 入口：薄组合 ---------------- */
extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;
    std::filesystem::path scnp(scn_path);
    std::string scn_dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();

    Engine eng;
    eng.vfs = mq_vfs_create();
    mq_vfs_mount(eng.vfs, "/", ("file://" + scn_dir).c_str());

    // 1) 读场景资源（VFS）→ 反序列化
    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(eng.vfs, ("/" + scnp.filename().string()).c_str(), &d, &n) == 0) { text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) { std::print("runtime: cannot read scene '{}'\n", scn_path); mq_vfs_destroy(eng.vfs); return 1; }
    mq_reflect_init();
    eng.scene_type = mq_reflect_type_by_name("scn.Scene");

    // 2) 设备 + 脚本
    std::string backend;
    eng.renderer = maque::render::create_renderer(&backend);
    if (!eng.renderer) { std::print("runtime: no WebGPU device\n"); mq_vfs_destroy(eng.vfs); return 2; }
    std::print("runtime: WebGPU backend '{}'\n", backend);

    Scene scene;
    eng.scene = &scene;
    if (!scene.deserialize(eng, text)) { std::print("runtime: failed to deserialize scene\n"); return 1; }

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

    // 3) 主循环（dispatch → extract → render）
    eng.main_loop(frames_dir ? frames_dir : "", scnp.filename().string());

    // 4) 析构
    mq_script_vm_destroy(eng.vm);
    delete eng.renderer;
    mq_vfs_destroy(eng.vfs);
    return eng.quit ? 0 : 1;
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
