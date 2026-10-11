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

/* 已注册方法数 / 第 i 个方法名（自省：MCP tools/list、AI 发现能力，docs/11 §3）。 */
uint32_t    mq_control_method_count(mq_control* c);
const char* mq_control_method_at(mq_control* c, uint32_t index);

/* ---- MCP 包装（上层：把 JSON-RPC domain.method 暴露为 tools，docs/11 §2）----
 * 支持 JSON-RPC 方法：
 *   initialize          → { protocolVersion, serverInfo, capabilities }
 *   tools/list          → { tools:[{name, description, inputSchema}] }
 *   tools/call          → { name, arguments } → 转发为同名 JSON-RPC 方法
 *   notifications/*     → 空结果（通知无需响应）
 * 未注册的方法：若已 set_tools_handler，则交给宿主（用于 UI 侧工具）。 */
void mq_control_set_tools_handler(mq_control* c, mq_control_handler fn, void* user);
const char* mq_control_dispatch_mcp(mq_control* c, const char* request);

#ifdef __cplusplus
}
#endif
