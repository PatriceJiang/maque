/* M1 demo：真实 WebGPU 渲染的"下落弹跳球"。
 * 点击 → 生成球 → 落体/弹跳 → 高度到 0 → 5s 后移除。
 * 每帧用 WebGPU 离屏渲染并 readback，输出 PPM 帧序列（终端附 ASCII 预览）。 */
#include "ball_demo/ball_world.h"
#include "render/gfx/mq_gfx.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>

using namespace ball_demo;

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

static void draw_grid(const BallWorld& world, float world_w, float world_h, int cols, int rows) {
    std::vector<std::string> g(rows, std::string(cols, '.'));
    const float sx = cols / world_w, sy = rows / world_h;
    for (auto& s : world.sprites()) {
        int col = (int)std::lround(s.x * sx);
        int row = rows - 1 - (int)std::lround(s.y * sy);
        if (col >= 0 && col < cols && row >= 0 && row < rows) g[row][col] = 'O';
    }
    for (auto& line : g) std::printf("  %s\n", line.c_str());
}

int main(int argc, char** argv) {
    std::string out_dir = (argc > 1) ? argv[1] : "ball_frames";
    std::filesystem::create_directories(out_dir);

    std::string backend;
    maque::gfx::Device* dev = maque::gfx::create_device(&backend);
    if (!dev) { std::printf("no WebGPU device\n"); return 2; }
    std::printf("WebGPU backend: %s\n", backend.c_str());

    BallWorld world;
    const int   W = 320, H = 240;
    const float dt = 1.0F / 60.0F;
    const int   total_frames = 60 * 8;
    const float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };

    mq_handle h = world.on_click(160.0F, 200.0F);
    std::printf("click at (160,200) -> ball spawned (index=%u gen=%u)\n", h.index, h.generation);

    int removed_frame = -1;
    float t = 0.0F;
    std::vector<maque::gfx::SpriteDraw> draws;
    std::vector<uint8_t> pixels;

    for (int frame = 0; frame < total_frames; ++frame) {
        world.step(dt);
        t += dt;

        // 世界(y 向上) -> 屏幕(y 向下)
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

        if (frame % 30 == 0) {
            BallWorld::BallView bv{};
            std::printf("frame=%3d t=%.2fs balls=%d", frame, t, world.ball_count());
            if (world.get_ball(h, bv))
                std::printf("  ball y=%.1f vy=%.1f%s", bv.y, bv.vy, bv.resting ? " [resting]" : "");
            std::printf("\n");
            draw_grid(world, (float)W, (float)H, 40, 10);
        }

        if (removed_frame < 0 && world.ball_count() == 0) {
            removed_frame = frame;
            std::printf(">>> ball removed at t=%.2fs (frame %d)\n", t, frame);
        }
    }

    std::printf("\nsummary: frames=%d (WebGPU) written to '%s'; ball removed %s\n",
                total_frames, out_dir.c_str(), removed_frame >= 0 ? "YES" : "NO");
    maque::gfx::destroy_device(dev);
    return removed_frame >= 0 ? 0 : 1;
}
