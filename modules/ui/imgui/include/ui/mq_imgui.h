/* maque::imgui 的 C ABI（模块公开面，docs/02）。
 *
 * 职责边界（重要）：
 *   本模块负责 **ImGui 上下文 + 帧 + 布局 + 绘制数据导出**，
 *   **不碰 GPU、不上传纹理、不提交网格** —— 与 spine2d 同样的分层思路：
 *   纹理句柄由宿主（runtime）用 renderer 的 create_texture 创建，
 *   命令里的 texture 只是宿主传进来的不透明值；网格由宿主按 cmd 数组提交。
 *   这样模块不依赖 gfx/platform，跨模块只有纯 C ABI。
 *
 * 坐标与尺寸：
 *   · 所有顶点已是**渲染目标像素空间**（y 向下），与引擎网格图元同一空间，
 *     宿主可直接提交、不需要再换算；
 *   · 宿主通过 mq_imgui_begin_frame 给出"显示尺寸 + 目标→显示的映射"，
 *     本模块负责把 ImGui 的显示坐标映射到目标像素坐标。
 *
 * 调用时序（每帧）：
 *   1. 宿主把输入喂进来（mq_imgui_input_*）
 *   2. mq_imgui_begin_frame(...)                 —— 开始一帧，记录显示/目标映射
 *   3. 脚本/宿主调用控件（由 runtime 包装成脚本 API）
 *   4. mq_imgui_end_frame(...)                   —— 结束并**导出**绘制命令
 *   5. 宿主提交命令（纹理 + 网格），随后呈现
 */
#ifndef MAQUE_UI_IMGUI_H
#define MAQUE_UI_IMGUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 层号：引擎 UI 用 layer=1，ImGui 必须在**所有**引擎图层之上（docs/13 §pass 层）。
 * 取高位是刻意的：将来加中间层也不会不小心盖住调试面板。 */
#define MQ_IMGUI_LAYER 100

/* 单条绘制命令（对应 ImGui 的一个 ImDrawCmd：同纹理、同裁剪矩形的一段索引）。
 * 顶点/索引指向**本模块内部缓冲**，仅在本帧 end_frame 之后到下一帧 begin_frame 之前有效。 */
typedef struct mq_imgui_cmd {
    const float *positions;      /* 2 分量/顶点，渲染目标像素空间（y 向下） */
    const float *uvs;            /* 2 分量/顶点，归一化 */
    const float *colors;         /* 4 分量/顶点，RGBA 0..1（非预乘） */
    const uint32_t *indices;     /* 相对本命令的**局部**索引（已减 base_vertex） */
    uint32_t vertex_count;
    uint32_t index_count;
    uint64_t texture;            /* 宿主传入的不透明纹理句柄（0 = 用当前字体图集） */
    float clip_x, clip_y, clip_w, clip_h;   /* 裁剪矩形（目标像素空间；w/h<=0 表示不裁剪） */
} mq_imgui_cmd;

/* 一帧的绘制结果。cmds 指向内部缓冲，同上有效期。 */
typedef struct mq_imgui_frame {
    const mq_imgui_cmd *cmds;
    uint32_t cmd_count;
} mq_imgui_frame;

/* 初始化/关闭。font_size<=0 → 用 ImGui 默认字体。
 * pixel_scale>0 时按该倍率放大字体栅格（HiDPI：窗口比目标大时文字才不糊）。 */
int mq_imgui_init(float font_size, float pixel_scale);
void mq_imgui_shutdown(void);
int mq_imgui_ready(void);

/* 字体图集（宿主负责上传为纹理，再把句柄传回来）：
 * 始终以 **RGBA32** 提供，宿主不必处理单通道格式。 */
const uint8_t *mq_imgui_font_pixels(int *out_w, int *out_h);
void mq_imgui_set_font_texture(uint64_t handle);
uint64_t mq_imgui_font_texture(void);
/* 图集是否重建过（generation 变化 → 宿主需重新上传纹理）。返回当前代数。 */
uint32_t mq_imgui_font_generation(void);

/* 输入（窗口像素坐标）。宿主每帧喂入；未调用的输入保持上一帧状态。 */
void mq_imgui_input_pointer(float x, float y, int down, float wheel);
void mq_imgui_input_key(int key, int down);
void mq_imgui_input_text(const char *utf8);
void mq_imgui_input_focus(int focused);

/* 帧：
 *   show_w/show_h  —— ImGui 的显示尺寸（= 窗口像素，面板按窗口排版，UI 大小不受画布缩放影响）
 *   target_w/target_h —— 渲染目标尺寸（顶点最终落在哪个像素空间）
 *   scale/off_x/off_y —— 目标→显示的映射（与画布策略同一份：显示坐标 = 目标坐标*scale + off）
 *   dt —— 帧间隔（秒）
 *   dpi_scale —— 显示/目标 的比值（鼠标命中用；<=0 时按 scale 推） */
int mq_imgui_begin_frame(float show_w, float show_h, float target_w, float target_h,
                         float scale, float off_x, float off_y, float dt, float dpi_scale);
int mq_imgui_end_frame(mq_imgui_frame *out);

/* ImGui 是否想要鼠标/键盘（宿主据此**阻断世界输入**，避免"点面板同时打到游戏上"）。 */
int mq_imgui_want_mouse(void);
int mq_imgui_want_keyboard(void);
/* 指针是否落在 ImGui 的任意窗口/控件上（比 want_mouse 更严格，用于命中测试）。 */
int mq_imgui_hovered(void);

#ifdef __cplusplus
}
#endif

#endif /* MAQUE_UI_IMGUI_H */
