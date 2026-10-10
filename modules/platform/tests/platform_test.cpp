// platform(SDL2) 单元测试（headless：dummy 视频驱动）
#include "core/platform/mq_platform.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    mq_platform* p = mq_platform_create("maque-test", 320, 240);
    CHECK(p != nullptr);

    mq_input_state s{};
    mq_platform_poll(p, &s);
    CHECK(s.events >= 0);
    CHECK(s.quit == 0);
    CHECK(s.mouse_x >= 0.0F && s.mouse_y >= 0.0F);

    mq_platform_destroy(p);

    if (g_failures == 0) { std::printf("platform_test: all checks passed\n"); return 0; }
    std::printf("platform_test: %d failure(s)\n", g_failures);
    return 1;
}
