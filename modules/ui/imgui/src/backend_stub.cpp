#include "ui/mq_imgui.h"

extern "C" {

int mq_imgui_backend_init(void*) { return 2; }
void mq_imgui_backend_shutdown(void) {}
void mq_imgui_backend_new_frame(void) {}
void mq_imgui_backend_render(void) {}
int mq_imgui_backend_active(void) { return 0; }

} // extern "C"
