/* 脚本后端公共首部（docs/14「统一后端 ABI」）：
 * ABI 只暴露不透明指针 `mq_script_vm*`；本文件给出**公共首部**布局，供派发层识别后端语言，
 * 各后端把私有状态放在派生结构里（公共首部位于偏移 0，标准布局 → 可安全上下转型）。 */
#pragma once

#include "core/script/mq_script.h"

struct mq_script_vm {
    const char* lang = nullptr;      /* 后端语言 id："js" / "lua" */
};

namespace maque::script::js {
mq_script_vm* vm_create(void);
void vm_destroy(mq_script_vm* vm);
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
