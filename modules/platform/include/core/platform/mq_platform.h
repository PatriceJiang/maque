/* maque 平台层（SDL2，docs/05/19）：窗口 + 设备输入采集。
 * 职责边界：**只做设备原始事件的归一化采集**，不做命中测试/分发/录制
 *（那些属于 input 模块与模拟帧处理点，见 docs/19 §3）。
 * 无显示环境下自动使用 SDL dummy 视频驱动（headless 安全）。 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_platform mq_platform;

/* 归一化指针设备类型（docs/19 §2）。 */
typedef enum mq_platform_pointer_type {
    MQ_PLATFORM_POINTER_MOUSE = 0,
    MQ_PLATFORM_POINTER_TOUCH = 1,
    MQ_PLATFORM_POINTER_PEN   = 2
} mq_platform_pointer_type;

/* 归一化指针事件类型（docs/19 §2）。 */
typedef enum mq_platform_pointer_event_type {
    MQ_PLATFORM_POINTER_DOWN   = 0,
    MQ_PLATFORM_POINTER_UP     = 1,
    MQ_PLATFORM_POINTER_MOVE   = 2,
    MQ_PLATFORM_POINTER_WHEEL  = 3,
    MQ_PLATFORM_POINTER_CANCEL = 4
} mq_platform_pointer_event_type;

/* 修饰键位标志（docs/19 §2）。 */
#define MQ_PLATFORM_MOD_SHIFT (1u << 0)
#define MQ_PLATFORM_MOD_CTRL  (1u << 1)
#define MQ_PLATFORM_MOD_ALT   (1u << 2)
#define MQ_PLATFORM_MOD_META  (1u << 3)

/* 单条归一化指针事件（POD，固定布局）。 */
typedef struct mq_platform_pointer_event {
    uint32_t pointer_id;    /* 鼠标恒 0；触摸每指唯一 */
    uint32_t type;          /* mq_platform_pointer_event_type */
    uint32_t pointer_type;  /* mq_platform_pointer_type */
    uint32_t button;        /* 0=左 1=右 2=中 */
    float    x, y;          /* 窗口/屏幕坐标（y 向下） */
    float    dx, dy;        /* 相对上一事件的位移 */
    float    pressure;      /* 压感 [0,1]，鼠标按下恒 1 */
    float    scroll_x, scroll_y;   /* Wheel 专用 */
    uint32_t modifiers;     /* MQ_PLATFORM_MOD_* */
} mq_platform_pointer_event;

/* 单条归一化键盘/文本事件（POD）。 */
typedef struct mq_platform_key_event {
    uint32_t key;           /* 归一化键码（见下） */
    uint32_t scancode;      /* 物理键位 */
    uint32_t modifiers;
    uint32_t repeat;        /* 系统按键重复标志 */
    uint32_t down;          /* 1=按下 0=弹起（MQ_KEY_TEXT 时恒 1） */
    char     text[16];      /* TextInput 时的 UTF-8 文本（无则空） */
} mq_platform_key_event;

/* 归一化键码（避免宿主依赖 SDL 常量）。 */
enum {
    MQ_KEY_UNKNOWN = 0,
    MQ_KEY_ESCAPE  = 1,
    MQ_KEY_SPACE   = 2,
    MQ_KEY_ENTER   = 3,
    MQ_KEY_TAB     = 4,
    MQ_KEY_BACKSPACE = 5,
    MQ_KEY_LEFT    = 6,
    MQ_KEY_RIGHT   = 7,
    MQ_KEY_UP      = 8,
    MQ_KEY_DOWN    = 9,
    MQ_KEY_TEXT    = 10,   /* TextInput 专用（text 字段有效） */
    MQ_KEY_A       = 32    /* 可打印键：ASCII 码值 */
};

/* 一帧的输入快照。事件按到达顺序写入宿主缓冲（顺序确定，docs/19 §9）。 */
typedef struct mq_input_frame {
    int   quit;             /* 请求退出（窗口关闭） */
    int   focused;          /* 窗口是否聚焦 */
    float mouse_x, mouse_y; /* 当前指针位置 */
    int   mouse_down;       /* 左键当前按住 */

    mq_platform_pointer_event* pointers;
    uint32_t                   pointer_count;
    uint32_t                   pointer_cap;

    mq_platform_key_event*     keys;
    uint32_t                   key_count;
    uint32_t                   key_cap;

    int   resized;          /* 本帧发生 resize */
    int   width, height;    /* 当前帧缓冲尺寸 */
} mq_input_frame;

/* 创建窗口；w/h <= 0 或 headless 环境下使用 dummy（无窗口）后端。 */
mq_platform* mq_platform_create(const char* title, int w, int h);
void         mq_platform_destroy(mq_platform* p);

/* 平台是否使用真实窗口（0 = dummy/headless）。 */
int  mq_platform_has_window(mq_platform* p);

/* 累积设备事件到内部队列（可每帧调用；事件在 poll 时统一消费，docs/19 §3）。 */
void mq_platform_pump(mq_platform* p);

/* 取本帧输入帧。宿主预分配事件缓冲（pointers/keys 及容量须已设置）；
 * 返回消费的事件总数。 */
uint32_t mq_platform_poll(mq_platform* p, mq_input_frame* out);

/* 向平台注入合成事件（AI/CI/回放注入，docs/11/19 §9）：不依赖真实设备。 */
void mq_platform_inject_pointer(mq_platform* p, const mq_platform_pointer_event* ev);
void mq_platform_inject_key(mq_platform* p, const mq_platform_key_event* ev);

/* 设置窗口标题 / 请求退出。 */
void mq_platform_set_title(mq_platform* p, const char* title);
void mq_platform_request_quit(mq_platform* p);

#ifdef __cplusplus
}
#endif
