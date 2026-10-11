/* maque 嵌入宿主接口（P3，docs/05 §平台抽象）：**主循环的所有权交给宿主**。
 *
 * 为什么需要：浏览器/WASM、嵌入式和自定义宿主都不允许引擎"霸占主循环"——
 * 宿主按自己的节拍推进引擎（requestAnimationFrame / 设备定时器 / 外部事件）。
 * 因此引擎暴露"创建 → 每帧 tick → 销毁"三个纯 C 函数：
 *
 *   native(桌面) ：宿主可以是 SDL 事件循环（见 samples/player）；
 *   WASM(浏览器)：宿主是 JS（emscripten_set_main_loop 调用 mq_host_tick）；
 *   嵌入式      ：宿主是 RTOS 任务或裸机节拍。
 *
 * 设计约束（与 docs/05/10 一致）：
 *   · 无全局可变状态：所有状态在 mq_host_scene 实例里 → 可多实例并存（ADR-0014）；
 *   · 固定步长在引擎内部，宿主只提供"该推进几帧"的节拍；
 *   · 输入经 mq_host_push_* 注入，与设备事件走同一确定性事件流（docs/19 §9）；
 *   · 每帧输出在宿主提供的 RGBA 缓冲里（无需 GPU 上下文即可显示帧）。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_host_scene mq_host_scene;   /* 不透明：引擎分配/释放 */

/* 注入指针事件的类型（与引擎语义事件一致，docs/19 §2；不要求宿主包含 input 头）。 */
enum {
    MQ_HOST_POINTER_DOWN   = 0,
    MQ_HOST_POINTER_UP     = 1,
    MQ_HOST_POINTER_MOVE   = 2,
    MQ_HOST_POINTER_WHEEL  = 3,
    MQ_HOST_POINTER_CANCEL = 4
};

typedef struct mq_host_desc {
    uint32_t struct_size;
    int32_t  width;          /* 帧缓冲宽（0 = 用场景声明） */
    int32_t  height;         /* 帧缓冲高（0 = 用场景声明） */
    const char* renderer;    /* 渲染插件路径；NULL = 平台默认（WASM 下为内置软渲染） */
    uint32_t flags;          /* 预留：bit0 无输入、bit1 不渲染 */
} mq_host_desc;

/* 创建/加载场景（.scn 经 VFS 解析）。失败返回 NULL。 */
mq_host_scene* mq_host_scene_create(const char* scn_path, const mq_host_desc* desc);
void           mq_host_scene_destroy(mq_host_scene* s);

/* 推进一帧（或 dt 对应帧数）；把 RGBA8 输出写入 out（out_size 必须 >= W*H*4）。
 * 返回 0 正常；非 0 表示渲染失败。宿主决定调用频率（浏览器 = rAF）。 */
int mq_host_tick(mq_host_scene* s, uint8_t* out, size_t out_size);

/* 帧缓冲尺寸（供宿主分配缓冲；可能与 desc 不同，例如按场景声明）。 */
void mq_host_size(mq_host_scene* s, int* w, int* h);

/* 输入注入（与设备事件同队列；确定性事件流，docs/19 §9）。 */
void mq_host_push_pointer(mq_host_scene* s, int type, int button, float x, float y);
void mq_host_push_key(mq_host_scene* s, int key, int down, const char* text);

/* 状态查询（宿主/AI 观察）。 */
uint64_t mq_host_frame(mq_host_scene* s);
uint64_t mq_host_checksum(mq_host_scene* s);
int      mq_host_should_quit(mq_host_scene* s);

/* 控制面请求（JSON-RPC 文本 → 响应文本；宿主可把它接到 WS/HTTP/PostMessage）。 */
const char* mq_host_control(mq_host_scene* s, const char* request);

#ifdef __cplusplus
}
#endif
