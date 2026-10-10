/* maque 控制平面（docs/11）：JSON-RPC（CDP 式 domain.method）+ 事件。
 * 传输无关：dispatch 一条请求 → 一条响应；宿主可接 stdio/WebSocket/MCP。
 * per-instance（无全局）。方法由宿主注册（引擎能力），协议帧由此固定。 */
#pragma once

#include <stddef.h>
#include "core/persist/mq_persist.h"   // mq_json

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_control mq_control;

/* 方法处理器：把结果（JSON 值文本）写入 out；返回 0 成功。 */
typedef int (*mq_control_handler)(void* user, const char* method,
                                  const mq_json* params, char* out, size_t out_cap);

mq_control* mq_control_create(void);
void        mq_control_destroy(mq_control* c);
void        mq_control_register(mq_control* c, const char* method, mq_control_handler fn, void* user);

/* 处理一条 JSON-RPC 请求，返回响应 JSON（归 c 所有）。 */
const char* mq_control_dispatch(mq_control* c, const char* request);

#ifdef __cplusplus
}
#endif
