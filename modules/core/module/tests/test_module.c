/* 一个最小测试模块 .so：仅导出 mq_module_query。 */
#include "test_iface.h"
#include <string.h>

static int test_add(int a, int b) { return a + b; }
static const char* test_greet(void) { return "hello from module"; }

static mq_test_api_v1 g_api = {
    { sizeof(mq_test_api_v1), MQ_ABI_VERSION },
    test_add,
    test_greet,
};

MQ_MODULE_EXPORT void* mq_module_query(const char* interface_name, uint32_t min_version) {
    if (min_version > MQ_ABI_VERSION) return 0;
    if (strcmp(interface_name, MQ_TEST_API_NAME) == 0) return &g_api;
    return 0;
}
