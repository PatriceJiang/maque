/* 控制面 · 字体图集与抓帧（plans/17 面板数据源；二进制走 base64 分块）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

/* Types.describe：反射自省（docs/06/11 A1）——类型/字段/属性导出给 AI/工具。 */
/* ---- base64 编码（控制面是行分隔 JSON → 二进制走 base64 分块；零第三方依赖）---- */
std::string b64_encode(const uint8_t* data, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)data[i] << 16 |
                           (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) |
                           (i + 2 < n ? (uint32_t)data[i + 2] : 0);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += i + 1 < n ? T[(v >> 6) & 63] : '=';
        out += i + 2 < n ? T[v & 63] : '=';
    }
    return out;
}

/* Font.info：所有已栅格化的 (字体, 字号) 图集（inspector 字体面板的数据源）*/
extern "C" int mq_control_Font_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    size_t n = 0;
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "[");
    bool first_font = true;
    for (size_t fi = 0; fi < eng->fonts.size(); ++fi) {
        mq_font* f = eng->fonts[fi].font;
        if (!f) continue;
        /* 字号集合：从图集纹理表里取（= 实际排版用过的字号） */
        bool first = true;
        for (const auto& fa : eng->font_atlases) {
            if (fa.font != f) continue;
            uint32_t aw = 0, ah = 0;
            mq_font_atlas_size(f, fa.size, &aw, &ah);
            const uint32_t glyphs = mq_font_glyph_count(f, fa.size);
            if (first) {
                n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0,
                                           "%s{\"path\":\"%s\",\"primary\":%s,\"atlases\":[",
                                           first_font ? "" : ",", eng->fonts[fi].path.c_str(),
                                           fi == 0 ? "true" : "false");
                first_font = false;
            }
            n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0,
                                       "%s{\"index\":%zu,\"size\":%.1f,\"w\":%u,\"h\":%u,"
                                       "\"glyphs\":%u,\"generation\":%u,\"texture\":%d}",
                                       first ? "" : ",", fi, (double)fa.size, aw, ah, glyphs,
                                       mq_font_atlas_generation(f, fa.size), fa.texture);
            first = false;
        }
        if (!first) n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]}");
    }
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]");
    return 0;
}

/* Font.glyphs：字形列表（码点 + 图集矩形）——面板据此画格线与悬浮信息 */
extern "C" int mq_control_Font_glyphs(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int idx = (int)mq_json_num(mq_json_get(params, "index"), 0);
    const float size = (float)mq_json_num(mq_json_get(params, "size"), 0);
    if (idx < 0 || (size_t)idx >= eng->fonts.size() || !eng->fonts[(size_t)idx].font) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    mq_font* f = eng->fonts[(size_t)idx].font;
    const uint32_t cnt = mq_font_glyph_count(f, size);
    size_t n = 0;
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "{\"ok\":true,\"glyphs\":[");
    for (uint32_t i = 0; i < cnt && n + 128 < cap; ++i) {
        mq_text_glyph g{};
        if (mq_font_glyph_at(f, size, i, &g) != MQ_TEXT_OK) continue;
        n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0,
                                   "%s{\"cp\":%u,\"ax\":%d,\"ay\":%d,\"w\":%d,\"h\":%d}",
                                   i ? "," : "", g.codepoint, g.ax, g.ay, (int)g.w, (int)g.h);
    }
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]}");
    return 0;
}

/* Font.atlas：图集像素（PPM P6 → base64 分块；offset/max 控制单次大小）*/
extern "C" int mq_control_Font_atlas(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int idx = (int)mq_json_num(mq_json_get(params, "index"), 0);
    const float size = (float)mq_json_num(mq_json_get(params, "size"), 0);
    const size_t offset = (size_t)mq_json_num(mq_json_get(params, "offset"), 0);
    size_t max_bytes = (size_t)mq_json_num(mq_json_get(params, "max"), 24 * 1024);
    if (max_bytes == 0 || max_bytes > (1u << 20)) max_bytes = 24 * 1024;   /* 允许单次传整图（≤1MB） */
    if (idx < 0 || (size_t)idx >= eng->fonts.size() || !eng->fonts[(size_t)idx].font) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    mq_font* f = eng->fonts[(size_t)idx].font;
    uint32_t aw = 0, ah = 0;
    mq_font_atlas_size(f, size, &aw, &ah);
    std::vector<uint8_t> rgba((size_t)aw * ah * 4);
    if (mq_font_atlas(f, size, rgba.data(), rgba.size(), &aw, &ah) != MQ_TEXT_OK) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    /* PPM P6（RGB；白字 + alpha 直接当灰度）——头长按**实际**计算（曾硬编码 15 → 越界崩溃） */
    char header[48];
    const int hlen = std::snprintf(header, sizeof header, "P6\n%u %u\n255\n", aw, ah);
    const size_t total = (size_t)hlen + (size_t)aw * ah * 3;
    auto byte_at = [&](size_t i) -> uint8_t {
        if (i < (size_t)hlen) return (uint8_t)header[i];
        const size_t p = i - (size_t)hlen;
        const size_t px = p / 3, ch = p % 3;
        const uint8_t a = rgba[px * 4 + 3];
        if (ch == 0) return a;                    /* 白字 + alpha → 灰度 */
        if (ch == 1) return a;
        return a;
    };
    const size_t take = std::min(max_bytes, offset < total ? total - offset : 0);
    std::string raw;
    raw.reserve(take);
    for (size_t i = 0; i < take; ++i) raw += (char)byte_at(offset + i);
    const std::string b64 = b64_encode((const uint8_t*)raw.data(), raw.size());
    std::snprintf(out, cap, "{\"ok\":true,\"w\":%u,\"h\":%u,\"total\":%zu,\"offset\":%zu,"
                            "\"size\":%zu,\"data\":\"%s\"}",
                  aw, ah, total, offset, take, b64.c_str());
    return 0;
}

/* Capture.screen：最近一帧渲染结果（PPM → base64 分块）——实时预览面板 */
extern "C" int mq_control_Capture_screen(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const size_t offset = (size_t)mq_json_num(mq_json_get(params, "offset"), 0);
    if (offset == 0) eng->frame_capture_requested = true;
    size_t max_bytes = (size_t)mq_json_num(mq_json_get(params, "max"), 24 * 1024);
    if (max_bytes == 0 || max_bytes > (1u << 20)) max_bytes = 24 * 1024;
    if (eng->frame_pixels.empty() || eng->frame_w <= 0 || eng->frame_h <= 0) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no frame\"}");
        return 0;
    }
    /* 缩略图：max_dim > 0 时盒式降采样（预览没必要传整帧；960x720 全帧约 2MB → 480x360 约 0.5MB） */
    const int src_w = eng->frame_w, src_h = eng->frame_h;
    const int max_dim = (int)mq_json_num(mq_json_get(params, "max_dim"), 0);
    int w = src_w, h = src_h;
    std::vector<uint8_t> small;
    const uint8_t* src = eng->frame_pixels.data();
    int src_stride = src_w * 4;
    if (max_dim > 0 && (src_w > max_dim || src_h > max_dim)) {
        const int f = (src_w > src_h ? (src_w + max_dim - 1) / max_dim : (src_h + max_dim - 1) / max_dim);
        w = src_w / f > 0 ? src_w / f : 1;
        h = src_h / f > 0 ? src_h / f : 1;
        small.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (int dy = 0; dy < f; ++dy)
                    for (int dx = 0; dx < f; ++dx) {
                        const int sx = x * f + dx, sy = y * f + dy;
                        if (sx >= src_w || sy >= src_h) continue;
                        const uint8_t* p = src + (size_t)sy * src_stride + (size_t)sx * 4;
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    }
                const uint32_t n = (uint32_t)(f * f);
                for (int c = 0; c < 4; ++c) small[((size_t)y * w + x) * 4 + c] = (uint8_t)(acc[c] / n);
            }
        src = small.data();
        src_stride = w * 4;
    }
    char header[48];
    const int hlen = std::snprintf(header, sizeof header, "P6\n%d %d\n255\n", w, h);
    const size_t total = (size_t)hlen + (size_t)w * h * 3;
    auto byte_at = [&](size_t i) -> uint8_t {
        if (i < (size_t)hlen) return (uint8_t)header[i];
        const size_t p = i - (size_t)hlen;
        const size_t px = p / 3, ch = p % 3;
        return src[px * 4 + ch];
    };
    const size_t take = std::min(max_bytes, offset < total ? total - offset : 0);
    std::string raw;
    raw.reserve(take);
    for (size_t i = 0; i < take; ++i) raw += (char)byte_at(offset + i);
    const std::string b64 = b64_encode((const uint8_t*)raw.data(), raw.size());
    std::snprintf(out, cap, "{\"ok\":true,\"w\":%d,\"h\":%d,\"total\":%zu,\"offset\":%zu,"
                            "\"size\":%zu,\"data\":\"%s\"}",
                  w, h, total, offset, take, b64.c_str());
    return 0;
}

/* Capture.camera：按需回读指定相机的最新 RenderTexture，并以 PPM/base64 分块返回。 */
extern "C" int mq_control_Capture_camera(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int camera = (int)mq_json_num(mq_json_get(params, "camera"), -1);
    Engine::RenderTextureState* rt = eng->render_texture_for(camera);
    if (!rt || !rt->ready) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"camera texture not ready\"}"); return 0; }
    const size_t offset = (size_t)mq_json_num(mq_json_get(params, "offset"), 0);
    size_t max_bytes = (size_t)mq_json_num(mq_json_get(params, "max"), 24 * 1024);
    if (max_bytes == 0 || max_bytes > (1u << 20)) max_bytes = 24 * 1024;
    if (offset > 0 && rt->capture_pixels.empty()) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"snapshot expired; restart at offset 0\"}");
        return 0;
    }
    if (!eng->read_camera_texture(*rt, offset == 0)) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"readback failed\"}"); return 0;
    }
    const int src_w = rt->width, src_h = rt->height;
    const int max_dim = (int)mq_json_num(mq_json_get(params, "max_dim"), 0);
    int w = src_w, h = src_h;
    std::vector<uint8_t> small;
    const uint8_t* src = rt->capture_pixels.data();
    if (max_dim > 0 && (src_w > max_dim || src_h > max_dim)) {
        const int f = src_w > src_h ? (src_w + max_dim - 1) / max_dim : (src_h + max_dim - 1) / max_dim;
        w = std::max(1, src_w / f); h = std::max(1, src_h / f);
        small.assign((size_t)w * h * 4u, 0);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t acc[4] = { 0, 0, 0, 0 }, n = 0;
            for (int dy = 0; dy < f; ++dy) for (int dx = 0; dx < f; ++dx) {
                const int sx = x * f + dx, sy = y * f + dy;
                if (sx >= src_w || sy >= src_h) continue;
                const uint8_t* p = src + ((size_t)sy * src_w + sx) * 4u;
                for (int c = 0; c < 4; ++c) acc[c] += p[c];
                ++n;
            }
            for (int c = 0; c < 4; ++c) small[((size_t)y * w + x) * 4u + c] = (uint8_t)(acc[c] / std::max(1u, n));
        }
        src = small.data();
    }
    char header[48];
    const int hlen = std::snprintf(header, sizeof header, "P6\n%d %d\n255\n", w, h);
    const size_t total = (size_t)hlen + (size_t)w * h * 3u;
    auto byte_at = [&](size_t i) -> uint8_t {
        if (i < (size_t)hlen) return (uint8_t)header[i];
        const size_t p = i - (size_t)hlen, px = p / 3u, ch = p % 3u;
        return src[px * 4u + ch];
    };
    const size_t take = std::min(max_bytes, offset < total ? total - offset : 0);
    std::string raw; raw.reserve(take);
    for (size_t i = 0; i < take; ++i) raw += (char)byte_at(offset + i);
    const std::string b64 = b64_encode((const uint8_t*)raw.data(), raw.size());
    std::snprintf(out, cap, "{\"ok\":true,\"camera\":%d,\"w\":%d,\"h\":%d,\"frame\":%llu,"
                            "\"total\":%zu,\"offset\":%zu,\"size\":%zu,\"data\":\"%s\"}",
                  camera, w, h, (unsigned long long)rt->capture_frame, total, offset, take, b64.c_str());
    return 0;
}

extern "C" int mq_control_Capture_camera_save(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int camera = (int)mq_json_num(mq_json_get(params, "camera"), -1);
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!path || !*path || !comp_by_name(*eng, camera, "render.RenderTexture", nullptr)) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"bad camera/path\"}"); return 0;
    }
    if (Engine::RenderTextureState* rt = eng->render_texture_for(camera)) rt->dirty = true;
    eng->camera_captures.push_back({ camera, path });
    std::snprintf(out, cap, "{\"ok\":true,\"deferred\":true,\"camera\":%d}", camera);
    return 0;
}
