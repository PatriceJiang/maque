/* 声明式引擎接口的脚本绑定入口（docs/21）。
 * mq_bind_generated(vm) 由 codegen 从 schema.yaml 的 api_list 生成
 * （generated/maque_bindings.cpp），把 <ns>.<fn> 注册到脚本全局。 */
#pragma once

#include "core/script/mq_script.h"

#ifdef __cplusplus
extern "C" {
#endif

void mq_bind_generated(mq_script_vm* vm, void* ctx);

#ifdef __cplusplus
}
#endif
