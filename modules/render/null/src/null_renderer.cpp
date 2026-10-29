#include "render/null/mq_renderer_null.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct NullRenderer {
    std::vector<uint8_t> textures;
    std::vector<uint8_t> meshes;
    std::vector<uint8_t> shader_passes;
};

uint8_t color_byte(float value) {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<uint8_t>(clamped * 255.0F + 0.5F);
}

int32_t fill(int32_t width, int32_t height, const float* bg, uint8_t* out, uint32_t out_size) {
    if (!out) return 0;
    if (width <= 0 || height <= 0) return 1;
    const uint64_t needed = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u;
    if (needed > out_size) return 2;
    const uint8_t pixel[4] = {
        color_byte(bg ? bg[0] : 0.0F),
        color_byte(bg ? bg[1] : 0.0F),
        color_byte(bg ? bg[2] : 0.0F),
        color_byte(bg ? bg[3] : 1.0F),
    };
    for (uint64_t i = 0; i < needed; i += 4) std::memcpy(out + i, pixel, sizeof pixel);
    return 0;
}

template <class T>
int32_t allocate_handle(std::vector<T>& slots) {
    slots.push_back(1);
    return static_cast<int32_t>(slots.size() - 1);
}

template <class T>
bool valid_handle(const std::vector<T>& slots, int32_t id) {
    return id >= 0 && static_cast<size_t>(id) < slots.size() && slots[static_cast<size_t>(id)] != 0;
}

void* null_create(const char** out_backend) {
    static constexpr char kName[] = "null";
    if (out_backend) *out_backend = kName;
    return new NullRenderer();
}

void null_destroy(void* self) { delete static_cast<NullRenderer*>(self); }

void null_destroy_texture(void* self, int32_t id) {
    auto* r = static_cast<NullRenderer*>(self);
    if (r && valid_handle(r->textures, id)) r->textures[static_cast<size_t>(id)] = 0;
}

int32_t null_create_texture(void* self, const uint8_t* rgba, int32_t w, int32_t h) {
    auto* r = static_cast<NullRenderer*>(self);
    if (!r || !rgba || w <= 0 || h <= 0) return -1;
    return allocate_handle(r->textures);
}

int32_t null_has_texture(void* self, int32_t id) {
    auto* r = static_cast<NullRenderer*>(self);
    return r && valid_handle(r->textures, id) ? 1 : 0;
}

int32_t null_render_views(void*, int32_t w, int32_t h,
                          const mq_renderer_view*, uint32_t,
                          const mq_renderer_item*, uint32_t,
                          const float* bg, uint8_t* out, uint32_t out_size) {
    return fill(w, h, bg, out, out_size);
}

int32_t null_render_mesh_views(void*, int32_t w, int32_t h,
                               const mq_renderer_view*, uint32_t,
                               const mq_renderer_item*, uint32_t,
                               const mq_renderer_mesh*, uint32_t,
                               const float* bg, uint8_t* out, uint32_t out_size) {
    return fill(w, h, bg, out, out_size);
}

int32_t null_attach_window(void*, const mq_renderer_window*) { return -1; }
int32_t null_resize_surface(void*, int32_t, int32_t) { return -1; }

int32_t null_caps(void*, mq_renderer_caps* out) {
    if (!out) return 1;
    std::memset(out, 0, sizeof(*out));
    out->max_texture_size = 16384;
    out->max_lights = 8;
    out->max_samples = 1;
    return 0;
}

int32_t null_create_texture2(void* self, const mq_renderer_texture_desc* desc, int32_t* out_id) {
    auto* r = static_cast<NullRenderer*>(self);
    if (out_id) *out_id = -1;
    if (!r || !desc || !out_id || desc->width <= 0 || desc->height <= 0) return 1;
    *out_id = allocate_handle(r->textures);
    return 0;
}

int32_t null_create_mesh(void* self, const mq_renderer_mesh_gpu* data, int32_t* out_id) {
    auto* r = static_cast<NullRenderer*>(self);
    if (out_id) *out_id = -1;
    if (!r || !data || !out_id) return 1;
    *out_id = allocate_handle(r->meshes);
    return 0;
}

void null_destroy_mesh(void* self, int32_t id) {
    auto* r = static_cast<NullRenderer*>(self);
    if (r && valid_handle(r->meshes, id)) r->meshes[static_cast<size_t>(id)] = 0;
}

int32_t null_render_frame3d(void*, const mq_renderer_frame3d* frame) {
    if (!frame) return 1;
    return fill(frame->width, frame->height, frame->bg, frame->out, frame->out_size);
}

int32_t null_present(void*) { return 0; }

int32_t null_create_shader_pass(void* self, const char* wgsl, int32_t* out_id) {
    auto* r = static_cast<NullRenderer*>(self);
    if (out_id) *out_id = -1;
    if (!r || !wgsl || !*wgsl || !out_id) return 1;
    *out_id = allocate_handle(r->shader_passes);
    return 0;
}

void null_destroy_shader_pass(void* self, int32_t id) {
    auto* r = static_cast<NullRenderer*>(self);
    if (r && valid_handle(r->shader_passes, id)) r->shader_passes[static_cast<size_t>(id)] = 0;
}

int32_t null_update_mesh(void* self, int32_t id, const void* vertices, uint64_t vertex_bytes) {
    auto* r = static_cast<NullRenderer*>(self);
    return r && valid_handle(r->meshes, id) && vertices && vertex_bytes > 0 ? 0 : 1;
}

const mq_renderer_v1 kApi = {
    { static_cast<uint32_t>(sizeof(mq_renderer_v1)), MQ_ABI_VERSION },
    null_create,
    null_destroy,
    null_destroy_texture,
    null_create_texture,
    null_has_texture,
    null_render_views,
    null_render_mesh_views,
    null_attach_window,
    null_resize_surface,
    null_caps,
    null_create_texture2,
    null_create_mesh,
    null_destroy_mesh,
    null_render_frame3d,
    null_present,
    null_create_shader_pass,
    null_destroy_shader_pass,
    null_update_mesh,
};

} // namespace

extern "C" const mq_renderer_v1* mq_renderer_null_api(void) { return &kApi; }
