/* 官方 ImGui 后端模式：面板直接画到 SDL 窗口（SDL_Renderer）。
 *
 * 为什么需要它：引擎自研的"ImGui draw data → 引擎网格图元"转换链在**拖动窗口**时
 * 出现"窗口动了、文字不跟"的错位（多轮定位未果）。官方后端自己管坐标/裁剪/字体纹理，
 * 行为与 inspector 一致（该路径在 inspector 里长期可用）。
 *
 * 用法（宿主）：游戏帧渲染并 present 之后 →
 *   mq_imgui_backend_new_frame() → 脚本面板 + stats → mq_imgui_backend_render()
 * 显示尺寸仍由宿主先用 mq_imgui_begin_frame(...) 提供（它负责 io.DisplaySize/输入）。
 */
#include "ui/mq_imgui.h"
#include "core/tracy/mq_tracy.h"

#include "imgui.h"
#include "imgui_impl_sdlrenderer2.h"

#include <SDL.h>

extern bool mq_imgui_g_backend;      // 定义在 imgui.cpp

namespace {
SDL_Renderer *g_ren = nullptr;
bool g_backend_ready = false;
}

extern "C" {

int mq_imgui_backend_init(void *sdl_renderer) {
    if (g_backend_ready) return 0;
    if (!sdl_renderer) return 2;
    g_ren = (SDL_Renderer *)sdl_renderer;
    /* 初始化官方 SDL_Renderer 后端：它会自己创建/更新字体图集纹理。 */
    if (!ImGui_ImplSDLRenderer2_Init(g_ren)) { g_ren = nullptr; return 3; }
    g_backend_ready = true;
    return 0;
}

void mq_imgui_backend_shutdown(void) {
    if (!g_backend_ready) return;
    ImGui_ImplSDLRenderer2_Shutdown();
    g_backend_ready = false;
    g_ren = nullptr;
}

void mq_imgui_backend_new_frame(void) {
    if (!g_backend_ready) return;
    ImGui_ImplSDLRenderer2_NewFrame();
}

void mq_imgui_backend_render(void) {
    MQ_TRACY_ZONE_N(imgui, "backend_render");
    if (!g_backend_ready || !g_ren) return;
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
}

} // extern "C"
