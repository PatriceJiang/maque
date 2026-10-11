// Lua 后端单测（docs/14「统一后端 ABI」）：与 js 后端同形的 API 面、宿主注册、
// 回调调用、错误捕获、沙箱（无 io/os/require…）、引擎 RNG、帧边界 GC、语言选择。
#include "core/script/mq_script.h"

#include <print>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

/* ---- 宿主函数样例（与 runtime 注册方式一致）---- */
static int host_add(void*, int argc, const double* a, double* ret) {
    double s = 0;
    for (int i = 0; i < argc; ++i) s += a[i];
    *ret = s;
    return 0;
}
static int host_greet(void*, const char* name, double* ret) {
    *ret = (double)std::strlen(name ? name : "");
    return 0;
}
static int host_name(void*, int, const double*, char* out, int cap) {
    std::snprintf(out, (size_t)cap, "entity-%d", 7);
    return 0;
}
static int host_setpos(void*, int nnum, const double* nums, int nstr, const char** strs, double* ret) {
    (void)strs;
    *ret = nnum >= 2 ? nums[0] + nums[1] : 0.0;
    (void)nstr;
    return 0;
}
static uint32_t g_rng_state = 12345;
static uint32_t test_rng(void*) { g_rng_state = g_rng_state * 1664525u + 1013904223u; return g_rng_state; }

int main() {
    // ---- 1) 后端选择：语言 id 与扩展名 ----
    CHECK(std::strcmp(mq_script_lang_for_path("game.lua"), "lua") == 0);
    CHECK(std::strcmp(mq_script_lang_for_path("game.js"), "js") == 0);
    CHECK(std::strcmp(mq_script_lang_for_path("noext"), "js") == 0);
    CHECK(std::strcmp(mq_script_lang_for_path(nullptr), "js") == 0);
    char backends[64] = { 0 };
    CHECK(mq_script_backends(backends, sizeof backends) == 0);
    CHECK(std::strstr(backends, "js") != nullptr);
    CHECK(std::strstr(backends, "lua") != nullptr);       // 本构建编入了 Lua

    mq_script_vm* lua = mq_script_vm_create_lang("lua");
    CHECK(lua != nullptr);
    CHECK(std::strcmp(mq_script_lang(lua), "lua") == 0);
    mq_script_vm* js = mq_script_vm_create_lang("js");
    CHECK(js != nullptr && std::strcmp(mq_script_lang(js), "js") == 0);
    CHECK(mq_script_vm_create_lang("brainfuck") == nullptr);
    CHECK(mq_script_vm_create() != nullptr);              // 默认后端

    // ---- 2) 宿主注册 + 脚本调用（同 API 面：num/str/ret_str/multi、命名空间）----
    CHECK(mq_script_register_num(lua, "host_add", host_add, nullptr) == 0);
    CHECK(mq_script_register_str(lua, "greet", host_greet, nullptr) == 0);
    CHECK(mq_script_register_ret_str(lua, "ent_name", host_name, nullptr) == 0);
    CHECK(mq_script_register_multi(lua, "set_pos", "nn", host_setpos, nullptr) == 0);
    CHECK(mq_script_register_num(lua, "hud.score", host_add, nullptr) == 0);   // 命名空间

    const char* src = R"(
        frames = 0
        function on_start()
            hits = host_add(2, 3, 4)              -- 多参数
            name_len = greet("maque")             -- 字符串参数
            ename = ent_name(7)                   -- 返回字符串
            pos = set_pos(1.5, 2.5)               -- multi kinds="nn"
            hud.score(10, 5)                      -- 命名空间调用
            return hits
        end
        function on_update(dt)
            frames = frames + 1
            return frames
        end
        function with_str(a, s)
            return a + #s
        end
    )";
    CHECK(mq_script_eval(lua, src, "test.lua") == 0);
    CHECK(mq_script_has(lua, "on_start") == 1);
    CHECK(mq_script_has(lua, "missing_fn") == 0);
    double ret = 0;
    CHECK(mq_script_call0(lua, "on_start", &ret) == 0);
    CHECK(ret == 9.0);                                    // 2+3+4
    CHECK(mq_script_call1(lua, "on_update", 0.016, &ret) == 0);
    CHECK(ret == 1.0);
    CHECK(mq_script_call1(lua, "on_update", 0.016, &ret) == 0);
    CHECK(ret == 2.0);
    CHECK(mq_script_call1s(lua, "with_str", 10.0, "abcd", &ret) == 0);
    CHECK(ret == 14.0);                                   // 10 + #"abcd"
    // 命名空间函数真的被调用（返回值未用，但不应报错）
    CHECK(mq_script_call1(lua, "missing", 1.0, &ret) == 1);   // 不存在 → 1

    // ---- 3) 跨语言等价：同一份语义在 Lua 与 JS 下结果一致 ----
    {
        const char* lua_src = R"(
            function compute(n)
                local s = 0
                for i = 1, n do s = s + i * 2 end
                return s
            end
        )";
        const char* js_src = R"(
            function compute(n) { let s = 0; for (let i = 1; i <= n; ++i) s += i * 2; return s; }
        )";
        CHECK(mq_script_eval(lua, lua_src, "eq.lua") == 0);
        CHECK(mq_script_eval(js, js_src, "eq.js") == 0);
        double a = 0, b = 0;
        CHECK(mq_script_call1(lua, "compute", 10.0, &a) == 0);
        CHECK(mq_script_call1(js, "compute", 10.0, &b) == 0);
        CHECK(a == b && a == 110.0);
    }

    // ---- 4) 引擎 RNG：math.random 与 js 的 Math.random 同源 ----
    {
        CHECK(mq_script_eval(lua, "function r() return math.random() end", "rng.lua") == 0);
        mq_script_set_rng(lua, test_rng, nullptr);
        g_rng_state = 999;
        const uint32_t first = test_rng(nullptr);
        double v = 0;
        CHECK(mq_script_call0(lua, "r", &v) == 0);
        CHECK(v > 0.0 && v < 1.0);
        g_rng_state = 999;
        const double expect = (double)(first) / 4294967296.0;
        g_rng_state = 999;
        CHECK(mq_script_call0(lua, "r", &v) == 0);
        CHECK(v == expect);                               // 由注入 RNG 决定（可回放）
    }

    // ---- 5) 沙箱：外部世界与影响确定性的入口都不可达 ----
    {
        const char* probe = R"(
            function probe()
                local blocked = 0
                if os == nil then blocked = blocked + 1 end
                if io == nil then blocked = blocked + 1 end
                if package == nil then blocked = blocked + 1 end
                if debug == nil then blocked = blocked + 1 end
                if require == nil then blocked = blocked + 1 end
                if dofile == nil then blocked = blocked + 1 end
                if loadfile == nil then blocked = blocked + 1 end
                if collectgarbage == nil then blocked = blocked + 1 end
                if coroutine == nil then blocked = blocked + 1 end
                return blocked
            end
        )";
        CHECK(mq_script_eval(lua, probe, "sandbox.lua") == 0);
        double blocked = 0;
        CHECK(mq_script_call0(lua, "probe", &blocked) == 0);
        CHECK(blocked == 9.0);
        // 安全库仍可用
        CHECK(mq_script_eval(lua, "function safe() return math.floor(2.7) + #('abc') end", "safe.lua") == 0);
        double s = 0;
        CHECK(mq_script_call0(lua, "safe", &s) == 0);
        CHECK(s == 5.0);
    }

    // ---- 6) 错误捕获：语法错误与运行错误都不崩，且记录 last_error ----
    {
        char err[256] = { 0 };
        CHECK(mq_script_eval(lua, "function broken( end", "bad.lua") == 2);
        CHECK(mq_script_last_error(lua, err, (int)sizeof err) == 0);
        CHECK(std::strlen(err) > 0);
        CHECK(mq_script_eval(lua, "function boom() error('boom!') end", "boom.lua") == 0);
        CHECK(mq_script_call0(lua, "boom", nullptr) == 2);
        err[0] = 0;
        CHECK(mq_script_last_error(lua, err, (int)sizeof err) == 0);
        CHECK(std::strstr(err, "boom!") != nullptr);
        // 出错后 VM 仍可用（受保护调用）
        CHECK(mq_script_eval(lua, "function ok() return 42 end", "ok.lua") == 0);
        double v = 0;
        CHECK(mq_script_call0(lua, "ok", &v) == 0 && v == 42.0);
    }

    // ---- 7) 帧边界 GC：可调用且不破坏状态 ----
    {
        CHECK(mq_script_eval(lua, "acc = 0\nfunction bump() acc = acc + 1 return acc end", "gc.lua") == 0);
        double v = 0;
        for (int i = 0; i < 100; ++i) {
            CHECK(mq_script_call0(lua, "bump", &v) == 0);
            mq_script_frame_boundary(lua);            // 帧边界（内部做固定步数 GC）
        }
        CHECK(v == 100.0);
        mq_script_gc(lua);
        CHECK(mq_script_call0(lua, "bump", &v) == 0 && v == 101.0);
    }

    // ---- 8) 回调用例（与 runtime 的事件派发一致：全局函数名 + 参数）----
    {
        const char* handlers = R"(
            log_lines = {}
            function note(s) log_lines[#log_lines + 1] = s end
            function on_pointer_down(target, x, y) note(target .. '@' .. x .. ',' .. y) end
            function on_text(target, s) note(s) end
        )";
        CHECK(mq_script_eval(lua, handlers, "events.lua") == 0);
        CHECK(mq_script_call3(lua, "on_pointer_down", -1.0, 40.0, 180.0, nullptr) == 0);
        CHECK(mq_script_call1s(lua, "on_text", 0.0, "hello", nullptr) == 0);
        CHECK(mq_script_eval(lua, "function last() return log_lines[#log_lines] end", "last.lua") == 0);
        double v = 0;
        CHECK(mq_script_call0(lua, "last", &v) == 0);      // 不崩即可（字符串结果按 0 返回）
        CHECK(mq_script_eval(lua, "function count() return #log_lines end", "count.lua") == 0);
        CHECK(mq_script_call0(lua, "count", &v) == 0 && v == 2.0);
    }

    mq_script_vm_destroy(lua);
    mq_script_vm_destroy(js);
    if (g_failures == 0) { std::print("lua_test: all checks passed\n"); return 0; }
    std::print("lua_test: {} failure(s)\n", g_failures);
    return 1;
}
