#include "core/script/mq_script.h"
#include "core/tracy/mq_tracy.h"
#include "vm_mem.h"
#include <print>

#include "quickjs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* ---- 脚本内存域（docs/29 §10.2）：QuickJS 自定义分配器 ----
 * JS_NewRuntime2 的 5 个回调；只有 MQ_TRACY_MEMORY_ENABLED 时存在（否则用默认 JS_NewRuntime）。
 * js_malloc_usable_size 用于 QuickJS 自身的 malloc_size 统计，这里返回 malloc 实际块大小。 */
#if MQ_TRACY_MEMORY_ENABLED
namespace {

void* qjs_calloc(void* /*opaque*/, size_t count, size_t size) {
    return maque::script::mem::tracked_calloc(count, size);
}
void* qjs_malloc(void* /*opaque*/, size_t size) { return maque::script::mem::tracked_malloc(size); }
void qjs_free(void* /*opaque*/, void* ptr) { maque::script::mem::tracked_free(ptr); }
void* qjs_realloc(void* /*opaque*/, void* ptr, size_t size) {
    return maque::script::mem::tracked_realloc(ptr, size);
}
size_t qjs_usable(const void* ptr) { return maque::script::mem::usable_size(ptr); }

const JSMallocFunctions kQjsMalloc = { qjs_calloc, qjs_malloc, qjs_free, qjs_realloc, qjs_usable };

} // namespace
#endif

struct HostFn {
    enum Kind { Num, Str, RetStr, Mixed, Multi, MultiRetStr, Bytes } kind = Num;
    bool is_str = false;                       /* Str：单字符串参数 */
    mq_script_num_fn    num = nullptr;
    mq_script_str_fn    str = nullptr;
    mq_script_ret_str_fn ret_str = nullptr;    /* 返回 JS string（宿主写入缓冲，不转移所有权） */
    mq_script_mixed_fn   mixed = nullptr;      /* 数值参数 + 尾部字符串 */
    mq_script_multi_fn   multi = nullptr;      /* 通用：任意数值/字符串组合 */
    mq_script_multi_ret_str_fn multi_ret_str = nullptr;
    mq_script_bytes_fn bytes = nullptr;        /* bytes → Uint8Array（VM 复制后释放宿主结果） */
    const char* kinds = "";                    /* 逐参数 'n' / 's' / 'b' */
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
    mq_script_module* current_module = nullptr; /* 当前模块加载/调用栈顶 */
    /* 原生 ESM（docs/34 P0）：loader 回调 + 模块缓存；键由 runtime 定义。 */
    mq_script_module_resolve_fn resolver = nullptr;
    mq_script_module_read_fn    reader = nullptr;
    void*                       loader_user = nullptr;
    std::unordered_map<std::string, struct JsEsmModule*> esm_modules;
};

struct JsModule : mq_script_module {
    JSValue exports = JS_UNDEFINED;      /* { has, call }；闭包持有模块词法环境 */
    JSValue global = JS_UNDEFINED;       /* 模块专属 globalThis，原型为共享宿主全局 */
};

/* 原生 ESM 模块：ns = 模块命名空间，def = default 导出（定义/class/工厂）。 */
struct JsEsmModule : mq_script_module {
    std::string key;
    JSValue ns = JS_UNDEFINED;
    JSValue def = JS_UNDEFINED;
};

/* 每挂载点实例：this/self 就是 obj。 */
struct JsInstance : mq_script_instance {
    JSValue obj = JS_UNDEFINED;
    std::string last_error;
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

static JSValue bytes_trampoline(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* vm = S(static_cast<mq_script_vm*>(JS_GetContextOpaque(ctx)));
    if (!vm || magic < 0 || magic >= (int)S(vm)->fns.size()) return JS_UNDEFINED;
    HostFn& h = S(vm)->fns[(size_t)magic];
    double nums[16] = { 0 };
    const void* bytes[16] = { nullptr };
    size_t sizes[16] = { 0 };
    const char* strings[16] = { nullptr };
    int nnum = 0, nbytes = 0, nstrings = 0;
    const size_t expected = std::strlen(h.kinds);
    if ((size_t)argc < expected) return JS_ThrowTypeError(ctx, "not enough arguments");
    for (size_t i = 0; i < expected; ++i) {
        if (h.kinds[i] == 'b') {
            size_t size = 0;
            uint8_t* data = nullptr;
            if (JS_IsString(argv[i])) {
                const char* text = JS_ToCStringLen(ctx, &size, argv[i]);
                if (!text) goto fail;
                strings[nstrings++] = text;
                data = (uint8_t*)text;
            } else if (JS_IsArrayBuffer(argv[i])) {
                data = JS_GetArrayBuffer(ctx, &size, argv[i]);
            } else {
                data = JS_GetUint8Array(ctx, &size, argv[i]);
            }
            if (!data && size != 0) goto fail;
            if (!data && !JS_IsArrayBuffer(argv[i]) && !JS_IsString(argv[i])) {
                for (int j = 0; j < nstrings; ++j) JS_FreeCString(ctx, strings[j]);
                return JS_ThrowTypeError(ctx, "bytes argument must be Uint8Array, ArrayBuffer, or string");
            }
            bytes[nbytes] = data;
            sizes[nbytes++] = size;
        } else {
            double value = 0.0;
            if (JS_ToFloat64(ctx, &value, argv[i])) goto fail;
            nums[nnum++] = value;
        }
    }
    {
        mq_script_bytes_result result{};
        const int rc = h.bytes ? h.bytes(h.user, nnum, nums, nbytes, bytes, sizes, &result) : 1;
        for (int i = 0; i < nstrings; ++i) JS_FreeCString(ctx, strings[i]);
        if (rc != 0 || (result.size != 0 && !result.data)) {
            if (result.release) result.release(result.release_user, result.data);
            return JS_ThrowInternalError(ctx, "host bytes function failed");
        }
        JSValue out = JS_NewUint8ArrayCopy(ctx, static_cast<const uint8_t*>(result.data), result.size);
        if (result.release) result.release(result.release_user, result.data);
        return out;
    }
fail:
    for (int i = 0; i < nstrings; ++i) JS_FreeCString(ctx, strings[i]);
    return JS_EXCEPTION;
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

/* ---- 原生 ESM loader（docs/34 P0）：runtime 提供解析/读取，后端负责编译与缓存 ---- */
static char* js_module_normalize_cb(JSContext* ctx, const char* base, const char* name, void* opaque) {
    auto* vm = S(static_cast<mq_script_vm*>(opaque));
    if (!vm || !S(vm)->resolver) return nullptr;
    const char* key = S(vm)->resolver(S(vm)->loader_user, base ? base : "", name ? name : "");
    if (!key || !*key) return nullptr;
    return js_strdup(ctx, key);
}

static JSModuleDef* js_module_load_cb(JSContext* ctx, const char* name, void* opaque) {
    auto* vm = S(static_cast<mq_script_vm*>(opaque));
    if (!vm || !S(vm)->reader) {
        JS_ThrowReferenceError(ctx, "no module reader for '%s'", name ? name : "?");
        return nullptr;
    }
    size_t len = 0;
    const char* src = S(vm)->reader(S(vm)->loader_user, name, &len);
    if (!src) {
        JS_ThrowReferenceError(ctx, "cannot read module '%s'", name ? name : "?");
        return nullptr;
    }
    JSValue val = JS_Eval(ctx, src, len, name ? name : "<module>",
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(val)) return nullptr;
    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(val));
    JS_FreeValue(ctx, val);
    return m;
}

namespace maque::script::js {

mq_script_vm* vm_create(void) {
    auto* vm = new JsVm();
    vm->lang = "js";                       // 公共首部：后端自省
#if MQ_TRACY_MEMORY_ENABLED
    /* 内存记账开启时用带记账的分配器（关掉 MAQUE_TRACY_MEM 也仍用它：账本可随时打开）。 */
    S(vm)->rt = JS_NewRuntime2(&kQjsMalloc, nullptr);
#else
    S(vm)->rt = JS_NewRuntime();
#endif
    if (!S(vm)->rt) { delete vm; return nullptr; }
    S(vm)->ctx = JS_NewContext(S(vm)->rt);
    if (!S(vm)->ctx) { JS_FreeRuntime(S(vm)->rt); delete vm; return nullptr; }
    JS_SetContextOpaque(S(vm)->ctx, vm);
    JS_SetModuleLoaderFunc(S(vm)->rt, js_module_normalize_cb, js_module_load_cb, vm);
    return vm;
}

void vm_destroy(mq_script_vm* vm) {
    if (!vm) return;
    JsVm* self = S(vm);
    for (auto* p : self->kinds_store) delete p;
    self->kinds_store.clear();
    for (auto& [key, m] : self->esm_modules) {
        (void)key;
        if (self->ctx && m) {
            if (!JS_IsUndefined(m->ns)) JS_FreeValue(self->ctx, m->ns);
            if (!JS_IsUndefined(m->def)) JS_FreeValue(self->ctx, m->def);
            delete m;
        }
    }
    self->esm_modules.clear();
    if (self->ctx) JS_FreeContext(self->ctx);
    if (self->rt) JS_FreeRuntime(self->rt);
    delete self;                 /* 必须按**派生类型**释放（公共首部无虚析构） */
}

/* 支持 "ns.fn" 形式：在全局上创建/复用命名空间对象后挂函数。 */
static int register_common(mq_script_vm* vm, const char* name, int magic, int trampoline = 0) {
    JSCFunctionMagic* call = trampoline == 2 ? bytes_trampoline
                          : trampoline == 1 ? multi_trampoline : host_trampoline;
    JSValue fn = JS_NewCFunctionMagic(S(vm)->ctx, call, name,
                                      trampoline ? 8 : 4, JS_CFUNC_generic_magic, magic);
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
    return register_common(vm, name, (int)S(vm)->fns.size() - 1, 1);
}

int register_bytes(mq_script_vm* vm, const char* name, const char* kinds,
                   mq_script_bytes_fn fn, void* user) {
    if (!vm || !name || !fn) return 1;
    HostFn h; h.kind = HostFn::Bytes; h.bytes = fn; h.user = user; h.kinds = intern_kinds(vm, kinds);
    S(vm)->fns.push_back(h);
    return register_common(vm, name, (int)S(vm)->fns.size() - 1, 2);
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

mq_script_module* current_module(mq_script_vm* vm) {
    return vm ? S(vm)->current_module : nullptr;
}

mq_script_module* module_load(mq_script_vm* vm, const char* src, const char* filename) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "js.module.load");
    MQ_TRACY_ZONE_TEXT(mq_zone, filename);
    if (!vm || !src) return nullptr;
    JsVm* v = S(vm);
    auto* module = new JsModule();
    module->vm = vm;
    module->global = JS_NewObject(v->ctx);
    JSValue host_global = JS_GetGlobalObject(v->ctx);
    const int prototype_rc = JS_SetPrototype(v->ctx, module->global, host_global);
    JS_FreeValue(v->ctx, host_global);
    if (JS_IsUndefined(module->global) || prototype_rc < 0) {
        throw_and_print(v->ctx);
        JS_FreeValue(v->ctx, module->global);
        delete module;
        return nullptr;
    }

    /* 保持普通顶层脚本写法：var/let/函数声明都在闭包内；globalThis 则指向模块自己的对象。
     * 末尾返回的 has/call 通过直接 eval 解析该闭包内的任意处理器名（包括动态 node_on 名称）。 */
    const std::string prefix = "(function(globalThis) {\"use strict\";";
    const std::string suffix = R"JS(
return {
  has: function(name) {
    let f;
    try { f = eval(name); } catch (_) { f = undefined; }
    if (typeof f !== "function" && !name.includes(".")) f = globalThis[name];
    return typeof f === "function";
  },
  call: function(name, args) {
    let f;
    try { f = eval(name); } catch (_) { f = undefined; }
    if (typeof f !== "function" && !name.includes(".")) f = globalThis[name];
    if (typeof f !== "function") throw new ReferenceError("script function not found: " + name);
    return f(...args);
  }
};
}))JS";
    const std::string wrapped = prefix + src + suffix;
    JSValue factory = JS_Eval(v->ctx, wrapped.c_str(), wrapped.size(),
                              filename ? filename : "<module>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) {
        throw_and_print(v->ctx);
        JS_FreeValue(v->ctx, module->global);
        delete module;
        return nullptr;
    }
    mq_script_module* prev = v->current_module;
    v->current_module = module;
    JSValue exports = JS_Call(v->ctx, factory, module->global, 1, &module->global);
    v->current_module = prev;
    JS_FreeValue(v->ctx, factory);
    if (JS_IsException(exports)) {
        throw_and_print(v->ctx);
        JS_FreeValue(v->ctx, module->global);
        delete module;
        return nullptr;
    }
    module->exports = exports;
    return module;
}

void module_destroy(mq_script_module* base) {
    if (!base || !base->vm) return;
    JsVm* v = S(base->vm);
    if (base->kind == 1) {
        auto* m = static_cast<JsEsmModule*>(base);
        if (v->ctx) {
            if (!JS_IsUndefined(m->ns)) JS_FreeValue(v->ctx, m->ns);
            if (!JS_IsUndefined(m->def)) JS_FreeValue(v->ctx, m->def);
        }
        v->esm_modules.erase(m->key);
        delete m;
        return;
    }
    auto* module = static_cast<JsModule*>(base);
    if (v->current_module == base) v->current_module = nullptr;
    JS_FreeValue(v->ctx, module->exports);
    JS_FreeValue(v->ctx, module->global);
    delete module;
}

int module_has(mq_script_module* base, const char* fn) {
    if (!base || !base->vm || !fn) return 0;
    if (base->kind == 1) return 0;                 /* ESM：用 instance_has */
    auto* module = static_cast<JsModule*>(base);
    JsVm* v = S(base->vm);
    JSValue has = JS_GetPropertyStr(v->ctx, module->exports, "has");
    JSValue name = JS_NewString(v->ctx, fn);
    mq_script_module* prev = v->current_module;
    v->current_module = base;
    JSValue result = JS_Call(v->ctx, has, module->exports, 1, &name);
    v->current_module = prev;
    JS_FreeValue(v->ctx, name);
    JS_FreeValue(v->ctx, has);
    if (JS_IsException(result)) { throw_and_print(v->ctx); return 0; }
    const int ok = JS_ToBool(v->ctx, result);
    JS_FreeValue(v->ctx, result);
    return ok;
}

int module_call(mq_script_module* base, const char* fn, int nnum, const double* nums,
                const char* text, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "js.module.call");
    MQ_TRACY_ZONE_TEXT(mq_zone, fn);
    if (ret) *ret = 0.0;
    if (!base || !base->vm || !fn || nnum < 0 || (nnum > 0 && !nums)) return 1;
    if (base->kind == 1) return 1;                 /* ESM：用 instance_call */
    if (!module_has(base, fn)) return 1;
    auto* module = static_cast<JsModule*>(base);
    JsVm* v = S(base->vm);
    JSValue call = JS_GetPropertyStr(v->ctx, module->exports, "call");
    JSValue name = JS_NewString(v->ctx, fn);
    JSValue args = JS_NewArray(v->ctx);
    for (int i = 0; i < nnum; ++i)
        JS_SetPropertyUint32(v->ctx, args, (uint32_t)i, JS_NewFloat64(v->ctx, nums[i]));
    if (text) JS_SetPropertyUint32(v->ctx, args, (uint32_t)nnum, JS_NewString(v->ctx, text));
    JSValue argv[2] = { name, args };
    mq_script_module* prev = v->current_module;
    v->current_module = base;
    JSValue result = JS_Call(v->ctx, call, module->exports, 2, argv);
    v->current_module = prev;
    JS_FreeValue(v->ctx, args);
    JS_FreeValue(v->ctx, name);
    JS_FreeValue(v->ctx, call);
    if (JS_IsException(result)) { throw_and_print(v->ctx); return 2; }
    if (ret) { double value = 0.0; JS_ToFloat64(v->ctx, &value, result); *ret = value; }
    JS_FreeValue(v->ctx, result);
    return 0;
}

int eval(mq_script_vm* vm, const char* src, const char* filename) {
    MQ_TRACY_ZONE_N(script, "js.eval");
    if (!vm || !src) return 1;
    JSValue v = JS_Eval(S(vm)->ctx, src, std::strlen(src), filename ? filename : "<script>",
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
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "js.call");
    MQ_TRACY_ZONE_TEXT(mq_zone, fn);
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
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "js.call");
    MQ_TRACY_ZONE_TEXT(mq_zone, fn);
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

/* ---- 原生 ESM 模块 + 每挂载点实例（docs/34 P0）---- */

int vm_set_module_loader(mq_script_vm* vm, mq_script_module_resolve_fn resolve_fn,
                         mq_script_module_read_fn read_fn, void* user) {
    if (!vm) return 1;
    S(vm)->resolver = resolve_fn;
    S(vm)->reader = read_fn;
    S(vm)->loader_user = user;
    return 0;
}

/* 类构造函数的 prototype 不可写；普通函数可写 → 用它区分 class 与工厂。 */
static bool js_definition_is_class(JSContext* ctx, JSValueConst fn) {
    JSAtom atom = JS_NewAtom(ctx, "prototype");
    JSPropertyDescriptor desc{};
    const int rc = JS_GetOwnProperty(ctx, &desc, fn, atom);
    JS_FreeAtom(ctx, atom);
    if (rc <= 0) return false;
    const bool klass = !(desc.flags & JS_PROP_WRITABLE);
    JS_FreeValue(ctx, desc.value);
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return klass;
}

static int js_set_instance_context(JSContext* ctx, JSValueConst inst, int32_t node, int32_t scene,
                                   const char* params, const char* package_key) {
    if (JS_DefinePropertyValueStr(ctx, inst, "node", JS_NewInt32(ctx, node), 0) < 0) return -1;
    if (JS_DefinePropertyValueStr(ctx, inst, "scene", JS_NewInt32(ctx, scene), 0) < 0) return -1;
    if (JS_DefinePropertyValueStr(ctx, inst, "package", JS_NewString(ctx, package_key ? package_key : ""), 0) < 0) return -1;
    JSValue p = JS_UNDEFINED;
    if (params && *params) {
        p = JS_ParseJSON(ctx, params, std::strlen(params), "<params>");
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p = JS_NewObject(ctx);
        }
    } else {
        p = JS_NewObject(ctx);
    }
    if (JS_DefinePropertyValueStr(ctx, inst, "params", p, 0) < 0) return -1;
    if (JS_DefinePropertyValueStr(ctx, inst, "props", JS_NewObject(ctx), 0) < 0) return -1;
    return 0;
}

mq_script_module* module_load_esm(mq_script_vm* vm, const char* key) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "esm.load");
    MQ_TRACY_ZONE_TEXT(mq_zone, key);
    if (!vm || !key) return nullptr;
    JsVm* v = S(vm);
    auto it = v->esm_modules.find(key);
    if (it != v->esm_modules.end()) return it->second;
    if (!v->reader) { v->last_error = "no module reader"; return nullptr; }

    size_t len = 0;
    const char* src = v->reader(v->loader_user, key, &len);
    if (!src) {
        v->last_error = std::string("cannot read module: ") + key;
        std::print(stderr, "[script] {}\n", v->last_error);
        return nullptr;
    }
    JSValue mod = JS_Eval(v->ctx, src, len, key,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(mod)) { throw_and_print(v->ctx); return nullptr; }
    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod));
    JSValue ret = JS_EvalFunction(v->ctx, mod);       /* takes ownership of mod */
    if (JS_IsException(ret)) { throw_and_print(v->ctx); return nullptr; }
    if (JS_IsPromise(ret)) {
        JSPromiseStateEnum st = JS_PromiseState(v->ctx, ret);
        while (st == JS_PROMISE_PENDING) {
            JSContext* c1 = nullptr;
            if (JS_ExecutePendingJob(v->rt, &c1) <= 0) break;
            st = JS_PromiseState(v->ctx, ret);
        }
        if (st == JS_PROMISE_REJECTED || st == JS_PROMISE_PENDING) {
            JSValue e = JS_PromiseResult(v->ctx, ret);
            const char* s = JS_ToCString(v->ctx, e);
            v->last_error = s ? s : (st == JS_PROMISE_PENDING ? "top-level await unsupported" : "module rejected");
            if (s) JS_FreeCString(v->ctx, s);
            JS_FreeValue(v->ctx, e);
            std::print(stderr, "[script] module '{}' failed: {}\n", key, v->last_error);
            JS_FreeValue(v->ctx, ret);
            return nullptr;
        }
        JSValue result = JS_PromiseResult(v->ctx, ret);
        JS_FreeValue(v->ctx, result);
    }
    JS_FreeValue(v->ctx, ret);

    JSValue ns = JS_GetModuleNamespace(v->ctx, m);
    if (JS_IsException(ns)) { throw_and_print(v->ctx); return nullptr; }
    JSValue def = JS_GetPropertyStr(v->ctx, ns, "default");
    auto* em = new JsEsmModule();
    em->vm = vm;
    em->kind = 1;
    em->key = key;
    em->ns = ns;
    em->def = def;
    v->esm_modules[key] = em;
    return em;
}

mq_script_instance* module_instantiate(mq_script_module* module, int32_t node, int32_t scene,
                                       const char* params, const char* package_key) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "esm.instantiate");
    if (!module || module->kind != 1) return nullptr;
    auto* em = static_cast<JsEsmModule*>(module);
    JsVm* v = S(module->vm);
    JSContext* ctx = v->ctx;
    const JSValue def = em->def;
    if (JS_IsUndefined(def)) {
        v->last_error = std::string("module has no default export: ") + em->key;
        std::print(stderr, "[script] {}\n", v->last_error);
        return nullptr;
    }

    JSValue inst = JS_UNDEFINED;
    if (JS_IsFunction(ctx, def)) {
        if (js_definition_is_class(ctx, def)) {
            inst = JS_CallConstructor(ctx, def, 0, nullptr);
        } else {
            JSValue seed = JS_NewObject(ctx);
            JSValue out = JS_Call(ctx, def, seed, 0, nullptr);
            if (JS_IsException(out)) {
                JS_FreeValue(ctx, seed);
                throw_and_print(ctx);
                return nullptr;
            }
            if (JS_IsObject(out)) {
                JS_FreeValue(ctx, seed);
                inst = out;
            } else {
                JS_FreeValue(ctx, out);
                inst = seed;
            }
        }
    } else if (JS_IsObject(def)) {
        inst = JS_NewObjectProto(ctx, def);
    } else {
        v->last_error = std::string("module default must be object/class/function: ") + em->key;
        std::print(stderr, "[script] {}\n", v->last_error);
        return nullptr;
    }
    if (JS_IsException(inst)) { throw_and_print(ctx); return nullptr; }
    if (js_set_instance_context(ctx, inst, node, scene, params, package_key) < 0) {
        JS_FreeValue(ctx, inst);
        throw_and_print(ctx);
        return nullptr;
    }
    auto* out = new JsInstance();
    out->vm = module->vm;
    out->obj = inst;
    return out;
}

int instance_has(mq_script_instance* instance, const char* fn) {
    if (!instance || !fn) return 0;
    auto* inst = static_cast<JsInstance*>(instance);
    JsVm* v = S(instance->vm);
    JSValue f = JS_GetPropertyStr(v->ctx, inst->obj, fn);
    const int ok = JS_IsFunction(v->ctx, f);
    JS_FreeValue(v->ctx, f);
    return ok;
}

int instance_call(mq_script_instance* instance, const char* fn, int nnum, const double* nums,
                  const char* text, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "esm.call");
    MQ_TRACY_ZONE_TEXT(mq_zone, fn);
    if (ret) *ret = 0.0;
    if (!instance || !fn || nnum < 0 || (nnum > 0 && !nums)) return 1;
    auto* inst = static_cast<JsInstance*>(instance);
    JsVm* v = S(instance->vm);
    JSContext* ctx = v->ctx;
    JSValue f = JS_GetPropertyStr(ctx, inst->obj, fn);
    if (!JS_IsFunction(ctx, f)) { JS_FreeValue(ctx, f); return 1; }
    JSValue args[8];
    const int argc = nnum + (text ? 1 : 0);
    if (argc > 8) { JS_FreeValue(ctx, f); return 2; }
    for (int i = 0; i < nnum; ++i) args[i] = JS_NewFloat64(ctx, nums[i]);
    if (text) args[nnum] = JS_NewString(ctx, text);
    JSValue r = JS_Call(ctx, f, inst->obj, argc, argc ? args : nullptr);
    for (int i = 0; i < argc; ++i) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, f);
    if (JS_IsException(r)) {
        char buf[512] = { 0 };
        last_error(instance->vm, buf, (int)sizeof buf);
        inst->last_error = buf;
        throw_and_print(ctx);
        return 2;
    }
    if (ret) { double d = 0; JS_ToFloat64(ctx, &d, r); *ret = d; }
    JS_FreeValue(ctx, r);
    return 0;
}

int instance_last_error(mq_script_instance* instance, char* out, int cap) {
    if (!instance || !out || cap <= 0) return 1;
    out[0] = '\0';
    const std::string& e = static_cast<JsInstance*>(instance)->last_error;
    if (e.empty()) return 1;
    std::snprintf(out, (size_t)cap, "%s", e.c_str());
    return 0;
}

void instance_destroy(mq_script_instance* instance) {
    if (!instance) return;
    auto* inst = static_cast<JsInstance*>(instance);
    JsVm* v = S(instance->vm);
    if (v && v->ctx && !JS_IsUndefined(inst->obj)) JS_FreeValue(v->ctx, inst->obj);
    delete inst;
}

/* 帧边界（docs/10 D7）：JS 的 cycle GC 只在帧边界运行（docs/14） */
void frame_boundary(mq_script_vm* vm) {
    if (!vm) return;
    gc(vm);
}

void gc(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "js.gc");
    if (vm && S(vm)->rt) JS_RunGC(S(vm)->rt);
}

} // namespace maque::script::js
