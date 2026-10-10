// gfx 冒烟测试：经模块宿主 dlopen 渲染插件，走生成的 C ABI vtable
#include "maque_ifaces.h"
#include "core/module/mq_module.h"

#include <print>
#include <vector>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    mq_host* host = mq_host_create();
    mq_loaded_module* mod = nullptr;
    if (mq_host_load(host, MQ_GFX_PATH, &mod) != 0) { std::print("gfx_test: SKIP (no plugin)\n"); return 0; }
    auto* api = (const mq_renderer_v1*)mq_host_query(host, MQ_RENDERER_API_NAME, 1);
    CHECK(api != nullptr);
    if (!api) return 1;

    const char* backend = nullptr;
    void* self = api->create(&backend);
    CHECK(self != nullptr);
    std::print("gfx_test: plugin backend='{}'\n", backend ? backend : "?");

    mq_renderer_item items[2] = {
        { 160.0F, 120.0F, 40.0F, 1.0F, 0.4F, 0.2F, 1.0F, -1 },
        {  60.0F,  60.0F, 20.0F, 0.2F, 1.0F, 0.4F, 1.0F, -1 },
    };
    float bg[4] = { 0.06F, 0.06F, 0.09F, 1.0F };
    mq_renderer_view v{ 0, 0, 320, 240, 0, 0 };
    std::vector<uint8_t> px(320u * 240u * 4u);
    int rc = api->render_views(self, 320, 240, &v, 1, items, 2, bg, px.data(), (uint32_t)px.size());
    CHECK(rc == 0);
    auto at = [&](int x, int y, int c) { return (int)px[((size_t)y * 320 + x) * 4 + c]; };
    CHECK(at(160, 120, 0) > 200 && at(160, 120, 1) < 150);
    CHECK(at(60, 60, 1) > 200);
    CHECK(at(2, 2, 0) < 40);

    api->destroy(self);
    mq_host_destroy(host);
    if (g_failures == 0) { std::print("gfx_test: all checks passed\n"); return 0; }
    return 1;
}
