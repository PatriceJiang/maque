/* 平台层实现（SDL2）：窗口 + 设备原始事件的**归一化采集**。
 * 事件先入内部队列（pump），在 poll 时统一消费（docs/19 §3：采样与消费分离）。
 * 注入（inject_*）与设备事件走同一队列 → AI/CI/回放与真机路径一致（docs/19 §9）。
 * 指针位置/按键状态只由事件流推导（不查设备状态）→ 顺序与回放确定。 */
#include "core/platform/mq_platform.h"
#include "core/compat/mq_env.h"   // setenv/unsetenv 的跨平台封装（Windows 无 setenv）

#include <print>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include <SDL.h>

#include <deque>
#include <cstdlib>
#include <cstring>

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
    SDL_Renderer* ren = nullptr;      /* 呈现用（懒创建）；无窗口/off 时为 null */
    SDL_Texture*  tex = nullptr;      /* 流式帧纹理 */
    int tex_w = 0, tex_h = 0;
    int present_mode = -1;            /* -1 未定；0 off；1 software；2 accelerated */
    bool present_logged = false;
    int  logical_w = 0, logical_h = 0;
    bool cap_pending = false;
    int  cap_max = -1, cap_every = 1, cap_seen = 0;
    float safe[4] = { 0, 0, 0, 0 };      /* 安全区内缩（设备像素） */
    int  display_dirty = 0;              /* 尺寸/显示器变化待消费 */
    int  cap_count = 0;               /* MAQUE_CAPTURE 已抓帧数 */
    bool        sdl = false;
    bool        window = false;
    bool        quit = false;
    int         w = 0, h = 0;
    float       mouse_x = 0, mouse_y = 0;
    int         mouse_down = 0;
    int         focused = 1;
    int         resized = 0;

    struct PEv { mq_platform_pointer_event ev; };
    struct KEv { mq_platform_key_event ev; };
    std::deque<PEv> queued_pointers;
    std::deque<KEv> queued_keys;
    float last_x = 0, last_y = 0;
    int   have_last = 0;
};

namespace {

/* 入队并更新指针状态（设备与注入共用；delta 在此统一计算）。 */
/* 抓帧：回读**窗口表面**（真正呈现给用户的像素，含画布留边）→ PPM */
static void capture_window(mq_platform* p) {
    if (p->cap_max < 0) {
        p->cap_max = 3; p->cap_every = 1;
        if (const char* v = std::getenv("MAQUE_CAPTURE_FRAMES")) p->cap_max = std::atoi(v);
        if (const char* v = std::getenv("MAQUE_CAPTURE_EVERY")) p->cap_every = std::atoi(v) > 0 ? std::atoi(v) : 1;
    }
    ++p->cap_seen;
    const char* cap = std::getenv("MAQUE_CAPTURE");
    if (!cap || !*cap || p->cap_count >= p->cap_max || (p->cap_seen % p->cap_every) != 0) return;
    SDL_Surface* ws = SDL_GetWindowSurface(p->win);
    if (!ws || SDL_LockSurface(ws) != 0) { ++p->cap_count; return; }
    char path[512];
    std::snprintf(path, sizeof path, "%s_%d.ppm", cap, p->cap_count);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", ws->w, ws->h);
        const SDL_PixelFormat* fmt = ws->format;
        for (int y = 0; y < ws->h; ++y) {
            const uint8_t* row = (const uint8_t*)ws->pixels + (size_t)y * ws->pitch;
            for (int x = 0; x < ws->w; ++x) {
                uint32_t v = 0;
                std::memcpy(&v, row + (size_t)x * fmt->BytesPerPixel, (size_t)fmt->BytesPerPixel);
                uint8_t r = 0, g = 0, b = 0, a = 0;
                SDL_GetRGBA(v, fmt, &r, &g, &b, &a);
                const uint8_t rgb[3] = { r, g, b };
                std::fwrite(rgb, 1, 3, f);
            }
        }
        std::fclose(f);
    }
    SDL_UnlockSurface(ws);
    ++p->cap_count;
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
#if defined(_WIN32)
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
             *   MAQUE_ALWAYS_ON_TOP=1   窗口置顶（演示时不被其它窗口遮挡） */
            Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
            if (const char* atop = std::getenv("MAQUE_ALWAYS_ON_TOP"); atop && *atop && *atop != '0')
                flags |= SDL_WINDOW_ALWAYS_ON_TOP;
            int wx = SDL_WINDOWPOS_UNDEFINED, wy = SDL_WINDOWPOS_UNDEFINED;
            if (const char* pos = std::getenv("MAQUE_WINDOW_POS"))
                std::sscanf(pos, "%d,%d", &wx, &wy);
            p->win = SDL_CreateWindow(title ? title : "maque", wx, wy, w, h, flags);
            p->window = p->win != nullptr;
            if (p->window) std::print("platform: SDL video driver='{}'\n", SDL_GetCurrentVideoDriver());
        }
        if (p->window) SDL_StartTextInput();
    }
    return p;
}

void mq_platform_destroy(mq_platform* p) {
    if (p && p->tex) { SDL_DestroyTexture(p->tex); p->tex = nullptr; }
    if (p && p->ren) { SDL_DestroyRenderer(p->ren); p->ren = nullptr; }
    if (!p) return;
    if (p->window) SDL_StopTextInput();
    if (p->win) SDL_DestroyWindow(p->win);
    if (p->sdl) SDL_Quit();
    delete p;
}

/* 真实窗口：SDL dummy 驱动也会返回非空窗口指针，故用驱动名判定。 */
/* 把 RGBA8 帧呈现到窗口（docs/05、M5）。
 *   · 无窗口 / dummy 驱动 / MAQUE_PRESENT=off → 安全空操作（headless/CI 不报错）
 *   · MAQUE_PRESENT=software|accelerated 可强制后端；默认 accelerated → software 回退
 * 呈现失败不致命（视为无窗口），避免把渲染问题升级成"跑不起来"。 */
/* 原生句柄：给官方 ImGui 后端用（它必须拿到真的 SDL_Window/SDL_Renderer）。 */
void *mq_platform_sdl_window(mq_platform *p) { return p ? (void *)p->win : nullptr; }
void *mq_platform_sdl_renderer(mq_platform *p) { return p ? (void *)p->ren : nullptr; }

/* 立即创建 SDL_Renderer：官方 ImGui 后端必须在**第一帧之前**拿到它
 *（否则会先走静态字体图集，之后切后端会撞 ImGui 的动态图集断言）。 */
void *mq_platform_ensure_renderer(mq_platform *p) {
    if (!p || !p->window || !p->win || p->present_mode == 0) return nullptr;
    if (!p->ren) {
        if (p->present_mode == 1) p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_SOFTWARE);
        else p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_ACCELERATED);
        if (!p->ren) p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_SOFTWARE);
    }
    return p->ren;
}

int mq_platform_present_rect(mq_platform* p, const void* rgba, int32_t w, int32_t h,
                             int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t filter) {
    if (!p || !rgba || w <= 0 || h <= 0) return 1;
    if (!p->win || !p->window) return 2;
    if (p->present_mode < 0) { mq_platform_present(p, rgba, w, h); return 0; }   /* 触发模式初始化 */
    /* 复用软件窗口表面路径：目标矩形由调用方（画布策略）给出 */
    SDL_Surface* ws = SDL_GetWindowSurface(p->win);
    if (!ws) return 4;
    SDL_Surface* src = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<void*>(rgba), w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!src) return 5;
    SDL_Rect dst{ dx, dy, dw, dh };
    SDL_FillRect(ws, nullptr, SDL_MapRGB(ws->format, 0, 0, 0));                 // 画布外为黑
    int rc = 0;
    const bool nearest = filter == 1 || (filter < 0 && dw % w == 0 && dw / w >= 2 && dw / w == dh / h);
    if (nearest && dw % w == 0) {
        const int k = dw / w;
        SDL_Surface* big = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_RGBA32);
        if (big) {
            const uint8_t* sp = (const uint8_t*)rgba;
            for (int y = 0; y < dh; ++y) {
                const uint8_t* srow = sp + (size_t)std::min(h - 1, y / k) * (size_t)w * 4;
                uint8_t* drow = (uint8_t*)big->pixels + (size_t)y * (size_t)big->pitch;
                for (int x = 0; x < dw; ++x)
                    std::memcpy(drow + (size_t)x * 4, srow + (size_t)std::min(w - 1, x / k) * 4, 4);
            }
            rc = SDL_BlitSurface(big, nullptr, ws, &dst);
            SDL_FreeSurface(big);
        } else rc = -1;
    } else {
        rc = SDL_BlitScaled(src, nullptr, ws, &dst);
    }
    SDL_FreeSurface(src);
    if (rc != 0) return 6;
    SDL_UpdateWindowSurface(p->win);
    capture_window(p);
    return 0;
}

int mq_platform_present(mq_platform* p, const void* rgba, int32_t w, int32_t h) {
    if (!p || !rgba || w <= 0 || h <= 0) return 1;
    if (!p->win || !p->window) return 2;                  // 无窗口：空操作
    if (p->present_mode < 0) {
        /* 默认 **软件** 呈现：帧本来就在 CPU（渲染插件回读），软件 blit 无需 GL 上下文，
         * 避免与 wgpu(EGL) 争用 —— 多实例/多窗口下 GL 上下文会 BadAccess。
         * MAQUE_PRESENT=accelerated 可切回 GPU blit；=off 关闭呈现。 */
        p->present_mode = 1;   /* 默认 software（见下）；accelerated 需显式 MAQUE_PRESENT */
        const char* mode = std::getenv("MAQUE_PRESENT");
        if (mode && *mode) {
            if (std::strcmp(mode, "off") == 0) p->present_mode = 0;
            else if (std::strcmp(mode, "accelerated") == 0) p->present_mode = 2;
        }
    }
    if (p->present_mode == 0) return 3;                    // 显式关闭
    if (p->present_mode == 1) {
        /* 纯软件路径：写入窗口表面（X11 下为 XShm/XImage）——不涉及 GL/EGL。
         * 目标尺寸 ≠ 窗口尺寸时按等比 + 居中留边缩放（与宿主输入映射同一规则）。 */
        SDL_Surface* ws = SDL_GetWindowSurface(p->win);
        if (!ws) return 4;
        SDL_Surface* src = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<void*>(rgba), w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
        if (!src) return 5;
        const float sc = std::min((float)ws->w / (float)w, (float)ws->h / (float)h);
        SDL_Rect dst;
        dst.w = (int)((float)w * sc);
        dst.h = (int)((float)h * sc);
        dst.x = (ws->w - dst.w) / 2;
        dst.y = (ws->h - dst.h) / 2;
        SDL_FillRect(ws, nullptr, SDL_MapRGB(ws->format, 0, 0, 0));   // 留边黑
        /* 放大过滤：**整数倍**时默认最近邻（pixel-perfect：字形/像素画放大后仍是硬边），
         * MAQUE_PRESENT_FILTER=linear|nearest 可强制；非整数倍只能线性缩放。 */
        int filter = -1;                                   // -1 自动
        if (const char* f = std::getenv("MAQUE_PRESENT_FILTER"); f && *f)
            filter = std::strcmp(f, "nearest") == 0 ? 1 : (std::strcmp(f, "linear") == 0 ? 0 : -1);
        const bool integer_scale = dst.w % w == 0 && dst.h % h == 0 && dst.w / w == dst.h / h && dst.w / w >= 2;
        const bool nearest = filter == 1 || (filter == -1 && integer_scale);
        int rc = 0;
        if (nearest && dst.w % w == 0) {
            const int k = dst.w / w;                       // 整数倍：逐像素复制（无插值）
            SDL_Surface* big = SDL_CreateRGBSurfaceWithFormat(0, dst.w, dst.h, 32, SDL_PIXELFORMAT_RGBA32);
            if (big) {
                const uint8_t* sp = (const uint8_t*)rgba;
                for (int y = 0; y < dst.h; ++y) {
                    const uint8_t* srow = sp + (size_t)(y / k) * (size_t)w * 4;
                    uint8_t* drow = (uint8_t*)big->pixels + (size_t)y * (size_t)big->pitch;
                    for (int x = 0; x < dst.w; ++x)
                        std::memcpy(drow + (size_t)x * 4, srow + (size_t)(x / k) * 4, 4);
                }
                rc = SDL_BlitSurface(big, nullptr, ws, &dst);
                SDL_FreeSurface(big);
            } else rc = -1;
        } else {
            rc = SDL_BlitScaled(src, nullptr, ws, &dst);
        }
        SDL_FreeSurface(src);
        if (rc != 0) return 6;
        if (!p->present_logged) {
            std::print("platform: present via window surface (software) target={}x{} window={}x{}\n",
                       w, h, ws->w, ws->h);
            p->present_logged = true;
        }
        if (p->cap_pending) { /* 抓帧见下（软件路径从源帧拷贝，内容一致） */ }
        SDL_UpdateWindowSurface(p->win);
        capture_window(p);
        return 0;
    }
    if (!p->ren) {
        /* SDL2：第二个参数是驱动索引（-1 = 自动）；用 flags 指定软件/加速 */
        if (p->present_mode == 1) p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_SOFTWARE);
        else p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_ACCELERATED);
        if (!p->ren) p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_SOFTWARE);
        if (!p->ren) { p->window = false; return 4; }       // 呈现不可用 → 降级为无窗口
    }
    if (!p->tex || p->tex_w != w || p->tex_h != h) {
        if (p->tex) SDL_DestroyTexture(p->tex);
        p->tex = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        p->tex_w = w; p->tex_h = h;
        if (!p->tex) return 5;
    }
    if (!p->present_logged) {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(p->ren, &info) == 0)
            std::print("platform: present via renderer='{}' flags={:#x} size={}x{}\n",
                       info.name ? info.name : "?", (unsigned)info.flags, w, h);
        p->present_logged = true;
    }
    if (p->logical_w != w || p->logical_h != h) {
        SDL_RenderSetLogicalSize(p->ren, w, h);   // 等比缩放 + 居中留边（与宿主输入映射同规则）
        p->logical_w = w; p->logical_h = h;
    }
    if (SDL_UpdateTexture(p->tex, nullptr, rgba, w * 4) != 0) return 6;
    SDL_RenderClear(p->ren);
    SDL_RenderCopy(p->ren, p->tex, nullptr, nullptr);       // 目标尺寸不同则自动缩放
    /* 可选：从**呈现缓冲**回读抓帧（MAQUE_CAPTURE=<前缀>，取前 N 帧）
     *   —— 用于在真实窗口路径上做像素级验证（CI/无合成器读取手段时尤其有用） */
    if (const char* cap = std::getenv("MAQUE_CAPTURE"); cap && *cap && p->cap_count < 3) {
        std::vector<uint8_t> px((size_t)w * h * 4);
        if (SDL_RenderReadPixels(p->ren, nullptr, SDL_PIXELFORMAT_RGBA32, px.data(), w * 4) == 0) {
            char path[512];
            std::snprintf(path, sizeof path, "%s_%d.ppm", cap, p->cap_count);
            FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int i = 0; i < w * h; ++i) std::fwrite(&px[(size_t)i * 4], 1, 3, f);
                std::fclose(f);
            }
            ++p->cap_count;
        }
    }
    SDL_RenderPresent(p->ren);
    return 0;
}

void mq_platform_drawable_size(mq_platform* p, int32_t* out_w, int32_t* out_h) {
    int w = 0, h = 0;
    if (p && p->win) SDL_GetWindowSizeInPixels(p->win, &w, &h);   /* 设备像素（HiDPI 下更大） */
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
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

} // extern "C"
