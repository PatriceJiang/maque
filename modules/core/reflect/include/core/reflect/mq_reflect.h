/* maque 反射运行时（per-instance，无全局可变状态；docs/06/07，ADR-0014）。
 * 每个引擎实例持有自己的 mq_reflect_ctx —— 支持多实例并存。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "maque_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQ_ATTR_TRANSIENT  (1u << 0)
#define MQ_ATTR_READONLY   (1u << 1)
#define MQ_ATTR_DEPRECATED (1u << 2)

#define MQ_TYPE_ENUM    (1u << 0)
#define MQ_TYPE_POD     (1u << 1)
#define MQ_TYPE_DYNAMIC (1u << 2)

/* 反射上下文（实例所有）。 */
typedef struct mq_reflect_ctx mq_reflect_ctx;

mq_reflect_ctx* mq_reflect_create(void);
void            mq_reflect_destroy(mq_reflect_ctx* ctx);

/* 把生成的内置注册表（mq_schema_types）载入本 ctx。 */
int mq_reflect_load_schema(mq_reflect_ctx* ctx);
/* 动态注册（模块 init 时）。 */
int mq_reflect_register(mq_reflect_ctx* ctx, const mq_type_desc* type);

const mq_type_desc*  mq_reflect_type_by_id(mq_reflect_ctx* ctx, uint32_t type_id);
const mq_type_desc*  mq_reflect_type_by_name(mq_reflect_ctx* ctx, const char* fq_name);

/* 枚举已注册类型（自省/AI：docs/06 A1、docs/11 Types domain）。
 * 顺序 = 注册顺序（确定性，禁地址依赖）。 */
uint32_t             mq_reflect_count(mq_reflect_ctx* ctx);
const mq_type_desc*  mq_reflect_type_at(mq_reflect_ctx* ctx, uint32_t index);

/* 字段查找（不需 ctx）。 */
const mq_field_desc* mq_reflect_field(const mq_type_desc* type, const char* field_name);
const mq_field_desc* mq_reflect_field_path(mq_reflect_ctx* ctx, const mq_type_desc* type,
                                           const char* path, uint32_t* out_absolute_offset);

/* 标量读写（数值/布尔/枚举）。 */
int mq_reflect_get_f64(mq_reflect_ctx* ctx, const void* obj, const mq_field_desc* field, double* out);
int mq_reflect_set_f64(mq_reflect_ctx* ctx, void* obj, const mq_field_desc* field, double value);
int mq_reflect_get_i64(mq_reflect_ctx* ctx, const void* obj, const mq_field_desc* field, int64_t* out);
int mq_reflect_set_i64(mq_reflect_ctx* ctx, void* obj, const mq_field_desc* field, int64_t value);

typedef struct mq_visitor {
    void* user;
    int (*on_enter)(void* user, const mq_type_desc* type, void* obj);
    int (*on_exit )(void* user, const mq_type_desc* type, void* obj);
    int (*on_field)(void* user, const mq_field_desc* field, const void* value_ptr);
} mq_visitor;

int mq_reflect_visit(mq_reflect_ctx* ctx, void* obj, const mq_type_desc* type, const mq_visitor* visitor);

#ifdef __cplusplus
}
#endif
