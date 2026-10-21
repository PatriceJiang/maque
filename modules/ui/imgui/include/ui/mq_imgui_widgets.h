/* maque::imgui 的**控件 C ABI**（与 mq_imgui.h 的帧管理部分配套）。
 *
 * 为什么控件也走 C ABI：runtime 只依赖本模块的 **C ABI**，不 include ImGui 头
 *（跨模块只暴露纯 C 接口，docs/02）。控件状态（文本输入缓冲、plot 历史）留在这里，
 * 所以这些函数**无状态参数**、不需要 Engine 指针。
 *
 * 约定（与 ImGui 即时模式一致）：
 *   · `mq_imgui_begin` 返回 1 = 窗口可见，此时才该调控件；**end 必须无条件调用**。
 *   · 值类控件（slider/drag/input/checkbox）**返回新值**，脚本不需要处理指针。
 */
#ifndef MAQUE_UI_IMGUI_WIDGETS_H
#define MAQUE_UI_IMGUI_WIDGETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 窗口 / 布局 ---- */
int  mq_imgui_begin(const char *title);
int  mq_imgui_begin_at(const char *title, float x, float y, float w, float h);
void mq_imgui_end(void);
void mq_imgui_set_pos(float x, float y);
void mq_imgui_set_size(float w, float h);
void mq_imgui_same_line(void);
void mq_imgui_separator(void);
void mq_imgui_spacing(void);
void mq_imgui_new_line(void);
void mq_imgui_begin_disabled(int disabled);
void mq_imgui_end_disabled(void);
int  mq_imgui_begin_child(const char *id, float w, float h, int border);
void mq_imgui_end_child(void);
void mq_imgui_set_next_item_width(float w);
void mq_imgui_push_item_width(float w);
void mq_imgui_pop_item_width(void);
void mq_imgui_push_id(const char *id);
void mq_imgui_pop_id(void);
void mq_imgui_tooltip(const char *text);
int  mq_imgui_is_item_hovered(void);
int  mq_imgui_is_item_active(void);
int  mq_imgui_is_window_hovered(void);
void mq_imgui_set_item_default(void);

/* ---- 菜单 / 标签页 / 表格 ---- */
int  mq_imgui_begin_menu_bar(void);
void mq_imgui_end_menu_bar(void);
int  mq_imgui_begin_menu(const char *label);
void mq_imgui_end_menu(void);
int  mq_imgui_menu_item(const char *label, int selected, int enabled);
int  mq_imgui_begin_tab_bar(const char *id);
void mq_imgui_end_tab_bar(void);
int  mq_imgui_begin_tab_item(const char *label);
void mq_imgui_end_tab_item(void);
int  mq_imgui_begin_table(const char *id, int cols);
void mq_imgui_end_table(void);
void mq_imgui_table_next_row(void);
void mq_imgui_table_next_column(void);
void mq_imgui_table_headers_row(void);

/* ---- 文本 ---- */
void mq_imgui_text(const char *text);
void mq_imgui_text_wrapped(const char *text);
void mq_imgui_text_disabled(const char *text);
void mq_imgui_text_colored(const char *text, float r, float g, float b, float a);
void mq_imgui_label_text(const char *label, const char *value);
void mq_imgui_bullet_text(const char *text);

/* ---- 基础控件 ---- */
int   mq_imgui_button(const char *label);
int   mq_imgui_small_button(const char *label);
int   mq_imgui_invisible_button(const char *id, float w, float h);
int   mq_imgui_arrow_button(const char *id, int dir);
float mq_imgui_checkbox(const char *label, float value);
int   mq_imgui_radio_button(const char *label, int active);
float mq_imgui_slider_float(const char *label, float value, float min, float max);
float mq_imgui_slider_int(const char *label, float value, float min, float max);
float mq_imgui_drag_float(const char *label, float value, float speed);
float mq_imgui_drag_int(const char *label, float value, float speed);
float mq_imgui_input_float(const char *label, float value);
float mq_imgui_input_int(const char *label, float value);
/* 返回**当前文本**：模块内部按 label 维持缓冲（脚本不必自己存）。 */
const char *mq_imgui_input_text(const char *label, const char *text);
float mq_imgui_combo(const char *label, float current, const char *items_csv);
float mq_imgui_list_box(const char *label, float current, const char *items_csv);
int   mq_imgui_selectable(const char *label, int selected);
void  mq_imgui_progress_bar(float fraction, float w, float h, const char *overlay);
/* 把值推进该 id 的环形历史并画折线（脚本不必自己存数组）。 */
void  mq_imgui_plot_value(const char *id, float value);
float mq_imgui_color_edit(const char *label, float packed);
int   mq_imgui_tree_node(const char *label);
void  mq_imgui_tree_pop(void);
int   mq_imgui_collapsing_header(const char *label);
void  mq_imgui_show_demo(int *open);

/* 局部字号：`mq_imgui_push_font_size(px)` 之后绘制的内容用该像素字号，`pop` 恢复。
 * 用途：默认 stats 面板要**比游戏自己的面板小一号**（ImGui 1.93 没有 SetWindowFontScale，
 * 官方替代是 `PushFont(NULL, size)`）。
 * 注：size 是**设计像素**（与 ImGui 显示空间一致）；传 <=0 表示沿用当前字号。 */
/* 覆盖层窗口：带 NoInputs（**不吃鼠标/键盘**）+ 可拖动标题栏。
 * 用于引擎默认 stats 面板 —— 默认开启时不能挡住游戏的点击/命中测试。 */
int   mq_imgui_begin_overlay(const char *title);
void  mq_imgui_push_font_size(float size_px);
void  mq_imgui_pop_font(void);

#ifdef __cplusplus
}
#endif

#endif /* MAQUE_UI_IMGUI_WIDGETS_H */
