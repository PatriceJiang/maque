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
    // ---- 自省：方法枚举（MCP tools/list 的来源）----
    mq_control_register(c, "Scene.info", h_perf, nullptr);
    CHECK(mq_control_method_count(c) == 2);
    CHECK(std::strcmp(mq_control_method_at(c, 0), "Perf.get") == 0);      // 字典序
    CHECK(std::strcmp(mq_control_method_at(c, 1), "Scene.info") == 0);
    CHECK(mq_control_method_at(c, 5) == nullptr);

    // ---- MCP 包装（docs/11 §2）----
    const char* m1 = mq_control_dispatch_mcp(c, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    CHECK(std::strstr(m1, "protocolVersion") && std::strstr(m1, "maque"));

    const char* m2 = mq_control_dispatch_mcp(c, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    CHECK(std::strstr(m2, "\"Perf.get\"") && std::strstr(m2, "\"Scene.info\"") && std::strstr(m2, "inputSchema"));

    const char* m3 = mq_control_dispatch_mcp(c,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"Perf.get\",\"arguments\":{}}}");
    CHECK(std::strstr(m3, "content") && std::strstr(m3, "frames"));

    const char* m4 = mq_control_dispatch_mcp(c,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"Nope\",\"arguments\":{}}}");
    CHECK(std::strstr(m4, "isError"));

    const char* m5 = mq_control_dispatch_mcp(c, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    CHECK(std::strstr(m5, "{}"));

    const char* m6 = mq_control_dispatch_mcp(c, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"nope/nope\"}");
    CHECK(std::strstr(m6, "-32601"));

    mq_control_destroy(c);
    if (g_failures == 0) { std::print("control_test: all checks passed\n"); return 0; }
    return 1;
}
