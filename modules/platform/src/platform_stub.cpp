#include "core/platform/mq_platform.h"

#include <cstring>

struct mq_platform {
    int width = 0;
    int height = 0;
    int quit = 0;
    float safe[4] = { 0, 0, 0, 0 };
};

extern "C" {

mq_platform* mq_platform_create(const char*, int w, int h) {
    auto* p = new mq_platform();
    p->width = w;
    p->height = h;
    return p;
}

void mq_platform_destroy(mq_platform* p) { delete p; }
int mq_platform_has_window(mq_platform*) { return 0; }
void mq_platform_pump(mq_platform*) {}

uint32_t mq_platform_poll(mq_platform* p, mq_input_frame* out) {
    if (!out) return 0;
    mq_platform_pointer_event* pointers = out->pointers;
    mq_platform_key_event* keys = out->keys;
    const uint32_t pointer_cap = out->pointer_cap;
    const uint32_t key_cap = out->key_cap;
    std::memset(out, 0, sizeof(*out));
    out->pointers = pointers;
    out->keys = keys;
    out->pointer_cap = pointer_cap;
    out->key_cap = key_cap;
    if (p) out->quit = p->quit;
    return 0;
}

int mq_platform_present(mq_platform*, const void*, int32_t, int32_t) { return 0; }
int mq_platform_draw_rect(mq_platform*, const void*, int32_t, int32_t,
                          int32_t, int32_t, int32_t, int32_t, int32_t) { return 0; }
int mq_platform_present_ui(mq_platform*) { return 0; }
int mq_platform_capture_now(mq_platform*) { return 1; }
int mq_platform_present_rect(mq_platform*, const void*, int32_t, int32_t,
                             int32_t, int32_t, int32_t, int32_t, int32_t) { return 0; }

void mq_platform_drawable_size(mq_platform*, int32_t* w, int32_t* h) {
    if (w) *w = 0;
    if (h) *h = 0;
}
void mq_platform_window_size(mq_platform*, int32_t* w, int32_t* h) {
    if (w) *w = 0;
    if (h) *h = 0;
}
int32_t mq_platform_set_window_size(mq_platform*, int32_t, int32_t) { return 1; }
int32_t mq_platform_set_window_maximized(mq_platform*, int32_t) { return 1; }
float mq_platform_content_scale(mq_platform*) { return 1.0F; }
void mq_platform_screen_size(mq_platform*, int32_t* w, int32_t* h) {
    if (w) *w = 0;
    if (h) *h = 0;
}
int mq_platform_display_changed(mq_platform*) { return 0; }
void* mq_platform_sdl_window(mq_platform*) { return nullptr; }
void* mq_platform_sdl_renderer(mq_platform*) { return nullptr; }
void* mq_platform_ensure_renderer(mq_platform*) { return nullptr; }
int mq_platform_get_native_window(mq_platform*, mq_platform_native_window* out) {
    if (out) std::memset(out, 0, sizeof(*out));
    return 0;
}
void mq_platform_set_safe_area(mq_platform* p, float l, float t, float r, float b) {
    if (!p) return;
    p->safe[0] = l;
    p->safe[1] = t;
    p->safe[2] = r;
    p->safe[3] = b;
}

int mq_platform_simulate_pointer(mq_platform*, int32_t, float, float, int32_t) { return 1; }
int mq_platform_spawn_detached(mq_platform*, const char*, const char*) { return 1; }
int mq_platform_process_stats(mq_platform*, double* cpu, uint64_t* memory) {
    if (cpu) *cpu = 0.0;
    if (memory) *memory = 0;
    return 0;
}
void mq_platform_inject_pointer(mq_platform*, const mq_platform_pointer_event*) {}
void mq_platform_inject_key(mq_platform*, const mq_platform_key_event*) {}
void mq_platform_set_title(mq_platform*, const char*) {}
void mq_platform_request_quit(mq_platform* p) { if (p) p->quit = 1; }

} // extern "C"
