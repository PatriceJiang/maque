/* maque 运行时实现：加载 .scn → 反序列化 → 加载 .js → 固定步长循环 → 渲染 → 退出。
 * 通用宿主 API（引擎能力）：log/cfg/clear/draw_sprite/rng/screen_w/screen_h/quit。
 * 游戏逻辑不在引擎里——全部在脚本资源中。 */
#include "runtime/mq_runtime.h"

#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "core/script/mq_script.h"
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

struct Runtime {
    maque::gfx::Device* dev = nullptr;
    std::vector<maque::gfx::SpriteDraw> draws;
    const mq_type_desc* scene_type = nullptr;
    scn::Scene scene{};
    uint32_t rng = 0;
    bool quit = false;
    int W = 0, H = 0;
    float bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
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

    // 1) 加载场景资源 + 反序列化
    std::string text = read_file(scn_path);
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

    // 脚本路径：相对场景目录
    std::filesystem::path scnp(scn_path);
    std::string script_path = rt.scene.script;
    if (!script_path.empty() && std::filesystem::path(script_path).is_relative())
        script_path = (scnp.parent_path() / script_path).string();

    // 2) 设备 + 脚本
    std::string backend;
    rt.dev = maque::gfx::create_device(&backend);
    if (!rt.dev) { std::printf("runtime: no WebGPU device\n"); return 2; }
    std::printf("runtime: WebGPU backend '%s'\n", backend.c_str());

    mq_script_vm* vm = mq_script_vm_create();
    mq_script_register_str(vm, "log", fn_log, &rt);
    mq_script_register_str(vm, "cfg", fn_cfg, &rt);
    mq_script_register_num(vm, "screen_w", fn_screen_w, &rt);
    mq_script_register_num(vm, "screen_h", fn_screen_h, &rt);
    mq_script_register_num(vm, "rng", fn_rng, &rt);
    mq_script_register_num(vm, "quit", fn_quit, &rt);
    mq_script_register_num(vm, "clear", fn_clear, &rt);
    mq_script_register_num(vm, "draw_sprite", fn_draw, &rt);

    if (!script_path.empty()) {
        std::string js = read_file(script_path);
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
        maque::gfx::render_offscreen(rt.dev, rt.W, rt.H, rt.draws.data(), rt.draws.size(),
                                     rt.bg, pixels);
        if (frames_dir) {
            char name[64]; std::snprintf(name, sizeof(name), "/frame_%04d.ppm", frame);
            write_ppm(std::string(frames_dir) + name, rt.W, rt.H, pixels);
        }
    }

    // 4) 退出
    std::printf("runtime: exited after %d frames (quit=%d)\n", frame, rt.quit ? 1 : 0);
    mq_script_vm_destroy(vm);
    maque::gfx::destroy_device(rt.dev);
    return rt.quit ? 0 : 1;
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
