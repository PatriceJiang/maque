// 可观测性单测：事件/指标/JSON 导出
#include "core/observability/mq_obs.h"
#include <print>
#include <cstring>
static int g_failures = 0;
#define CHECK(c) do { if(!(c)){ std::print("FAIL {}:{}\n", __FILE__, __LINE__); ++g_failures; } } while(0)
int main() {
    mq_obs_ctx* o = mq_obs_create();
    CHECK(o != nullptr);
    mq_obs_event(o, "render", 1, "frame");
    mq_obs_counter_add(o, "draws", 3);
    mq_obs_counter_add(o, "draws", 2);
    mq_obs_gauge_set(o, "frame_ms", 16.7);
    CHECK(mq_obs_gauge_get(o, "frame_ms") > 16.0);
    const char* j = mq_obs_dump_json(o);
    CHECK(std::strstr(j, "\"draws\":5") != nullptr);
    CHECK(std::strstr(j, "render") != nullptr);
    mq_obs_destroy(o);
    if (g_failures == 0) { std::print("obs_test: all checks passed\n"); return 0; }
    return 1;
}
