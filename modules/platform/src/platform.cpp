#include "core/platform/mq_platform.h"

#include <SDL.h>
#include <cstdlib>
#include <cstring>

struct mq_platform {
    SDL_Window* win = nullptr;
    bool        sdl = false;
};

extern "C" {

mq_platform* mq_platform_create(const char* title, int w, int h) {
    // 无显示 → dummy 视频驱动（headless 安全；必须在 SDL_Init 之前设置）
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        setenv("SDL_VIDEODRIVER", "dummy", 0);

    auto* p = new mq_platform();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
        p->sdl = true;
        p->win = SDL_CreateWindow(title ? title : "maque",
                                  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  w, h, SDL_WINDOW_SHOWN);
    }
    return p;
}

void mq_platform_destroy(mq_platform* p) {
    if (!p) return;
    if (p->win) SDL_DestroyWindow(p->win);
    if (p->sdl) SDL_Quit();
    delete p;
}

void mq_platform_poll(mq_platform* p, mq_input_state* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!p) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        out->events++;
        switch (e.type) {
            case SDL_QUIT: out->quit = 1; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_SPACE)  out->key_space = 1;
                if (e.key.keysym.sym == SDLK_ESCAPE) out->key_escape = 1;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) out->mouse_down = 1;
                break;
            default: break;
        }
    }
    int mx = 0, my = 0;
    Uint32 m = SDL_GetMouseState(&mx, &my);
    out->mouse_x = (float)mx;
    out->mouse_y = (float)my;
    if (m & SDL_BUTTON(SDL_BUTTON_LEFT)) out->mouse_down = 1;
}

} // extern "C"
