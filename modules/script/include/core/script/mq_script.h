/* maque 脚本模块（M2，docs/14）—— quickjs 为主 VM 的最小 C ABI。
 * 宿主以"数值/字符串"回调注册原生函数（保持 C ABI 干净，隐藏 JSValue）。
 * 确定性：脚本跑在模拟线程；cycle GC 仅帧边界由 mq_script_gc 驱动。 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_script_vm mq_script_vm;

mq_script_vm* mq_script_vm_create(void);
void          mq_script_vm_destroy(mq_script_vm* vm);

/* 求值脚本（全局作用域）。成功 0。 */
int  mq_script_eval(mq_script_vm* vm, const char* src, const char* filename);
/* 是否存在全局函数。 */
int  mq_script_has(mq_script_vm* vm, const char* fn);
/* 调用全局函数（0/1 个数值参数）；ret 可空。成功 0。 */
int  mq_script_call0(mq_script_vm* vm, const char* fn, double* ret);
int  mq_script_call1(mq_script_vm* vm, const char* fn, double arg, double* ret);

/* 帧边界 GC（确定性：模拟帧内不隐式回收）。返回本次回收对象数估计。 */
void mq_script_gc(mq_script_vm* vm);

/* 原生函数（宿主提供）。返回值作为 JS number；返回非 0 表示 JS 异常。 */
typedef int (*mq_script_num_fn)(void* user, int argc, const double* argv, double* ret);
typedef int (*mq_script_str_fn)(void* user, const char* arg, double* ret);
int mq_script_register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user);
int mq_script_register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user);

#ifdef __cplusplus
}
#endif
