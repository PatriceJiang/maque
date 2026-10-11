/* maque 输入系统（docs/19，M5）：设备原始事件 → 确定性事件流 + 命中分发。
 *
 * 职责（与 platform 的边界）：
 *   platform 只做**设备归一化采集**；本模块负责：
 *     ① 每帧固定处理点消费事件（顺序确定，docs/19 §3/§9）；
 *     ② 指针/键盘**状态**（按住、悬停、焦点、指针捕获）；
 *     ③ **输入即数据**：录制/回放/注入同一份记录（docs/10 §3）；
 *     ④ DOM 式**捕获→目标→冒泡**分发（docs/19 §5）与命中测试（§4）。
 *
 * per-instance，无全局可变状态；不含墙钟时间（采样时间戳不参与模拟判定）。
 * 事件记录格式：自描述小端二进制（帧号 + 事件），跨平台稳定，可嵌入快照。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/platform/mq_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_input mq_input;

/* 事件序号：记录里用帧号+序号定位，回放时按序喂入。 */
#define MQ_INPUT_LOG_VERSION 1u

/* 分发阶段（docs/19 §5）。 */
typedef enum mq_input_phase {
    MQ_INPUT_PHASE_CAPTURE = 0,
    MQ_INPUT_PHASE_TARGET  = 1,
    MQ_INPUT_PHASE_BUBBLE  = 2
} mq_input_phase;

/* 语义事件类型（分发用；比原始指针事件更贴近节点）。 */
typedef enum mq_input_event_type {
    MQ_INPUT_POINTER_DOWN = 0,
    MQ_INPUT_POINTER_UP   = 1,
    MQ_INPUT_POINTER_MOVE = 2,
    MQ_INPUT_POINTER_WHEEL = 3,
    MQ_INPUT_POINTER_ENTER = 4,
    MQ_INPUT_POINTER_LEAVE = 5,
    MQ_INPUT_CLICK        = 6,
    MQ_INPUT_DOUBLE_CLICK = 7,
    MQ_INPUT_KEY_DOWN     = 8,
    MQ_INPUT_KEY_UP       = 9,
    MQ_INPUT_TEXT         = 10
} mq_input_event_type;

/* 命中的节点信息（UI 场景下由引擎命中测试填充）。 */
typedef struct mq_input_target {
    int      entity;        /* 实体索引（-1 = 无/全局） */
    uint32_t handle;        /* 实体世代句柄索引（脚本可见 id） */
    int      depth;         /* 树深（用于 UI z-order + 树深稳定排序） */
    int      z;             /* UI z-order */
    float    local_x, local_y;   /* 目标本地坐标 */
    int      consumed;      /* 已被消费（阻止默认行为） */
    int      stopped;       /* 停止传播 */
} mq_input_target;

/* 语义事件（分发载荷）。 */
typedef struct mq_input_event {
    uint32_t type;          /* mq_input_event_type */
    uint32_t phase;         /* mq_input_phase */
    uint32_t pointer_id;
    uint32_t pointer_type;  /* mq_platform_pointer_type */
    uint32_t button;
    uint32_t modifiers;
    uint32_t key;           /* 键码（键盘事件） */
    char     text[16];      /* 文本输入（UTF-8） */
    float    x, y;          /* 屏幕坐标 */
    float    dx, dy;        /* 位移 */
    float    scroll_x, scroll_y;
    float    pressure;
    int      target;        /* 命中的实体索引（-1 = 全局） */
} mq_input_event;

/* 事件处理器：返回非 0 表示已消费（stop_propagation 语义由 target->stopped 表达）。 */
typedef int (*mq_input_handler)(void* user, const mq_input_event* ev, mq_input_target* target);

/* 命中测试回调：引擎提供（节点树/UI 层），返回命中的实体索引（-1 = 未命中）。
 * 必须**稳定排序**（z-order + 树深；禁地址依赖，docs/19 §9）。 */
typedef int (*mq_input_hit_test)(void* user, const mq_input_event* ev, mq_input_target* out);

/* 父节点查询：返回 entity 的父实体（-1 = 根/无），用于捕获与冒泡路径（docs/19 §5）。 */
typedef int (*mq_input_parent_fn)(void* user, int entity);

mq_input* mq_input_create(void);
void      mq_input_destroy(mq_input* in);

/* ---- 帧处理点（固定步长内调用；顺序不可变） ---- */

/* 把平台帧并入本帧事件流（保持到达顺序）。 */
void mq_input_begin_frame(mq_input* in, const mq_input_frame* frame, uint64_t frame_index);

/* 设置命中测试与分发根。 */
void mq_input_set_hit_test(mq_input* in, mq_input_hit_test fn, void* user);
/* 设置父节点查询（捕获/冒泡路径；不设置则只在目标阶段分发）。 */
void mq_input_set_parent_fn(mq_input* in, mq_input_parent_fn fn, void* user);

/* 注册处理器（按注册顺序调用；UI 层优先）。 */
void mq_input_add_handler(mq_input* in, mq_input_handler fn, void* user);

/* 分发本帧全部事件（命中测试 → 捕获/目标/冒泡）。返回处理的事件数。 */
uint32_t mq_input_dispatch(mq_input* in);

/* 帧结束：清理瞬时状态（click 判定、焦点变更），记录本帧日志。 */
void mq_input_end_frame(mq_input* in, uint64_t frame_index);

/* ---- 状态查询（模拟可读，供脚本/组件） ---- */
int   mq_input_quit(mq_input* in);
void  mq_input_pointer_pos(mq_input* in, uint32_t pointer_id, float* x, float* y);
int   mq_input_pointer_down(mq_input* in, uint32_t pointer_id);
int   mq_input_key_down(mq_input* in, uint32_t key);
int   mq_input_focus(mq_input* in);                    /* 键盘焦点实体（-1 = 无） */
void  mq_input_set_focus(mq_input* in, int entity);    /* Tab/点击设置焦点 */
void  mq_input_set_capture(mq_input* in, uint32_t pointer_id, int entity);  /* 指针捕获（拖拽） */
int   mq_input_get_capture(mq_input* in, uint32_t pointer_id);
void  mq_input_release_capture(mq_input* in, uint32_t pointer_id);
int   mq_input_frame_index(mq_input* in);

/* ---- 输入即数据：录制 / 回放 / 注入（docs/10 §3、docs/11、docs/19 §9） ---- */

void mq_input_record_begin(mq_input* in);
void mq_input_record_end(mq_input* in);
int  mq_input_recording(mq_input* in);

/* 载入录制（回放模式）：此后每帧按记录喂入事件，忽略设备输入。 */
int  mq_input_replay_begin(mq_input* in, const void* log, size_t size);
void mq_input_replay_end(mq_input* in);
int  mq_input_replaying(mq_input* in);

/* 注入一条事件（AI/CI；进入同一事件流，记录在录制中）。 */
void mq_input_inject(mq_input* in, const mq_input_event* ev);

/* 取出录制/回放共用的日志（归 in 所有）。 */
const void* mq_input_log_bytes(mq_input* in, size_t* out_size);
/* 日志的 JSON 形态（供控制平面/AI 读写；归 in 所有）。 */
const char* mq_input_log_json(mq_input* in);
int         mq_input_log_load_json(mq_input* in, const char* json);

/* 日志元数据（键值对，随日志一起保存/读取）：如 frames=总帧数、trail=世界校验和。
 * 用于"回放多少帧"与**失步检测**（docs/10 §5：checksum 对比、报首个分歧帧）。 */
void        mq_input_log_set_meta(mq_input* in, const char* key, const char* value);
const char* mq_input_log_get_meta(mq_input* in, const char* key);

#ifdef __cplusplus
}
#endif
