/* M1 demo：正规流程 —— 加载 .scn → 反序列化 → 运行 → 退出。
 * 场景参数来自 scenes/*.scn（文本，反射驱动反序列化）；每帧经真实 WebGPU 渲染并输出 PPM。 */
#include "ball_demo/ball_world.h"
#include "render/gfx/mq_gfx.h"
#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "maque_types.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

using namespace ball_demo;

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

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
    const std::string out_dir  = (argc > 2) ? argv[2] : "ball_frames";

    // 1) 加载 .scn（文本）
    std::string text = read_file(scn_path);
    if (text.empty()) { std::printf("cannot read scene: %s\n", scn_path.c_str()); return 3; }

    // 2) 反序列化（反射驱动：JSON → scn::BallScene）
    mq_reflect_init();
    const mq_type_desc* stype = mq_reflect_type_by_name("scn.BallScene");
    if (!stype) { std::printf("scene type not found\n"); return 3; }
    scn::BallScene scene{};
    if (mq_persist_load_json(stype, text.c_str(), text.size(), &scene) != 0) {
        std::printf("failed to deserialize scene: %s\n", scn_path.c_str()); return 3;
    }
    std::printf("scene loaded: %s (w=%d h=%d g=%.0f e=%.2f r=%.0f delay=%.1f click=(%.0f,%.0f))\n",
                scn_path.c_str(), scene.width, scene.height, scene.gravity, scene.restitution,
                scene.radius, scene.remove_delay, scene.click.x, scene.click.y);

    // 3) 运行
    std::filesystem::create_directories(out_dir);
    std::string backend;
    maque::gfx::Device* dev = maque::gfx::create_device(&backend);
    if (!dev) { std::printf("no WebGPU device\n"); return 2; }
    std::printf("WebGPU backend: %s\n", backend.c_str());

    Config cfg;
    cfg.gravity = scene.gravity; cfg.restitution = scene.restitution;
    cfg.radius = scene.radius; cfg.remove_delay = scene.remove_delay;
    BallWorld world(cfg);

    const int W = scene.width, H = scene.height;
    const float dt = 1.0F / 60.0F;
    const int total_frames = 60 * 8;
    const float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };

    mq_handle h = world.on_click(scene.click.x, scene.click.y);
    std::printf("click at (%.0f,%.0f) -> ball spawned\n", scene.click.x, scene.click.y);

    int removed_frame = -1;
    float t = 0.0F;
    std::vector<maque::gfx::SpriteDraw> draws;
    std::vector<uint8_t> pixels;

    for (int frame = 0; frame < total_frames; ++frame) {
        world.step(dt);
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
        if (removed_frame < 0 && world.ball_count() == 0) {
            removed_frame = frame;
            std::printf(">>> ball removed at t=%.2fs (frame %d)\n", t, frame);
        }
    }

    // 4) 退出
    std::printf("done: %d frames -> '%s'; ball removed %s\n",
                total_frames, out_dir.c_str(), removed_frame >= 0 ? "YES" : "NO");
    maque::gfx::destroy_device(dev);
    return removed_frame >= 0 ? 0 : 1;
}
