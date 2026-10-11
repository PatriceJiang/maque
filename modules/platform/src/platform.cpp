/* 平台层实现（SDL2）：窗口 + 设备原始事件的**归一化采集**。
 * 事件先入内部队列（pump），在 poll 时统一消费（docs/19 §3：采样与消费分离）。
 * 注入（inject_*）与设备事件走同一队列 → AI/CI/回放与真机路径一致（docs/19 §9）。
 * 指针位置/按键状态只由事件流推导（不查设备状态）→ 顺序与回放确定。 */
#include "core/platform/mq_platform.h"

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
        default: break;
    }
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 32);
    if (k >= 32 && k < 127) return (uint32_t)k;
    return MQ_KEY_UNKNOWN;
}

} // namespace

struct mq_platform {
    SDL_Window* win = nullptr;
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

extern "C" {

mq_platform* mq_platform_create(const char* title, int w, int h) {
    // 无显示或显式 headless → dummy 视频驱动（headless 安全；必须在 SDL_Init 之前设置）
    const char* hl = std::getenv("MAQUE_HEADLESS");
    if ((!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) || (hl && *hl && *hl != '0'))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    auto* p = new mq_platform();
    p->w = w > 0 ? w : 320;
    p->h = h > 0 ? h : 240;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
        p->sdl = true;
        if (w > 0 && h > 0) {
            p->win = SDL_CreateWindow(title ? title : "maque",
                                      SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                      w, h, SDL_WINDOW_SHOWN);
            p->window = p->win != nullptr;
        }
        if (p->window) SDL_StartTextInput();
    }
    return p;
}

void mq_platform_destroy(mq_platform* p) {
    if (!p) return;
    if (p->window) SDL_StopTextInput();
    if (p->win) SDL_DestroyWindow(p->win);
    if (p->sdl) SDL_Quit();
    delete p;
}

/* 真实窗口：SDL dummy 驱动也会返回非空窗口指针，故用驱动名判定。 */
int mq_platform_has_window(mq_platform* p) {
    if (!p || !p->win || !p->sdl) return 0;
    const char* drv = SDL_GetCurrentVideoDriver();
    return drv && std::strcmp(drv, "dummy") != 0;
}

void mq_platform_pump(mq_platform* p) {
    if (!p || !p->sdl) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: p->quit = 1; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                    e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    p->resized = 1; p->w = e.window.data1; p->h = e.window.data2;
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) p->focused = 1;
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
