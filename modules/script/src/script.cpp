/* 脚本后端派发层（docs/14「统一后端 ABI」）：
 * 公共 ABI（abi/script.h）只在此实现一次，按 VM 的 `ScriptOps` 表转发到具体后端：
 *   · js     → modules/script/src/js_backend.cpp（quickjs-ng）
 *   · lua    → modules/script/src/lua_backend.cpp（官方 Lua 5.4）
 *   · native → modules/script/src/native_backend.cpp（C/C++ 编译型 guest，plans/26）
 * 语言由脚本文件扩展名决定（mq_script_lang_for_path），也可显式指定（mq_script_vm_create_lang）。 */
#include "vm_base.h"
#include "core/tracy/mq_tracy.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void js_frame_boundary(mq_script_vm* vm) {
    maque::script::js::frame_boundary(vm);
    maque::script::js::gc(vm);
}

/* JS/Lua 的 module/instance 面在各自命名空间里已有实现；native 的 P0 为 nullptr。 */
const ScriptOps kJsOps = {
    .id = "js",
    .create = &maque::script::js::vm_create,
    .destroy = &maque::script::js::vm_destroy,
    .module_load = &maque::script::js::module_load,
    .module_destroy = &maque::script::js::module_destroy,
    .module_has = &maque::script::js::module_has,
    .module_call = &maque::script::js::module_call,
    .current_module = &maque::script::js::current_module,
    .vm_set_module_loader = &maque::script::js::vm_set_module_loader,
    .module_load_esm = &maque::script::js::module_load_esm,
    .module_instantiate = &maque::script::js::module_instantiate,
    .instance_has = &maque::script::js::instance_has,
    .instance_call = &maque::script::js::instance_call,
    .instance_last_error = &maque::script::js::instance_last_error,
    .instance_destroy = &maque::script::js::instance_destroy,
    .eval = &maque::script::js::eval,
    .has = &maque::script::js::has,
    .call0 = &maque::script::js::call0,
    .call1 = &maque::script::js::call1,
    .call2 = &maque::script::js::call2,
    .call3 = &maque::script::js::call3,
    .call0s = &maque::script::js::call0s,
    .call1s = &maque::script::js::call1s,
    .call2s = &maque::script::js::call2s,
    .last_error = &maque::script::js::last_error,
    .set_rng = &maque::script::js::set_rng,
    .gc = &maque::script::js::gc,
    .frame_boundary = &js_frame_boundary,
    .register_num = &maque::script::js::register_num,
    .register_str = &maque::script::js::register_str,
    .register_ret_str = &maque::script::js::register_ret_str,
    .register_mixed = &maque::script::js::register_mixed,
    .register_multi = &maque::script::js::register_multi,
    .register_multi_ret_str = &maque::script::js::register_multi_ret_str,
    .register_bytes = &maque::script::js::register_bytes,
};

const ScriptOps kLuaOps = {
    .id = "lua",
    .create = &maque::script::lua::vm_create,
    .destroy = &maque::script::lua::vm_destroy,
    .module_load = &maque::script::lua::module_load,
    .module_destroy = &maque::script::lua::module_destroy,
    .module_has = &maque::script::lua::module_has,
    .module_call = &maque::script::lua::module_call,
    .current_module = &maque::script::lua::current_module,
    /* Lua 的 ESM/每挂载点实例面未实现：显式空，派发层返回“不支持”。 */
    .vm_set_module_loader = nullptr,
    .module_load_esm = nullptr,
    .module_instantiate = nullptr,
    .instance_has = nullptr,
    .instance_call = nullptr,
    .instance_last_error = nullptr,
    .instance_destroy = nullptr,
    .eval = &maque::script::lua::eval,
    .has = &maque::script::lua::has,
    .call0 = &maque::script::lua::call0,
    .call1 = &maque::script::lua::call1,
    .call2 = &maque::script::lua::call2,
    .call3 = &maque::script::lua::call3,
    .call0s = &maque::script::lua::call0s,
    .call1s = &maque::script::lua::call1s,
    .call2s = &maque::script::lua::call2s,
    .last_error = &maque::script::lua::last_error,
    .set_rng = &maque::script::lua::set_rng,
    .gc = &maque::script::lua::gc,
    .frame_boundary = &maque::script::lua::frame_boundary,
    .register_num = &maque::script::lua::register_num,
    .register_str = &maque::script::lua::register_str,
    .register_ret_str = &maque::script::lua::register_ret_str,
    .register_mixed = &maque::script::lua::register_mixed,
    .register_multi = &maque::script::lua::register_multi,
    .register_multi_ret_str = &maque::script::lua::register_multi_ret_str,
    .register_bytes = &maque::script::lua::register_bytes,
};

/* native 的 module/instance 面刻意留空：P0 走旧“每脚本一个 VM”路径。
 * 空字段由派发层当作“不支持”返回（不静默降级到 JS）。 */
const ScriptOps kNativeOps = {
    .id = "native",
    .create = &maque::script::native::vm_create,
    .destroy = &maque::script::native::vm_destroy,
    .module_load = nullptr,
    .module_destroy = nullptr,
    .module_has = nullptr,
    .module_call = nullptr,
    .current_module = nullptr,
    .vm_set_module_loader = nullptr,
    .module_load_esm = nullptr,
    .module_instantiate = nullptr,
    .instance_has = nullptr,
    .instance_call = nullptr,
    .instance_last_error = nullptr,
    .instance_destroy = nullptr,
    .eval = &maque::script::native::eval,
    .has = &maque::script::native::has,
    .call0 = &maque::script::native::call0,
    .call1 = &maque::script::native::call1,
    .call2 = &maque::script::native::call2,
    .call3 = &maque::script::native::call3,
    .call0s = &maque::script::native::call0s,
    .call1s = &maque::script::native::call1s,
    .call2s = &maque::script::native::call2s,
    .last_error = &maque::script::native::last_error,
    .set_rng = &maque::script::native::set_rng,
    .gc = &maque::script::native::gc,
    .frame_boundary = &maque::script::native::frame_boundary,
    .register_num = &maque::script::native::register_num,
    .register_str = &maque::script::native::register_str,
    .register_ret_str = &maque::script::native::register_ret_str,
    .register_mixed = &maque::script::native::register_mixed,
    .register_multi = &maque::script::native::register_multi,
    .register_multi_ret_str = &maque::script::native::register_multi_ret_str,
    .register_bytes = &maque::script::native::register_bytes,
};

const ScriptOps* find_ops(const char* lang) {
    if (!lang || !*lang) return &kJsOps;
    if (std::strcmp(lang, "js") == 0 || std::strcmp(lang, "javascript") == 0 ||
        std::strcmp(lang, "quickjs") == 0)
        return &kJsOps;
    if (std::strcmp(lang, "lua") == 0) return &kLuaOps;
    if (std::strcmp(lang, "native") == 0 || std::strcmp(lang, "c") == 0 ||
        std::strcmp(lang, "cpp") == 0 || std::strcmp(lang, "c++") == 0)
        return &kNativeOps;
    return nullptr;
}

bool has_suffix(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    const size_t n = std::strlen(s);
    const size_t m = std::strlen(suffix);
    return n >= m && std::strcmp(s + n - m, suffix) == 0;
}

} // namespace

extern "C" {

mq_script_vm* mq_script_vm_create(void) { return mq_script_vm_create_lang(nullptr); }

mq_script_vm* mq_script_vm_create_lang(const char* lang) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "vm.create");
    MQ_TRACY_ZONE_TEXT(mq_zone, lang);
    const ScriptOps* ops = find_ops(lang);
    if (!ops) return nullptr;
    mq_script_vm* vm = ops->create ? ops->create() : nullptr;
    if (!vm) return nullptr;                     /* 未编入的后端（lua/native）明确失败 */
    vm->ops = ops;
    return vm;
}

int mq_script_vm_configure_native(mq_script_vm* vm, const mq_script_vm_native_config* cfg) {
    if (!vm || !vm->ops || std::strcmp(vm->ops->id, "native") != 0) return 1;
    return maque::script::native::configure(vm, cfg);
}

const char* mq_script_lang_for_path(const char* path) {
    if (!path) return "js";
    if (has_suffix(path, ".lua")) return "lua";
    if (has_suffix(path, ".c") || has_suffix(path, ".cc") || has_suffix(path, ".cpp") ||
        has_suffix(path, ".cxx") || has_suffix(path, ".c++"))
        return "native";
    if (has_suffix(path, ".so") || has_suffix(path, ".dll") || has_suffix(path, ".dylib"))
        return "native";
    if (has_suffix(path, ".wasm")) return "wasm";
    return "js";
}

int mq_script_backends(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::string list = "js";
    if (mq_script_vm* probe = maque::script::lua::vm_create()) {
        maque::script::lua::vm_destroy(probe);
        list += ",lua";
    }
    if (mq_script_vm* probe = maque::script::native::vm_create()) {
        maque::script::native::vm_destroy(probe);
        list += ",native";
    }
    std::snprintf(out, (size_t)cap, "%s", list.c_str());
    return 0;
}

const char* mq_script_lang(mq_script_vm* vm) { return vm ? (vm->lang ? vm->lang : "js") : ""; }

void mq_script_vm_destroy(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "vm.destroy");
    if (!vm || !vm->ops || !vm->ops->destroy) return;
    vm->ops->destroy(vm);
}

mq_script_module* mq_script_module_load(mq_script_vm* vm, const char* src, const char* filename) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "module.load");
    MQ_TRACY_ZONE_TEXT(mq_zone, filename);
    if (!vm || !src || !vm->ops || !vm->ops->module_load) return nullptr;
    return vm->ops->module_load(vm, src, filename);
}

void mq_script_module_destroy(mq_script_module* module) {
    if (!module || !module->vm || !module->vm->ops || !module->vm->ops->module_destroy) return;
    module->vm->ops->module_destroy(module);
}

int mq_script_module_has(mq_script_module* module, const char* fn) {
    if (!module || !module->vm || !module->vm->ops || !module->vm->ops->module_has || !fn) return 0;
    return module->vm->ops->module_has(module, fn);
}

int mq_script_module_call(mq_script_module* module, const char* fn, int nnum, const double* nums,
                          const char* text, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "module.call");
    MQ_TRACY_ZONE_TEXT(mq_zone, fn);
    if (ret) *ret = 0.0;
    if (!module || !module->vm || !module->vm->ops || !module->vm->ops->module_call || !fn ||
        nnum < 0 || (nnum > 0 && !nums))
        return 1;
    return module->vm->ops->module_call(module, fn, nnum, nums, text, ret);
}

mq_script_module* mq_script_current_module(mq_script_vm* vm) {
    if (!vm || !vm->ops || !vm->ops->current_module) return nullptr;
    return vm->ops->current_module(vm);
}

int mq_script_vm_set_module_loader(mq_script_vm* vm, mq_script_module_resolve_fn resolve_fn,
                                   mq_script_module_read_fn read_fn, void* user) {
    if (!vm || !vm->ops || !vm->ops->vm_set_module_loader) return 1;
    return vm->ops->vm_set_module_loader(vm, resolve_fn, read_fn, user);
}

mq_script_module* mq_script_module_load_esm(mq_script_vm* vm, const char* key) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "esm.load");
    MQ_TRACY_ZONE_TEXT(mq_zone, key);
    if (!vm || !key || !vm->ops || !vm->ops->module_load_esm) return nullptr;
    return vm->ops->module_load_esm(vm, key);
}

mq_script_instance* mq_script_module_instantiate(mq_script_module* module, int32_t node, int32_t scene,
                                                 const char* params, const char* package_key) {
    if (!module || !module->vm || !module->vm->ops || !module->vm->ops->module_instantiate) return nullptr;
    return module->vm->ops->module_instantiate(module, node, scene, params, package_key);
}

int mq_script_instance_has(mq_script_instance* instance, const char* fn) {
    if (!instance || !instance->vm || !instance->vm->ops || !instance->vm->ops->instance_has) return 0;
    return instance->vm->ops->instance_has(instance, fn);
}

int mq_script_instance_call(mq_script_instance* instance, const char* fn, int nnum, const double* nums,
                            const char* text, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "instance.call");
    MQ_TRACY_ZONE_TEXT(mq_zone, fn);
    if (!instance || !instance->vm || !instance->vm->ops || !instance->vm->ops->instance_call) return 1;
    return instance->vm->ops->instance_call(instance, fn, nnum, nums, text, ret);
}

int mq_script_instance_last_error(mq_script_instance* instance, char* out, int cap) {
    if (!instance || !instance->vm || !instance->vm->ops || !instance->vm->ops->instance_last_error)
        return 1;
    return instance->vm->ops->instance_last_error(instance, out, cap);
}

void mq_script_instance_destroy(mq_script_instance* instance) {
    if (!instance || !instance->vm || !instance->vm->ops || !instance->vm->ops->instance_destroy) return;
    instance->vm->ops->instance_destroy(instance);
}

int mq_script_eval(mq_script_vm* vm, const char* src, const char* filename) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, filename);
    if (!vm || !vm->ops || !vm->ops->eval) return 1;
    return vm->ops->eval(vm, src, filename);
}

int mq_script_has(mq_script_vm* vm, const char* fn) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->has) return 0;
    return vm->ops->has(vm, fn);
}

int mq_script_call0(mq_script_vm* vm, const char* fn, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call0) return 1;
    return vm->ops->call0(vm, fn, ret);
}
int mq_script_call1(mq_script_vm* vm, const char* fn, double a, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call1) return 1;
    return vm->ops->call1(vm, fn, a, ret);
}
int mq_script_call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call2) return 1;
    return vm->ops->call2(vm, fn, a, b, ret);
}
int mq_script_call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call3) return 1;
    return vm->ops->call3(vm, fn, a, b, c, ret);
}
int mq_script_call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call0s) return 1;
    return vm->ops->call0s(vm, fn, s, ret);
}
int mq_script_call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call1s) return 1;
    return vm->ops->call1s(vm, fn, a, s, ret);
}
int mq_script_call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm || !vm->ops || !vm->ops->call2s) return 1;
    return vm->ops->call2s(vm, fn, a, b, s, ret);
}

int mq_script_last_error(mq_script_vm* vm, char* out, int cap) {
    MQ_TRACY_ZONE_N(script, "last.error");
    if (!vm || !vm->ops || !vm->ops->last_error) return 1;
    return vm->ops->last_error(vm, out, cap);
}

void mq_script_set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user) {
    if (!vm || !vm->ops || !vm->ops->set_rng) return;
    vm->ops->set_rng(vm, fn, user);
}

void mq_script_gc(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "gc");
    if (!vm || !vm->ops || !vm->ops->gc) return;
    vm->ops->gc(vm);
}

/* 帧边界钩子（docs/10 D7）：GC 等只在此处发生 —— 各后端语义一致。 */
void mq_script_frame_boundary(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "frame.boundary");
    if (!vm || !vm->ops || !vm->ops->frame_boundary) return;
    vm->ops->frame_boundary(vm);
}

int mq_script_register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_num) return 1;
    return vm->ops->register_num(vm, name, fn, user);
}
int mq_script_register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_str) return 1;
    return vm->ops->register_str(vm, name, fn, user);
}
int mq_script_register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_ret_str) return 1;
    return vm->ops->register_ret_str(vm, name, fn, user);
}
int mq_script_register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_mixed) return 1;
    return vm->ops->register_mixed(vm, name, fn, user);
}
int mq_script_register_multi(mq_script_vm* vm, const char* name, const char* kinds,
                             mq_script_multi_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_multi) return 1;
    return vm->ops->register_multi(vm, name, kinds, fn, user);
}
int mq_script_register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                                     mq_script_multi_ret_str_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_multi_ret_str) return 1;
    return vm->ops->register_multi_ret_str(vm, name, kinds, fn, user);
}
int mq_script_register_bytes(mq_script_vm* vm, const char* name, const char* kinds,
                             mq_script_bytes_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm || !vm->ops || !vm->ops->register_bytes) return 1;
    return vm->ops->register_bytes(vm, name, kinds, fn, user);
}

} // extern "C"
