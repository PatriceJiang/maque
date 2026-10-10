// maque 模块宿主单元测试（独立：仅依赖 maque::module + 一个测试 .so）
#include "core/module/mq_module.h"
#include <print>
#include "test_iface.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::print("usage: {} <module.so>\n", argv[0]); return 2; }

    mq_host* host = mq_host_create();
    CHECK(host != nullptr);
    CHECK(mq_host_module_count(host) == 0);

    mq_loaded_module* mod = nullptr;
    CHECK(mq_host_load(host, argv[1], &mod) == 0);
    CHECK(mod != nullptr);
    CHECK(mq_host_module_count(host) == 1);

    // 查询接口并调用
    auto* api = (mq_test_api_v1*)mq_host_query(host, MQ_TEST_API_NAME, 1);
    CHECK(api != nullptr);
    CHECK(api && api->add(2, 3) == 5);
    CHECK(api && std::strcmp(api->greet(), "hello from module") == 0);

    // 未知接口 / 过高版本 → NULL
    CHECK(mq_host_query(host, "maque.nonexistent", 1) == nullptr);
    CHECK(mq_host_query(host, MQ_TEST_API_NAME, 999) == nullptr);

    // 加载不存在的模块 → 失败
    mq_loaded_module* bad = nullptr;
    CHECK(mq_host_load(host, "/no/such/module.so", &bad) != 0);
    CHECK(bad == nullptr);

    // 卸载
    mq_host_unload(host, mod);
    CHECK(mq_host_module_count(host) == 0);

    mq_host_destroy(host);

    if (g_failures == 0) { std::print("host_test: all checks passed\n"); return 0; }
    std::print("host_test: {} failure(s)\n", g_failures);
    return 1;
}
