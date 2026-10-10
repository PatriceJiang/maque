// 脚本模块（quickjs）单元测试
#include "core/script/mq_script.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static int host_add(void*, int argc, const double* a, double* ret) {
    if (argc != 2) return 1; *ret = a[0] + a[1]; return 0;
}
static int host_len(void*, const char* s, double* ret) {
    *ret = (double)std::strlen(s); return 0;
}

int main() {
    mq_script_vm* vm = mq_script_vm_create();
    CHECK(vm != nullptr);
    if (!vm) return 1;

    CHECK(mq_script_register_num(vm, "host_add", host_add, nullptr) == 0);
    CHECK(mq_script_register_str(vm, "host_len", host_len, nullptr) == 0);

    const char* src =
        "let n = 0;\n"
        "function on_start() { n = host_add(2, 3); }\n"
        "function value() { return n; }\n"
        "function slen() { return host_len('hello'); }\n";
    CHECK(mq_script_eval(vm, src, "test.js") == 0);
    CHECK(mq_script_has(vm, "on_start"));
    CHECK(!mq_script_has(vm, "nope"));

    double r = 0;
    CHECK(mq_script_call0(vm, "on_start", &r) == 0);
    CHECK(mq_script_call0(vm, "value", &r) == 0 && r == 5.0);
    CHECK(mq_script_call0(vm, "slen", &r) == 0 && r == 5.0);

    // 调用不存在的函数
    CHECK(mq_script_call0(vm, "missing", &r) != 0);

    // 语法错误
    CHECK(mq_script_eval(vm, "function ( {", "bad.js") != 0);

    mq_script_gc(vm);
    mq_script_vm_destroy(vm);

    if (g_failures == 0) { std::printf("script_test: all checks passed\n"); return 0; }
    std::printf("script_test: %d failure(s)\n", g_failures);
    return 1;
}
