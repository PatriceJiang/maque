/* maque 脚本模块（M2，docs/14）—— quickjs 为主 VM 的最小 C ABI。
 * 宿主以"数值/字符串"回调注册原生函数（保持 C ABI 干净，隐藏 JSValue）。
 * 确定性：脚本跑在模拟线程；cycle GC 仅帧边界由 mq_script_gc 驱动。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

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
/* 调用全局函数（0/1/3 个数值参数）；ret 可空。成功 0。
 * 多参数形态供事件分发（docs/19 §8）：只传标量，不跨边界传对象。 */
int  mq_script_call0(mq_script_vm* vm, const char* fn, double* ret);
int  mq_script_call1(mq_script_vm* vm, const char* fn, double arg, double* ret);
int  mq_script_call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret);
int  mq_script_call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret);
/* 数值 + 字符串（文本输入等事件）：字符串不跨边界传对象。 */
int  mq_script_call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret);
int  mq_script_call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret);
int  mq_script_call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret);

/* 最近一次求值/调用失败的错误文本（热重载诊断用）。成功写入返回 0；无错误返回 1。 */
int mq_script_last_error(mq_script_vm* vm, char* out, int cap);

/* 注入引擎 RNG（docs/10 D2）：覆盖 quickjs 的 Math.random（其默认种子 = 墙钟时间，
 * 会破坏回放）。宿主应在创建 VM 后立即设置；未设置时 Math.random 保持 quickjs 默认。 */
typedef uint32_t (*mq_script_rng_fn)(void* user);
void mq_script_set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user);

/* 帧边界 GC（确定性：模拟帧内不隐式回收）。返回本次回收对象数估计。 */
void mq_script_gc(mq_script_vm* vm);

/* 原生函数（宿主提供）。返回值作为 JS number；返回非 0 表示 JS 异常。
 * ret_str：把结果文本写入 ret（容量 ret_cap），由 VM 复制为 JS string —— 宿主不转移所有权。 */
typedef int (*mq_script_num_fn)(void* user, int argc, const double* argv, double* ret);
typedef int (*mq_script_str_fn)(void* user, const char* arg, double* ret);
typedef int (*mq_script_ret_str_fn)(void* user, int argc, const double* argv, char* ret, int ret_cap);
int mq_script_register_num(mq_script_vm* vm, const char* name, mq_script_num_fn fn, void* user);
int mq_script_register_str(mq_script_vm* vm, const char* name, mq_script_str_fn fn, void* user);
int mq_script_register_ret_str(mq_script_vm* vm, const char* name, mq_script_ret_str_fn fn, void* user);

/* 数值参数 + 单个尾部字符串（如 set_label(id, text)）：字符串不跨边界传对象。 */
typedef int (*mq_script_mixed_fn)(void* user, int argc, const double* argv, const char* s, double* ret);
int mq_script_register_mixed(mq_script_vm* vm, const char* name, mq_script_mixed_fn fn, void* user);

#ifdef __cplusplus
}
#endif
