/* 平台层实现（SDL2）：窗口 + 设备原始事件的**归一化采集**。
 * 事件先入内部队列（pump），在 poll 时统一消费（docs/19 §3：采样与消费分离）。
 * 注入（inject_*）与设备事件走同一队列 → AI/CI/回放与真机路径一致（docs/19 §9）。
 * 指针位置/按键状态只由事件流推导（不查设备状态）→ 顺序与回放确定。 */
/* Windows 头必须禁用 min/max 宏（否则 <windows.h> 的 `min` 宏会吃掉 std::min），
 * 且要在 SDL.h 之前定义（SDL 内部也可能 include windows.h）。 */
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif
#include "core/platform/mq_platform.h"
#include "core/tracy/mq_tracy.h"   // Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时 no-op）
#include "core/compat/mq_env.h"   // setenv/unsetenv 的跨平台封装（Windows 无 setenv）

#include <print>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include <SDL.h>
#include <SDL_syswm.h>   /* mq_platform_get_native_window：直接呈现要原生句柄（plans/18-pbr-3d.md） */
#if defined(__ANDROID__)
#  include <android/native_window.h>
#endif

#include <deque>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <time.h>
#endif

namespace {

uint32_t mods_from_sdl(SDL_Keymod m) {
    uint32_t out = 0;
    if (m & KMOD_SHIFT) out |= MQ_PLATFORM_MOD_SHIFT;
    if (m & KMOD_CTRL)  out |= MQ_PLATFORM_MOD_CTRL;
    if (m & KMOD_ALT)   out |= MQ_PLATFORM_MOD_ALT;
    if (m & KMOD_GUI)   out |= MQ_PLATFORM_MOD_META;
    return out;
}

/* SDL 键码 → 归一化键码（宿主不依赖 SDL 常量）。 */
uint32_t key_from_sdl(SDL_Keycode k) {
    switch (k) {
        case SDLK_ESCAPE:    return MQ_KEY_ESCAPE;
        case SDLK_SPACE:     return MQ_KEY_SPACE;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  return MQ_KEY_ENTER;
        case SDLK_TAB:       return MQ_KEY_TAB;
        case SDLK_BACKSPACE: return MQ_KEY_BACKSPACE;
        case SDLK_LEFT:      return MQ_KEY_LEFT;
        case SDLK_RIGHT:     return MQ_KEY_RIGHT;
        case SDLK_UP:        return MQ_KEY_UP;
        case SDLK_DOWN:      return MQ_KEY_DOWN;
        /* 功能键：规范空间里单独占 1001..1012（ASCII 里没有对应，VK_F1=0x70 会与 'p' 冲突）。
         * 引擎用 F2 开关 ImGui 调参面板（runtime 侧监听），其余键游戏可用。 */
        case SDLK_F1:        return MQ_KEY_F1;
        case SDLK_F2:        return MQ_KEY_F2;
        case SDLK_F3:        return MQ_KEY_F3;
        case SDLK_F4:        return MQ_KEY_F4;
        case SDLK_F5:        return MQ_KEY_F5;
        case SDLK_F6:        return MQ_KEY_F6;
        case SDLK_F7:        return MQ_KEY_F7;
        case SDLK_F8:        return MQ_KEY_F8;
        case SDLK_F9:        return MQ_KEY_F9;
        case SDLK_F10:       return MQ_KEY_F10;
        case SDLK_F11:       return MQ_KEY_F11;
        case SDLK_F12:       return MQ_KEY_F12;
        default: break;
    }
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 32);
    if (k >= 32 && k < 127) return (uint32_t)k;
    return MQ_KEY_UNKNOWN;
}

} // namespace

struct mq_platform {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;      /* 唯一呈现通道（懒创建）；无窗口/off/创建失败时为 null */
    SDL_Texture*  frame_tex = nullptr;/* 游戏帧流式纹理（每帧 UpdateTexture → RenderCopy） */
    int frame_tex_w = 0, frame_tex_h = 0;
    int present_mode = -1;            /* -1 未定；0 off；1 software；2 accelerated */
    bool present_logged = false;
    int  cap_max = -1, cap_every = 1, cap_seen = 0;
    float safe[4] = { 0, 0, 0, 0 };      /* 安全区内缩（设备像素） */
    int  display_dirty = 0;              /* 尺寸/显示器变化待消费 */
    int  cap_count = 0;               /* MAQUE_CAPTURE 已抓帧数 */
    bool        sdl = false;
    bool        window = false;
    bool        quit = false;
    int         w = 0, h = 0;
#if defined(__ANDROID__)
    const uint8_t* android_frame = nullptr;
    ANativeWindow* android_window = nullptr;
    int android_frame_w = 0, android_frame_h = 0;
    int android_dx = 0, android_dy = 0, android_dw = 0, android_dh = 0;
    int android_present_every = -1, android_present_seq = 0;
    bool android_pending = false;
#endif
    float       mouse_x = 0, mouse_y = 0;
    int         mouse_down = 0;
    int         focused = 1;
    int         resized = 0;
    /* 进程自省（docs/29 Tracy 基础数据）：CPU% 需要"上次采样"做差分。 */
    uint64_t    ps_prev_cpu_100ns = 0;   /* 上次累计 CPU 时间（100ns 单位，Windows/POSIX 统一折算） */
    uint64_t    ps_prev_wall_ns = 0;     /* 上次采样墙钟（ns） */

    struct PEv { mq_platform_pointer_event ev; };
    struct KEv { mq_platform_key_event ev; };
    std::deque<PEv> queued_pointers;
    std::deque<KEv> queued_keys;
    float last_x = 0, last_y = 0;
    int   have_last = 0;
};

/* 窗口逻辑尺寸（用户看到的点/像素，DPI 缩放后；与 drawable_size 的设备像素区分）。
 * 只在窗口存在时写；resize/最大化后立即回读，保证 runtime 当帧就能看到新尺寸。 */
static void refresh_window_size(mq_platform* p) {
    if (!p || !p->win) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(p->win, &w, &h);
    if (w > 0 && h > 0) { p->w = w; p->h = h; }
}

namespace {

/* 入队并更新指针状态（设备与注入共用；delta 在此统一计算）。 */

/* ---- 呈现：唯一通道 = SDL_Renderer ----------------------------------------
 * 为什么不用「窗口表面（SDL_GetWindowSurface）」这条旧路径：
 *   SDL2 规定**不能**对同一个窗口混用 window surface 与 SDL_Renderer。一旦为了
 *   ImGui 官方后端创建了 renderer，`SDL_UpdateWindowSurface` 写的内容就不再是
 *   用户看到的东西 —— 实测现象正是「游戏帧呈不上屏、只剩 ImGui 面板」。
 * 现在的结构（与 ImGui 的「先录制命令、画面渲染完后单独一趟 pass」模型一致）：
 *   ① 引擎帧（CPU RGBA）→ 流式纹理 → RenderCopy 到 renderer 后台缓冲（游戏画面）
 *   ② ImGui 官方后端在**同一个** renderer 后台缓冲上再画一趟（调试面板）
 *   ③ 一次 SDL_RenderPresent 把整帧上屏（抓帧也在这里，回读后台缓冲）
 * 默认后端：Windows 用 accelerated（Direct3D，不建 GL/EGL 上下文 → 不与 wgpu 争用）；
 * 其它平台默认 software（SDL_RENDERER_SOFTWARE 同样不建 GL/EGL 上下文）。
 * Android 不走本通道：draw/present 直接锁定当前 ANativeWindow 写 CPU RGBA 帧。
 * MAQUE_PRESENT=off|software|accelerated 可显式覆盖。
 */
static void ensure_present_mode(mq_platform* p) {
    if (p->present_mode >= 0) return;
#if defined(_WIN32)
    p->present_mode = 2;                                  // Windows 默认 Direct3D
#else
    p->present_mode = 1;                                  // 其它平台默认纯软件（不碰 GL/EGL）
#endif
    const char* mode = std::getenv("MAQUE_PRESENT");
    if (mode && *mode) {
        if (std::strcmp(mode, "off") == 0 || std::strcmp(mode, "0") == 0) p->present_mode = 0;
        else if (std::strcmp(mode, "accelerated") == 0) p->present_mode = 2;
        else if (std::strcmp(mode, "software") == 0) p->present_mode = 1;
    }
}

/* 懒创建 renderer（唯一后端的入口；无窗口 / present off / 创建失败 → null）。 */
static SDL_Renderer* ensure_renderer(mq_platform* p) {
    if (!p || !p->win || !p->window) return nullptr;
    ensure_present_mode(p);
    if (p->present_mode == 0) return nullptr;
    if (!p->ren) {
        const Uint32 flags = p->present_mode == 2 ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE;
        p->ren = SDL_CreateRenderer(p->win, -1, flags);
        if (!p->ren && p->present_mode == 2) {           // 加速不可用 → 软件回退
            p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_SOFTWARE);
            if (p->ren) p->present_mode = 1;
        }
        if (!p->ren) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "maque: SDL_CreateRenderer failed: %s", SDL_GetError());
            p->present_mode = 0;
            return nullptr;
        }
#if defined(__ANDROID__)
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(p->ren, &info) == 0)
            SDL_Log("maque: present renderer=%s mode=%s", info.name ? info.name : "?",
                    p->present_mode == 2 ? "accelerated" : "software");
#endif
    }
    return p->ren;
}

/* 抓帧：回读 **renderer 后台缓冲**（= 用户即将看到的整帧：游戏画面 + ImGui 面板）→ PPM。
 * 必须在 SDL_RenderPresent **之前**调用（present 之后后台缓冲内容未定义）。 */
static void capture_renderer(mq_platform* p) {
    if (p->cap_max < 0) {
        p->cap_max = 3; p->cap_every = 1;
        if (const char* v = std::getenv("MAQUE_CAPTURE_FRAMES")) p->cap_max = std::atoi(v);
        if (const char* v = std::getenv("MAQUE_CAPTURE_EVERY")) p->cap_every = std::atoi(v) > 0 ? std::atoi(v) : 1;
    }
    ++p->cap_seen;
    const char* cap = std::getenv("MAQUE_CAPTURE");
    if (!cap || !*cap || p->cap_count >= p->cap_max || (p->cap_seen % p->cap_every) != 0) return;
    int w = 0, h = 0;
    if (!p->ren || SDL_GetRendererOutputSize(p->ren, &w, &h) != 0 || w <= 0 || h <= 0) return;
    std::vector<uint8_t> px((size_t)w * (size_t)h * 4);
    if (SDL_RenderReadPixels(p->ren, nullptr, SDL_PIXELFORMAT_RGBA32, px.data(), w * 4) != 0) return;
    char path[512];
    std::snprintf(path, sizeof path, "%s_%d.ppm", cap, p->cap_count);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; ++i) std::fwrite(&px[(size_t)i * 4], 1, 3, f);
        std::fclose(f);
    }
    ++p->cap_count;
}

/* 游戏帧 → renderer 后台缓冲（**不 present**；随后可叠 ImGui，最后统一 present 一次）。 */
static int draw_frame_rect(mq_platform* p, const void* rgba, int32_t w, int32_t h,
                           int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t filter) {
    MQ_TRACY_ZONE_N(platform, "present.draw");   // Tracy：帧纹理上传 + RenderCopy
    if (!p || !rgba || w <= 0 || h <= 0) return 1;
#if defined(__ANDROID__)
    if (dw <= 0 || dh <= 0) return 3;
    p->android_frame = static_cast<const uint8_t*>(rgba);
    p->android_frame_w = w; p->android_frame_h = h;
    p->android_dx = dx; p->android_dy = dy; p->android_dw = dw; p->android_dh = dh;
    p->android_pending = true;
    return 0;
#endif
    SDL_Renderer* ren = ensure_renderer(p);
    if (!ren) return 2;                                  // 无窗口 / present off / renderer 不可用
    if (dw <= 0 || dh <= 0) return 3;
    if (!p->frame_tex || p->frame_tex_w != w || p->frame_tex_h != h) {
        if (p->frame_tex) SDL_DestroyTexture(p->frame_tex);
        p->frame_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        p->frame_tex_w = w; p->frame_tex_h = h;
        if (!p->frame_tex) return 4;
        SDL_SetTextureBlendMode(p->frame_tex, SDL_BLENDMODE_NONE);   // 不透明整帧覆盖
    }
    /* 放大过滤：整数倍时 nearest（pixel-perfect：字形/像素画放大后仍是硬边）；
     * 非整数倍只能线性（与旧软件路径同一规则）。 */
    const bool integer_scale = dw % w == 0 && dh % h == 0 && dw / w == dh / h && dw / w >= 2;
    const bool nearest = filter == 1 || (filter < 0 && integer_scale);
    SDL_SetTextureScaleMode(p->frame_tex, nearest ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    if (SDL_UpdateTexture(p->frame_tex, nullptr, rgba, w * 4) != 0) return 5;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);                                 // 画布外留黑（letterbox）
    SDL_Rect dst{ dx, dy, dw, dh };
    if (SDL_RenderCopy(ren, p->frame_tex, nullptr, &dst) != 0) return 6;
    if (!p->present_logged) {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(ren, &info) == 0) {
            std::print("platform: present via renderer='{}' ({}) frame={}x{} target={},{} {}x{} window={}x{}\n",
                       info.name ? info.name : "?",
                       p->present_mode == 2 ? "accelerated" : "software",
                       w, h, dx, dy, dw, dh, p->w, p->h);
        }
        p->present_logged = true;
    }
    return 0;
}

#if defined(__ANDROID__)
static int present_android_frame(mq_platform* p) {
    if (!p || !p->win || !p->android_pending || !p->android_frame) return 2;
    if (p->android_present_every < 0) {
        p->android_present_every = 1;
        if (const char* every = std::getenv("MAQUE_ANDROID_PRESENT_EVERY"))
            p->android_present_every = std::max(1, std::atoi(every));
    }
    if ((p->android_present_seq++ % p->android_present_every) != 0) {
        p->android_pending = false;
        p->android_frame = nullptr;
        return 0;
    }
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(p->win, &wm) || wm.subsystem != SDL_SYSWM_ANDROID || !wm.info.android.window)
        return 3;
    ANativeWindow* window = wm.info.android.window;
    if (p->android_window != window) {
        if (ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888) != 0) return 4;
        p->android_window = window;
    }

    ANativeWindow_Buffer out{};
    if (ANativeWindow_lock(window, &out, nullptr) != 0) return 4;
    const int x0 = std::max(0, p->android_dx);
    const int y0 = std::max(0, p->android_dy);
    const int x1 = std::min(out.width, p->android_dx + p->android_dw);
    const int y1 = std::min(out.height, p->android_dy + p->android_dh);
    const uint8_t* src = p->android_frame;

    if (out.format == WINDOW_FORMAT_RGBA_8888 || out.format == WINDOW_FORMAT_RGBX_8888) {
        auto* bits = static_cast<uint8_t*>(out.bits);
        for (int y = 0; y < out.height; ++y)
            std::memset(bits + (size_t)y * (size_t)out.stride * 4, 0, (size_t)out.stride * 4);

        if (p->android_dw == p->android_frame_w && p->android_dh == p->android_frame_h) {
            const size_t copy_bytes = (size_t)std::max(0, x1 - x0) * 4;
            for (int y = y0; y < y1; ++y) {
                const int sy = y - p->android_dy;
                const int sx = x0 - p->android_dx;
                uint8_t* row = bits + (size_t)y * (size_t)out.stride * 4 + (size_t)x0 * 4;
                const uint8_t* source = src + ((size_t)sy * p->android_frame_w + sx) * 4;
                std::memcpy(row, source, copy_bytes);
            }
        } else {
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = bits + (size_t)y * (size_t)out.stride * 4;
                const int sy = std::clamp((y - p->android_dy) * p->android_frame_h / p->android_dh,
                                          0, p->android_frame_h - 1);
                for (int x = x0; x < x1; ++x) {
                    const int sx = std::clamp((x - p->android_dx) * p->android_frame_w / p->android_dw,
                                              0, p->android_frame_w - 1);
                    const uint8_t* s = src + ((size_t)sy * p->android_frame_w + sx) * 4;
                    uint8_t* d = row + (size_t)x * 4;
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                }
            }
        }
    } else if (out.format == WINDOW_FORMAT_RGB_565) {
        auto* bits = static_cast<uint16_t*>(out.bits);
        for (int y = 0; y < out.height; ++y) {
            uint16_t* row = bits + (size_t)y * (size_t)out.stride;
            std::fill(row, row + out.stride, 0);
            if (y < y0 || y >= y1) continue;
            const int sy = std::clamp((y - p->android_dy) * p->android_frame_h / p->android_dh,
                                      0, p->android_frame_h - 1);
            for (int x = x0; x < x1; ++x) {
                const int sx = std::clamp((x - p->android_dx) * p->android_frame_w / p->android_dw,
                                          0, p->android_frame_w - 1);
                const uint8_t* s = src + ((size_t)sy * p->android_frame_w + sx) * 4;
                row[x] = (uint16_t)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
            }
        }
    } else {
        ANativeWindow_unlockAndPost(window);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "maque: unsupported ANativeWindow format=%d", out.format);
        return 5;
    }

    const int rc = ANativeWindow_unlockAndPost(window);
    p->android_pending = false;
    p->android_frame = nullptr;
    if (!p->present_logged) {
        SDL_Log("maque: present via ANativeWindow format=%d buffer=%dx%d stride=%d frame=%dx%d target=%d,%d %dx%d",
                out.format, out.width, out.height, out.stride,
                p->android_frame_w, p->android_frame_h,
                p->android_dx, p->android_dy, p->android_dw, p->android_dh);
        p->present_logged = true;
    }
    return rc == 0 ? 0 : 6;
}
#endif

/* 提交后台缓冲：抓帧（如需，present 之前）→ 单次 SDL_RenderPresent。 */
static int present_renderer(mq_platform* p) {
    MQ_TRACY_ZONE_N(platform, "present");        // Tracy：抓帧（如需）+ SDL_RenderPresent
#if defined(__ANDROID__)
    return present_android_frame(p);
#endif
    if (!p || !p->ren) return 2;
    capture_renderer(p);
    SDL_RenderPresent(p->ren);
    return 0;
}

void queue_pointer(mq_platform* p, const mq_platform_pointer_event& in) {
    mq_platform::PEv q{};
    q.ev = in;
    if (in.type == MQ_PLATFORM_POINTER_MOVE) {
        q.ev.dx = p->have_last ? in.x - p->last_x : 0.0F;
        q.ev.dy = p->have_last ? in.y - p->last_y : 0.0F;
        p->last_x = in.x; p->last_y = in.y; p->have_last = 1;
        p->mouse_x = in.x; p->mouse_y = in.y;
    } else if (in.type == MQ_PLATFORM_POINTER_DOWN || in.type == MQ_PLATFORM_POINTER_UP) {
        p->mouse_x = in.x; p->mouse_y = in.y;
        p->last_x = in.x; p->last_y = in.y; p->have_last = 1;
        if (in.button == 0) p->mouse_down = in.type == MQ_PLATFORM_POINTER_DOWN ? 1 : 0;
    }
    p->queued_pointers.push_back(q);
}

void queue_key(mq_platform* p, const mq_platform_key_event& in) {
    mq_platform::KEv q{};
    q.ev = in;
    p->queued_keys.push_back(q);
}

} // namespace

/* 无可用显示 → 强制 SDL dummy 驱动。
 *   Linux/其它 UNIX：看 DISPLAY / WAYLAND_DISPLAY（无 X11/Wayland 时 dummy 才安全）。
 *   Windows：没有这两个变量，**不能**据此判 headless，否则真实窗口永远不会创建
 *   （曾经的缺陷：Windows 上恒为 dummy）。仅看 MAQUE_HEADLESS。 */
static bool maque_should_force_headless() {
    const char* hl = std::getenv("MAQUE_HEADLESS");
    if (hl && *hl && *hl != '0') return true;
#if defined(_WIN32) || defined(__ANDROID__)
    return false;
#else
    return !std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY");
#endif
}

extern "C" {

mq_platform* mq_platform_create(const char* title, int w, int h) {
    // 无显示或显式 headless → dummy 视频驱动（headless 安全；必须在 SDL_Init 之前设置）
    if (maque_should_force_headless()) maque::compat::set_env("SDL_VIDEODRIVER", "dummy");

    /* 不让 SDL 接管信号（必须在 SDL_Init 前设置）：SDL 只把 SIGTERM/SIGINT 转成 SDL_QUIT 事件，
     * 无窗口（headless/CI）时没人处理该事件 → 进程杀不掉（CI 超时清理会卡住）。
     * 引擎自己装处理器走优雅退出（runtime 的 on_quit_signal）。 */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    auto* p = new mq_platform();
    p->w = w > 0 ? w : 320;
    p->h = h > 0 ? h : 240;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
        p->sdl = true;
        if (w > 0 && h > 0) {
            /* 强制软件帧缓冲：SDL 不初始化 GL/EGL（渲染插件 wgpu 自己用 EGL，
             * 二者争用会导致 EGL BadAccess —— 多实例时尤其明显） */
            SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
            /* 演示/自动化友好开关（可选）：
             *   MAQUE_WINDOW_POS=x,y    指定窗口位置（默认交给 WM）
             *   MAQUE_ALWAYS_ON_TOP=1   窗口置顶（演示时不被其它窗口遮挡）
             * 窗口**默认可 resize / 可最大化**（SDL_WINDOW_RESIZABLE）：用户拖拽/最大化后
             * SDL 发 SIZE_CHANGED → runtime 自动重建渲染目标（FBO）。 */
            Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
#if defined(__ANDROID__)
            flags |= SDL_WINDOW_VULKAN;
#endif
            if (const char* atop = std::getenv("MAQUE_ALWAYS_ON_TOP"); atop && *atop && *atop != '0')
                flags |= SDL_WINDOW_ALWAYS_ON_TOP;
            const char* max_env = std::getenv("MAQUE_WINDOW_MAXIMIZED");
            const bool start_maximized = max_env && *max_env && *max_env != '0';
            if (start_maximized) flags |= SDL_WINDOW_MAXIMIZED;
            int wx = SDL_WINDOWPOS_UNDEFINED, wy = SDL_WINDOWPOS_UNDEFINED;
            if (const char* pos = std::getenv("MAQUE_WINDOW_POS"))
                std::sscanf(pos, "%d,%d", &wx, &wy);
            p->win = SDL_CreateWindow(title ? title : "maque", wx, wy, w, h, flags);
            p->window = p->win != nullptr;
            if (p->window) {
                std::print("platform: SDL video driver='{}'\n", SDL_GetCurrentVideoDriver());
                /* 某些 WM 忽略创建 flag：显式再最大化一次（幂等），并同步逻辑尺寸 */
                if (start_maximized) SDL_MaximizeWindow(p->win);
                refresh_window_size(p);
            }
        }
        if (p->window) {
#if defined(__ANDROID__)
            const char* text_input = std::getenv("MAQUE_TEXT_INPUT");
            if (text_input && *text_input && *text_input != '0') SDL_StartTextInput();
#else
            SDL_StartTextInput();
#endif
        }
    }
    return p;
}

void mq_platform_destroy(mq_platform* p) {
    if (p && p->frame_tex) { SDL_DestroyTexture(p->frame_tex); p->frame_tex = nullptr; }
    if (p && p->ren) { SDL_DestroyRenderer(p->ren); p->ren = nullptr; }
    if (!p) return;
    if (p->window) SDL_StopTextInput();
    if (p->win) SDL_DestroyWindow(p->win);
    if (p->sdl) SDL_Quit();
    delete p;
}

/* 真实窗口：SDL dummy 驱动也会返回非空窗口指针，故用驱动名判定。 */
/* 原生 SDL 句柄：桌面 ImGui 后端使用 SDL_Renderer；Android 正常路径由 wgpu swapchain 呈现，
 * AVD 功能回退直接写当前 ANativeWindow。 */
void *mq_platform_sdl_window(mq_platform *p) { return p ? (void *)p->win : nullptr; }
void *mq_platform_sdl_renderer(mq_platform *p) { return p ? (void *)p->ren : nullptr; }

/* 原生窗口句柄（plans/18-pbr-3d.md §2.4）：renderer 用它在 wgpu 侧建 surface 做**直接呈现**。
 * 返回 1=可用 0=无窗口/后端不支持（调用方回退离屏 + platform 呈现）。
 *   kind=1 win32 : display=HINSTANCE, handle=HWND
 *   kind=2 xlib  : display=Display*, handle=X11 Window（转 void*）
 *   kind=3 wl      : display=wl_display*, handle=wl_surface*
 *   kind=4 android : display=null, handle=ANativeWindow*
 * 其它子系统（dummy/offscreen/WASM）→ 0。 */
int mq_platform_get_native_window(mq_platform* p, mq_platform_native_window* out) {
    if (!p || !out || !p->win) return 0;
    std::memset(out, 0, sizeof *out);
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(p->win, &wm)) return 0;
    switch (wm.subsystem) {
#if defined(_WIN32)
    case SDL_SYSWM_WINDOWS:
        out->kind = 1;
        out->display = (void*)wm.info.win.hinstance;
        out->handle = (void*)wm.info.win.window;
        return 1;
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    case SDL_SYSWM_X11:
        out->kind = 2;
        out->display = (void*)wm.info.x11.display;
        out->handle = (void*)(uintptr_t)wm.info.x11.window;
        return 1;
    case SDL_SYSWM_WAYLAND:
        out->kind = 3;
        out->display = (void*)wm.info.wl.display;
        out->handle = (void*)wm.info.wl.surface;
        return 1;
#endif
#if defined(__ANDROID__)
    case SDL_SYSWM_ANDROID:
        out->kind = 4;
        out->display = nullptr;
        out->handle = (void*)wm.info.android.window;
        return out->handle ? 1 : 0;
#endif
    default:
        return 0;
    }
}

/* 立即创建 SDL_Renderer：官方 ImGui 后端必须在**第一帧之前**拿到它
 *（否则会先走静态字体图集，之后切后端会撞 ImGui 的动态图集断言）。 */
void *mq_platform_ensure_renderer(mq_platform *p) {
#if defined(__ANDROID__)
    (void)p;
    return nullptr;
#else
    return (void *)ensure_renderer(p);
#endif
}

/* 把游戏帧画进 renderer 后台缓冲（**不 present**）：给"游戏帧 → ImGui pass → 单次 present"
 * 的两阶段呈现用。目标矩形由画布策略给出（缩放/留边）。 */
int mq_platform_draw_rect(mq_platform* p, const void* rgba, int32_t w, int32_t h,
                          int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t filter) {
    return draw_frame_rect(p, rgba, w, h, dx, dy, dw, dh, filter);
}

/* 提交当前后台缓冲（抓帧 + 单次 SDL_RenderPresent）。ImGui 面板画完后调用它。 */
int mq_platform_present_ui(mq_platform *p) { return present_renderer(p); }

int mq_platform_capture_now(mq_platform *p) {
    if (!p || !p->win || !p->window || !p->ren) return 1;
    capture_renderer(p);
    return 0;
}

/* 画 + 立即 present（单阶段便捷接口；自洽，不假设调用方还会叠画其它东西）。 */
int mq_platform_present_rect(mq_platform* p, const void* rgba, int32_t w, int32_t h,
                             int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t filter) {
    const int rc = draw_frame_rect(p, rgba, w, h, dx, dy, dw, dh, filter);
    if (rc != 0) return rc;
    return present_renderer(p);
}

int mq_platform_present(mq_platform* p, const void* rgba, int32_t w, int32_t h) {
    if (!p || !rgba || w <= 0 || h <= 0) return 1;
    if (!p->win || !p->window) return 2;                  // 无窗口：空操作
#if defined(__ANDROID__)
    int ow = 0, oh = 0;
    SDL_GetWindowSizeInPixels(p->win, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 4;
    const float sc = std::min((float)ow / (float)w, (float)oh / (float)h);
    const int dst_w = (int)((float)w * sc), dst_h = (int)((float)h * sc);
    return mq_platform_present_rect(p, rgba, w, h, (ow - dst_w) / 2, (oh - dst_h) / 2,
                                    dst_w, dst_h, -1);
#else
    SDL_Renderer* ren = ensure_renderer(p);
    if (!ren) return 3;                                   // MAQUE_PRESENT=off / renderer 不可用
    /* 目标尺寸 ≠ 窗口尺寸时按等比 + 居中留边缩放（与宿主输入映射同一规则）。 */
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 4;
    const float sc = std::min((float)ow / (float)w, (float)oh / (float)h);
    const int dst_w = (int)((float)w * sc);
    const int dst_h = (int)((float)h * sc);
    const int dst_x = (ow - dst_w) / 2;
    const int dst_y = (oh - dst_h) / 2;
    int filter = -1;                                      // -1 自动
    if (const char* f = std::getenv("MAQUE_PRESENT_FILTER"); f && *f)
        filter = std::strcmp(f, "nearest") == 0 ? 1 : (std::strcmp(f, "linear") == 0 ? 0 : -1);
    return mq_platform_present_rect(p, rgba, w, h, dst_x, dst_y, dst_w, dst_h, filter);
#endif
}

void mq_platform_drawable_size(mq_platform* p, int32_t* out_w, int32_t* out_h) {
    int w = 0, h = 0;
    if (p && p->win) SDL_GetWindowSizeInPixels(p->win, &w, &h);   /* 设备像素（HiDPI 下更大） */
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* 窗口**逻辑**尺寸（用户拖拽/最大化后用户看到的点尺寸）——渲染倍率按它跟随窗口，
 * 与 drawable_size（设备像素）分开：HiDPI 下 target = 逻辑宽 × ss，present 再做设备像素缩放。 */
void mq_platform_window_size(mq_platform* p, int32_t* out_w, int32_t* out_h) {
    int w = 0, h = 0;
    if (p && p->win) SDL_GetWindowSize(p->win, &w, &h);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* 程序化 resize：走 **真实** SDL 窗口路径（SIZE_CHANGED → 平台队列 → runtime 重建渲染目标）。
 * 事件可能异步到达；这里顺手回读并标脏，保证调用方当帧就能看到新尺寸。 */
int32_t mq_platform_set_window_size(mq_platform* p, int32_t w, int32_t h) {
    if (!p || !p->win) return 1;
    if (w <= 0 || h <= 0) return 2;
    SDL_SetWindowSize(p->win, w, h);
    refresh_window_size(p);
    p->resized = 1; p->display_dirty = 1;
    return 0;
}

/* 最大化 / 还原（窗口创建时已带 SDL_WINDOW_RESIZABLE，否则多数 WM 不响应最大化）。 */
int32_t mq_platform_set_window_maximized(mq_platform* p, int32_t on) {
    if (!p || !p->win) return 1;
    if (on) SDL_MaximizeWindow(p->win); else SDL_RestoreWindow(p->win);
    refresh_window_size(p);
    p->resized = 1; p->display_dirty = 1;
    return 0;
}

float mq_platform_content_scale(mq_platform* p) {
    if (!p || !p->win) return 1.0F;
    /* 测试/远程桌面可强制：MAQUE_DPI_SCALE=2 */
    if (const char* env = std::getenv("MAQUE_DPI_SCALE")) {
        const float v = (float)std::atof(env);
        if (v > 0.0F) return v;
    }
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(p->win, &lw, &lh);
    SDL_GetWindowSizeInPixels(p->win, &pw, &ph);
    if (lw > 0 && pw > 0) return (float)pw / (float)lw;
    return 1.0F;
}

/* 应用可见的桌面**逻辑**尺寸。SDL 在 DPI-unaware 进程里看到的就是逻辑分辨率
 * （例：2560x1600 @150% → 1707x1067），所以这个值已经含 OS DPI 缩放 ——
 * UI 按它算"固定大小"即可：同一块屏上不随游戏窗口变化，换屏/换缩放自动适配。 */
void mq_platform_screen_size(mq_platform* p, int32_t* out_w, int32_t* out_h) {
    int w = 0, h = 0;
    if (p && p->sdl) {
        int di = -1;
        if (p->win) di = SDL_GetWindowDisplayIndex(p->win);
        if (di < 0) di = 0;
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(di, &bounds) == 0 && bounds.w > 0 && bounds.h > 0) {
            w = bounds.w; h = bounds.h;
        } else {
            SDL_DisplayMode dm{};
            if (SDL_GetDesktopDisplayMode(di, &dm) == 0) { w = dm.w; h = dm.h; }
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

int mq_platform_display_changed(mq_platform* p) {
    if (!p) return 0;
    const int d = p->display_dirty;
    p->display_dirty = 0;
    return d;
}

void mq_platform_set_safe_area(mq_platform* p, float l, float t, float r, float b) {
    if (!p) return;
    p->safe[0] = l; p->safe[1] = t; p->safe[2] = r; p->safe[3] = b;
}

/* 测试/CI：把指针事件压入 SDL 队列 —— 与真实 OS 事件走**同一条**翻译路径
 * （SDL 事件 → mq_platform_pointer_event → 输入模块 → 命中测试 → 脚本回调）。
 * 用于在没有 XTest/uinput 的环境（Wayland 会话常见）验证输入通路。 */
int mq_platform_simulate_pointer(mq_platform* p, int32_t kind, float x, float y, int32_t button) {
    if (!p || !p->sdl) return 1;
    SDL_Event e{};
    if (kind == 0) {
        e.type = SDL_MOUSEMOTION;
        e.motion.x = (Sint32)x; e.motion.y = (Sint32)y;
        e.motion.which = 0; e.motion.state = 0;
    } else {
        e.type = kind == 1 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.x = (Sint32)x; e.button.y = (Sint32)y;
        e.button.button = (Uint8)(button <= 0 ? SDL_BUTTON_LEFT : button);
        e.button.clicks = 1;
    }
    return SDL_PushEvent(&e) == 1 ? 0 : 2;
}

int mq_platform_has_window(mq_platform* p) {
    if (!p || !p->win || !p->sdl) return 0;
    const char* drv = SDL_GetCurrentVideoDriver();
    return drv && std::strcmp(drv, "dummy") != 0;
}

void mq_platform_pump(mq_platform* p) {
    MQ_TRACY_ZONE_N(platform, "pump");           // Tracy：SDL 事件泵
    if (!p || !p->sdl) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (const char* ti = std::getenv("MAQUE_TRACE_INPUT"); ti && *ti && *ti != '0' && e.type != SDL_MOUSEMOTION)
            std::print("platform: sdl_event type={} ({})\n", (int)e.type,
                       e.type == SDL_MOUSEBUTTONDOWN ? "mousedown" :
                       e.type == SDL_MOUSEBUTTONUP ? "mouseup" :
                       e.type == SDL_MOUSEMOTION ? "mousemotion" :
                       e.type == SDL_WINDOWEVENT ? "windowevent" :
                       e.type == SDL_KEYDOWN ? "keydown" : "other");
        switch (e.type) {
            case SDL_QUIT: p->quit = 1; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                    e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    p->resized = 1; p->display_dirty = 1; p->w = e.window.data1; p->h = e.window.data2;
                }
                /* 最大化/还原：SDL2 在不同平台可能只发 MAXIMIZED/RESTORED（或随 SIZE_CHANGED 一起发）。
                 * 统一标脏并回读窗口逻辑尺寸 → runtime 下一帧重建渲染目标（FBO）。 */
                if (e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                    e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    p->resized = 1; p->display_dirty = 1;
                    refresh_window_size(p);
                }
                /* 换显示器（不同 DPI）同样要求重解析画布（docs/17 多显示器适配） */
                if (e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) p->display_dirty = 1; else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) p->focused = 1;
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) p->focused = 0;
                break;
            case SDL_MOUSEMOTION: {
                mq_platform_pointer_event ev{};
                ev.pointer_id = 0; ev.type = MQ_PLATFORM_POINTER_MOVE;
                ev.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
                ev.x = (float)e.motion.x; ev.y = (float)e.motion.y;
                queue_pointer(p, ev);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                const bool down = e.type == SDL_MOUSEBUTTONDOWN;
                mq_platform_pointer_event ev{};
                ev.pointer_id = 0;
                ev.type = down ? MQ_PLATFORM_POINTER_DOWN : MQ_PLATFORM_POINTER_UP;
                ev.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
                ev.button = e.button.button == SDL_BUTTON_LEFT ? 0
                          : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                ev.x = (float)e.button.x; ev.y = (float)e.button.y;
                ev.pressure = down ? 1.0F : 0.0F;
                queue_pointer(p, ev);
                break;
            }
            case SDL_MOUSEWHEEL: {
                mq_platform_pointer_event ev{};
                ev.pointer_id = 0; ev.type = MQ_PLATFORM_POINTER_WHEEL;
                ev.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
                ev.x = p->mouse_x; ev.y = p->mouse_y;
                ev.scroll_x = (float)e.wheel.preciseX;
                ev.scroll_y = (float)e.wheel.preciseY;
                queue_pointer(p, ev);
                break;
            }
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
            case SDL_FINGERMOTION: {
                mq_platform_pointer_event ev{};
                ev.pointer_id = (uint32_t)(e.tfinger.fingerId + 1);   // 触摸 id 从 1 起（鼠标恒 0）
                ev.type = e.type == SDL_FINGERDOWN ? MQ_PLATFORM_POINTER_DOWN
                        : e.type == SDL_FINGERUP   ? MQ_PLATFORM_POINTER_UP
                                                   : MQ_PLATFORM_POINTER_MOVE;
                ev.pointer_type = MQ_PLATFORM_POINTER_TOUCH;
                ev.x = e.tfinger.x * (float)p->w; ev.y = e.tfinger.y * (float)p->h;
                ev.pressure = e.tfinger.pressure;
                queue_pointer(p, ev);
                break;
            }
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                mq_platform_key_event ev{};
                ev.key = key_from_sdl(e.key.keysym.sym);
                ev.scancode = (uint32_t)e.key.keysym.scancode;
                ev.modifiers = mods_from_sdl((SDL_Keymod)e.key.keysym.mod);
                ev.repeat = e.key.repeat ? 1u : 0u;
                ev.down = e.type == SDL_KEYDOWN ? 1u : 0u;
                queue_key(p, ev);
                break;
            }
            case SDL_TEXTINPUT: {
                mq_platform_key_event ev{};
                ev.key = MQ_KEY_TEXT;
                ev.down = 1;
                std::strncpy(ev.text, e.text.text, sizeof(ev.text) - 1);
                queue_key(p, ev);
                break;
            }
            default: break;
        }
    }
}

uint32_t mq_platform_poll(mq_platform* p, mq_input_frame* out) {
    MQ_TRACY_ZONE_N(platform, "input.poll");     // Tracy：设备事件出队（→ 输入帧）
    if (!out) return 0;
    // 宿主预分配的缓冲与容量跨帧保留
    mq_platform_pointer_event* pbuf = out->pointers; const uint32_t pcap = out->pointer_cap;
    mq_platform_key_event*     kbuf = out->keys;     const uint32_t kcap = out->key_cap;
    std::memset(out, 0, sizeof(*out));
    out->pointers = pbuf; out->pointer_cap = pcap;
    out->keys = kbuf;     out->key_cap = kcap;
    if (!p) { out->quit = 1; return 0; }

    uint32_t total = 0;
    while (!p->queued_pointers.empty()) {
        if (out->pointer_count < out->pointer_cap)
            out->pointers[out->pointer_count++] = p->queued_pointers.front().ev;
        p->queued_pointers.pop_front();
        ++total;
    }
    while (!p->queued_keys.empty()) {
        if (out->key_count < out->key_cap)
            out->keys[out->key_count++] = p->queued_keys.front().ev;
        p->queued_keys.pop_front();
        ++total;
    }
    out->quit = p->quit; out->focused = p->focused; out->resized = p->resized;
    out->mouse_down = p->mouse_down;
    out->mouse_x = p->mouse_x; out->mouse_y = p->mouse_y;
    out->width = p->w; out->height = p->h;
    p->resized = 0;
    return total;
}

void mq_platform_inject_pointer(mq_platform* p, const mq_platform_pointer_event* ev) {
    if (!p || !ev) return;
    if (ev->pointer_type == MQ_PLATFORM_POINTER_TOUCH && ev->pointer_id == 0) {
        mq_platform_pointer_event touch = *ev;      // 触摸 id 规范为 ≥1
        touch.pointer_id = 1;
        queue_pointer(p, touch);
        return;
    }
    queue_pointer(p, *ev);
}

void mq_platform_inject_key(mq_platform* p, const mq_platform_key_event* ev) {
    if (!p || !ev) return;
    queue_key(p, *ev);
}

void mq_platform_set_title(mq_platform* p, const char* title) {
    if (p && p->win && title) SDL_SetWindowTitle(p->win, title);
}

void mq_platform_request_quit(mq_platform* p) { if (p) p->quit = 1; }

/* ---- 进程/工具（docs/29 Tracy）--------------------------------------------
 * ① spawn_detached：F3 启动 Tracy profiler GUI —— 子进程与引擎**无生命周期绑定**，
 *    引擎退出不等待/不杀它（方便"游戏退出后继续看 trace"）。
 * ② process_stats：给 Tracy 基础数据喂 CPU% / 常驻内存。CPU% 是**自上次调用**的差分
 *    （调用方按 1Hz 采样即可）；首个采样返回 0%。
 */
int mq_platform_spawn_detached(mq_platform* p, const char* exe, const char* args) {
    (void)p;
    if (!exe || !*exe) return 1;
#if defined(_WIN32)
    /* CreateProcess 的 lpCommandLine 可写且包含程序名：拼成 `"exe" args`。 */
    std::string cmd = "\"";
    cmd += exe;
    cmd += "\"";
    if (args && *args) { cmd += ' '; cmd += args; }
    std::vector<wchar_t> wcmd(cmd.begin(), cmd.end());
    wcmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return 2;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
#else
    /* POSIX：fork + setsid（脱离会话）→ exec；参数按空白切分（工具路径/参数不含空格引号）。 */
    std::vector<std::string> argv_storage;
    argv_storage.emplace_back(exe);
    if (args && *args) {
        std::string cur;
        for (const char* c = args; ; ++c) {
            if (*c == '\0' || *c == ' ') {
                if (!cur.empty()) { argv_storage.push_back(cur); cur.clear(); }
                if (*c == '\0') break;
            } else cur.push_back(*c);
        }
    }
    std::vector<char*> argv;
    for (auto& s : argv_storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        setsid();
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return 0;
#endif
}

int mq_platform_process_stats(mq_platform* p, double* out_cpu_percent, uint64_t* out_mem_bytes) {
    if (!p) return 1;
    uint64_t cpu_100ns = 0;      /* 累计 CPU 时间，100ns 单位 */
    uint64_t mem_bytes = 0;
    uint64_t wall_ns = 0;
#if defined(_WIN32)
    FILETIME t_create{}, t_exit{}, t_kernel{}, t_user{};
    if (GetProcessTimes(GetCurrentProcess(), &t_create, &t_exit, &t_kernel, &t_user)) {
        auto to_u64 = [](const FILETIME& f) {
            return ((uint64_t)f.dwHighDateTime << 32) | (uint64_t)f.dwLowDateTime;
        };
        cpu_100ns = to_u64(t_kernel) + to_u64(t_user);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) mem_bytes = (uint64_t)pmc.WorkingSetSize;
    uint64_t qpc = 0;
    LARGE_INTEGER li{};
    if (QueryPerformanceCounter(&li)) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart > 0) {
            qpc = (uint64_t)li.QuadPart;
            wall_ns = (uint64_t)((double)qpc * 1e9 / (double)freq.QuadPart);
        }
    }
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        const uint64_t us = (uint64_t)ru.ru_utime.tv_sec * 1000000ull + (uint64_t)ru.ru_utime.tv_usec +
                            (uint64_t)ru.ru_stime.tv_sec * 1000000ull + (uint64_t)ru.ru_stime.tv_usec;
        cpu_100ns = us * 10ull;
    }
    /* 常驻内存：Linux 读 /proc/self/statm（RSS 页数）；其它平台退回 ru_maxrss（KB）。 */
    long rss_pages = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        long size = 0;
        if (std::fscanf(f, "%ld %ld", &size, &rss_pages) != 2) rss_pages = 0;
        std::fclose(f);
        if (rss_pages > 0) mem_bytes = (uint64_t)rss_pages * (uint64_t)sysconf(_SC_PAGESIZE);
    }
    if (mem_bytes == 0) mem_bytes = (uint64_t)ru.ru_maxrss * 1024ull;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wall_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
    double pct = 0.0;
    if (p->ps_prev_wall_ns != 0 && wall_ns > p->ps_prev_wall_ns) {
        const double wall_s = (double)(wall_ns - p->ps_prev_wall_ns) / 1e9;
        const double cpu_s = (double)(cpu_100ns - p->ps_prev_cpu_100ns) / 1e7;   /* 100ns → s */
        if (wall_s > 0.0) pct = cpu_s / wall_s * 100.0;
        if (pct < 0.0) pct = 0.0;
    }
    p->ps_prev_cpu_100ns = cpu_100ns;
    p->ps_prev_wall_ns = wall_ns;
    if (out_cpu_percent) *out_cpu_percent = pct;
    if (out_mem_bytes) *out_mem_bytes = mem_bytes;
    return 0;
}

} // extern "C"
