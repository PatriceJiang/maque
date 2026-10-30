// 脚本模块（quickjs）单元测试
#include "core/script/mq_script.h"
#include <print>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
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
static uint32_t s_rng_state = 1u;
static uint32_t host_rng(void*) { s_rng_state = s_rng_state * 1664525u + 1013904223u; return s_rng_state; }

struct ByteState { int releases = 0; };
static void host_bytes_release(void* user, void* data) {
    ++static_cast<ByteState*>(user)->releases;
    std::free(data);
}
static int host_bytes(void* user, int nnum, const double* nums, int nbytes,
                      const void** bytes, const size_t* sizes, mq_script_bytes_result* ret) {
    if (!user || !ret || nnum != 1 || nbytes != 1) return 1;
    auto* out = static_cast<uint8_t*>(std::malloc(sizes[0]));
    if (!out && sizes[0] != 0) return 1;
    const auto* in = static_cast<const uint8_t*>(bytes[0]);
    for (size_t i = 0; i < sizes[0]; ++i) out[i] = (uint8_t)(in[i] + (int)nums[0]);
    ret->data = out;
    ret->size = sizes[0];
    ret->release = host_bytes_release;
    ret->release_user = user;
    return 0;
}

int main() {
    mq_script_vm* vm = mq_script_vm_create();
    CHECK(vm != nullptr);
    if (!vm) return 1;

    CHECK(mq_script_register_num(vm, "host_add", host_add, nullptr) == 0);
    CHECK(mq_script_register_str(vm, "host_len", host_len, nullptr) == 0);
    ByteState byte_state;
    CHECK(mq_script_register_bytes(vm, "host_bytes", "bn", host_bytes, &byte_state) == 0);

    const char* src =
        "let n = 0;\n"
        "function on_start() { n = host_add(2, 3); }\n"
        "function value() { return n; }\n"
        "function slen() { return host_len('hello'); }\n"
        "function bytes_u8() { let r=host_bytes(new Uint8Array([0,127,128,255]),1);"
        " return r instanceof Uint8Array && r.length===4 && r[0]===1 && r[1]===128 && r[2]===129 && r[3]===0 ? 1 : 0; }\n"
        "function bytes_ab() { let a=new Uint8Array([0,255]); let r=host_bytes(a.buffer,2);"
        " return r.length===2 && r[0]===2 && r[1]===1 ? 1 : 0; }\n"
        "function bytes_text() { let r=host_bytes('A\\0B',0);"
        " return r.length===3 && r[0]===65 && r[1]===0 && r[2]===66 ? 1 : 0; }\n";
    CHECK(mq_script_eval(vm, src, "test.js") == 0);
    CHECK(mq_script_has(vm, "on_start"));
    CHECK(!mq_script_has(vm, "nope"));

    double r = 0;
    CHECK(mq_script_call0(vm, "on_start", &r) == 0);
    CHECK(mq_script_call0(vm, "value", &r) == 0 && r == 5.0);
    CHECK(mq_script_call0(vm, "slen", &r) == 0 && r == 5.0);
    CHECK(mq_script_call0(vm, "bytes_u8", &r) == 0 && r == 1.0);
    CHECK(mq_script_call0(vm, "bytes_ab", &r) == 0 && r == 1.0);
    CHECK(mq_script_call0(vm, "bytes_text", &r) == 0 && r == 1.0);
    CHECK(byte_state.releases == 3);

    // 调用不存在的函数
    CHECK(mq_script_call0(vm, "missing", &r) != 0);

    // 语法错误
    CHECK(mq_script_eval(vm, "function ( {", "bad.js") != 0);

    // ---- 同一 VM 内二次 eval 的语义（工具/测试路径；引擎热更**不**走这条：热更 = 新建 VM，
    //      旧 VM 销毁 → globalThis 也随 VM 消失，脚本状态不跨重载，docs/14 §7）：
    //      IIFE 形态可安全重复 eval（顶层 let/const 不重复声明），状态载体显式放 globalThis ----
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

    // 同一 VM 内重复 eval 同一形态脚本（新词法作用域）：函数被替换，globalThis 载体状态保留
    const char* v2 =
        "(function () {\n"
        "  var S = globalThis.__st || (globalThis.__st = { n: 0 });\n"
        "  globalThis.bump = function () { S.n = S.n + 10; return S.n; };\n"
        "  globalThis.version = function () { return 2; };\n"
        "})();";
    CHECK(mq_script_eval(vm, v2, "hot2.js") == 0);
    CHECK(mq_script_call0(vm, "bump", &b) == 0 && b == 12.0);    // 2 + 10：同 VM 内载体状态延续
    CHECK(mq_script_call0(vm, "version", &b) == 0 && b == 2.0);  // 函数已替换

    // 语法错误不得破坏已加载的脚本
    CHECK(mq_script_eval(vm, "function ( {", "bad2.js") != 0);
    CHECK(mq_script_call0(vm, "version", &b) == 0 && b == 2.0);
    char err[256] = { 0 };
    CHECK(mq_script_last_error(vm, err, (int)sizeof err) == 0 && err[0] != '\0');
    CHECK(mq_script_call0(vm, "bump", &b) == 0 && b == 22.0);    // 旧脚本仍可调用

    // ---- 引擎 RNG 注入（docs/10 D2）：Math.random 必须可复现，不再走墙钟熵 ----
    {
        mq_script_vm* a = mq_script_vm_create();
        mq_script_vm* b = mq_script_vm_create();
        mq_script_set_rng(a, host_rng, nullptr);
        mq_script_set_rng(b, host_rng, nullptr);
        CHECK(mq_script_eval(a, "function r(){return Math.random();}", "r.js") == 0);
        CHECK(mq_script_eval(b, "function r(){return Math.random();}", "r.js") == 0);
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        CHECK(mq_script_call0(a, "r", &x1) == 0);
        CHECK(mq_script_call0(a, "r", &x2) == 0);
        CHECK(x1 != x2);                                  // 序列在推进
        s_rng_state = 1u;                                 // 复位同一状态
        CHECK(mq_script_call0(b, "r", &y1) == 0);
        CHECK(mq_script_call0(b, "r", &y2) == 0);
        CHECK(x1 == y1 && x2 == y2);                      // 同种子 → 同序列（可回放）
        CHECK(x1 >= 0.0 && x1 < 1.0);
        // per-instance 错误：A 的异常不得覆盖 B
        CHECK(mq_script_eval(a, "throw new Error('boom-A')", "bad.js") != 0);
        char ea[128] = { 0 }, eb[128] = { 0 };
        CHECK(mq_script_last_error(a, ea, (int)sizeof ea) == 0);
        CHECK(std::strstr(ea, "boom-A") != nullptr);
        CHECK(mq_script_last_error(b, eb, (int)sizeof eb) != 0);   // B 无错
        mq_script_vm_destroy(a);
        mq_script_vm_destroy(b);
    }

    mq_script_gc(vm);
    mq_script_vm_destroy(vm);

    if (g_failures == 0) { std::print("script_test: all checks passed\n"); return 0; }
    std::print("script_test: {} failure(s)\n", g_failures);
    return 1;
}
