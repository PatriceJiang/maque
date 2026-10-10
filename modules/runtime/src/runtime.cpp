/* maque 运行时实现：加载 .scn → 反序列化 → 加载 .js → 固定步长循环 → 渲染 → 退出。
 * 通用宿主 API（引擎能力）：log/cfg/clear/draw_sprite/rng/screen_w/screen_h/quit。
 * 游戏逻辑不在引擎里——全部在脚本资源中。 */
#include "runtime/mq_runtime.h"

#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "core/script/mq_script.h"
#include "core/handle/mq_handle.h"
#include "core/vfs/mq_vfs.h"
#include "render/gfx/mq_gfx.h"
#include "maque_types.h"

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

namespace {

/* 实体/组件（引擎世界模型，docs/12/17）。渲染层从组件提取，不回写。 */
struct Entity {
    float x = 0, y = 0;
    float radius = 8;
    float r = 0.4f, g = 0.8f, b = 1.0f;
    bool  has_sprite = false;
};

struct Runtime {
    maque::gfx::Device* dev = nullptr;
    std::vector<maque::gfx::SpriteDraw> draws;   // 脚本直接提交的绘制
    const mq_type_desc* scene_type = nullptr;
    scn::Scene scene{};
    uint32_t rng = 0;
    bool quit = false;
    int W = 0, H = 0;
    float bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };

    mq_handle_table* entities = nullptr;          // 实体：{transform + sprite 组件}

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }

    Entity* entity(mq_handle h) { return (Entity*)mq_handle_table_get(entities, h); }
    Entity* entity_by_index(double id) {
        if (id < 0) return nullptr;
        return entity(mq_handle_table_handle_at(entities, (uint32_t)id));
    }
    mq_handle spawn(float x, float y, float radius, float r, float g, float b) {
        void* p = nullptr;
        mq_handle h = mq_handle_table_alloc(entities, &p);
        if (h.index == MQ_HANDLE_INDEX_NONE) return h;
        auto* e = (Entity*)p;
        e->has_sprite = true;
        e->x = x; e->y = y; e->radius = radius; e->r = r; e->g = g; e->b = b;
        return h;
    }
};

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

double cfg_field(Runtime* rt, const char* path) {
    const mq_type_desc* t = rt->scene_type;
    void* obj = &rt->scene;
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

/* ---- 宿主 API（通用引擎能力） ---- */
int fn_log(void*, const char* s, double*) { std::printf("[game] %s\n", s); return 0; }
int fn_cfg(void* u, const char* path, double* ret) { *ret = cfg_field((Runtime*)u, path); return 0; }
int fn_screen_w(void* u, int, const double*, double* ret) { *ret = (double)((Runtime*)u)->W; return 0; }
int fn_screen_h(void* u, int, const double*, double* ret) { *ret = (double)((Runtime*)u)->H; return 0; }
int fn_rng(void* u, int, const double*, double* ret) {
    *ret = (double)((Runtime*)u)->next_rng() / 4294967296.0; return 0;
}
int fn_quit(void* u, int, const double*, double*) { ((Runtime*)u)->quit = true; return 0; }
int fn_clear(void* u, int argc, const double* a, double*) {
    auto* rt = (Runtime*)u;
    if (argc >= 3) { rt->bg[0] = (float)a[0]; rt->bg[1] = (float)a[1]; rt->bg[2] = (float)a[2]; }
    return 0;
}
int fn_draw(void* u, int argc, const double* a, double*) {
    auto* rt = (Runtime*)u;
    if (argc < 6) return 0;
    rt->draws.push_back({ (float)a[0], (float)a[1], (float)a[2], (float)a[3], (float)a[4], (float)a[5], 1.0F });
    return 0;
}

/* ---- 实体/组件 API（引擎世界） ---- */
int fn_entity_spawn(void* u, int argc, const double* a, double* ret) {
    auto* rt = (Runtime*)u;
    float x = argc >= 1 ? (float)a[0] : 0.0f;
    float y = argc >= 2 ? (float)a[1] : 0.0f;
    float radius = argc >= 3 ? (float)a[2] : 8.0f;
    float r = argc >= 6 ? (float)a[3] : 0.4f;
    float g = argc >= 6 ? (float)a[4] : 0.8f;
    float b = argc >= 6 ? (float)a[5] : 1.0f;
    mq_handle h = rt->spawn(x, y, radius, r, g, b);
    *ret = (h.index == MQ_HANDLE_INDEX_NONE) ? -1.0 : (double)h.index;
    return 0;
}
int fn_entity_set_pos(void* u, int argc, const double* a, double*) {
    auto* rt = (Runtime*)u; if (argc < 3) return 0;
    if (Entity* e = rt->entity_by_index(a[0])) { e->x = (float)a[1]; e->y = (float)a[2]; }
    return 0;
}
int fn_entity_x(void* u, int argc, const double* a, double* ret) {
    auto* rt = (Runtime*)u; Entity* e = argc >= 1 ? rt->entity_by_index(a[0]) : nullptr;
    *ret = e ? e->x : 0.0; return 0;
}
int fn_entity_y(void* u, int argc, const double* a, double* ret) {
    auto* rt = (Runtime*)u; Entity* e = argc >= 1 ? rt->entity_by_index(a[0]) : nullptr;
    *ret = e ? e->y : 0.0; return 0;
}
int fn_entity_count(void* u, int, const double*, double* ret) {
    *ret = (double)mq_handle_table_count(((Runtime*)u)->entities); return 0;
}
int fn_entity_destroy(void* u, int argc, const double* a, double*) {
    auto* rt = (Runtime*)u;
    if (argc >= 1) mq_handle_table_free(rt->entities, mq_handle_table_handle_at(rt->entities, (uint32_t)a[0]));
    return 0;
}

bool write_ppm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;

    // 资源经 VFS 加载（docs/04）：把场景所在目录挂载为根。
    std::filesystem::path scnp(scn_path);
    std::string scn_dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();
    mq_vfs* vfs = mq_vfs_create();
    mq_vfs_mount(vfs, "/", ("file://" + scn_dir).c_str());

    // 1) 加载场景资源 + 反序列化
    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, ("/" + scnp.filename().string()).c_str(), &d, &n) == 0) {
          text.assign((char*)d, n); mq_vfs_free(d);
      } }
    if (text.empty()) { std::printf("runtime: cannot read scene '%s'\n", scn_path); return 1; }
    mq_reflect_init();
    Runtime rt;
    rt.scene_type = mq_reflect_type_by_name("scn.Scene");
    if (!rt.scene_type || mq_persist_load_json(rt.scene_type, text.c_str(), text.size(), &rt.scene) != 0) {
        std::printf("runtime: failed to deserialize scene\n"); return 1;
    }
    rt.W = rt.scene.width > 0 ? rt.scene.width : 320;
    rt.H = rt.scene.height > 0 ? rt.scene.height : 240;
    rt.rng = rt.scene.seed * 2654435761u + 12345u;
    std::printf("runtime: scene '%s' title='%s' script='%s' %dx%d seed=%u\n",
                scn_path, rt.scene.title.c_str(), rt.scene.script.c_str(), rt.W, rt.H, rt.scene.seed);

    // 脚本路径：相对场景目录（经 VFS 以 '/' 为根）
    std::string script_path = rt.scene.script;

    // 2) 设备 + 脚本
    std::string backend;
    rt.dev = maque::gfx::create_device(&backend);
    if (!rt.dev) { std::printf("runtime: no WebGPU device\n"); return 2; }
    std::printf("runtime: WebGPU backend '%s'\n", backend.c_str());
    rt.entities = mq_handle_table_create(sizeof(Entity), alignof(Entity));

    // 从场景资源加载实体树（游戏即资源；docs/20）
    {
        mq_json* doc = mq_json_parse(text.c_str(), text.size());
        if (doc) {
            const mq_json* ents = mq_json_get(doc, "entities");
            uint32_t n = mq_json_len(ents);
            for (uint32_t i = 0; i < n; ++i) {
                const mq_json* e = mq_json_at(ents, i);
                rt.spawn((float)mq_json_num(mq_json_get(e, "x"), 0.0),
                         (float)mq_json_num(mq_json_get(e, "y"), 0.0),
                         (float)mq_json_num(mq_json_get(e, "radius"), 8.0),
                         (float)mq_json_num(mq_json_get(e, "r"), 0.4),
                         (float)mq_json_num(mq_json_get(e, "g"), 0.8),
                         (float)mq_json_num(mq_json_get(e, "b"), 1.0));
            }
            mq_json_free(doc);
            if (n) std::printf("runtime: loaded %u entities from scene\n", n);
        }
    }

    mq_script_vm* vm = mq_script_vm_create();
    mq_script_register_str(vm, "log", fn_log, &rt);
    mq_script_register_str(vm, "cfg", fn_cfg, &rt);
    mq_script_register_num(vm, "screen_w", fn_screen_w, &rt);
    mq_script_register_num(vm, "screen_h", fn_screen_h, &rt);
    mq_script_register_num(vm, "rng", fn_rng, &rt);
    mq_script_register_num(vm, "quit", fn_quit, &rt);
    mq_script_register_num(vm, "clear", fn_clear, &rt);
    mq_script_register_num(vm, "draw_sprite", fn_draw, &rt);
    mq_script_register_num(vm, "entity_spawn", fn_entity_spawn, &rt);
    mq_script_register_num(vm, "entity_set_pos", fn_entity_set_pos, &rt);
    mq_script_register_num(vm, "entity_x", fn_entity_x, &rt);
    mq_script_register_num(vm, "entity_y", fn_entity_y, &rt);
    mq_script_register_num(vm, "entity_count", fn_entity_count, &rt);
    mq_script_register_num(vm, "entity_destroy", fn_entity_destroy, &rt);

    if (!script_path.empty()) {
        std::string js;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, ("/" + script_path).c_str(), &d, &n) == 0) { js.assign((char*)d, n); mq_vfs_free(d); } }
        if (js.empty()) { std::printf("runtime: cannot read script '%s'\n", script_path.c_str()); return 1; }
        if (mq_script_eval(vm, js.c_str(), script_path.c_str()) != 0) { std::printf("runtime: script error\n"); return 1; }
    }
    if (mq_script_has(vm, "on_start")) mq_script_call0(vm, "on_start", nullptr);

    // 3) 固定步长循环
    if (frames_dir) std::filesystem::create_directories(frames_dir);
    const float dt = 1.0F / 60.0F;
    const int   cap = 60 * 60;                    // 硬上限 60s，防死循环
    std::vector<uint8_t> pixels;
    int frame = 0;
    for (; frame < cap && !rt.quit; ++frame) {
        rt.draws.clear();
        if (mq_script_has(vm, "update")) mq_script_call1(vm, "update", dt, nullptr);
        mq_script_gc(vm);                          // 帧边界 GC
        // 渲染层从组件提取（只读，不回写；docs/12）
        for (uint32_t i = 0; i < mq_handle_table_capacity(rt.entities); ++i) {
            mq_handle h = mq_handle_table_handle_at(rt.entities, i);
            auto* e = (Entity*)mq_handle_table_get(rt.entities, h);
            if (e && e->has_sprite)
                rt.draws.push_back({ e->x, (float)rt.H - e->y, e->radius, e->r, e->g, e->b, 1.0F });
        }
        maque::gfx::render_offscreen(rt.dev, rt.W, rt.H, rt.draws.data(), rt.draws.size(),
                                     rt.bg, pixels);
        if (frames_dir) {
            char name[64]; std::snprintf(name, sizeof(name), "/frame_%04d.ppm", frame);
            write_ppm(std::string(frames_dir) + name, rt.W, rt.H, pixels);
        }
    }

    // 4) 退出
    std::printf("runtime: exited after %d frames (quit=%d)\n", frame, rt.quit ? 1 : 0);
    if (rt.entities) mq_handle_table_destroy(rt.entities);
    mq_script_vm_destroy(vm);
    maque::gfx::destroy_device(rt.dev);
    if (vfs) mq_vfs_destroy(vfs);
    return rt.quit ? 0 : 1;
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
