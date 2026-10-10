/* maque 句柄与世代槽表 (docs/07 §3)。
 * 句柄 = {index, generation}：释放即世代自增 → 悬垂可检测。
 * 用途：scene/实体、资源引用、模块对象等一切"稳定可序列化引用"的底座。
 * 跨模块接口为 C ABI（docs/02）；表拥有元素存储，引用方不持有裸指针。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_handle {
    uint32_t index;
    uint32_t generation;
} mq_handle;

#define MQ_HANDLE_INDEX_NONE 0xFFFFFFFFu
#ifdef __cplusplus
#define MQ_HANDLE_INVALID mq_handle{ MQ_HANDLE_INDEX_NONE, 0u }
#else
#define MQ_HANDLE_INVALID ((mq_handle){ MQ_HANDLE_INDEX_NONE, 0u })
#endif

/* 句柄相等（用于比较，勿直接 memcmp 结构体填充）。 */
static inline int mq_handle_equal(mq_handle a, mq_handle b) {
    return a.index == b.index && a.generation == b.generation;
}

typedef struct mq_handle_table mq_handle_table;

/* 创建/销毁。elem_size 为每槽元素字节数；elem_align 为对齐（<=16）。 */
mq_handle_table* mq_handle_table_create(size_t elem_size, size_t elem_align);
void             mq_handle_table_destroy(mq_handle_table* table);

/* 分配一个槽：返回句柄，并（可选）输出指向槽存储的指针（未初始化、已清零）。
 * 存储由表拥有；其生命周期管理（构造/析构）由调用方负责。 */
mq_handle mq_handle_table_alloc(mq_handle_table* table, void** out_storage);

/* 释放句柄（世代自增）。成功 0，句柄无效 1。 */
int mq_handle_table_free(mq_handle_table* table, mq_handle h);

/* 由句柄取槽存储；无效（已释放/越界/世代不符）返回 NULL。 */
void* mq_handle_table_get(mq_handle_table* table, mq_handle h);

/* 句柄当前是否有效。 */
int mq_handle_table_valid(mq_handle_table* table, mq_handle h);

/* 存活槽数量。 */
uint32_t mq_handle_table_count(mq_handle_table* table);

/* 能力上限（当前槽容量，含已释放）。用于遍历 index ∈ [0,capacity)。 */
uint32_t mq_handle_table_capacity(mq_handle_table* table);

/* 取某 index 当前句柄（若存活）；否则返回 MQ_HANDLE_INVALID。用于遍历。 */
mq_handle mq_handle_table_handle_at(mq_handle_table* table, uint32_t index);

#ifdef __cplusplus
}
#endif
