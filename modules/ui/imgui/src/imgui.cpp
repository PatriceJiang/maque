/* maque::imgui 实现（C ABI 见 include/ui/mq_imgui.h）。
 *
 * 分层：本文件只做 "ImGui 上下文 + 帧 + 布局 + 绘制数据导出"。
 * 纹理上传与网格提交由宿主（modules/runtime）负责 —— 所以这里不 include 任何
 * gfx/platform 头，跨模块只有纯 C ABI。
 *
 * 导出的顶点已经是**渲染目标像素空间**（y 向下）：ImGui 的显示坐标经过
 * "目标→显示映射"的逆变换回来（target = (show - off) / scale），
 * 这样面板按窗口像素排版（UI 大小不随画布缩放变化），但落点仍与引擎图元同一空间。
 */
#include "ui/mq_imgui.h"
#include "core/tracy/mq_tracy.h"

#include "imgui.h"

/* 官方后端模式（backend_sdl.cpp 设置 / init 前由 set_backend_mode 声明）。 */
bool mq_imgui_g_backend = false;
bool mq_imgui_g_backend_pref = false;
static ImFont *g_small_font = nullptr;   // 15px 小字号（stats 用）

#include <cmath>
#include <cstdlib>
#include <print>
#include <cstdlib>
#include <print>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool g_ready = false;
bool g_trace = false;                  // MAQUE_IMGUI_TRACE=1：打印每帧命令几何（排障用）
float g_pixel_scale = 1.0F;
uint64_t g_font_tex = 0;
uint32_t g_font_gen = 0;
ImTextureData *g_font_texdata = nullptr;   // 字体图集（1.93：ImFontAtlas::TexData）

/* 每帧映射（begin_frame 写入，end_frame 使用） */
float g_show_w = 0, g_show_h = 0;
float g_target_w = 0, g_target_h = 0;
float g_scale = 1.0F, g_off_x = 0, g_off_y = 0;
float g_dpi = 1.0F;

/* 输入状态（宿主每帧喂；ImGui 需要"持续下压"语义，所以这里保存） */
float g_ptr_x = 0, g_ptr_y = 0;
bool g_ptr_down = false;
bool g_focused = true;

/* 字体图集的 RGBA32 副本（宿主上传用） */
std::vector<uint8_t> g_font_rgba;
int g_font_w = 0, g_font_h = 0;
unsigned char *g_font_px = nullptr;    // ImGui 持有的图集像素（GetTexDataAsRGBA32 返回）
bool g_font_built = false;             // 静态图集是否已 Build（只会做一次）

/* 导出缓冲（仅本帧有效）。
 * ⚠️ 构建期只存**偏移**，不要存 vector 指针：边 push_back 边取 pointer 会在扩容后悬空
 *（ASan 实测 heap-use-after-free）。全部构建完后再统一回填指针。 */
std::vector<float> g_pos, g_uv, g_col;
std::vector<uint32_t> g_idx;
std::vector<mq_imgui_cmd> g_cmds;
std::vector<size_t> g_vtx_off, g_idx_off;   // 每条命令在各自缓冲里的起始偏移
mq_imgui_frame g_frame{};

/* 显示坐标 → 目标像素坐标（与 begin_frame 的映射互逆）。 */
inline float to_target_x(float x) { return g_scale != 0.0F ? (x - g_off_x) / g_scale : x; }
inline float to_target_y(float y) { return g_scale != 0.0F ? (y - g_off_y) / g_scale : y; }

/* 字体图集在 ImGui 1.93 里就是一个 ImTextureData（ImFontAtlas::TexData），
 * 由**后端**负责：上传纹理后 SetTexID() + SetStatus(OK)。
 *
 * ⚠️ 关键时序（实测踩坑）：设了 `RendererHasTextures` 之后，图集是 ImGui 在
 * **NewFrame() 内部**懒构建的（imgui_draw.cpp: ImFontAtlasBuildBeginFrame）。
 * 所以 init 阶段 `TexData` 还是空的 —— 这里**只设标志**，不认领、不 Build、不加代数；
 * 真正的认领放到 NewFrame 之后（见 mq_imgui_begin_frame 末尾）。 */
void adopt_font_texture() {
    ImGuiIO &io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    g_font_texdata = nullptr;      // 等 NewFrame 建好后再认领
}

} // namespace

extern "C" {

int mq_imgui_init(float font_size, float pixel_scale) {
    return mq_imgui_init_with_fonts(nullptr, font_size, 0.0F, pixel_scale);
}

int mq_imgui_init_with_font(const char *ttf_path, float font_size, float pixel_scale) {
    return mq_imgui_init_with_fonts(ttf_path, font_size, 0.0F, pixel_scale);
}

int mq_imgui_init_with_fonts(const char *ttf_path, float title_size, float body_size, float pixel_scale) {
    if (g_ready) return 0;
    g_trace = std::getenv("MAQUE_IMGUI_TRACE") != nullptr;
    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) return 1;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;          // 不落盘：确定性 + 不污染工作目录
    io.LogFilename = nullptr;
    io.BackendPlatformName = "maque";
    /* 键盘导航默认关闭：调试面板不抢键盘焦点（除非用户点了输入框） */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    /* 字号（逻辑像素；标题 / 正文两档）与 UI 缩放（样式尺寸一并缩放）。
     * 这样"屏幕分辨率决定 UI 大小、窗口大小不参与"——同一块屏上外观固定。 */
    const float title_px = title_size > 0.0F ? title_size : 15.0F;
    const float body_px = body_size > 0.0F ? body_size : title_px * 0.78F;
    g_pixel_scale = pixel_scale > 0.0F ? pixel_scale : 1.0F;
    ImGui::GetStyle().ScaleAllSizes(g_pixel_scale);   // 内边距/间距/标题栏高度/滚动条…
    /* 让字体图集直接用 **RGBA32**：宿主只处理一种纹理格式，引擎的 normal 混合即可正确工作
     *（Alpha8 需要后端自己扩成 RGBA，多一步且易错）。
     * 注：1.93 里这是**公开字段** TexDesiredFormat，不是 Set 方法。 */
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;

    /* 字体：TTF（含 CJK）+ 常用简体中文范围；没有 TTF 才退默认（只有 ASCII → 中文会是 '?'）。
     * 一次烘**两档**：标题（title_px）与正文（body_px），都乘 UI 缩放 → 屏幕像素级清晰。 */
    bool loaded = false;
    if (ttf_path && *ttf_path) {
        ImFontConfig cfg;
        cfg.SizePixels = title_px * g_pixel_scale;
        /* OverrideGlyphOffset 保持默认；范围取"常用简体中文"（约 2500 字）——够面板文案用，
         * 且图集不会过大（全量 CJK 会到几 MB 图集，没必要）。 */
        cfg.GlyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
        if (io.Fonts->AddFontFromFileTTF(ttf_path, cfg.SizePixels, &cfg)) {
            loaded = true;
            /* 第二档：正文小字号（stats 面板内容用）。**必须也乘 g_pixel_scale** ——
             * 之前写死 15px，UI 缩放变化时标题跟着变、正文不变 → 比例失衡。 */
            ImFontConfig small_cfg;
            small_cfg.SizePixels = body_px * g_pixel_scale;
            small_cfg.GlyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
            g_small_font = io.Fonts->AddFontFromFileTTF(ttf_path, small_cfg.SizePixels, &small_cfg);
        }
    }
    if (!loaded) io.Fonts->AddFontDefault();
    std::print(stderr, "[imgui] fonts: path='{}' title={:.1f}px body={:.1f}px scale={:.2f} → {}\n",
               ttf_path && *ttf_path ? ttf_path : "(default)", title_px * g_pixel_scale,
               body_px * g_pixel_scale, g_pixel_scale,
               loaded ? "TTF(含 CJK 范围)" : "内置默认（仅 ASCII）");
    /* 按 ImGui 1.93 官方后端（imgui_impl_*）的模式：设好 RendererHasTextures 后**由 ImGui
     * 自己构建图集**，后端**不**手动调 Fonts->Build()。
     *
     * 踩坑记录（实测）：手动 Build() 会命中
     *   "Called ImFontAtlas::Build() before ImGuiBackendFlags_RendererHasTextures got set!"
     * 因为 Build() 内部 ImFontAtlasBuildUpdateRendererHasTexturesFromContext() 会把
     * `io.BackendFlags & RendererHasTextures` 同步到 atlas->RendererHasTextures，
     * 而 Build() 开头断言该字段为 false。一旦标志已设，任何"懒构建"路径（例如访问
     * 图集像素触发的构建）都会踩这个断言 —— 第一帧里就炸，且 init 的返回值看不出来。
     *
     * 所以这里只设标志 + 触发一次懒构建（走 ImGui 自己的路径），不手动 Build()。 */
    /* ⚠️ **不再设置 ImGuiBackendFlags_RendererHasTextures**（不走动态图集）。
     * 我们走传统静态路径：宿主第一次要像素时 `Build()` + `GetTexDataAsRGBA32()`，
     * 整张图上传一次、`SetTexID()` 绑定，之后不再更新。
     * 动态图集需要后端完整实现 WantCreate/WantUpdates 状态机，我们试了几轮都对不上时机
     *（症状：短文本正常、长文本/控件糊成白条）。静态路径简单且可预测。 */
    g_font_built = false;
    g_font_px = nullptr;
    g_font_w = g_font_h = 0;
    g_font_texdata = nullptr;
    g_ready = true;
    return 0;
}

void mq_imgui_shutdown(void) {
    if (!g_ready) return;
    /* ⚠️ 必须先关官方后端（它会把自己的 BackendRendererUserData 清掉），
     * 否则 DestroyContext 时命中断言 "Forgot to shutdown Renderer backend?"（实测退出崩溃）。 */
    if (mq_imgui_g_backend || mq_imgui_g_backend_pref) mq_imgui_backend_shutdown();
    ImGui::DestroyContext();
    g_ready = false;
    g_font_rgba.clear();
    g_pos.clear(); g_uv.clear(); g_col.clear(); g_idx.clear(); g_cmds.clear();
}

int mq_imgui_ready(void) { return g_ready ? 1 : 0; }

ImFont *mq_imgui_small_font(void) { return g_small_font; }

void mq_imgui_set_backend_mode(int on) { mq_imgui_g_backend_pref = on != 0; }

const uint8_t *mq_imgui_font_pixels(int *out_w, int *out_h) {
    if (!g_ready) return nullptr;
    if (mq_imgui_g_backend_pref || mq_imgui_g_backend) return nullptr;   // 官方后端自管图集
    /* **传统静态图集路径**（不走 RendererHasTextures 的动态图集）：
     * 一次性 `Build()` + `GetTexDataAsRGBA32()` 拿到整张图，宿主上传一次即可。
     *
     * 为什么放弃动态图集：1.92+ 的动态图集要求后端实现完整的
     * `WantCreate / WantUpdates / WantDestroy` 状态机（见 imgui_impl_sdlrenderer2.cpp），
     * 我们几次尝试（按代数重传、按 dirty 重传、整张重传）都没能对上它的更新时机，
     * 表现为"短文本正常、长文本/控件糊成白条"。静态路径行为可预测：
     * 图集内容在 Build 时一次成型，之后不再变。代价是**只能用 GlyphRanges 里预载的字形**
     *（面板文案都是固定文案，没问题；引擎自己的文本渲染走 text 模块，不受影响）。 */
    ImGuiIO &io = ImGui::GetIO();
    if (!g_font_built) {
        unsigned char *px = nullptr;
        int w = 0, h = 0, bpp = 0;
        io.Fonts->Build();
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h, &bpp);
        if (!px || w <= 0 || h <= 0) return nullptr;
        g_font_px = px;
        g_font_w = w;
        g_font_h = h;
        g_font_built = true;
        ++g_font_gen;
        std::print(stderr, "[imgui] font atlas built (static): {}x{} bpp={}\n", w, h, bpp);
    }
    if (out_w) *out_w = g_font_w;
    if (out_h) *out_h = g_font_h;
    return (const uint8_t *)g_font_px;
}

void mq_imgui_set_font_texture(uint64_t handle) {
    g_font_tex = handle;
    ImGuiIO &io = ImGui::GetIO();
    if (handle != 0) {
        io.Fonts->SetTexID((ImTextureID)handle);      // 传统 API：图集绑定纹理句柄
        if (io.Fonts->TexData) {
            io.Fonts->TexData->SetTexID((ImTextureID)handle);
            io.Fonts->TexData->SetStatus(ImTextureStatus_OK);
        }
    }
}

int mq_imgui_font_dirty(void) {
    if (!g_ready) return 0;
    /* 静态图集：只有"还没上传过"算脏（Build 之后内容不再变）。 */
    return (g_font_tex == 0 || !g_font_built) ? 1 : 0;
}
uint64_t mq_imgui_font_texture(void) { return g_font_tex; }
uint32_t mq_imgui_font_generation(void) { return g_font_gen; }

uint64_t mq_imgui_font_atlas_bytes(void) {
    if (!g_ready || !g_font_built || !g_font_px) return 0;
    return (uint64_t)g_font_w * (uint64_t)g_font_h * 4u;
}

void mq_imgui_input_pointer(float x, float y, int down, float wheel) {
    g_ptr_x = x;
    g_ptr_y = y;
    g_ptr_down = down != 0;
    if (wheel != 0.0F && g_ready) ImGui::GetIO().AddMouseWheelEvent(0.0F, wheel);
}

/* 引擎规范键码（MQ_KEY_* / 大写 ASCII / F1..F12=1001..1012）→ ImGuiKey。
 * ⚠️ 两套码空间**完全不同**，绝对不能直接强转：
 *   · 引擎的 SPACE=2，而 ImGuiKey 2 是别的键；
 *   · 引擎的 'A'=32，ImGuiKey 32 也不是字母 A；
 *   · F 键在引擎里是 1001..1012，超出 ImGui 的合法 key 范围 →
 *     直接 `AddKeyEvent((ImGuiKey)1002, ...)` 会写越界（实测：按 F2 崩）。
 * 返回 ImGuiKey_None 表示"不映射"（不喂给 ImGui）。 */
static ImGuiKey to_imgui_key(int key) {
    switch (key) {
        case 1:  return ImGuiKey_Escape;       /* MQ_KEY_ESCAPE */
        case 2:  return ImGuiKey_Space;        /* MQ_KEY_SPACE */
        case 3:  return ImGuiKey_Enter;        /* MQ_KEY_ENTER */
        case 4:  return ImGuiKey_Tab;          /* MQ_KEY_TAB */
        case 5:  return ImGuiKey_Backspace;    /* MQ_KEY_BACKSPACE */
        case 6:  return ImGuiKey_LeftArrow;    /* MQ_KEY_LEFT */
        case 7:  return ImGuiKey_RightArrow;   /* MQ_KEY_RIGHT */
        case 8:  return ImGuiKey_UpArrow;      /* MQ_KEY_UP */
        case 9:  return ImGuiKey_DownArrow;    /* MQ_KEY_DOWN */
        case 1001: return ImGuiKey_F1;  case 1002: return ImGuiKey_F2;
        case 1003: return ImGuiKey_F3;  case 1004: return ImGuiKey_F4;
        case 1005: return ImGuiKey_F5;  case 1006: return ImGuiKey_F6;
        case 1007: return ImGuiKey_F7;  case 1008: return ImGuiKey_F8;
        case 1009: return ImGuiKey_F9;  case 1010: return ImGuiKey_F10;
        case 1011: return ImGuiKey_F11; case 1012: return ImGuiKey_F12;
        default: break;
    }
    /* 'A'..'Z' / '0'..'9' 等可打印 ASCII → ImGuiKey_A.. / ImGuiKey_0.. */
    if (key == 32) return ImGuiKey_Space;
    if (key >= 'A' && key <= 'Z') return (ImGuiKey)(ImGuiKey_A + (key - 'A'));
    if (key >= '0' && key <= '9') return (ImGuiKey)(ImGuiKey_0 + (key - '0'));
    if (key == '-') return ImGuiKey_Minus;
    if (key == '=') return ImGuiKey_Equal;
    if (key == ',') return ImGuiKey_Comma;
    if (key == '.') return ImGuiKey_Period;
    if (key == '/') return ImGuiKey_Slash;
    return ImGuiKey_None;                       /* 不认识的键：不喂给 ImGui（安全） */
}

void mq_imgui_input_key(int key, int down) {
    if (!g_ready) return;
    const ImGuiKey k = to_imgui_key(key);
    if (k == ImGuiKey_None) return;
    ImGui::GetIO().AddKeyEvent(k, down != 0);
}

void mq_imgui_input_chars(const char *utf8) {
    if (!g_ready || !utf8) return;
    ImGui::GetIO().AddInputCharactersUTF8(utf8);
}

void mq_imgui_input_focus(int focused) { g_focused = focused != 0; }

int mq_imgui_begin_frame(float show_w, float show_h, float target_w, float target_h,
                         float scale, float off_x, float off_y, float dt, float dpi_scale) {
    MQ_TRACY_ZONE_N(imgui, "begin_frame");
    if (!g_ready) return 1;
    if (show_w <= 0.0F || show_h <= 0.0F || target_w <= 0.0F || target_h <= 0.0F) return 2;
    g_show_w = show_w; g_show_h = show_h;
    g_target_w = target_w; g_target_h = target_h;
    g_scale = scale > 0.0F ? scale : 1.0F;
    g_off_x = off_x; g_off_y = off_y;
    g_dpi = dpi_scale > 0.0F ? dpi_scale : g_scale;

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(show_w, show_h);
    io.DisplayFramebufferScale = ImVec2(g_dpi > 0.0F ? g_dpi : 1.0F, g_dpi > 0.0F ? g_dpi : 1.0F);
    io.DeltaTime = dt > 0.0F ? dt : (1.0F / 60.0F);
    io.AddFocusEvent(g_focused);

    /* 指针：ImGui 的坐标是**显示空间**；宿主给的是窗口像素 = 显示空间 → 直接用。 */
    io.AddMousePosEvent(g_ptr_x, g_ptr_y);
    io.AddMouseButtonEvent(0, g_ptr_down);

    ImGui::NewFrame();

    /* 静态图集路径：这里**不再认领 TexData / 不递增代数**。
     * 图集在宿主第一次调 `mq_imgui_font_pixels()` 时一次性 Build 好并上传，
     * 之后内容不变；`mq_imgui_font_dirty()` 只在"还没上传过"时为 1。 */
    return 0;
}

int mq_imgui_end_frame(mq_imgui_frame *out) {
    MQ_TRACY_ZONE_N(imgui, "end_frame");
    if (!g_ready || !out) return 1;
    ImGui::Render();

    g_pos.clear(); g_uv.clear(); g_col.clear(); g_idx.clear(); g_cmds.clear();
    g_vtx_off.clear(); g_idx_off.clear();
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount <= 0) {
        out->cmds = nullptr;
        out->cmd_count = 0;
        return 0;
    }

    for (int li = 0; li < dd->CmdListsCount; ++li) {
        const ImDrawList *list = dd->CmdLists[li];
        const ImDrawIdx *idx = list->IdxBuffer.Data;
        const ImDrawVert *vtx = list->VtxBuffer.Data;
        /* 刻意**不**设 io.BackendFlags |= RendererHasVtxOffset：
         * 那样 ImGui 会用 imdrawcmd 的 VtxOffset 做增量式顶点偏移（字段是"相对上一条的增量"），
         * 增加出错面。不设时 VtxOffset 恒为 0 —— ImGui 自己的断言就假设
         * "本命令用到的最大局部索引 + 1 == 本命令的顶点数"（见 imgui.cpp 里那段 assert），
         * 我们照这个语义导出，宿主自然不需要处理偏移。 */
        for (const ImDrawCmd &c : list->CmdBuffer) {
            if (c.UserCallback) continue;                  // 自定义回调本模块不支持（宿主自己处理）
            if (c.ElemCount == 0) continue;
            const uint32_t icount = (uint32_t)c.ElemCount;
            /* 诊断（MAQUE_IMGUI_TRACE=1）：打印每条命令的真实几何参数。
             * 面板"糊成一片/白条"的排查全靠它 —— 猜了三轮都没中，数据先摆出来。 */
            if (g_trace) {
                uint32_t mn0 = UINT32_MAX, mx0 = 0;
                float bx0 = 1e9F, by0 = 1e9F, bx1 = -1e9F, by1 = -1e9F;
                for (uint32_t i = 0; i < icount; ++i) {
                    const uint32_t v = (uint32_t)idx[c.IdxOffset + i];
                    mn0 = v < mn0 ? v : mn0;
                    mx0 = v > mx0 ? v : mx0;
                }
                for (uint32_t v = mn0; v <= mx0; ++v) {
                    const ImDrawVert &dv = vtx[v];
                    bx0 = dv.pos.x < bx0 ? dv.pos.x : bx0;
                    by0 = dv.pos.y < by0 ? dv.pos.y : by0;
                    bx1 = dv.pos.x > bx1 ? dv.pos.x : bx1;
                    by1 = dv.pos.y > by1 ? dv.pos.y : by1;
                }
                std::print(stderr,
                           "[imgui.cmd] cmd={} elem={} idxRange=[{},{}] nvtx={} bbox=({:.0f},{:.0f})-({:.0f},{:.0f}) "
                           "clip=({:.0f},{:.0f})-({:.0f},{:.0f}) vtxOff={}\n",
                           (int)(&c - list->CmdBuffer.Data), icount, mn0, mx0, mx0 - mn0 + 1,
                           bx0, by0, bx1, by1,
                           c.ClipRect.x, c.ClipRect.y, c.ClipRect.z, c.ClipRect.w,
                           (uint32_t)c.VtxOffset);
            }
            /* 本命令真正用到的顶点区间 = 索引的 [min, max]（跨整个 list 顶点缓冲的下标）。
             * 为什么不用 `VtxOffset`：实测该字段在本集成里恒为 0（不设 RendererHasVtxOffset），
             * 而同一 list 的多条命令会**共享**同一个顶点缓冲，各自只用其中一段 ——
             * 用 VtxOffset 会把整段缓冲都拷进来（多拷贝不影响正确性但浪费），
             * 更关键的是下面索引的**基准**必须是这一段，否则索引全错位。
             *
             * ⚠️ 索引导出约定：**命令内局部下标（0 基）**，与顶点块一一对应。
             * 宿主把它拷到自己缓冲的 offset `base` 处，再把索引 + base 即可。
             * （曾经导出"模块全局下标"，宿主又加了一次自己的 base → 第二条命令索引整体偏移，画面糊成一片。） */
            uint32_t mn = UINT32_MAX, mx = 0;
            for (uint32_t i = 0; i < icount; ++i) {
                const uint32_t v = (uint32_t)idx[c.IdxOffset + i];
                mn = v < mn ? v : mn;
                mx = v > mx ? v : mx;
            }
            if (mn == UINT32_MAX) continue;                // 理论上不可能（ElemCount>0）
            const uint32_t vstart = mn;
            const uint32_t vcount = mx - mn + 1;
            const uint32_t vbase = (uint32_t)(g_pos.size() / 2);

            /* 顶点：ImDrawVert 默认 pos(8B)+uv(8B)+col(4B)=20B；位置是显示坐标
             * → 映射回目标像素空间（这样面板按窗口像素排版，但落点与引擎图元同空间）。 */
            for (uint32_t i = 0; i < vcount; ++i) {
                const ImDrawVert &v = vtx[vstart + i];
                g_pos.push_back(to_target_x(v.pos.x));
                g_pos.push_back(to_target_y(v.pos.y));
                g_uv.push_back(v.uv.x);
                g_uv.push_back(v.uv.y);
                const ImU32 col = v.col;
                const float rr = (float)((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0F;
                const float gg = (float)((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0F;
                const float bb = (float)((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0F;
                const float aa = (float)((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0F;
                g_col.push_back(rr); g_col.push_back(gg); g_col.push_back(bb); g_col.push_back(aa);
            }
            /* 索引：转成**命令内局部下标（0 基）** —— 宿主导入时再加它自己的 base。 */
            for (uint32_t i = 0; i < icount; ++i)
                g_idx.push_back((uint32_t)idx[c.IdxOffset + i] - vstart);

            mq_imgui_cmd cmd{};
            /* 只记偏移；指针在全部几何建好后统一回填（见循环之后）。 */
            g_vtx_off.push_back((size_t)vbase * 2);
            g_idx_off.push_back(g_idx.size() - icount);
            cmd.vertex_count = vcount;
            cmd.index_count = icount;
            /* 纹理 ID：**不能**直接调 ImDrawCmd::GetTexID() —— 1.93 里它对
             * "尚未上传的 ImTextureData"会断言。这里按同一语义安全取值：
             * 优先用 ImTextureData 上由后端设好的 TexID（字体图集走这条），
             * 否则退到低层 _TexID（宿主直接传 TEXID 的场景）。 */
            const ImTextureRef &tref = c.TexRef;
            cmd.texture = tref._TexData ? (uint64_t)tref._TexData->TexID : (uint64_t)tref._TexID;
            const ImVec4 &cr = c.ClipRect;
            cmd.clip_x = to_target_x(cr.x);
            cmd.clip_y = to_target_y(cr.y);
            cmd.clip_w = to_target_x(cr.z) - cmd.clip_x;
            cmd.clip_h = to_target_y(cr.w) - cmd.clip_y;
            g_cmds.push_back(cmd);
        }
    }
    /* 全部几何建好后再回填指针 —— 此时 vector 不会再扩容，指针在本帧内稳定。 */
    for (size_t i = 0; i < g_cmds.size(); ++i) {
        const size_t voff = g_vtx_off[i];
        g_cmds[i].positions = g_pos.data() + voff;
        g_cmds[i].uvs = g_uv.data() + voff;
        g_cmds[i].colors = g_col.data() + (voff / 2) * 4;
        g_cmds[i].indices = g_idx.data() + g_idx_off[i];
    }
    g_frame.cmds = g_cmds.empty() ? nullptr : g_cmds.data();
    g_frame.cmd_count = (uint32_t)g_cmds.size();
    *out = g_frame;
    return 0;
}

int mq_imgui_want_mouse(void) {
    if (!g_ready) return 0;
    return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
}

int mq_imgui_want_keyboard(void) {
    if (!g_ready) return 0;
    return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0;
}

int mq_imgui_hovered(void) {
    if (!g_ready) return 0;
    return ImGui::IsAnyItemHovered() || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ? 1 : 0;
}

} // extern "C"
