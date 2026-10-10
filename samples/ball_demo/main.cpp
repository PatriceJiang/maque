/* M2 demo：正规流程 + 脚本驱动。
 * 加载 .scn → 反序列化 → 加载 .js(quickjs) → 运行(WebGPU) → 退出。
 * 物理/渲染在引擎；"生成球/移除时机"等游戏逻辑在 JS 脚本。 */
#include "ball_demo/ball_world.h"
#include "render/gfx/mq_gfx.h"
#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "core/script/mq_script.h"
#include "maque_types.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

using namespace ball_demo;

static BallWorld* g_world = nullptr;
static const mq_type_desc* g_scene_type = nullptr;
static scn::BallScene g_scene{};

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 反射驱动：按路径读场景标量字段（cfg("click.x")）。 */
static double cfg_field(const char* path) {
    const mq_type_desc* t = g_scene_type;
    void* obj = &g_scene;
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

static int host_log(void*, const char* s, double*) { std::printf("%s\n", s); return 0; }
static int host_cfg(void*, const char* path, double* ret) { *ret = cfg_field(path); return 0; }
static int host_spawn(void*, int argc, const double* a, double* ret) {
    if (argc != 2) { *ret = -1; return 0; }
    mq_handle h = g_world->on_click((float)a[0], (float)a[1]);
    *ret = (h.index == MQ_HANDLE_INDEX_NONE) ? -1.0 : (double)h.index; return 0;
}
static int host_query(void* u, int argc, const double* a, double* ret) {
    *ret = 0; if (argc < 1) return 0;
    int which = (int)(intptr_t)u;
    mq_handle h = g_world->handle_at((uint32_t)a[0]);
    BallWorld::BallView bv{};
    if (!g_world->get_ball(h, bv)) { *ret = (which == 4) ? 1.0 : 0.0; return 0; }
    switch (which) {
        case 0: *ret = bv.x; break;
        case 1: *ret = bv.y; break;
        case 2: *ret = bv.vy; break;
        case 3: *ret = bv.resting ? 1.0 : 0.0; break;
        case 4: *ret = bv.rest_time; break;
    }
    return 0;
}
static int host_remove(void*, int argc, const double* a, double*) {
    if (argc >= 1) g_world->remove(g_world->handle_at((uint32_t)a[0]));
    return 0;
}
static int host_count(void*, int, const double*, double* ret) { *ret = (double)g_world->ball_count(); return 0; }

static bool write_ppm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    const std::string scn_path = (argc > 1) ? argv[1] : "scenes/ball_demo.scn";
    const std::string js_path  = (argc > 2) ? argv[2] : "scenes/ball_demo.js";
    const std::string out_dir  = (argc > 3) ? argv[3] : "ball_frames";

    // 1) 加载 .scn + 反序列化
    std::string text = read_file(scn_path);
    if (text.empty()) { std::printf("cannot read scene: %s\n", scn_path.c_str()); return 3; }
    mq_reflect_init();
    g_scene_type = mq_reflect_type_by_name("scn.BallScene");
    if (!g_scene_type || mq_persist_load_json(g_scene_type, text.c_str(), text.size(), &g_scene) != 0) {
        std::printf("failed to load scene\n"); return 3;
    }
    std::printf("scene: %s (w=%d h=%d g=%.0f e=%.2f delay=%.1f click=(%.0f,%.0f))\n",
                scn_path.c_str(), g_scene.width, g_scene.height, g_scene.gravity,
                g_scene.restitution, g_scene.remove_delay, g_scene.click.x, g_scene.click.y);

    Config cfg;
    cfg.gravity = g_scene.gravity; cfg.restitution = g_scene.restitution;
    cfg.radius = g_scene.radius; cfg.remove_delay = g_scene.remove_delay;
    cfg.auto_remove = false;                       // 移除由脚本决定
    BallWorld world(cfg);
    g_world = &world;

    // 2) 脚本（quickjs）
    std::string js = read_file(js_path);
    if (js.empty()) { std::printf("cannot read script: %s\n", js_path.c_str()); return 3; }
    mq_script_vm* vm = mq_script_vm_create();
    mq_script_register_str(vm, "log", host_log, nullptr);
    mq_script_register_str(vm, "cfg", host_cfg, nullptr);
    mq_script_register_num(vm, "spawn_ball", host_spawn, nullptr);
    mq_script_register_num(vm, "ball_x", host_query, (void*)(intptr_t)0);
    mq_script_register_num(vm, "ball_y", host_query, (void*)(intptr_t)1);
    mq_script_register_num(vm, "ball_vy", host_query, (void*)(intptr_t)2);
    mq_script_register_num(vm, "ball_resting", host_query, (void*)(intptr_t)3);
    mq_script_register_num(vm, "ball_rest_time", host_query, (void*)(intptr_t)4);
    mq_script_register_num(vm, "remove_ball", host_remove, nullptr);
    mq_script_register_num(vm, "ball_count", host_count, nullptr);
    if (mq_script_eval(vm, js.c_str(), js_path.c_str()) != 0) { std::printf("script eval failed\n"); return 3; }
    if (mq_script_call0(vm, "on_start", nullptr) != 0) { std::printf("on_start failed\n"); return 3; }

    // 3) 运行
    std::filesystem::create_directories(out_dir);
    std::string backend;
    maque::gfx::Device* dev = maque::gfx::create_device(&backend);
    if (!dev) { std::printf("no WebGPU device\n"); return 2; }
    std::printf("WebGPU backend: %s\n", backend.c_str());

    const int W = g_scene.width, H = g_scene.height;
    const float dt = 1.0F / 60.0F;
    const int total_frames = 60 * 8;
    const float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };

    int removed_frame = -1;
    float t = 0.0F;
    std::vector<maque::gfx::SpriteDraw> draws;
    std::vector<uint8_t> pixels;

    for (int frame = 0; frame < total_frames; ++frame) {
        world.step(dt);
        double removed = 0;
        mq_script_call1(vm, "update", dt, &removed);   // 脚本逻辑
        mq_script_gc(vm);                              // 帧边界 GC（确定性）
        t += dt;

        draws.clear();
        for (auto& s : world.sprites()) {
            float r = ((s.color >> 16) & 0xFF) / 255.0F;
            float g = ((s.color >> 8) & 0xFF) / 255.0F;
            float b = (s.color & 0xFF) / 255.0F;
            draws.push_back({ s.x, (float)H - s.y, s.radius, r, g, b, 1.0F });
        }
        maque::gfx::render_offscreen(dev, W, H, draws.data(), draws.size(), bg, pixels);
        char name[64];
        std::snprintf(name, sizeof(name), "/frame_%04d.ppm", frame);
        write_ppm(out_dir + name, W, H, pixels);

        if (frame % 60 == 0) std::printf("frame=%3d t=%.2fs balls=%d\n", frame, t, world.ball_count());
        if (removed_frame < 0 && world.ball_count() == 0 && frame > 0) {
            removed_frame = frame;
            std::printf(">>> ball removed at t=%.2fs (frame %d)\n", t, frame);
        }
    }

    // 4) 退出
    std::printf("done: %d frames -> '%s'; ball removed %s\n",
                total_frames, out_dir.c_str(), removed_frame >= 0 ? "YES" : "NO");
    mq_script_vm_destroy(vm);
    maque::gfx::destroy_device(dev);
    return removed_frame >= 0 ? 0 : 1;
}
