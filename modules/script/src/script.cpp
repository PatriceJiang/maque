#include "core/script/mq_script.h"
#include <print>

#include "quickjs.h"

#include <cstdio>
#include <string>
#include <vector>

struct HostFn {
    enum Kind { Num, Str, RetStr, Mixed } kind = Num;
    bool is_str = false;                       /* Str：单字符串参数 */
    mq_script_num_fn    num = nullptr;
    mq_script_str_fn    str = nullptr;
    mq_script_ret_str_fn ret_str = nullptr;    /* 返回 JS string（宿主写入缓冲，不转移所有权） */
    mq_script_mixed_fn   mixed = nullptr;      /* 数值参数 + 尾部字符串 */
    void* user = nullptr;
};

struct mq_script_vm {
    JSRuntime*  rt = nullptr;
    JSContext*  ctx = nullptr;
    std::vector<HostFn> fns;
    mq_script_rng_fn rng = nullptr;      /* 引擎 RNG（docs/10 D2） */
    void*            rng_user = nullptr;
    std::string      last_error;         /* 最近错误（per-instance，ADR-0014） */
};

/* Math.random 的替代实现：走注入的引擎 RNG（未注入则退回 quickjs 默认种子路径）。 */
static JSValue js_math_random(JSContext* ctx, JSValueConst, int, JSValueConst*, int) {
    auto* vm = static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx));
    if (!vm || !vm->rng) return JS_UNDEFINED;          // 由调用方决定行为
    const uint32_t v = vm->rng(vm->rng_user);
    return JS_NewFloat64(ctx, (double)v / 4294967296.0);
}

/* 统一 trampoline：用 magic 作为 HostFn 下标。 */
static JSValue host_trampoline(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* vm = static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx));
    if (!vm || magic < 0 || magic >= (int)vm->fns.size()) return JS_UNDEFINED;
    HostFn& h = vm->fns[magic];
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
    auto* vm = static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx));
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    const std::string msg = s ? s : "(unknown)";
    if (vm) vm->last_error = msg;                       // per-instance（ADR-0014）
    std::print(stderr, "[script] exception: {}\n", msg);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

extern "C" {

mq_script_vm* mq_script_vm_create(void) {
    auto* vm = new mq_script_vm();
    vm->rt = JS_NewRuntime();
    if (!vm->rt) { delete vm; return nullptr; }
    vm->ctx = JS_NewContext(vm->rt);
    if (!vm->ctx) { JS_FreeRuntime(vm->rt); delete vm; return nullptr; }
    JS_SetContextOpaque(vm->ctx, vm);
    return vm;
}

void mq_script_vm_destroy(mq_script_vm* vm) {
    if (!vm) return;
    if (vm->ctx) JS_FreeContext(vm->ctx);
    if (vm->rt) JS_FreeRuntime(vm->rt);
    delete vm;
}

/* 支持 "ns.fn" 形式：在全局上创建/复用命名空间对象后挂函数。 */
static int register_common(mq_script_vm* vm, const char* name, int magic) {
    JSValue fn = JS_NewCFunctionMagic(vm->ctx, host_trampoline, name, 4,
                                      JS_CFUNC_generic_magic, magic);
    JSValue global = JS_GetGlobalObject(vm->ctx);
    std::string n(name);
    auto dot = n.rfind('.');
    if (dot == std::string::npos) {
        JS_SetPropertyStr(vm->ctx, global, name, fn);
    } else {
        std::string ns = n.substr(0, dot);
        std::string fnm = n.substr(dot + 1);
        JSValue obj = JS_GetPropertyStr(vm->ctx, global, ns.c_str());
        if (!JS_IsObject(obj)) {
            JS_FreeValue(vm->ctx, obj);
            obj = JS_NewObject(vm->ctx);
            JS_SetPropertyStr(vm->ctx, global, ns.c_str(), JS_DupValue(vm->ctx, obj));
        }
        JS_SetPropertyStr(vm->ctx, obj, fnm.c_str(), fn);
        JS_FreeValue(vm->ctx, obj);
    }
    JS_FreeValue(vm->ctx, global);
    return 0;
}

int mq_script_register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Num; h.is_str = false; h.num = fn; h.user = user;
    vm->fns.push_back(h);
    return register_common(vm, name, (int)vm->fns.size() - 1);
}

int mq_script_register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Str; h.is_str = true; h.str = fn; h.user = user;
    vm->fns.push_back(h);
    return register_common(vm, name, (int)vm->fns.size() - 1);
}

/* 数值 + 尾部字符串（如 set_label(id, text)）。 */
int mq_script_register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::Mixed; h.mixed = fn; h.user = user;
    vm->fns.push_back(h);
    return register_common(vm, name, (int)vm->fns.size() - 1);
}

/* 返回 JS string 的原生函数（如 entity_name）：结果写入宿主缓冲，VM 复制。 */
int mq_script_register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.kind = HostFn::RetStr; h.ret_str = fn; h.user = user;
    vm->fns.push_back(h);
    return register_common(vm, name, (int)vm->fns.size() - 1);
}

int mq_script_eval(mq_script_vm* vm, const char* src, const char* filename) {
    if (!vm || !src) return 1;
    JSValue v = JS_Eval(vm->ctx, src, __builtin_strlen(src), filename ? filename : "<script>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { throw_and_print(vm->ctx); return 2; }
    JS_FreeValue(vm->ctx, v);
    return 0;
}

int mq_script_has(mq_script_vm* vm, const char* fn) {
    if (!vm || !fn) return 0;
    JSValue global = JS_GetGlobalObject(vm->ctx);
    JSValue f = JS_GetPropertyStr(vm->ctx, global, fn);
    int ok = JS_IsFunction(vm->ctx, f);
    JS_FreeValue(vm->ctx, f);
    JS_FreeValue(vm->ctx, global);
    return ok;
}

static int call_impl(mq_script_vm* vm, const char* fn, int argc, double* argv, double* ret) {
    if (ret) *ret = 0.0;
    JSValue global = JS_GetGlobalObject(vm->ctx);
    JSValue f = JS_GetPropertyStr(vm->ctx, global, fn);
    JS_FreeValue(vm->ctx, global);
    if (!JS_IsFunction(vm->ctx, f)) { JS_FreeValue(vm->ctx, f); return 1; }
    JSValue args[3];
    if (argc > 3) argc = 3;
    for (int i = 0; i < argc; ++i) args[i] = JS_NewFloat64(vm->ctx, argv[i]);
    JSValue r = JS_Call(vm->ctx, f, JS_UNDEFINED, argc, argc ? args : nullptr);
    for (int i = 0; i < argc; ++i) JS_FreeValue(vm->ctx, args[i]);
    JS_FreeValue(vm->ctx, f);
    if (JS_IsException(r)) { throw_and_print(vm->ctx); return 2; }
    if (ret) { double d = 0; JS_ToFloat64(vm->ctx, &d, r); *ret = d; }
    JS_FreeValue(vm->ctx, r);
    return 0;
}

int mq_script_call0(mq_script_vm* vm, const char* fn, double* ret) { return call_impl(vm, fn, 0, nullptr, ret); }
int mq_script_call1(mq_script_vm* vm, const char* fn, double arg, double* ret) { return call_impl(vm, fn, 1, &arg, ret); }
int mq_script_call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret) {
    double args[3] = { a, b, c };
    return call_impl(vm, fn, 3, args, ret);
}

void mq_script_set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user) {
    if (!vm || !vm->ctx) return;
    vm->rng = fn;
    vm->rng_user = user;
    if (!fn) return;
    /* 覆盖 Math.random：确定性要求脚本随机与引擎同源（docs/10 D2、断点回放） */
    JSValue global = JS_GetGlobalObject(vm->ctx);
    JSValue math = JS_GetPropertyStr(vm->ctx, global, "Math");
    if (JS_IsObject(math)) {
        JSValue f = JS_NewCFunctionMagic(vm->ctx, js_math_random, "random", 0,
                                         JS_CFUNC_generic_magic, 0);
        JS_SetPropertyStr(vm->ctx, math, "random", f);
    }
    JS_FreeValue(vm->ctx, math);
    JS_FreeValue(vm->ctx, global);
}

int mq_script_last_error(mq_script_vm* vm, char* out, int cap) {
    if (!out || cap <= 0) return 1;
    const std::string msg = vm ? vm->last_error : std::string();
    if (msg.empty()) { out[0] = '\0'; return 1; }
    std::snprintf(out, (size_t)cap, "%s", msg.c_str());
    return 0;
}

void mq_script_gc(mq_script_vm* vm) {
    if (vm && vm->rt) JS_RunGC(vm->rt);
}

} // extern "C"
