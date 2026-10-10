// 控制平面单测：JSON-RPC 帧 + 方法分发
#include "core/control/mq_control.h"
#include <print>
#include <cstring>
static int g_failures = 0;
#define CHECK(c) do { if(!(c)){ std::print("FAIL {}:{}\n", __FILE__, __LINE__); ++g_failures; } } while(0)
static int h_perf(void*, const char*, const mq_json*, char* out, size_t cap) {
    std::snprintf(out, cap, "{\"frames\":42}"); return 0;
}
int main() {
    mq_control* c = mq_control_create();
    mq_control_register(c, "Perf.get", h_perf, nullptr);
    const char* r1 = mq_control_dispatch(c, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Perf.get\"}");
    CHECK(std::strstr(r1, "\"id\":1") && std::strstr(r1, "\"frames\":42") && std::strstr(r1, "\"result\""));
    const char* r2 = mq_control_dispatch(c, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"Nope\"}");
    CHECK(std::strstr(r2, "-32601") && std::strstr(r2, "method not found"));
    mq_control_destroy(c);
    if (g_failures == 0) { std::print("control_test: all checks passed\n"); return 0; }
    return 1;
}
