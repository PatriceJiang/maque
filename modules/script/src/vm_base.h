/* 脚本后端公共首部（docs/14「统一后端 ABI」）：
 * ABI 只暴露不透明指针 `mq_script_vm*`；本文件给出**公共首部**布局，供派发层识别后端语言，
 * 各后端把私有状态放在派生结构里（公共首部位于偏移 0，标准布局 → 可安全上下转型）。
 *
 * 后端派发用 `ScriptOps` 表（plans/26-cpp-scripts.md §4.5）：新增语言只加一张表，
 * 不再在每个导出函数里写 `is_lua ? lua : js`。表由 script.cpp 在创建 VM 后挂上。 */
#pragma once

#include "core/script/mq_script.h"

struct mq_script_vm;
struct ScriptOps;

struct mq_script_vm {
    const char* lang = nullptr;      /* 后端语言 id："js" / "lua" / "native" */
    const ScriptOps* ops = nullptr;  /* 后端操作表；创建后由 script.cpp 设置 */
};

/* 同一 VM 内的独立脚本实例。模块由调用方销毁，且必须先于所属 VM 销毁。
 * kind：0 = legacy 作用域模块（module_load），1 = 原生 ESM 模块（module_load_esm）。 */
struct mq_script_module {
    mq_script_vm* vm = nullptr;
    int kind = 0;
};

/* 每挂载点的脚本实例（docs/34 P0）：this/self 就是它。 */
struct mq_script_instance {
    mq_script_vm* vm = nullptr;
};

/* 一个后端的完整操作面。字段可以为空：派发层把空实现当作“不支持”返回
 * （例如 native 的 ESM/module/instance 面在 P0 不实现）。 */
struct ScriptOps {
    const char* id;
    mq_script_vm* (*create)();
    void (*destroy)(mq_script_vm* vm);

    mq_script_module* (*module_load)(mq_script_vm*, const char* src, const char* filename);
    void (*module_destroy)(mq_script_module*);
    int (*module_has)(mq_script_module*, const char* fn);
    int (*module_call)(mq_script_module*, const char* fn, int nnum, const double* nums,
                       const char* text, double* ret);
    mq_script_module* (*current_module)(mq_script_vm*);
    int (*vm_set_module_loader)(mq_script_vm*, mq_script_module_resolve_fn,
                                mq_script_module_read_fn, void*);
    mq_script_module* (*module_load_esm)(mq_script_vm*, const char* key);
    mq_script_instance* (*module_instantiate)(mq_script_module*, int32_t node, int32_t scene,
                                              const char* params, const char* package_key);
    int (*instance_has)(mq_script_instance*, const char* fn);
    int (*instance_call)(mq_script_instance*, const char* fn, int nnum, const double* nums,
                         const char* text, double* ret);
    int (*instance_last_error)(mq_script_instance*, char* out, int cap);
    void (*instance_destroy)(mq_script_instance*);

    int (*eval)(mq_script_vm*, const char* src, const char* filename);
    int (*has)(mq_script_vm*, const char* fn);
    int (*call0)(mq_script_vm*, const char* fn, double* ret);
    int (*call1)(mq_script_vm*, const char* fn, double arg, double* ret);
    int (*call2)(mq_script_vm*, const char* fn, double a, double b, double* ret);
    int (*call3)(mq_script_vm*, const char* fn, double a, double b, double c, double* ret);
    int (*call0s)(mq_script_vm*, const char* fn, const char* s, double* ret);
    int (*call1s)(mq_script_vm*, const char* fn, double a, const char* s, double* ret);
    int (*call2s)(mq_script_vm*, const char* fn, double a, double b, const char* s, double* ret);
    int (*last_error)(mq_script_vm*, char* out, int cap);
    void (*set_rng)(mq_script_vm*, mq_script_rng_fn, void*);
    void (*gc)(mq_script_vm*);
    void (*frame_boundary)(mq_script_vm*);

    int (*register_num)(mq_script_vm*, const char* name, mq_script_num_fn, void*);
    int (*register_str)(mq_script_vm*, const char* name, mq_script_str_fn, void*);
    int (*register_ret_str)(mq_script_vm*, const char* name, mq_script_ret_str_fn, void*);
    int (*register_mixed)(mq_script_vm*, const char* name, mq_script_mixed_fn, void*);
    int (*register_multi)(mq_script_vm*, const char* name, const char* kinds,
                          mq_script_multi_fn, void*);
    int (*register_multi_ret_str)(mq_script_vm*, const char* name, const char* kinds,
                                  mq_script_multi_ret_str_fn, void*);
    int (*register_bytes)(mq_script_vm*, const char* name, const char* kinds,
                          mq_script_bytes_fn, void*);
};

namespace maque::script::js {
mq_script_vm* vm_create(void);
void vm_destroy(mq_script_vm* vm);
mq_script_module* module_load(mq_script_vm* vm, const char* src, const char* filename);
void module_destroy(mq_script_module* module);
int module_has(mq_script_module* module, const char* fn);
int module_call(mq_script_module* module, const char* fn, int nnum, const double* nums,
                const char* text, double* ret);
mq_script_module* current_module(mq_script_vm* vm);
int vm_set_module_loader(mq_script_vm* vm, mq_script_module_resolve_fn resolve_fn,
                         mq_script_module_read_fn read_fn, void* user);
mq_script_module* module_load_esm(mq_script_vm* vm, const char* key);
mq_script_instance* module_instantiate(mq_script_module* module, int32_t node, int32_t scene,
                                       const char* params, const char* package_key);
int instance_has(mq_script_instance* instance, const char* fn);
int instance_call(mq_script_instance* instance, const char* fn, int nnum, const double* nums,
                  const char* text, double* ret);
int instance_last_error(mq_script_instance* instance, char* out, int cap);
void instance_destroy(mq_script_instance* instance);
int eval(mq_script_vm* vm, const char* src, const char* filename);
int has(mq_script_vm* vm, const char* fn);
int call0(mq_script_vm* vm, const char* fn, double* ret);
int call1(mq_script_vm* vm, const char* fn, double arg, double* ret);
int call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret);
int call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret);
int call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret);
int call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret);
int call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret);
int last_error(mq_script_vm* vm, char* out, int cap);
void set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user);
void gc(mq_script_vm* vm);
int register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user);
int register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user);
int register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user);
int register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user);
int register_multi(mq_script_vm* vm, const char* name, const char* kinds, mq_script_multi_fn fn, void* user);
int register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                           mq_script_multi_ret_str_fn fn, void* user);
int register_bytes(mq_script_vm* vm, const char* name, const char* kinds,
                   mq_script_bytes_fn fn, void* user);
void frame_boundary(mq_script_vm* vm);      /* 帧边界钩子（GC 等，docs/10 D7） */
} // namespace maque::script::js

namespace maque::script::lua {
/* 与 js 后端同形（未编入 Lua 时 create 返回 nullptr） */
mq_script_vm* vm_create(void);
void vm_destroy(mq_script_vm* vm);
mq_script_module* module_load(mq_script_vm* vm, const char* src, const char* filename);
void module_destroy(mq_script_module* module);
int module_has(mq_script_module* module, const char* fn);
int module_call(mq_script_module* module, const char* fn, int nnum, const double* nums,
                const char* text, double* ret);
mq_script_module* current_module(mq_script_vm* vm);
int vm_set_module_loader(mq_script_vm* vm, mq_script_module_resolve_fn resolve_fn,
                         mq_script_module_read_fn read_fn, void* user);
mq_script_module* module_load_esm(mq_script_vm* vm, const char* key);
mq_script_instance* module_instantiate(mq_script_module* module, int32_t node, int32_t scene,
                                       const char* params, const char* package_key);
int instance_has(mq_script_instance* instance, const char* fn);
int instance_call(mq_script_instance* instance, const char* fn, int nnum, const double* nums,
                  const char* text, double* ret);
int instance_last_error(mq_script_instance* instance, char* out, int cap);
void instance_destroy(mq_script_instance* instance);
int eval(mq_script_vm* vm, const char* src, const char* filename);
int has(mq_script_vm* vm, const char* fn);
int call0(mq_script_vm* vm, const char* fn, double* ret);
int call1(mq_script_vm* vm, const char* fn, double arg, double* ret);
int call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret);
int call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret);
int call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret);
int call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret);
int call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret);
int last_error(mq_script_vm* vm, char* out, int cap);
void set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user);
void gc(mq_script_vm* vm);
int register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user);
int register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user);
int register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user);
int register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user);
int register_multi(mq_script_vm* vm, const char* name, const char* kinds, mq_script_multi_fn fn, void* user);
int register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                           mq_script_multi_ret_str_fn fn, void* user);
int register_bytes(mq_script_vm* vm, const char* name, const char* kinds,
                   mq_script_bytes_fn fn, void* user);
void frame_boundary(mq_script_vm* vm);
} // namespace maque::script::lua

namespace maque::script::native {
/* native 后端（plans/26-cpp-scripts.md）：C/C++ 编译型 guest。
 * 只有核心面；ESM/module/instance 面为 nullptr（P0 走旧“每脚本一 VM”路径）。 */
mq_script_vm* vm_create(void);
void vm_destroy(mq_script_vm* vm);
int configure(mq_script_vm* vm, const mq_script_vm_native_config* cfg);
int eval(mq_script_vm* vm, const char* src, const char* filename);
int has(mq_script_vm* vm, const char* fn);
int call0(mq_script_vm* vm, const char* fn, double* ret);
int call1(mq_script_vm* vm, const char* fn, double arg, double* ret);
int call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret);
int call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret);
int call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret);
int call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret);
int call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret);
int last_error(mq_script_vm* vm, char* out, int cap);
void set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user);
void gc(mq_script_vm* vm);
void frame_boundary(mq_script_vm* vm);
int register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user);
int register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user);
int register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user);
int register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user);
int register_multi(mq_script_vm* vm, const char* name, const char* kinds, mq_script_multi_fn fn, void* user);
int register_multi_ret_str(mq_script_vm* vm, const char* name, const char* kinds,
                           mq_script_multi_ret_str_fn fn, void* user);
int register_bytes(mq_script_vm* vm, const char* name, const char* kinds,
                   mq_script_bytes_fn fn, void* user);
} // namespace maque::script::native
