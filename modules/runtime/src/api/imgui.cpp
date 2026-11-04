/* 宿主 API · 引擎侧 ImGui 面板（docs/28；视图层，不进快照）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

/* ---- ImGui 脚本 API ---- */
/* ImGui 脚本 API 实现（host 命名空间）。
 *
 * 分两类：
 *   ① **Engine 相关**（启用开关/帧计数/输入隔离）→ 读 Engine 成员；
 *   ② **纯控件** → 一行转发到 `mq_imgui_*` C ABI（模块边界干净：runtime 不 include imgui.h）。
 *
 * 所有函数都过同一道闸：未启用/未初始化时返回 0 或 ""，**脚本侧无需防御**
 *（面板是可选开发工具，关掉后脚本里的 imgui_* 调用变成无害空操作）。
 */

/* 统一前置：面板未启用 → 不调 ImGui（避免 ImGui 状态机被半初始化）。
 * 返回 null 表示"不要把调用透给模块"。 */
static Engine* imgui_ctx(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->imgui_on) return nullptr;
    if (!eng->imgui_inited) eng->imgui_setup();
    return eng->imgui_inited ? eng : nullptr;
}

float imgui_ready(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_inited) ? 1.0F : 0.0F;
}

float imgui_enabled(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on) ? 1.0F : 0.0F;
}

float imgui_set_enabled(void* ctx, float on) {
    auto* eng = (Engine*)ctx;
    if (!eng) return 0.0F;
    eng->imgui_enable(on != 0.0F);          // 与引擎级 F2 热键同一条实现
    return eng->imgui_on ? 1.0F : 0.0F;
}

float imgui_begin(void* ctx, const char* title) {
    Engine* eng = imgui_ctx(ctx);
    return eng ? (float)mq_imgui_begin(title) : 0.0F;
}

void imgui_end(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end(); }

float imgui_begin_closed(void* ctx, const char* title, float x, float y, float w, float h) {
    Engine* eng = imgui_ctx(ctx);
    return eng ? (float)mq_imgui_begin_at(title, x, y, w, h) : 0.0F;
}

void imgui_set_pos(void* ctx, float x, float y) { if (imgui_ctx(ctx)) mq_imgui_set_pos(x, y); }

void imgui_set_size(void* ctx, float w, float h) { if (imgui_ctx(ctx)) mq_imgui_set_size(w, h); }

void imgui_same_line(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_same_line(); }

void imgui_separator(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_separator(); }

void imgui_spacing(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_spacing(); }

void imgui_new_line(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_new_line(); }

void imgui_begin_disabled(void* ctx, float disabled) { if (imgui_ctx(ctx)) mq_imgui_begin_disabled(disabled != 0.0F); }

void imgui_end_disabled(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_disabled(); }

float imgui_begin_child(void* ctx, const char* id, float w, float h, float border) {
    Engine* eng = imgui_ctx(ctx);
    return eng ? (float)mq_imgui_begin_child(id, w, h, border != 0.0F) : 0.0F;
}

void imgui_end_child(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_child(); }

float imgui_begin_menu_bar(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_menu_bar() : 0.0F; }

void imgui_end_menu_bar(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_menu_bar(); }

float imgui_begin_menu(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_menu(label) : 0.0F;
}

void imgui_end_menu(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_menu(); }

float imgui_menu_item(void* ctx, const char* label, float selected, float enabled) {
    Engine* e = imgui_ctx(ctx);
    return e ? (float)mq_imgui_menu_item(label, selected != 0.0F, enabled != 0.0F) : 0.0F;
}

float imgui_begin_tab_bar(void* ctx, const char* id) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_tab_bar(id) : 0.0F;
}

void imgui_end_tab_bar(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_tab_bar(); }

float imgui_begin_tab_item(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_tab_item(label) : 0.0F;
}

void imgui_end_tab_item(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_tab_item(); }

float imgui_begin_table(void* ctx, const char* id, float cols) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_table(id, (int)cols) : 0.0F;
}

void imgui_end_table(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_table(); }

void imgui_table_next_row(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_table_next_row(); }

void imgui_table_next_column(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_table_next_column(); }

void imgui_table_headers_row(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_table_headers_row(); }

void imgui_text(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_text(text); }

void imgui_text_wrapped(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_text_wrapped(text); }

void imgui_text_disabled(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_text_disabled(text); }

void imgui_text_colored(void* ctx, const char* text, float r, float g, float b, float a) {
    if (imgui_ctx(ctx)) mq_imgui_text_colored(text, r, g, b, a);
}

void imgui_label_text(void* ctx, const char* label, const char* value) {
    if (imgui_ctx(ctx)) mq_imgui_label_text(label, value);
}

void imgui_bullet_text(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_bullet_text(text); }

float imgui_button(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_button(label) : 0.0F;
}

float imgui_small_button(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_small_button(label) : 0.0F;
}

float imgui_invisible_button(void* ctx, const char* id, float w, float h) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_invisible_button(id, w, h) : 0.0F;
}

float imgui_arrow_button(void* ctx, const char* id, float dir) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_arrow_button(id, (int)dir) : 0.0F;
}

float imgui_checkbox(void* ctx, const char* label, float value) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_checkbox(label, value) : value;
}

float imgui_radio_button(void* ctx, const char* label, float active) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_radio_button(label, active != 0.0F) : 0.0F;
}

float imgui_slider_float(void* ctx, const char* label, float value, float min, float max) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_slider_float(label, value, min, max) : value;
}

float imgui_slider_int(void* ctx, const char* label, float value, float min, float max) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_slider_int(label, value, min, max) : value;
}

float imgui_drag_float(void* ctx, const char* label, float value, float speed) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_drag_float(label, value, speed) : value;
}

float imgui_drag_int(void* ctx, const char* label, float value, float speed) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_drag_int(label, value, speed) : value;
}

float imgui_input_float(void* ctx, const char* label, float value) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_input_float(label, value) : value;
}

float imgui_input_int(void* ctx, const char* label, float value) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_input_int(label, value) : value;
}

const char* imgui_input_text(void* ctx, const char* label, const char* text) {
    Engine* e = imgui_ctx(ctx);
    return e ? mq_imgui_input_text(label, text) : (text ? text : "");
}

float imgui_combo(void* ctx, const char* label, float current, const char* items) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_combo(label, current, items) : current;
}

float imgui_list_box(void* ctx, const char* label, float current, const char* items) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_list_box(label, current, items) : current;
}

float imgui_selectable(void* ctx, const char* label, float selected) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_selectable(label, selected != 0.0F) : 0.0F;
}

void imgui_progress_bar(void* ctx, float fraction, float w, float h, const char* overlay) {
    if (imgui_ctx(ctx)) mq_imgui_progress_bar(fraction, w, h, overlay);
}

void imgui_plot_value(void* ctx, const char* id, float value) {
    if (imgui_ctx(ctx)) mq_imgui_plot_value(id, value);
}

float imgui_color_edit(void* ctx, const char* label, float packed) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_color_edit(label, packed) : packed;
}

float imgui_tree_node(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_tree_node(label) : 0.0F;
}

void imgui_tree_pop(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_tree_pop(); }

float imgui_collapsing_header(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_collapsing_header(label) : 0.0F;
}

void imgui_set_next_item_width(void* ctx, float w) { if (imgui_ctx(ctx)) mq_imgui_set_next_item_width(w); }

void imgui_push_item_width(void* ctx, float w) { if (imgui_ctx(ctx)) mq_imgui_push_item_width(w); }

void imgui_pop_item_width(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_pop_item_width(); }

void imgui_push_id(void* ctx, const char* id) { if (imgui_ctx(ctx)) mq_imgui_push_id(id); }

void imgui_pop_id(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_pop_id(); }

void imgui_tooltip(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_tooltip(text); }

float imgui_is_item_hovered(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_is_item_hovered() : 0.0F; }

float imgui_is_item_active(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_is_item_active() : 0.0F; }

float imgui_is_window_hovered(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_is_window_hovered() : 0.0F; }

void imgui_set_item_default(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_set_item_default(); }

float imgui_show_demo(void* ctx) {
    Engine* eng = imgui_ctx(ctx);
    if (!eng) return 0.0F;
    int open = eng->imgui_demo ? 1 : 0;
    mq_imgui_show_demo(&open);
    eng->imgui_demo = open != 0;
    return eng->imgui_demo ? 1.0F : 0.0F;
}

float imgui_want_mouse(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on && eng->imgui_inited) ? (float)mq_imgui_want_mouse() : 0.0F;
}

float imgui_want_keyboard(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on && eng->imgui_inited) ? (float)mq_imgui_want_keyboard() : 0.0F;
}

float imgui_hovered(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on && eng->imgui_inited) ? (float)mq_imgui_hovered() : 0.0F;
}

float imgui_frame(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng ? (float)eng->imgui_frames : 0.0F;
}

float imgui_last_cmd_count(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng ? (float)eng->imgui_last_cmds : 0.0F;
}

}  // namespace host
