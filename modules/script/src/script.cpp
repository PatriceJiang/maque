/* 脚本后端派发层（docs/14「统一后端 ABI」）：
 * 公共 ABI（abi/script.h）只在此实现一次，按 VM 的语言 id 转发到具体后端：
 *   · js  → modules/script/src/js_backend.cpp（quickjs-ng）
 *   · lua → modules/script/src/lua_backend.cpp（官方 Lua 5.4，沙箱 + 确定性 GC）
 * 语言由脚本文件扩展名决定（mq_script_lang_for_path），也可显式指定（mq_script_vm_create_lang）。 */
#include "vm_base.h"
#include "core/tracy/mq_tracy.h"

#include <cstring>
#include <string>

namespace {

/* 后端注册表（编译期确定；未编入的后端 create 返回 nullptr → 此处不列出）。 */
} // namespace

namespace {
bool is_lua(const mq_script_vm* vm) { return vm && vm->lang && std::strcmp(vm->lang, "lua") == 0; }
} // namespace

extern "C" {

mq_script_vm* mq_script_vm_create(void) { return mq_script_vm_create_lang(nullptr); }

mq_script_vm* mq_script_vm_create_lang(const char* lang) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "vm.create");
    MQ_TRACY_ZONE_TEXT(mq_zone, lang);
    if (!lang || !*lang) return maque::script::js::vm_create();          /* 默认后端：js */
    if (std::strcmp(lang, "js") == 0 || std::strcmp(lang, "javascript") == 0 ||
        std::strcmp(lang, "quickjs") == 0)
        return maque::script::js::vm_create();
    if (std::strcmp(lang, "lua") == 0) {
        mq_script_vm* vm = maque::script::lua::vm_create();
        if (!vm) return nullptr;                                          /* 未编入 Lua */
        return vm;
    }
    return nullptr;
}

const char* mq_script_lang_for_path(const char* path) {
    if (!path) return "js";
    const char* dot = std::strrchr(path, '.');
    if (dot && (std::strcmp(dot, ".lua") == 0)) return "lua";
    return "js";
}

int mq_script_backends(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::string list = "js";
    if (maque::script::lua::vm_create) {
        /* 探测 Lua 是否编入：创建/销毁一次（无副作用） */
        if (mq_script_vm* probe = maque::script::lua::vm_create()) {
            maque::script::lua::vm_destroy(probe);
            list += ",lua";
        }
    }
    std::snprintf(out, (size_t)cap, "%s", list.c_str());
    return 0;
}

const char* mq_script_lang(mq_script_vm* vm) { return vm ? (vm->lang ? vm->lang : "js") : ""; }

void mq_script_vm_destroy(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "vm.destroy");
    if (!vm) return;
    if (vm->lang && std::strcmp(vm->lang, "lua") == 0) maque::script::lua::vm_destroy(vm);
    else maque::script::js::vm_destroy(vm);
}

int mq_script_eval(mq_script_vm* vm, const char* src, const char* filename) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, filename);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::eval(vm, src, filename)
                      : maque::script::js::eval(vm, src, filename);
}

int mq_script_has(mq_script_vm* vm, const char* fn) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 0;
    return is_lua(vm) ? maque::script::lua::has(vm, fn) : maque::script::js::has(vm, fn);
}

int mq_script_call0(mq_script_vm* vm, const char* fn, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call0(vm, fn, ret) : maque::script::js::call0(vm, fn, ret);
}
int mq_script_call1(mq_script_vm* vm, const char* fn, double a, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call1(vm, fn, a, ret) : maque::script::js::call1(vm, fn, a, ret);
}
int mq_script_call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call2(vm, fn, a, b, ret) : maque::script::js::call2(vm, fn, a, b, ret);
}
int mq_script_call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call3(vm, fn, a, b, c, ret)
                      : maque::script::js::call3(vm, fn, a, b, c, ret);
}
int mq_script_call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call0s(vm, fn, s, ret) : maque::script::js::call0s(vm, fn, s, ret);
}
int mq_script_call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call1s(vm, fn, a, s, ret)
                      : maque::script::js::call1s(vm, fn, a, s, ret);
}
int mq_script_call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret) {
    MQ_TRACY_ZONE_NAMED(mq_zone, script, fn);
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::call2s(vm, fn, a, b, s, ret)
                      : maque::script::js::call2s(vm, fn, a, b, s, ret);
}

int mq_script_last_error(mq_script_vm* vm, char* out, int cap) {
    MQ_TRACY_ZONE_N(script, "last.error");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::last_error(vm, out, cap)
                      : maque::script::js::last_error(vm, out, cap);
}

void mq_script_set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user) {
    if (!vm) return;
    if (is_lua(vm)) maque::script::lua::set_rng(vm, fn, user);
    else maque::script::js::set_rng(vm, fn, user);
}

void mq_script_gc(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "gc");
    if (!vm) return;
    if (is_lua(vm)) maque::script::lua::gc(vm);
    else maque::script::js::gc(vm);
}

/* 帧边界钩子（docs/10 D7）：GC 等只在此处发生 —— 两个后端语义一致 */
void mq_script_frame_boundary(mq_script_vm* vm) {
    MQ_TRACY_ZONE_N(script, "frame.boundary");
    if (!vm) return;
    if (is_lua(vm)) maque::script::lua::frame_boundary(vm);
    else { maque::script::js::frame_boundary(vm); maque::script::js::gc(vm); }
}

int mq_script_register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::register_num(vm, name, fn, user)
                      : maque::script::js::register_num(vm, name, fn, user);
}
int mq_script_register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::register_str(vm, name, fn, user)
                      : maque::script::js::register_str(vm, name, fn, user);
}
int mq_script_register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::register_ret_str(vm, name, fn, user)
                      : maque::script::js::register_ret_str(vm, name, fn, user);
}
int mq_script_register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::register_mixed(vm, name, fn, user)
                      : maque::script::js::register_mixed(vm, name, fn, user);
}
int mq_script_register_multi(mq_script_vm* vm, const char* name, const char* kinds,
                             mq_script_multi_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::register_multi(vm, name, kinds, fn, user)
                      : maque::script::js::register_multi(vm, name, kinds, fn, user);
}
int mq_script_register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                                     mq_script_multi_ret_str_fn fn, void* user) {
    MQ_TRACY_ZONE_N(script, "register");
    if (!vm) return 1;
    return is_lua(vm) ? maque::script::lua::register_multi_ret_str(vm, name, kinds, fn, user)
                      : maque::script::js::register_multi_ret_str(vm, name, kinds, fn, user);
}

} // extern "C"
