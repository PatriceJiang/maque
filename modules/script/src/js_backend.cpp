#include "core/script/mq_script.h"
#include <print>

#include "quickjs.h"

#include <cstdio>
#include <string>
#include <vector>

struct HostFn {
    enum Kind { Num, Str, RetStr, Mixed, Multi, MultiRetStr } kind = Num;
    bool is_str = false;                       /* Str：单字符串参数 */
    mq_script_num_fn    num = nullptr;
    mq_script_str_fn    str = nullptr;
    mq_script_ret_str_fn ret_str = nullptr;    /* 返回 JS string（宿主写入缓冲，不转移所有权） */
    mq_script_mixed_fn   mixed = nullptr;      /* 数值参数 + 尾部字符串 */
    mq_script_multi_fn   multi = nullptr;      /* 通用：任意数值/字符串组合 */
    mq_script_multi_ret_str_fn multi_ret_str = nullptr;
    const char* kinds = "";                    /* 逐参数 'n' / 's' */
    void* user = nullptr;
};

#include "vm_base.h"

/* JS 后端私有状态：公共首部 mq_script_vm（lang）在前，便于派发层识别后端。 */
struct JsVm : mq_script_vm {
    JSRuntime*  rt = nullptr;
    JSContext*  ctx = nullptr;
    std::vector<HostFn> fns;
    mq_script_rng_fn rng = nullptr;      /* 引擎 RNG（docs/10 D2） */
    void*            rng_user = nullptr;
    std::string      last_error;         /* 最近错误（per-instance，ADR-0014） */
    std::vector<std::string*> kinds_store;   /* kinds 字符串存储（VM 释放时一并回收） */
};

/* 公共指针 → 后端实例（公共首部在偏移 0；标准布局） */
static JsVm* S(mq_script_vm* vm) { return static_cast<JsVm*>(vm); }
static const JsVm* S(const mq_script_vm* vm) { return static_cast<const JsVm*>(vm); }

/* Math.random 的替代实现：走注入的引擎 RNG（未注入则退回 quickjs 默认种子路径）。 */
static JSValue js_math_random(JSContext* ctx, JSValueConst, int, JSValueConst*, int) {
    auto* vm = S(static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx)));
    if (!vm || !S(vm)->rng) return JS_UNDEFINED;          // 由调用方决定行为
    const uint32_t v = S(vm)->rng(S(vm)->rng_user);
    return JS_NewFloat64(ctx, (double)v / 4294967296.0);
}

/* 统一 trampoline：用 magic 作为 HostFn 下标。 */
/* 通用编组：按 kinds 拆分 JS 实参为数值/字符串数组后调用宿主（docs/21 §6.1）。 */
static JSValue multi_trampoline(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* vm = S(static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx)));
    if (!vm) return JS_UNDEFINED;
    HostFn& h = S(vm)->fns[(size_t)magic];
    double nums[16] = { 0 };
    const char* strs[16] = { nullptr };
    int nnum = 0, nstr = 0;
    for (int i = 0; i < argc && i < 16; ++i) {
        const char kind = h.kinds[i] ? h.kinds[i] : 'n';
        if (kind == 's') {
            const char* cs = JS_ToCString(ctx, argv[i]);
            strs[nstr++] = cs ? cs : "";
        } else {
            double d = 0;
            JS_ToFloat64(ctx, &d, argv[i]);
            nums[nnum++] = d;
        }
    }
    double ret = 0;
    int rc = 0;
    char sbuf[1024] = { 0 };
    if (h.kind == HostFn::MultiRetStr) rc = h.multi_ret_str(h.user, nnum, nums, nstr, strs, sbuf, (int)sizeof sbuf);
    else rc = h.multi(h.user, nnum, nums, nstr, strs, &ret);
    for (int i = 0; i < nstr; ++i) if (strs[i]) JS_FreeCString(ctx, strs[i]);
    if (rc != 0) return JS_EXCEPTION;
    if (h.kind == HostFn::MultiRetStr) return JS_NewString(ctx, sbuf);
    return JS_NewFloat64(ctx, ret);
}

static JSValue host_trampoline(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* vm = S(static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx)));
    if (!vm || magic < 0 || magic >= (int)S(vm)->fns.size()) return JS_UNDEFINED;
    HostFn& h = S(vm)->fns[magic];
    double ret = 0.0;
    int rc = 0;
    if (h.kind == HostFn::RetStr) {
        std::vector<double> args(argc);
        for (int i = 0; i < argc; ++i) {
            double d = 0.0;
            if (JS_ToFloat64(ctx, &d, argv[i])) { JS_ThrowTypeError(ctx, "numeric arg required"); return JS_EXCEPTION; }
            args[i] = d;
        }
        char buf[1024];
        buf[0] = '\0';
        rc = h.ret_str ? h.ret_str(h.user, argc, args.data(), buf, (int)sizeof buf) : 0;
        if (rc) return JS_EXCEPTION;
        return JS_NewString(ctx, buf);
    } else if (h.kind == HostFn::Mixed) {
        std::vector<double> args(argc > 0 ? argc - 1 : 0);
        for (int i = 0; i + 1 < argc; ++i) {
            double d = 0.0;
            if (JS_ToFloat64(ctx, &d, argv[i])) { JS_ThrowTypeError(ctx, "numeric arg required"); return JS_EXCEPTION; }
            args[i] = d;
        }
        const char* s = JS_ToCString(ctx, argc > 0 ? argv[argc - 1] : JS_UNDEFINED);
        rc = h.mixed ? h.mixed(h.user, (int)args.size(), args.data(), s ? s : "", &ret) : 0;
        if (s) JS_FreeCString(ctx, s);
    } else if (h.is_str) {
        const char* s = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        rc = h.str ? h.str(h.user, s ? s : "", &ret) : 0;
        if (s) JS_FreeCString(ctx, s);
    } else {
        std::vector<double> args(argc);
        for (int i = 0; i < argc; ++i) {
            double d = 0.0;
            if (JS_ToFloat64(ctx, &d, argv[i])) { JS_ThrowTypeError(ctx, "numeric arg required"); return JS_EXCEPTION; }
            args[i] = d;
        }
        rc = h.num ? h.num(h.user, argc, args.data(), &ret) : 0;
    }
    if (rc) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, ret);
}

static void throw_and_print(JSContext* ctx) {
    auto* vm = S(static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx)));
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    const std::string msg = s ? s : "(unknown)";
    if (vm) S(vm)->last_error = msg;                       // per-instance（ADR-0014）
    std::print(stderr, "[script] exception: {}\n", msg);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

namespace maque::script::js {

mq_script_vm* vm_create(void) {
    auto* vm = new JsVm();
    vm->lang = "js";                       // 公共首部：后端自省
    S(vm)->rt = JS_NewRuntime();
    if (!S(vm)->rt) { delete vm; return nullptr; }
    S(vm)->ctx = JS_NewContext(S(vm)->rt);
    if (!S(vm)->ctx) { JS_FreeRuntime(S(vm)->rt); delete vm; return nullptr; }
    JS_SetContextOpaque(S(vm)->ctx, vm);
    return vm;
}

void vm_destroy(mq_script_vm* vm) {
    if (!vm) return;
    JsVm* self = S(vm);
    for (auto* p : self->kinds_store) delete p;
    self->kinds_store.clear();
    if (self->ctx) JS_FreeContext(self->ctx);
    if (self->rt) JS_FreeRuntime(self->rt);
    delete self;                 /* 必须按**派生类型**释放（公共首部无虚析构） */
}

/* 支持 "ns.fn" 形式：在全局上创建/复用命名空间对象后挂函数。 */
static int register_common(mq_script_vm* vm, const char* name, int magic, bool multi = false) {
    JSValue fn = JS_NewCFunctionMagic(S(vm)->ctx, multi ? multi_trampoline : host_trampoline, name,
                                      multi ? 8 : 4, JS_CFUNC_generic_magic, magic);
    JSValue global = JS_GetGlobalObject(S(vm)->ctx);
    std::string n(name);
    auto dot = n.rfind('.');
    if (dot == std::string::npos) {
        JS_SetPropertyStr(S(vm)->ctx, global, name, fn);
    } else {
        std::string ns = n.substr(0, dot);
        std::string fnm = n.substr(dot + 1);
        JSValue obj = JS_GetPropertyStr(S(vm)->ctx, global, ns.c_str());
        if (!JS_IsObject(obj)) {
            JS_FreeValue(S(vm)->ctx, obj);
            obj = JS_NewObject(S(vm)->ctx);
            JS_SetPropertyStr(S(vm)->ctx, global, ns.c_str(), JS_DupValue(S(vm)->ctx, obj));
        }
        JS_SetPropertyStr(S(vm)->ctx, obj, fnm.c_str(), fn);
        JS_FreeValue(S(vm)->ctx, obj);
    }
    JS_FreeValue(S(vm)->ctx, global);
    return 0;
}

int register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Num; h.is_str = false; h.num = fn; h.user = user;
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1);
}

int register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Str; h.is_str = true; h.str = fn; h.user = user;
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1);
}

/* 数值 + 尾部字符串（如 set_label(id, text)）。 */
/* kinds 字符串需存活：存进 VM 的 arena（per-instance）。 */
static const char* intern_kinds(mq_script_vm* vm, const char* kinds) {
    auto* copy = new std::string(kinds ? kinds : "");
    S(vm)->kinds_store.push_back(copy);
    return copy->c_str();
}

int register_multi(mq_script_vm* vm, const char* name, const char* kinds,
                             mq_script_multi_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Multi; h.multi = fn; h.user = user; h.kinds = intern_kinds(vm, kinds);
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1, true);
}

int register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                                     mq_script_multi_ret_str_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::MultiRetStr; h.multi_ret_str = fn; h.user = user; h.kinds = intern_kinds(vm, kinds);
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1, true);
}

int register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Mixed; h.mixed = fn; h.user = user;
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1);
}

/* 返回 JS string 的原生函数（如 entity_name）：结果写入宿主缓冲，VM 复制。 */
int register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::RetStr; h.ret_str = fn; h.user = user;
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1);
}

int eval(mq_script_vm* vm, const char* src, const char* filename) {
    if (!vm || !src) return 1;
    JSValue v = JS_Eval(S(vm)->ctx, src, __builtin_strlen(src), filename ? filename : "<script>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { throw_and_print(S(vm)->ctx); return 2; }
    JS_FreeValue(S(vm)->ctx, v);
    return 0;
}

int has(mq_script_vm* vm, const char* fn) {
    if (!vm || !fn) return 0;
    JSValue global = JS_GetGlobalObject(S(vm)->ctx);
    JSValue f = JS_GetPropertyStr(S(vm)->ctx, global, fn);
    int ok = JS_IsFunction(S(vm)->ctx, f);
    JS_FreeValue(S(vm)->ctx, f);
    JS_FreeValue(S(vm)->ctx, global);
    return ok;
}

static int call_impl(mq_script_vm* vm, const char* fn, int argc, double* argv, double* ret) {
    if (ret) *ret = 0.0;
    JSValue global = JS_GetGlobalObject(S(vm)->ctx);
    JSValue f = JS_GetPropertyStr(S(vm)->ctx, global, fn);
    JS_FreeValue(S(vm)->ctx, global);
    if (!JS_IsFunction(S(vm)->ctx, f)) { JS_FreeValue(S(vm)->ctx, f); return 1; }
    JSValue args[3];
    if (argc > 3) argc = 3;
    for (int i = 0; i < argc; ++i) args[i] = JS_NewFloat64(S(vm)->ctx, argv[i]);
    JSValue r = JS_Call(S(vm)->ctx, f, JS_UNDEFINED, argc, argc ? args : nullptr);
    for (int i = 0; i < argc; ++i) JS_FreeValue(S(vm)->ctx, args[i]);
    JS_FreeValue(S(vm)->ctx, f);
    if (JS_IsException(r)) { throw_and_print(S(vm)->ctx); return 2; }
    if (ret) { double d = 0; JS_ToFloat64(S(vm)->ctx, &d, r); *ret = d; }
    JS_FreeValue(S(vm)->ctx, r);
    return 0;
}

int call0(mq_script_vm* vm, const char* fn, double* ret) { return call_impl(vm, fn, 0, nullptr, ret); }
int call1(mq_script_vm* vm, const char* fn, double arg, double* ret) { return call_impl(vm, fn, 1, &arg, ret); }
int call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret) {
    double args[3] = { a, b, c };
    return call_impl(vm, fn, 3, args, ret);
}
int call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret) {
    double args[2] = { a, b };
    return call_impl(vm, fn, 2, args, ret);
}
/* 数值参数 + 末尾字符串：统一实现（n 为数值个数，0..2）。 */
static int call_with_str(mq_script_vm* vm, const char* fn, int n, const double* nums,
                         const char* s, double* ret) {
    if (ret) *ret = 0.0;
    JSValue global = JS_GetGlobalObject(S(vm)->ctx);
    JSValue f = JS_GetPropertyStr(S(vm)->ctx, global, fn);
    JS_FreeValue(S(vm)->ctx, global);
    if (!JS_IsFunction(S(vm)->ctx, f)) { JS_FreeValue(S(vm)->ctx, f); return 1; }
    JSValue args[3];
    for (int i = 0; i < n; ++i) args[i] = JS_NewFloat64(S(vm)->ctx, nums[i]);
    args[n] = JS_NewString(S(vm)->ctx, s ? s : "");
    JSValue r = JS_Call(S(vm)->ctx, f, JS_UNDEFINED, n + 1, args);
    for (int i = 0; i <= n; ++i) JS_FreeValue(S(vm)->ctx, args[i]);
    JS_FreeValue(S(vm)->ctx, f);
    if (JS_IsException(r)) { throw_and_print(S(vm)->ctx); return 2; }
    if (ret) { double d = 0; JS_ToFloat64(S(vm)->ctx, &d, r); *ret = d; }
    JS_FreeValue(S(vm)->ctx, r);
    return 0;
}
int call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret) {
    const double n[1] = { a };
    return call_with_str(vm, fn, 1, n, s, ret);
}
int call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret) {
    const double n[2] = { a, b };
    return call_with_str(vm, fn, 2, n, s, ret);
}
int call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret) {
    return call_with_str(vm, fn, 0, nullptr, s, ret);
}

void set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user) {
    if (!vm || !S(vm)->ctx) return;
    S(vm)->rng = fn;
    S(vm)->rng_user = user;
    if (!fn) return;
    /* 覆盖 Math.random：确定性要求脚本随机与引擎同源（docs/10 D2、断点回放） */
    JSValue global = JS_GetGlobalObject(S(vm)->ctx);
    JSValue math = JS_GetPropertyStr(S(vm)->ctx, global, "Math");
    if (JS_IsObject(math)) {
        JSValue f = JS_NewCFunctionMagic(S(vm)->ctx, js_math_random, "random", 0,
                                         JS_CFUNC_generic_magic, 0);
        JS_SetPropertyStr(S(vm)->ctx, math, "random", f);
    }
    JS_FreeValue(S(vm)->ctx, math);
    JS_FreeValue(S(vm)->ctx, global);
}

int last_error(mq_script_vm* vm, char* out, int cap) {
    if (!out || cap <= 0) return 1;
    const std::string msg = vm ? S(vm)->last_error : std::string();
    if (msg.empty()) { out[0] = '\0'; return 1; }
    std::snprintf(out, (size_t)cap, "%s", msg.c_str());
    return 0;
}

/* 帧边界（docs/10 D7）：JS 的 cycle GC 只在帧边界运行（docs/14） */
void frame_boundary(mq_script_vm* vm) {
    if (!vm) return;
    gc(vm);
}

void gc(mq_script_vm* vm) {
    if (vm && S(vm)->rt) JS_RunGC(S(vm)->rt);
}

} // namespace maque::script::js
