/* maque::imgui 控件实现（C ABI 见 include/ui/mq_imgui_widgets.h）。
 *
 * 状态说明：ImGui 是即时模式的，但有两类状态必须由我们维持：
 *   ① `input_text` 的编辑缓冲 —— 按 label 存一份（脚本不必自己管）；
 *   ② `plot_value` 的历史环形缓冲 —— 按 id 存一份（否则脚本要自己存数组）。
 * 两者都放在本模块，所以控件的 C ABI **不需要 Engine 指针**。
 */
#include "ui/mq_imgui_widgets.h"

#include "imgui.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/* input_text：label → 编辑缓冲（ImGui 需要稳定的 char* 直到输入结束） */
std::unordered_map<std::string, std::string> g_text;
/* plot_value：id → 环形历史 */
std::unordered_map<std::string, std::vector<float>> g_plots;
constexpr size_t kPlotMax = 120;

/* 逗号分隔串 → 静态 char* 数组（ImGui 需要 const char* const*，生命周期需覆盖调用） */
std::vector<std::string> g_items_store;
std::vector<const char *> g_items_ptr;

void split_csv(const char *csv) {
    g_items_store.clear();
    g_items_ptr.clear();
    if (!csv) return;
    std::string s(csv), cur;
    for (char c : s) {
        if (c == ',') { g_items_store.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    g_items_store.push_back(cur);
    for (auto &it : g_items_store) {
        /* 去掉前导空格（"a, b, c" 这种写法） */
        size_t i = 0;
        while (i < it.size() && it[i] == ' ') ++i;
        if (i) it.erase(0, i);
    }
    for (auto &it : g_items_store) g_items_ptr.push_back(it.c_str());
}

} // namespace

extern "C" {

/* ---- 窗口 / 布局 ---- */
int mq_imgui_begin(const char *title) {
    return ImGui::Begin(title && *title ? title : "panel") ? 1 : 0;
}

int mq_imgui_begin_at(const char *title, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    return ImGui::Begin(title && *title ? title : "panel") ? 1 : 0;
}

void mq_imgui_end(void) { ImGui::End(); }
/* 位置/尺寸用 **FirstUseEver**（不是 Always）：
 * 脚本每帧都会调这两个函数，若用 Always 就把窗口位置/尺寸**每帧强制写回**，
 * 用户拖动窗口会"弹回原位"、拉伸边框也无效（实测反馈）。
 * FirstUseEver = 只在某个窗口第一次出现时生效，之后尊重用户拖动/缩放。 */
void mq_imgui_set_pos(float x, float y) { ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver); }
void mq_imgui_set_size(float w, float h) { ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver); }
void mq_imgui_same_line(void) { ImGui::SameLine(); }
void mq_imgui_separator(void) { ImGui::Separator(); }
void mq_imgui_spacing(void) { ImGui::Spacing(); }
void mq_imgui_new_line(void) { ImGui::NewLine(); }
void mq_imgui_begin_disabled(int disabled) { ImGui::BeginDisabled(disabled != 0); }
void mq_imgui_end_disabled(void) { ImGui::EndDisabled(); }
int mq_imgui_begin_child(const char *id, float w, float h, int border) {
    return ImGui::BeginChild(id ? id : "##child", ImVec2(w, h), border != 0) ? 1 : 0;
}
void mq_imgui_end_child(void) { ImGui::EndChild(); }
void mq_imgui_set_next_item_width(float w) { ImGui::SetNextItemWidth(w); }
void mq_imgui_push_item_width(float w) { ImGui::PushItemWidth(w); }
void mq_imgui_pop_item_width(void) { ImGui::PopItemWidth(); }
void mq_imgui_push_id(const char *id) { ImGui::PushID(id ? id : ""); }
void mq_imgui_pop_id(void) { ImGui::PopID(); }
void mq_imgui_tooltip(const char *text) { if (text && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text); }
int mq_imgui_is_item_hovered(void) { return ImGui::IsItemHovered() ? 1 : 0; }
int mq_imgui_is_item_active(void) { return ImGui::IsItemActive() ? 1 : 0; }
int mq_imgui_is_window_hovered(void) { return ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ? 1 : 0; }
void mq_imgui_set_item_default(void) { if (ImGui::IsItemActive()) ImGui::SetItemDefaultFocus(); }

/* ---- 菜单 / 标签页 / 表格 ---- */
int mq_imgui_begin_menu_bar(void) { return ImGui::BeginMenuBar() ? 1 : 0; }
void mq_imgui_end_menu_bar(void) { ImGui::EndMenuBar(); }
int mq_imgui_begin_menu(const char *label) { return ImGui::BeginMenu(label ? label : "menu") ? 1 : 0; }
void mq_imgui_end_menu(void) { ImGui::EndMenu(); }
int mq_imgui_menu_item(const char *label, int selected, int enabled) {
    return ImGui::MenuItem(label ? label : "item", nullptr, selected != 0, enabled != 0) ? 1 : 0;
}
int mq_imgui_begin_tab_bar(const char *id) { return ImGui::BeginTabBar(id ? id : "##tabs") ? 1 : 0; }
void mq_imgui_end_tab_bar(void) { ImGui::EndTabBar(); }
int mq_imgui_begin_tab_item(const char *label) {
    return ImGui::BeginTabItem(label ? label : "tab") ? 1 : 0;
}
void mq_imgui_end_tab_item(void) { ImGui::EndTabItem(); }
int mq_imgui_begin_table(const char *id, int cols) {
    return ImGui::BeginTable(id ? id : "##table", cols > 0 ? cols : 1) ? 1 : 0;
}
void mq_imgui_end_table(void) { ImGui::EndTable(); }
void mq_imgui_table_next_row(void) { ImGui::TableNextRow(); }
void mq_imgui_table_next_column(void) { ImGui::TableNextColumn(); }
void mq_imgui_table_headers_row(void) { ImGui::TableHeadersRow(); }

/* ---- 文本 ---- */
void mq_imgui_text(const char *text) { ImGui::TextUnformatted(text ? text : ""); }
void mq_imgui_text_wrapped(const char *text) { ImGui::TextWrapped("%s", text ? text : ""); }
void mq_imgui_text_disabled(const char *text) { ImGui::TextDisabled("%s", text ? text : ""); }
void mq_imgui_text_colored(const char *text, float r, float g, float b, float a) {
    ImGui::TextColored(ImVec4(r, g, b, a), "%s", text ? text : "");
}
void mq_imgui_label_text(const char *label, const char *value) {
    ImGui::LabelText(label ? label : "", "%s", value ? value : "");
}
void mq_imgui_bullet_text(const char *text) { ImGui::BulletText("%s", text ? text : ""); }

/* ---- 基础控件 ---- */
int mq_imgui_button(const char *label) { return ImGui::Button(label ? label : "##b") ? 1 : 0; }
int mq_imgui_small_button(const char *label) { return ImGui::SmallButton(label ? label : "##sb") ? 1 : 0; }
int mq_imgui_invisible_button(const char *id, float w, float h) {
    return ImGui::InvisibleButton(id ? id : "##ib", ImVec2(w, h)) ? 1 : 0;
}
int mq_imgui_arrow_button(const char *id, int dir) {
    return ImGui::ArrowButton(id ? id : "##ab", (ImGuiDir)(dir < 0 ? 0 : (dir > 3 ? 3 : dir))) ? 1 : 0;
}
float mq_imgui_checkbox(const char *label, float value) {
    bool v = value != 0.0F;
    if (ImGui::Checkbox(label ? label : "##cb", &v)) return v ? 1.0F : 0.0F;   // 变了就返回新值
    return value;
}
int mq_imgui_radio_button(const char *label, int active) {
    return ImGui::RadioButton(label ? label : "##rb", active != 0) ? 1 : 0;
}
float mq_imgui_slider_float(const char *label, float value, float min, float max) {
    float v = value;
    ImGui::SliderFloat(label ? label : "##sf", &v, min, max);
    return v;
}
float mq_imgui_slider_int(const char *label, float value, float min, float max) {
    int v = (int)value;
    ImGui::SliderInt(label ? label : "##si", &v, (int)min, (int)max);
    return (float)v;
}
float mq_imgui_drag_float(const char *label, float value, float speed) {
    float v = value;
    ImGui::DragFloat(label ? label : "##df", &v, speed > 0.0F ? speed : 0.1F);
    return v;
}
float mq_imgui_drag_int(const char *label, float value, float speed) {
    int v = (int)value;
    ImGui::DragInt(label ? label : "##di", &v, speed > 0.0F ? speed : 1.0F);
    return (float)v;
}
float mq_imgui_input_float(const char *label, float value) {
    float v = value;
    ImGui::InputFloat(label ? label : "##if", &v);
    return v;
}
float mq_imgui_input_int(const char *label, float value) {
    int v = (int)value;
    ImGui::InputInt(label ? label : "##ii", &v);
    return (float)v;
}
const char *mq_imgui_input_text(const char *label, const char *text) {
    const std::string key(label ? label : "##it");
    auto it = g_text.find(key);
    if (it == g_text.end()) it = g_text.emplace(key, text ? text : "").first;
    /* 用固定大小缓冲：ImGui 的 InputText 需要可写缓冲（std::string 不能直接给）。 */
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", it->second.c_str());
    if (ImGui::InputText(key.c_str(), buf, sizeof buf)) it->second = buf;
    return it->second.c_str();
}
float mq_imgui_combo(const char *label, float current, const char *items_csv) {
    split_csv(items_csv);
    if (g_items_ptr.empty()) return current;
    int v = (int)current;
    if (v < 0 || v >= (int)g_items_ptr.size()) v = 0;
    ImGui::Combo(label ? label : "##cb2", &v, g_items_ptr.data(), (int)g_items_ptr.size());
    return (float)v;
}
float mq_imgui_list_box(const char *label, float current, const char *items_csv) {
    split_csv(items_csv);
    if (g_items_ptr.empty()) return current;
    int v = (int)current;
    if (v < 0 || v >= (int)g_items_ptr.size()) v = 0;
    ImGui::ListBox(label ? label : "##lb", &v, g_items_ptr.data(), (int)g_items_ptr.size());
    return (float)v;
}
int mq_imgui_selectable(const char *label, int selected) {
    return ImGui::Selectable(label ? label : "##sel", selected != 0) ? 1 : 0;
}
void mq_imgui_progress_bar(float fraction, float w, float h, const char *overlay) {
    ImGui::ProgressBar(fraction, ImVec2(w, h), overlay && *overlay ? overlay : nullptr);
}
void mq_imgui_plot_value(const char *id, float value) {
    const std::string key(id ? id : "plot");
    std::vector<float> &hist = g_plots[key];
    hist.push_back(value);
    if (hist.size() > kPlotMax) hist.erase(hist.begin());
    ImGui::PlotLines(key.c_str(), hist.data(), (int)hist.size(), 0,
                     nullptr, FLT_MAX, FLT_MAX, ImVec2(0, 40));
}
float mq_imgui_color_edit(const char *label, float packed) {
    const int pk = (int)packed;
    float rgb[3] = { (float)((pk >> 16) & 0xFF) / 255.0F,
                     (float)((pk >> 8) & 0xFF) / 255.0F,
                     (float)(pk & 0xFF) / 255.0F };
    if (ImGui::ColorEdit3(label ? label : "##col", rgb)) {
        const int r = (int)(rgb[0] * 255.0F + 0.5F);
        const int g = (int)(rgb[1] * 255.0F + 0.5F);
        const int b = (int)(rgb[2] * 255.0F + 0.5F);
        return (float)((r << 16) | (g << 8) | b);
    }
    return packed;
}
int mq_imgui_tree_node(const char *label) { return ImGui::TreeNode(label ? label : "##tn") ? 1 : 0; }
void mq_imgui_tree_pop(void) { ImGui::TreePop(); }
int mq_imgui_collapsing_header(const char *label) {
    return ImGui::CollapsingHeader(label ? label : "##ch") ? 1 : 0;
}
int mq_imgui_begin_overlay(const char *title) {
    /* 可拖动（有标题栏）但不参与命中：NoInputs 让 ImGui 不把悬停/点击算到它头上。 */
    /* 可拖动（标题栏）+ 自适应内容。
     * 注意：**不能加 NoInputs** —— 它会连标题栏拖动一起禁掉（实测"无法拖动"），
     * 也会让内容绘制异常。需要"不挡游戏点击"时应改用 ImGui 的 hover 判定，而不是 NoInputs。 */
    /* 覆盖层**固定位置、不可手动移动/缩放**（NoMove | NoResize）。
     *
     * 原因（实测两次）：只要允许用户拖动这个窗口，就会出现"窗口动了、文字没跟着走"的
     * 错位 —— 我们的绘制数据导出里，窗口位置与内容坐标来自同一份 ImGui draw list，
     * 本应同步；实测不同步说明拖动路径上还有未定位的时序问题（ImGui 在拖动帧会把
     * 窗口位置更新推迟到下一帧，而文本布局用的是上一帧起点）。
     * 在定位清楚之前，覆盖层固定不动是**正确且可用**的：
     * 它是常驻信息栏，固定左上角即可，也避免挡住游戏内容。 */
    return ImGui::Begin(title && *title ? title : "overlay", nullptr,
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize) ? 1 : 0;
}

void mq_imgui_push_font_size(float size_px) {
    /* NULL = 沿用当前字体，只改字号（1.92+ 的 per-window/push 字号方式）。 */
    ImGui::PushFont(nullptr, size_px > 0.0F ? size_px : 0.0F);
}
void mq_imgui_pop_font(void) { ImGui::PopFont(); }

void mq_imgui_show_demo(int *open) {
    /* int* 不能直接当 bool* 用（尺寸不同）→ 内部维护一份，回写调用方。 */
    static bool s_open = false;
    if (open && *open == 0 && s_open) s_open = false;      /* 脚本可显式关 */
    if (open && *open != 0) s_open = true;
    ImGui::ShowDemoWindow(&s_open);
    if (open) *open = s_open ? 1 : 0;
}

} // extern "C"
