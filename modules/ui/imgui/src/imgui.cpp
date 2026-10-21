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

#include "imgui.h"

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
float g_font_size = 0.0F;
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
    return mq_imgui_init_with_font(nullptr, font_size, pixel_scale);
}

int mq_imgui_init_with_font(const char *ttf_path, float font_size, float pixel_scale) {
    if (g_ready) return 0;
    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) return 1;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;          // 不落盘：确定性 + 不污染工作目录
    io.LogFilename = nullptr;
    io.BackendPlatformName = "maque";
    /* 键盘导航默认关闭：调试面板不抢键盘焦点（除非用户点了输入框） */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    g_font_size = font_size > 0.0F ? font_size : 0.0F;
    g_pixel_scale = pixel_scale > 0.0F ? pixel_scale : 1.0F;
    /* 让字体图集直接用 **RGBA32**：宿主只处理一种纹理格式，引擎的 normal 混合即可正确工作
     *（Alpha8 需要后端自己扩成 RGBA，多一步且易错）。
     * 注：1.93 里这是**公开字段** TexDesiredFormat，不是 Set 方法。 */
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;

    /* 字体：TTF（含 CJK）+ 常用简体中文范围；没有 TTF 才退默认（只有 ASCII → 中文会是 '?'）。 */
    bool loaded = false;
    if (ttf_path && *ttf_path && g_font_size > 0.0F) {
        ImFontConfig cfg;
        cfg.SizePixels = g_font_size * g_pixel_scale;
        /* OverrideGlyphOffset 保持默认；范围取"常用简体中文"（约 2500 字）——够面板文案用，
         * 且图集不会过大（全量 CJK 会到几 MB 图集，没必要）。 */
        cfg.GlyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
        if (io.Fonts->AddFontFromFileTTF(ttf_path, cfg.SizePixels, &cfg)) loaded = true;
    }
    if (!loaded) {
        if (g_font_size > 0.0F) io.Fonts->AddFontDefault();
    }
    std::print(stderr, "[imgui] font: path='{}' size={:.1f}px → {}\n",
               ttf_path && *ttf_path ? ttf_path : "(default)", g_font_size,
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
    adopt_font_texture();
    g_font_texdata = nullptr;                // NewFrame 之后才建好（见上）
    g_ready = true;
    return 0;
}

void mq_imgui_shutdown(void) {
    if (!g_ready) return;
    ImGui::DestroyContext();
    g_ready = false;
    g_font_rgba.clear();
    g_pos.clear(); g_uv.clear(); g_col.clear(); g_idx.clear(); g_cmds.clear();
}

int mq_imgui_ready(void) { return g_ready ? 1 : 0; }

const uint8_t *mq_imgui_font_pixels(int *out_w, int *out_h) {
    if (!g_ready) return nullptr;
    /* **不**在这里 Build：设了 RendererHasTextures 之后，图集由 ImGui 在
     * NewFrame() 内部构建（imgui_draw.cpp: ImFontAtlasBuildBeginFrame）。
     * 手动 Build 会命中 "Build() before RendererHasTextures got set" 断言并早退，
     * 图集永远停留在未构建状态（实测：512x128 且没有 CJK 字形 → 中文变方块）。
     * 所以这里只读取"NewFrame 之后认领好的" TexData。 */
    if (!g_font_texdata || !g_font_texdata->Pixels) return nullptr;
    if (out_w) *out_w = g_font_texdata->Width;
    if (out_h) *out_h = g_font_texdata->Height;
    return (const uint8_t *)g_font_texdata->Pixels;
}

void mq_imgui_set_font_texture(uint64_t handle) {
    g_font_tex = handle;
    if (g_font_texdata && handle != 0) {
        g_font_texdata->SetTexID((ImTextureID)handle);
        g_font_texdata->SetStatus(ImTextureStatus_OK);
    }
}

int mq_imgui_font_dirty(void) {
    if (!g_ready) return 0;
    if (!g_font_texdata) return 1;                       // 还没认领 → 需要（等 NewFrame 后）
    return g_font_texdata->Status != ImTextureStatus_OK ? 1 : 0;
}
uint64_t mq_imgui_font_texture(void) { return g_font_tex; }
uint32_t mq_imgui_font_generation(void) { return g_font_gen; }

void mq_imgui_input_pointer(float x, float y, int down, float wheel) {
    g_ptr_x = x;
    g_ptr_y = y;
    g_ptr_down = down != 0;
    if (wheel != 0.0F && g_ready) ImGui::GetIO().AddMouseWheelEvent(0.0F, wheel);
}

void mq_imgui_input_key(int key, int down) {
    if (!g_ready) return;
    ImGuiIO &io = ImGui::GetIO();
    if (key >= 0 && key < 512) io.AddKeyEvent((ImGuiKey)key, down != 0);
}

void mq_imgui_input_chars(const char *utf8) {
    if (!g_ready || !utf8) return;
    ImGui::GetIO().AddInputCharactersUTF8(utf8);
}

void mq_imgui_input_focus(int focused) { g_focused = focused != 0; }

int mq_imgui_begin_frame(float show_w, float show_h, float target_w, float target_h,
                         float scale, float off_x, float off_y, float dt, float dpi_scale) {
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

    /* NewFrame 内部（ImFontAtlasBuildBeginFrame）才会真正构建字体图集 → 这里认领。
     * 认领后：TexID 用宿主回填的句柄（未回填时给非零占位，避免 GetTexID 断言），
     * 代数 +1 通知宿主"该上传/重传纹理了"。 */
    if (io.Fonts->TexData && io.Fonts->TexData != g_font_texdata) {
        g_font_texdata = io.Fonts->TexData;
        g_font_texdata->SetTexID(g_font_tex != 0 ? (ImTextureID)g_font_tex : (ImTextureID)1);
        g_font_texdata->SetStatus(ImTextureStatus_OK);
        ++g_font_gen;
        std::print(stderr, "[imgui] font atlas {}x{} (gen={})\n",
                   g_font_texdata->Width, g_font_texdata->Height, g_font_gen);
    }
    return 0;
}

int mq_imgui_end_frame(mq_imgui_frame *out) {
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
            /* 本命令的顶点区间：ImGui 用 `VtxOffset` 指出它在**整个 list 顶点缓冲**里的起点。
             * 不处理它就会把别的命令的顶点拷进来 → 画面糊成一片/串行（实测过）。
             * 同一 list 里 `VtxOffset` 单调递增，所以本命令顶点数 = 上一条的起点差；
             * 最后一条用 max(索引)+1-VtxOffset 推。 */
            const uint32_t vstart = c.VtxOffset;
            const uint32_t vbase = (uint32_t)(g_pos.size() / 2);

            /* 顶点：ImDrawVert 默认 pos(8B)+uv(8B)+col(4B)=20B；位置是显示坐标
             * → 映射回目标像素空间（这样面板按窗口像素排版，但落点与引擎图元同空间）。
             * 顶点数用索引覆盖范围推：max(索引) - VtxOffset + 1（索引相对 list 起点）。 */
            uint32_t lmax = vstart;
            for (uint32_t i = 0; i < icount; ++i) {
                const uint32_t v = (uint32_t)idx[c.IdxOffset + i];
                if (v > lmax) lmax = v;
            }
            const uint32_t vcount = lmax - vstart + 1;
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
            for (uint32_t i = 0; i < icount; ++i)
                g_idx.push_back((uint32_t)idx[c.IdxOffset + i] - vstart + vbase);

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
