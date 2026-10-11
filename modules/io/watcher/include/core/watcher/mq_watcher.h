/* maque 资源热重载监视（docs/08 §热重载、docs/14）：**只在帧边界检查**。
 *
 * 设计约束：
 *   · 检查不是模拟的一部分（不读墙钟参与判定）——只在帧边界把"文件是否变了"作为
 *     外部输入喂给引擎；热重载本身由宿主在帧边界执行（docs/10 D7 帧内无异步）。
 *   · 不做线程/回调；宿主轮询，行为可预期、可测试（可注入式替换 mtime）。
 *   · per-instance，无全局状态。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_watcher mq_watcher;

typedef enum mq_watch_kind {
    MQ_WATCH_MODIFIED = 0,   /* 内容/时间戳变化 */
    MQ_WATCH_REMOVED  = 1,   /* 文件消失 */
    MQ_WATCH_ADDED    = 2    /* 之前缺失，现在出现 */
} mq_watch_kind;

/* 变化回调（帧边界调用；返回非 0 停止遍历）。 */
typedef int (*mq_watch_fn)(void* user, const char* path, mq_watch_kind kind);

mq_watcher* mq_watcher_create(void);
void        mq_watcher_destroy(mq_watcher* w);

/* 监视一个文件（首次调用即记录基线时间戳）。重复路径返回已存在的下标。 */
int  mq_watcher_add(mq_watcher* w, const char* path);
int  mq_watcher_remove(mq_watcher* w, const char* path);
uint32_t mq_watcher_count(mq_watcher* w);
const char* mq_watcher_path_at(mq_watcher* w, uint32_t index);

/* 检查全部目标，对每个变化调用 fn。返回变化数。 */
uint32_t mq_watcher_poll(mq_watcher* w, mq_watch_fn fn, void* user);

/* 测试/注入：直接设置某路径的时间戳基线（避免依赖真实文件系统与时钟）。 */
int  mq_watcher_set_stamp(mq_watcher* w, const char* path, uint64_t stamp);
uint64_t mq_watcher_get_stamp(mq_watcher* w, const char* path);

/* 查询文件当前时间戳（size + mtime 混合；缺失返回 0）。 */
uint64_t mq_watcher_stat(const char* path);

#ifdef __cplusplus
}
#endif
