/* maque 二进制序列化 + 世界快照（docs/10 §4/§5、docs/08）。
 *
 * 为什么需要：JSON 是开发期文本格式（可 diff），但**回滚/崩溃复现/热重载迁移**
 * 需要"性能优先"的**逐位可逆**快照：反射驱动的二进制 codec + 预分配快照池。
 *
 * 规则（docs/10）：
 *   D5 零初始化、**按字段**序列化（绝不含 padding 的裸 memcmp）；
 *   容器带长度前缀；浮点按 IEEE-754 原样；无墙钟/无地址。
 * 快照语义：`encode → decode` 必须逐位一致（往返测试），同构建同平台（L1）确定。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/persist/mq_persist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 反射驱动的二进制 codec ---- */

/* 把 type 的实例 obj 编码为字节流；返回写入长度（0 = 失败/不支持）。 */
size_t mq_persist_encode(mq_reflect_ctx* ctx, const mq_type_desc* type, const void* obj,
                         void* out, size_t out_cap);

/* 从字节流解码进 out（obj 必须已构造/零初始化）。成功返回消耗字节数，失败 <0。 */
long mq_persist_decode(mq_reflect_ctx* ctx, const mq_type_desc* type, const void* in,
                       size_t in_size, void* out);

/* ---- 快照池 ---- */

typedef struct mq_snapshot mq_snapshot;

/* 快照池：预分配 arena，避免运行期抖动（docs/07 §8、docs/10 §4）。 */
typedef struct mq_snapshot_pool mq_snapshot_pool;

mq_snapshot_pool* mq_snapshot_pool_create(uint32_t count, size_t capacity_bytes);
void              mq_snapshot_pool_destroy(mq_snapshot_pool* pool);

/* 从池中取一个快照（循环复用；池满则复用最旧者）。 */
mq_snapshot* mq_snapshot_pool_acquire(mq_snapshot_pool* pool);

mq_snapshot* mq_snapshot_create(void);
void         mq_snapshot_destroy(mq_snapshot* s);

/* 写入/读取（缓冲复用：容量足够则不重新分配）。 */
void* mq_snapshot_begin(mq_snapshot* s, size_t want);
void  mq_snapshot_commit(mq_snapshot* s, size_t used);

const void* mq_snapshot_bytes(mq_snapshot* s, size_t* out_size);

/* 元数据：帧号 + 描述（用于崩溃上下文/分歧定位，docs/10 §7）。 */
void        mq_snapshot_set_frame(mq_snapshot* s, uint64_t frame);
uint64_t    mq_snapshot_frame(mq_snapshot* s);
void        mq_snapshot_set_desc(mq_snapshot* s, const char* desc);
const char* mq_snapshot_desc(mq_snapshot* s);

/* 拷贝（rollback 前的保护点）。 */
mq_snapshot* mq_snapshot_clone(mq_snapshot* s);

/* 内容校验和（快照 → 校验和；用于"快照往返逐位一致"与失步定位）。 */
uint64_t mq_snapshot_checksum(mq_snapshot* s);

#ifdef __cplusplus
}
#endif
