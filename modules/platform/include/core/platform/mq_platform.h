/* maque 平台层（SDL2，docs/05/19）：窗口 + 输入事件。
 * 无显示环境下自动使用 SDL dummy 视频驱动（headless 安全）。 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_platform mq_platform;

/* 本帧输入状态快照。 */
typedef struct mq_input_state {
    int   quit;         /* 请求退出 */
    int   events;       /* 本帧事件数 */
    float mouse_x, mouse_y;
    int   mouse_down;   /* 左键按下 */
    int   key_space;
    int   key_escape;
} mq_input_state;

mq_platform* mq_platform_create(const char* title, int w, int h);
void         mq_platform_destroy(mq_platform* p);
void         mq_platform_poll(mq_platform* p, mq_input_state* out);

#ifdef __cplusplus
}
#endif
