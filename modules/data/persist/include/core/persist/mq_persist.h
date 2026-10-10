/* maque 持久化（反射驱动，docs/08）：文本(JSON) ↔ 反射类型。
 * 需要 mq_reflect_ctx（per-instance）。 */
#pragma once

#include <stddef.h>
#include "maque_desc.h"
#include "core/reflect/mq_reflect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 把 JSON 文本加载进 out（type 的实例）。成功 0。 */
int mq_persist_load_json(mq_reflect_ctx* ctx, const mq_type_desc* type,
                         const char* json, size_t len, void* out);

/* ---- 通用 JSON DOM ---- */
typedef struct mq_json mq_json;
mq_json*       mq_json_parse(const char* text, size_t len);
void           mq_json_free(mq_json* doc);
const mq_json* mq_json_get(const mq_json* obj, const char* key);
uint32_t       mq_json_len(const mq_json* arr);
const mq_json* mq_json_at(const mq_json* arr, uint32_t i);
double         mq_json_num(const mq_json* v, double def);
const char*    mq_json_str(const mq_json* v, const char* def);

/* 把一个已解析 JSON 节点按反射写入 out（异构组件：type → 描述符）。 */
int mq_persist_apply_json(mq_reflect_ctx* ctx, const mq_type_desc* type,
                          const mq_json* node, void* out);

#ifdef __cplusplus
}
#endif
