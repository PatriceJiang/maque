#include "render/null/mq_renderer_null.h"

#include <array>
#include <cstdint>
#include <print>

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::print("FAIL {}:{}: {}\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)
}

int main() {
    const mq_renderer_v1* api = mq_renderer_null_api();
    CHECK(api != nullptr);
    CHECK(api->header.struct_size == sizeof(mq_renderer_v1));
    const char* backend = nullptr;
    void* self = api->create(&backend);
    CHECK(self != nullptr);
    CHECK(backend && backend[0] == 'n');

    const std::array<uint8_t, 4> white = { 255, 255, 255, 255 };
    const int32_t texture = api->create_texture(self, white.data(), 1, 1);
    CHECK(texture >= 0);
    CHECK(api->has_texture(self, texture) == 1);
    api->destroy_texture(self, texture);
    CHECK(api->has_texture(self, texture) == 0);

    std::array<uint8_t, 16> pixels{};
    const float bg[4] = { 1.0F, 0.5F, 0.0F, 1.0F };
    CHECK(api->render_views(self, 2, 2, nullptr, 0, nullptr, 0, bg,
                            pixels.data(), static_cast<uint32_t>(pixels.size())) == 0);
    CHECK(pixels[0] == 255 && pixels[1] == 128 && pixels[2] == 0 && pixels[3] == 255);

    mq_renderer_render_texture_desc rtd{};
    rtd.struct_size = sizeof rtd;
    rtd.width = 2; rtd.height = 2; rtd.pixel_format = 0; rtd.filter = 1;
    rtd.wrap_u = rtd.wrap_v = 1;
    rtd.clear_color[3] = 0.0F;
    int32_t target = -1;
    CHECK(api->create_render_texture(self, &rtd, &target) == 0);
    CHECK(target >= 0 && api->has_texture(self, target) == 1);
    mq_renderer_frame3d frame{};
    frame.width = 2; frame.height = 2;
    frame.bg[0] = 0.25F; frame.bg[1] = 0.5F; frame.bg[2] = 0.75F; frame.bg[3] = 1.0F;
    CHECK(api->render_to_texture(self, target, &frame) == 0);
    std::array<uint8_t, 16> captured{};
    CHECK(api->read_texture(self, target, captured.data(), (uint32_t)captured.size()) == 0);
    CHECK(captured[0] == 64 && captured[1] == 128 && captured[2] == 191 && captured[3] == 255);
    mq_renderer_view3d view3d{};
    view3d.clear_mode = 1;
    view3d.clear_color[0] = 0.1F; view3d.clear_color[1] = 0.8F;
    view3d.clear_color[2] = 0.2F; view3d.clear_color[3] = 1.0F;
    frame.views = &view3d; frame.view_count = 1;
    CHECK(api->render_to_texture(self, target, &frame) == 0);
    CHECK(api->read_texture(self, target, captured.data(), (uint32_t)captured.size()) == 0);
    CHECK(captured[0] == 26 && captured[1] == 204 && captured[2] == 51 && captured[3] == 255);
    frame.views = nullptr; frame.view_count = 0;
    rtd.width = 1; rtd.height = 3;
    CHECK(api->configure_render_texture(self, target, &rtd) == 0);
    frame.width = 1; frame.height = 3;
    CHECK(api->render_to_texture(self, target, &frame) == 0);
    CHECK(api->has_texture(self, target) == 1);                 // 重配后句柄稳定
    api->destroy_texture(self, target);
    CHECK(api->has_texture(self, target) == 0);

    api->destroy(self);
    std::print("renderer_null: {} failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
