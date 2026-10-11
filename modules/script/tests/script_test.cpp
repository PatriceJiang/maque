// 脚本模块（quickjs）单元测试
#include "core/script/mq_script.h"
#include <print>

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

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

    // ---- 热重载语义（docs/14 §6）：IIFE 形态 + 状态经 globalThis 载体迁移 ----
    const char* v1 =
        "(function () {\n"
        "  var S = globalThis.__st || (globalThis.__st = { n: 0 });\n"
        "  globalThis.bump = function () { S.n = S.n + 1; return S.n; };\n"
        "  globalThis.version = function () { return 1; };\n"
        "})();";
    CHECK(mq_script_eval(vm, v1, "hot1.js") == 0);
    double a = 0, b = 0;
    CHECK(mq_script_call0(vm, "bump", &a) == 0 && a == 1.0);
    CHECK(mq_script_call0(vm, "bump", &a) == 0 && a == 2.0);
    CHECK(mq_script_call0(vm, "version", &a) == 0 && a == 1.0);

    // 重载同一形态脚本（新词法作用域）：函数被替换，globalThis 载体状态保留
    const char* v2 =
        "(function () {\n"
        "  var S = globalThis.__st || (globalThis.__st = { n: 0 });\n"
        "  globalThis.bump = function () { S.n = S.n + 10; return S.n; };\n"
        "  globalThis.version = function () { return 2; };\n"
        "})();";
    CHECK(mq_script_eval(vm, v2, "hot2.js") == 0);
    CHECK(mq_script_call0(vm, "bump", &b) == 0 && b == 12.0);    // 2 + 10：状态迁移成功
    CHECK(mq_script_call0(vm, "version", &b) == 0 && b == 2.0);  // 函数已替换

    // 语法错误不得破坏已加载的脚本
    CHECK(mq_script_eval(vm, "function ( {", "bad2.js") != 0);
    CHECK(mq_script_call0(vm, "version", &b) == 0 && b == 2.0);
    char err[256] = { 0 };
    CHECK(mq_script_last_error(vm, err, (int)sizeof err) == 0 && err[0] != '\0');
    CHECK(mq_script_call0(vm, "bump", &b) == 0 && b == 22.0);    // 旧脚本仍可调用

    mq_script_gc(vm);
    mq_script_vm_destroy(vm);

    if (g_failures == 0) { std::print("script_test: all checks passed\n"); return 0; }
    std::print("script_test: {} failure(s)\n", g_failures);
    return 1;
}
