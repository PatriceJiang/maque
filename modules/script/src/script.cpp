#include "core/script/mq_script.h"

#include "quickjs.h"

#include <cstdio>
#include <string>
#include <vector>

struct HostFn {
    bool is_str = false;
    mq_script_num_fn num = nullptr;
    mq_script_str_fn str = nullptr;
    void* user = nullptr;
};

struct mq_script_vm {
    JSRuntime*  rt = nullptr;
    JSContext*  ctx = nullptr;
    std::vector<HostFn> fns;
};

/* 统一 trampoline：用 magic 作为 HostFn 下标。 */
static JSValue host_trampoline(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* vm = static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx));
    if (!vm || magic < 0 || magic >= (int)vm->fns.size()) return JS_UNDEFINED;
    HostFn& h = vm->fns[magic];
    double ret = 0.0;
    int rc = 0;
    if (h.is_str) {
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
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    std::fprintf(stderr, "[script] exception: %s\n", s ? s : "(unknown)");
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

static int register_common(mq_script_vm* vm, const char* name, int magic) {
    JSValue fn = JS_NewCFunctionMagic(vm->ctx, host_trampoline, name, 4,
                                      JS_CFUNC_generic_magic, magic);
    JSValue global = JS_GetGlobalObject(vm->ctx);
    JS_SetPropertyStr(vm->ctx, global, name, fn);
    JS_FreeValue(vm->ctx, global);
    return 0;
}

int mq_script_register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.is_str = false; h.num = fn; h.user = user;
    vm->fns.push_back(h);
    return register_common(vm, name, (int)vm->fns.size() - 1);
}

int mq_script_register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user) {
    if (!vm) return 1;
    HostFn h; h.is_str = true; h.str = fn; h.user = user;
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
    JSValue args[1];
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

void mq_script_gc(mq_script_vm* vm) {
    if (vm && vm->rt) JS_RunGC(vm->rt);
}

} // extern "C"
