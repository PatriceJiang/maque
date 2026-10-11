/* Lua 脚本后端（docs/14「统一后端 ABI」的 lua 后端）—— 与 js 后端**同形**：
 *   · 同一套宿主注册接口（register_num/str/ret_str/mixed/multi…）→ DSL 生成的绑定与 runtime
 *     注册的宿主函数在两个后端上完全一致（脚本侧无需为语言写两遍 API）；
 *   · 同一套回调约定：脚本定义全局函数 on_start / update(dt) / on_pointer_down(target,x,y) …
 *   · 命名空间：`hud.score` 这类名字在 Lua 侧落到表 `hud = { score = fn }`（同 JS 的对象）。
 *
 * 沙箱与确定性（docs/10）：
 *   · 只开基础库（base/table/string/math/utf8），**不开** io / os / package / debug / coroutine，
 *     并移除 dofile/loadfile/load/require/collectgarbage → 脚本不能触达外部世界或左右 GC 时机；
 *   · `math.random`/`math.randomseed` 由**引擎 RNG** 接管（与 JS 后端的 Math.random 一致，
 *     同一 seed → 同一序列 → 可回放）；
 *   · GC 停用自动步进，改为**帧边界**固定步数（frame_boundary），避免 GC 时机影响确定性。 */
#include "vm_base.h"

#if defined(MAQUE_HAVE_LUA)
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#endif

#include <print>
#include <cstring>
#include <string>
#include <vector>

namespace maque::script::lua {

#if !defined(MAQUE_HAVE_LUA)
/* 未编入 Lua：保持接口存在（调用方据 vm_create 返回 nullptr 判定不可用） */
mq_script_vm* vm_create(void) { return nullptr; }
void vm_destroy(mq_script_vm*) {}
int eval(mq_script_vm*, const char*, const char*) { return 1; }
int has(mq_script_vm*, const char*) { return 0; }
int call0(mq_script_vm*, const char*, double*) { return 1; }
int call1(mq_script_vm*, const char*, double, double*) { return 1; }
int call2(mq_script_vm*, const char*, double, double, double*) { return 1; }
int call3(mq_script_vm*, const char*, double, double, double, double*) { return 1; }
int call0s(mq_script_vm*, const char*, const char*, double*) { return 1; }
int call1s(mq_script_vm*, const char*, double, const char*, double*) { return 1; }
int call2s(mq_script_vm*, const char*, double, double, const char*, double*) { return 1; }
int last_error(mq_script_vm*, char* out, int cap) { if (out && cap) out[0] = 0; return 1; }
void set_rng(mq_script_vm*, mq_script_rng_fn, void*) {}
void gc(mq_script_vm*) {}
int register_num(mq_script_vm*, const char*, mq_script_num_fn, void*) { return 1; }
int register_str(mq_script_vm*, const char*, mq_script_str_fn, void*) { return 1; }
int register_ret_str(mq_script_vm*, const char*, mq_script_ret_str_fn, void*) { return 1; }
int register_mixed(mq_script_vm*, const char*, mq_script_mixed_fn, void*) { return 1; }
int register_multi(mq_script_vm*, const char*, const char*, mq_script_multi_fn, void*) { return 1; }
int register_multi_ret_str(mq_script_vm*, const char*, const char*, mq_script_multi_ret_str_fn, void*) { return 1; }
void frame_boundary(mq_script_vm*) {}

#else  /* MAQUE_HAVE_LUA */

namespace {

struct HostFn {
    enum Kind { Num, Str, RetStr, Mixed, Multi, MultiRetStr } kind = Num;
    mq_script_num_fn     num = nullptr;
    mq_script_str_fn     str = nullptr;
    mq_script_ret_str_fn ret_str = nullptr;
    mq_script_mixed_fn   mixed = nullptr;
    mq_script_multi_fn   multi = nullptr;
    mq_script_multi_ret_str_fn multi_ret_str = nullptr;
    std::string kinds;                 /* 逐参数 'n' / 's' */
    void* user = nullptr;
};

/* kinds 字符串存活在 map 里（Lua 的 upvalue 只存下标） */
struct LuaVm : mq_script_vm {
    lua_State* L = nullptr;
    std::vector<HostFn> fns;
    mq_script_rng_fn rng = nullptr;
    void* rng_user = nullptr;
    std::string last_error;
};

LuaVm* S(mq_script_vm* vm) { return static_cast<LuaVm*>(vm); }
const LuaVm* S(const mq_script_vm* vm) { return static_cast<const LuaVm*>(vm); }

/* 从 lua_State 取回 VM：extraspace 存 mq_script_vm*（公共指针） */
mq_script_vm* vm_of(lua_State* L) { return *static_cast<mq_script_vm**>(lua_getextraspace(L)); }

/* ---- 宿主函数 trampoline（与 js 后端的编组规则一致）---- */

/* 取下标对应的 HostFn：upvalue[1] = 函数下标（整数） */
HostFn* host_of(lua_State* L) {
    LuaVm* vm = S(vm_of(L));
    const int idx = (int)lua_tointeger(L, lua_upvalueindex(1));
    if (!vm || idx < 0 || idx >= (int)vm->fns.size()) return nullptr;
    return &vm->fns[(size_t)idx];
}

/* 简单编组：数值参数与字符串参数按 kinds 拆分（最多 16 个，与 js 后端一致） */
int trampoline(lua_State* L) {
    HostFn* h = host_of(L);
    if (!h) { lua_pushnumber(L, 0.0); return 1; }
    const int argc = lua_gettop(L);
    double nums[16] = { 0 };
    const char* strs[16] = { nullptr };
    int nnum = 0, nstr = 0;
    for (int i = 1; i <= argc && (nnum + nstr) < 16; ++i) {
        const char kind = h->kinds.empty() ? 'n'
                        : (size_t)(nnum + nstr) < h->kinds.size() ? h->kinds[(size_t)(nnum + nstr)] : 'n';
        if (kind == 's' || lua_type(L, i) == LUA_TSTRING) {
            strs[nstr++] = lua_tostring(L, i);
        } else {
            nums[nnum++] = lua_tonumber(L, i);
        }
    }
    double ret = 0.0;
    char sbuf[1024] = { 0 };
    int rc = 0;
    switch (h->kind) {
        case HostFn::RetStr:    rc = h->ret_str ? h->ret_str(h->user, nnum, nums, sbuf, (int)sizeof sbuf) : 0; break;
        case HostFn::Mixed:     rc = h->mixed ? h->mixed(h->user, nnum, nums, nstr ? strs[0] : "", &ret) : 0; break;
        case HostFn::Str:       rc = h->str ? h->str(h->user, nstr ? strs[0] : "", &ret) : 0; break;
        case HostFn::Multi:     rc = h->multi ? h->multi(h->user, nnum, nums, nstr, strs, &ret) : 0; break;
        case HostFn::MultiRetStr:
            rc = h->multi_ret_str ? h->multi_ret_str(h->user, nnum, nums, nstr, strs, sbuf, (int)sizeof sbuf) : 0;
            break;
        case HostFn::Num:       rc = h->num ? h->num(h->user, nnum, nums, &ret) : 0; break;
    }
    if (rc != 0) { lua_pushnil(L); lua_pushstring(L, "host function failed"); return 2; }
    if (h->kind == HostFn::RetStr || h->kind == HostFn::MultiRetStr) { lua_pushstring(L, sbuf); return 1; }
    lua_pushnumber(L, ret);
    return 1;
}

/* math.random → 引擎 RNG（与 js 后端 Math.random 语义一致：uint32/2^32） */
int l_math_random(lua_State* L) {
    LuaVm* vm = S(vm_of(L));
    if (!vm || !vm->rng) { lua_pushnumber(L, 0.0); return 1; }
    const uint32_t v = vm->rng(vm->rng_user);
    lua_pushnumber(L, (double)v / 4294967296.0);
    return 1;
}

/* 注册宿主函数：`a.b.c` → **合并**进全局表 a.b（不覆盖既有表，例如 Lua 的 math
 * 需要保留 abs/floor… 同时补上 clamp/lerp/saturate）。 */
int register_common(LuaVm* vm, const char* name, int magic, bool multi) {
    lua_State* L = vm->L;
    const char* dot = std::strrchr(name, '.');
    if (!dot) {
        lua_pushinteger(L, magic);
        lua_pushcclosure(L, trampoline, 1);
        lua_setglobal(L, name);
        (void)multi;
        return 0;
    }
    const std::string ns(name, (size_t)(dot - name));            // "a" / "a.b"
    const std::string first = ns.substr(0, ns.find('.'));
    lua_getglobal(L, first.c_str());
    if (!lua_istable(L, -1)) {                                  // 无既有表 → 新建并挂全局
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, first.c_str());
    }
    /* 逐级进入子命名空间（栈顶始终是当前层的表） */
    size_t start = ns.find('.');
    while (start != std::string::npos) {
        const size_t next = ns.find('.', start + 1);
        const std::string seg = ns.substr(start + 1, next == std::string::npos ? std::string::npos : next - start - 1);
        lua_getfield(L, -1, seg.c_str());
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, seg.c_str());
        }
        lua_remove(L, -2);
        start = next;
    }
    lua_pushinteger(L, magic);
    lua_pushcclosure(L, trampoline, 1);
    lua_setfield(L, -2, dot + 1);
    lua_pop(L, 1);
    return 0;
}

void set_error(LuaVm* vm, const char* msg) {
    if (vm) vm->last_error = msg ? msg : "(unknown)";
    std::print(stderr, "[script:lua] {}\n", msg ? msg : "(unknown)");
}

/* 受保护调用：错误写入 last_error 并打印（与 js 后端的 throw_and_print 对应） */
bool pcall_fn(LuaVm* vm, int nargs, int nresults, const char* what) {
    lua_State* L = vm->L;
    if (lua_pcall(L, nargs, nresults, 0) != LUA_OK) {
        set_error(vm, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    (void)what;
    return true;
}

/* 脚本函数 → 压栈；不存在返回 false（栈不变） */
bool push_function(LuaVm* vm, const char* fn) {
    lua_State* L = vm->L;
    const char* dot = std::strrchr(fn, '.');
    if (!dot) { lua_getglobal(L, fn); }
    else {
        lua_getglobal(L, std::string(fn, (size_t)(dot - fn)).c_str());
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        lua_getfield(L, -1, dot + 1);
        lua_remove(L, -2);
    }
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    return true;
}

int call_impl(mq_script_vm* vm, const char* fn, int argc, const double* argv, double* ret) {
    if (ret) *ret = 0.0;
    LuaVm* v = S(vm);
    if (!v || !v->L || !fn) return 1;
    if (!push_function(v, fn)) return 1;
    for (int i = 0; i < argc; ++i) lua_pushnumber(v->L, argv[i]);
    if (!pcall_fn(v, argc, 1, fn)) return 2;
    if (ret) *ret = lua_tonumber(v->L, -1);
    lua_pop(v->L, 1);
    return 0;
}

int call_with_str(mq_script_vm* vm, const char* fn, int n, const double* nums,
                  const char* s, double* ret) {
    if (ret) *ret = 0.0;
    LuaVm* v = S(vm);
    if (!v || !v->L || !fn) return 1;
    if (!push_function(v, fn)) return 1;
    for (int i = 0; i < n; ++i) lua_pushnumber(v->L, nums[i]);
    lua_pushstring(v->L, s ? s : "");
    if (!pcall_fn(v, n + 1, 1, fn)) return 2;
    if (ret) *ret = lua_tonumber(v->L, -1);
    lua_pop(v->L, 1);
    return 0;
}

/* 沙箱：移除触达外部世界与影响确定性的入口 */
void sandbox(LuaVm* vm) {
    lua_State* L = vm->L;
    static const char* kRemoved[] = {
        "dofile", "loadfile", "load", "loadstring", "require", "collectgarbage",
        "os", "io", "package", "debug", "coroutine", "print",
    };
    for (const char* name : kRemoved) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    /* 只保留安全的基础库（base 已开；表/字符串/math/utf8 显式打开） */
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 0); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 0); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 0);  lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 0);  lua_pop(L, 1);
    /* math.random / math.randomseed → 引擎 RNG */
    lua_getglobal(L, "math");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, l_math_random);
        lua_setfield(L, -2, "random");
        lua_pushcfunction(L, l_math_random);
        lua_setfield(L, -2, "randomseed");     // 幂等：种子由引擎决定（确定性）
    }
    lua_pop(L, 1);
}

} // namespace

mq_script_vm* vm_create(void) {
    auto* vm = new LuaVm();
    vm->lang = "lua";
    vm->L = luaL_newstate();
    if (!vm->L) { delete vm; return nullptr; }
    *static_cast<mq_script_vm**>(lua_getextraspace(vm->L)) = vm;   /* 反向指针：宿主函数取回 VM */
    luaL_openlibs(vm->L);                            /* 先开标准库，再按白名单收窄 */
    sandbox(vm);
    lua_gc(vm->L, LUA_GCSTOP, 0);                    /* GC 只在帧边界步进（确定性） */
    return vm;
}

void vm_destroy(mq_script_vm* vm) {
    if (!vm) return;
    LuaVm* v = S(vm);
    if (v->L) lua_close(v->L);
    delete v;
}

int register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user) {
    if (!vm || !name) return 1;
    LuaVm* v = S(vm);
    HostFn h; h.kind = HostFn::Num; h.num = fn; h.user = user;
    v->fns.push_back(h);
    return register_common(v, name, (int)v->fns.size() - 1, false);
}

int register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user) {
    if (!vm || !name) return 1;
    LuaVm* v = S(vm);
    HostFn h; h.kind = HostFn::Str; h.str = fn; h.user = user; h.kinds = "s";
    v->fns.push_back(h);
    return register_common(v, name, (int)v->fns.size() - 1, false);
}

int register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user) {
    if (!vm || !name) return 1;
    LuaVm* v = S(vm);
    HostFn h; h.kind = HostFn::RetStr; h.ret_str = fn; h.user = user;
    v->fns.push_back(h);
    return register_common(v, name, (int)v->fns.size() - 1, false);
}

int register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user) {
    if (!vm || !name) return 1;
    LuaVm* v = S(vm);
    HostFn h; h.kind = HostFn::Mixed; h.mixed = fn; h.user = user; h.kinds = "s";
    v->fns.push_back(h);
    return register_common(v, name, (int)v->fns.size() - 1, false);
}

int register_multi(mq_script_vm* vm, const char* name, const char* kinds, mq_script_multi_fn fn, void* user) {
    if (!vm || !name) return 1;
    LuaVm* v = S(vm);
    HostFn h; h.kind = HostFn::Multi; h.multi = fn; h.user = user; h.kinds = kinds ? kinds : "";
    v->fns.push_back(h);
    return register_common(v, name, (int)v->fns.size() - 1, true);
}

int register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                           mq_script_multi_ret_str_fn fn, void* user) {
    if (!vm || !name) return 1;
    LuaVm* v = S(vm);
    HostFn h; h.kind = HostFn::MultiRetStr; h.multi_ret_str = fn; h.user = user; h.kinds = kinds ? kinds : "";
    v->fns.push_back(h);
    return register_common(v, name, (int)v->fns.size() - 1, true);
}

int eval(mq_script_vm* vm, const char* src, const char* filename) {
    LuaVm* v = S(vm);
    if (!v || !v->L || !src) return 1;
    const std::string chunk = filename ? std::string("@") + filename : "@<script>";
    if (luaL_loadbuffer(v->L, src, std::strlen(src), chunk.c_str()) != LUA_OK) {
        set_error(v, lua_tostring(v->L, -1));
        lua_pop(v->L, 1);
        return 2;
    }
    if (!pcall_fn(v, 0, 0, "eval")) return 2;
    return 0;
}

int has(mq_script_vm* vm, const char* fn) {
    LuaVm* v = S(vm);
    if (!v || !v->L || !fn) return 0;
    if (!push_function(v, fn)) return 0;
    lua_pop(v->L, 1);
    return 1;
}

int call0(mq_script_vm* vm, const char* fn, double* ret) { return call_impl(vm, fn, 0, nullptr, ret); }
int call1(mq_script_vm* vm, const char* fn, double arg, double* ret) { return call_impl(vm, fn, 1, &arg, ret); }
int call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret) {
    const double args[2] = { a, b };
    return call_impl(vm, fn, 2, args, ret);
}
int call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret) {
    const double args[3] = { a, b, c };
    return call_impl(vm, fn, 3, args, ret);
}
int call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret) {
    return call_with_str(vm, fn, 0, nullptr, s, ret);
}
int call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret) {
    const double n[1] = { a };
    return call_with_str(vm, fn, 1, n, s, ret);
}
int call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret) {
    const double n[2] = { a, b };
    return call_with_str(vm, fn, 2, n, s, ret);
}

int last_error(mq_script_vm* vm, char* out, int cap) {
    LuaVm* v = S(vm);
    if (!v || !out || cap <= 0) return 1;
    std::snprintf(out, (size_t)cap, "%s", v->last_error.c_str());
    return v->last_error.empty() ? 1 : 0;
}

void set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user) {
    LuaVm* v = S(vm);
    if (!v) return;
    v->rng = fn;
    v->rng_user = user;
}

void gc(mq_script_vm* vm) {
    LuaVm* v = S(vm);
    if (v && v->L) lua_gc(v->L, LUA_GCCOLLECT, 0);
}

/* 帧边界：固定步数 GC（确定性；Lua GC 本身与时间无关） */
void frame_boundary(mq_script_vm* vm) {
    LuaVm* v = S(vm);
    if (v && v->L) lua_gc(v->L, LUA_GCSTEP, 1);
}

#endif /* MAQUE_HAVE_LUA */

} // namespace maque::script::lua
