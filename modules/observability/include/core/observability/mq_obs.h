/* maque 可观测性（docs/09，A2）：结构化事件 + 指标。
 * per-instance（无全局）；供 AI/CI/控制平面查询。 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_obs_ctx mq_obs_ctx;

mq_obs_ctx* mq_obs_create(void);
void        mq_obs_destroy(mq_obs_ctx* ctx);

/* 结构化事件（进入环形缓冲；JSON Lines 输出到 stdout）。level: 0=debug 1=info 2=warn 3=error */
void mq_obs_event(mq_obs_ctx* ctx, const char* domain, int level, const char* event);

/* 指标：计数器与瞬时值。 */
void mq_obs_counter_add(mq_obs_ctx* ctx, const char* name, double delta);
void mq_obs_gauge_set(mq_obs_ctx* ctx, const char* name, double value);
double mq_obs_gauge_get(mq_obs_ctx* ctx, const char* name);

/* 导出为 JSON（指标 + 最近事件）。返回的缓冲归 ctx 所有，下次调用前有效。 */
const char* mq_obs_dump_json(mq_obs_ctx* ctx);

#ifdef __cplusplus
}
#endif
