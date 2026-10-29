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

    api->destroy(self);
    std::print("renderer_null: {} failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
