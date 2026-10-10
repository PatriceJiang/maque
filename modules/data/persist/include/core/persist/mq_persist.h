/* maque 持久化（最小切片，docs/08）：把文本(JSON)反序列化进反射描述的类型。
 * 反射模型即 canonical IR：加载/保存都按 mq_type_desc 的字段偏移进行。
 * 后续扩展为 codec × backend（binary/yaml…，经 VFS）。 */
#pragma once

#include <stddef.h>
#include "maque_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 把 JSON 文本加载进 out（out 为 type 的实例）。成功 0，失败非 0。
 * 支持：标量/布尔/枚举/字符串、嵌套 struct、定长数组；忽略未知键；空值跳过。 */
int mq_persist_load_json(const mq_type_desc* type, const char* json, size_t len, void* out);

/* ---- 通用 JSON DOM（供引擎按资源结构加载，如场景实体树）----
 * 生命周期：mq_json_parse 返回文档；其成员指针在文档存活期有效。 */
typedef struct mq_json mq_json;
mq_json*       mq_json_parse(const char* text, size_t len);   /* 失败返回 NULL */
void           mq_json_free(mq_json* doc);
const mq_json* mq_json_get(const mq_json* obj, const char* key); /* 对象成员或 NULL */
uint32_t       mq_json_len(const mq_json* arr);                  /* 数组长度或 0 */
const mq_json* mq_json_at(const mq_json* arr, uint32_t i);       /* 数组元素或 NULL */
double         mq_json_num(const mq_json* v, double def);
const char*    mq_json_str(const mq_json* v, const char* def);

#ifdef __cplusplus
}
#endif
