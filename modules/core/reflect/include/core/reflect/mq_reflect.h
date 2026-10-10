/* maque 反射运行时 —— 加载/查询 schema 描述符，按字段访问与遍历 (docs/06)。
 * 跨模块接口为 C ABI（docs/02）；描述符来自 codegen 的 maque_desc.h。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "maque_desc.h"   /* mq_type_desc / mq_field_desc / mq_type_kind */

#ifdef __cplusplus
extern "C" {
#endif

/* 字段属性位（对应 mq_field_desc.attrs_flags） */
#define MQ_ATTR_TRANSIENT  (1u << 0)
#define MQ_ATTR_READONLY   (1u << 1)
#define MQ_ATTR_DEPRECATED (1u << 2)

/* 类型标志位（对应 mq_type_desc.flags） */
#define MQ_TYPE_ENUM    (1u << 0)
#define MQ_TYPE_POD     (1u << 1)
#define MQ_TYPE_DYNAMIC (1u << 2)

/* 初始化：把生成的内置注册表并入运行时注册表。幂等。返回 0 成功。 */
int mq_reflect_init(void);

/* 动态注册一个类型（模块 init 时调用）；已存在同 type_id 则返回非 0。 */
int mq_reflect_register(const mq_type_desc* type);

/* 查询：按 type_id / 全限定名；未找到返回 NULL。 */
const mq_type_desc*  mq_reflect_type_by_id(uint32_t type_id);
const mq_type_desc*  mq_reflect_type_by_name(const char* fq_name);

/* 在给定类型里按字段名查找字段；未找到返回 NULL。 */
const mq_field_desc* mq_reflect_field(const mq_type_desc* type, const char* field_name);

/* 按 "a.b.c" 路径解析字段；out_absolute_offset（可空）给出相对根对象的字节偏移。 */
const mq_field_desc* mq_reflect_field_path(const mq_type_desc* type, const char* path,
                                           uint32_t* out_absolute_offset);

/* 标量读写（数值/布尔/枚举）。成功返回 0，失败非 0。 */
int mq_reflect_get_f64(const void* obj, const mq_field_desc* field, double* out);
int mq_reflect_set_f64(void* obj, const mq_field_desc* field, double value);
int mq_reflect_get_i64(const void* obj, const mq_field_desc* field, int64_t* out);
int mq_reflect_set_i64(void* obj, const mq_field_desc* field, int64_t value);

/* 遍历访问器。回调返回非 0 即中止遍历。 */
typedef struct mq_visitor {
    void* user;
    int (*on_enter)(void* user, const mq_type_desc* type, void* obj);      /* 可选 */
    int (*on_exit )(void* user, const mq_type_desc* type, void* obj);      /* 可选 */
    int (*on_field)(void* user, const mq_field_desc* field, const void* value_ptr);
} mq_visitor;

int mq_reflect_visit(void* obj, const mq_type_desc* type, const mq_visitor* visitor);

#ifdef __cplusplus
}
#endif
