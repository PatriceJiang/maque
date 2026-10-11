/* 文本渲染基础设施（docs/13/17）：FreeType 字形栅格化 → 图集 → 文本排版。
 *
 * 设计要点（docs/10 确定性）：
 *   · 纯函数式：所有状态在 mq_font 实例内；无全局可变状态（ADR-0014）；
 *   · 排版只做整数/浮点算术，遍历按码点升序 → 逐位可复现；
 *   · 图集按码点升序做行式打包（确定性），输出 RGBA8（白字 + alpha）；
 *   · 无 FreeType 时回退到内置 5x7 位图字体（保证 headless/CI 亦可用）。
 * 富文本标记：<color=#rrggbb>…</color>、<size=NN>…</size>、<b>…</b>、<br/>（大小写不敏感）。 */
#include "text/mq_text.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#if defined(MAQUE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace {

constexpr uint32_t kFirstCp = 32;
constexpr uint32_t kLastCp = 126;          /* 内置位图字体覆盖 ASCII 可见字符 */
constexpr uint32_t kGlyphCount = kLastCp - kFirstCp + 1;


struct Glyph {
    uint32_t cp = 0;
    int      ax = 0, ay = 0;      /* 图集位置 */
    int      w = 0, h = 0;        /* 位图尺寸 */
    int      bearing_x = 0, bearing_y = 0;
    float    advance = 0.0F;
};

/* 字形图集：**按需栅格化 + 增量打包**（任意 Unicode，含 CJK，docs/13 §字形管线）。
 *   · 宽固定 kAtlasW、高按需增长到 kAtlasMaxH；行式打包（追加顺序 = 码点出现顺序 → 确定性）
 *   · generation：每次新增字形自增，宿主据此决定纹理是否重传
 *   · 容量用尽后跳过新字形（不影响已排好的字形） */
constexpr uint32_t kAtlasW = 512;
constexpr uint32_t kAtlasMaxH = 1024;
constexpr uint32_t kAtlasPad = 1;

struct Atlas {
    float size = 0.0F;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::vector<Glyph> glyphs;
    uint32_t generation = 0;
    uint32_t cur_x = kAtlasPad, cur_y = kAtlasPad, row_h = 0;
    bool valid = false;
    bool full = false;
};

} // namespace

struct mq_font {
    std::string path;
    bool builtin = false;
    /* 度量与位图（按字号缓存；确定性：同字号同结果） */
    std::vector<Atlas> atlases;
#if defined(MAQUE_HAVE_FREETYPE)
    FT_Face face = nullptr;
#endif
    std::vector<uint8_t> builtin_bits;      /* 内置字体：kGlyphCount × 5 字节 */
};

/* ============================ 内置位图字体 ============================ */

namespace {
#include "builtin5x7.inc"


/* 内置字体：把 5x7 位图按字号放大为 RGBA8 字形（最近邻，确定性）。 */
void builtin_glyph_bitmap(const mq_font* f, uint32_t cp, float size, std::vector<uint8_t>& out,
                          int* out_w, int* out_h) {
    const int scale = (int)(size / 8.0F + 0.5F);
    const int s = scale < 1 ? 1 : scale;
    if (cp < kFirstCp || cp > kLastCp) { *out_w = 0; *out_h = 0; out.clear(); return; }
    const uint8_t* col = f->builtin_bits.data() + (size_t)(cp - kFirstCp) * 5;
    const int w = 5 * s, h = 7 * s;
    out.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 5; ++x) {
            const bool on = (col[x] >> y) & 1u;
            if (!on) continue;
            for (int dy = 0; dy < s; ++dy) {
                for (int dx = 0; dx < s; ++dx) {
                    const int px = x * s + dx, py = y * s + dy;
                    uint8_t* p = out.data() + ((size_t)py * w + px) * 4;
                    p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
                }
            }
        }
    }
    *out_w = w; *out_h = h;
}

} // namespace

/* ============================ 字体加载 ============================ */

extern "C" mq_font* mq_font_builtin(void) {
    auto* f = new (std::nothrow) mq_font();
    if (!f) return nullptr;
    f->builtin = true;
    f->builtin_bits.resize((size_t)kGlyphCount * 5);
    for (uint32_t i = 0; i < kGlyphCount; ++i)
        std::memcpy(f->builtin_bits.data() + (size_t)i * 5, kBuiltin5x7[i], 5);
    return f;
}

extern "C" mq_font* mq_font_load_file(const char* path) {
#if defined(MAQUE_HAVE_FREETYPE)
    if (!path || !*path) return nullptr;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Face(lib, path, 0, &face) != 0) { FT_Done_FreeType(lib); return nullptr; }
    auto* f = new (std::nothrow) mq_font();
    if (!f) { FT_Done_Face(face); FT_Done_FreeType(lib); return nullptr; }
    f->path = path;
    f->face = face;
    /* 库句柄随 face 存活：FreeType 允许 Done_Face 后 Done_Library，这里保存到 face->generic */
    face->generic.data = lib;
    return f;
#else
    (void)path;
    return nullptr;
#endif
}

extern "C" void mq_font_destroy(mq_font* font) {
    if (!font) return;
#if defined(MAQUE_HAVE_FREETYPE)
    if (font->face) {
        FT_Library lib = (FT_Library)font->face->generic.data;
        FT_Done_Face(font->face);
        if (lib) FT_Done_FreeType(lib);
    }
#endif
    delete font;
}

/* ============================ 图集构建 ============================ */

namespace {

const Glyph* ensure_glyph(mq_font* f, Atlas& a, uint32_t cp, float size);

/* 图集惰性创建：ASCII 预热（常见文本零额外开销、行为与旧版一致），其余码点按需栅格化。 */
Atlas& atlas_for(mq_font* f, float size) {
    const float sz = size > 0.0F ? size : 16.0F;
    for (auto& a : f->atlases) if (a.size == sz && a.valid) return a;
    Atlas a;
    a.size = sz;
    a.w = kAtlasW;
    a.h = kAtlasPad * 2;
    a.rgba.assign((size_t)a.w * a.h * 4, 0);
    a.valid = true;
    f->atlases.push_back(std::move(a));
    Atlas& out = f->atlases.back();
    for (uint32_t cp = kFirstCp; cp <= kLastCp; ++cp) (void)ensure_glyph(f, out, cp, sz);
    return out;
}

/* 按需栅格化一个码点并打包：已存在返回既有；放不下返回 nullptr（调用方跳过）。 */
const Glyph* ensure_glyph(mq_font* f, Atlas& a, uint32_t cp, float size) {
    for (const auto& g : a.glyphs) if (g.cp == cp) return &g;
    if (a.full) return nullptr;

    Glyph g;
    g.cp = cp;
    std::vector<uint8_t> bmp;                 /* RGBA（白字 + alpha） */
    int bw = 0, bh = 0;
    if (f->builtin) {
        builtin_glyph_bitmap(f, cp, size, bmp, &bw, &bh);
        g.advance = (float)bw + 1.0F;
        g.bearing_y = bh;
    }
#if defined(MAQUE_HAVE_FREETYPE)
    else if (f->face) {
        if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)(size + 0.5F)) != 0) return nullptr;
        if (FT_Load_Char(f->face, cp, FT_LOAD_RENDER) != 0) return nullptr;
        FT_GlyphSlot sl = f->face->glyph;
        bw = (int)sl->bitmap.width;
        bh = (int)sl->bitmap.rows;
        g.bearing_x = sl->bitmap_left;
        g.bearing_y = sl->bitmap_top;
        g.advance = (float)(sl->advance.x >> 6);
        if (bw > 0 && bh > 0) {
            bmp.assign((size_t)bw * bh * 4, 0);
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) {
                    uint8_t* d = bmp.data() + ((size_t)y * bw + x) * 4;
                    d[0] = d[1] = d[2] = 255;
                    d[3] = sl->bitmap.buffer[(size_t)y * sl->bitmap.pitch + x];
                }
        }
    }
#endif
    else return nullptr;

    uint32_t px = a.cur_x, py = a.cur_y;
    if (bw > 0 && bh > 0) {
        if (px + (uint32_t)bw + kAtlasPad > a.w && px > kAtlasPad) {      /* 换行 */
            px = kAtlasPad;
            py += a.row_h + kAtlasPad;
            a.row_h = 0;
        }
        if (py + (uint32_t)bh + kAtlasPad > kAtlasMaxH) { a.full = true; return nullptr; }
        /* 图集高度按需增长：**必须先扩容 rgba 再写入**（否则越界写 → 堆损坏） */
        const uint32_t need_h = py + (uint32_t)bh + kAtlasPad;
        if (need_h > a.h) {
            a.rgba.resize((size_t)a.w * need_h * 4, 0);
            a.h = need_h;
        }
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const uint8_t alpha = bmp[((size_t)y * bw + x) * 4 + 3];
                if (!alpha) continue;
                uint8_t* d = a.rgba.data() + (((size_t)(py + y) * a.w) + (uint32_t)(px + x)) * 4;
                d[0] = 255; d[1] = 255; d[2] = 255; d[3] = alpha;
            }
        g.ax = (int)px; g.ay = (int)py;
        a.cur_x = px + (uint32_t)bw + kAtlasPad;
        a.row_h = std::max(a.row_h, (uint32_t)bh);
    }
    g.w = bw > 0 ? bw : 0;
    g.h = bh > 0 ? bh : 0;
    a.glyphs.push_back(g);
    ++a.generation;
    return &a.glyphs.back();
}

/* ---- UTF-8：排版按**码点**（旧实现按字节 → CJK 会被拆成 3 个伪字形）---- */
const char* utf8_next(const char* p, uint32_t* out_cp) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x80) { *out_cp = c; return p + 1; }
    uint32_t cp = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
    else { *out_cp = 0xFFFD; return p + 1; }
    for (int i = 1; i <= extra; ++i) {
        const unsigned char cc = (unsigned char)p[i];
        if ((cc & 0xC0) != 0x80) { *out_cp = 0xFFFD; return p + 1; }      /* 截断 → 替换符 */
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    *out_cp = cp;
    return p + extra + 1;
}

/* CJK/假名/谚文/全角标点之间可换行（无空格语言 → 逐字断行机会） */
bool is_cjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

const Glyph* find_glyph(const Atlas& a, uint32_t cp) {
    if (cp >= 'a' && cp <= 'z' && a.glyphs.size() && a.glyphs[0].cp == kFirstCp) {
        /* 内置字体：小写复用大写形状（文档化取舍） */
    }
    for (const auto& g : a.glyphs) if (g.cp == cp) return &g;
    return nullptr;
}

/* 设备字号：逻辑字号 × 设备缩放，**取整**（FreeType 整数 ppem → 笔画落在像素网格） */
float device_size_of(const mq_text_style& st, float logical) {
    const float sc = st.device_scale > 0.0F ? st.device_scale : 1.0F;
    float px = logical * sc;
    if (px < 1.0F) px = 1.0F;
    return (float)(int)(px + 0.5F);
}

/* 字体 ascent（设备像素；用于把 FreeType bitmap_top 换算成位图顶部 y） */
void font_extents(mq_font* f, float device_size, float* ascent) {
    *ascent = device_size * 0.8F;                     /* 内置位图字体缺省 */
#if defined(MAQUE_HAVE_FREETYPE)
    if (f && !f->builtin && f->face) {
        if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)(device_size + 0.5F)) == 0)
            *ascent = (float)(f->face->size->metrics.ascender >> 6);
    }
#endif
}

} // namespace

extern "C" int mq_font_atlas(mq_font* font, float size, uint8_t* out_rgba, size_t cap,
                             uint32_t* out_w, uint32_t* out_h) {
    if (!font || !out_w || !out_h) return MQ_TEXT_ERR_ARG;
    Atlas& a = atlas_for(font, size);
    if (!a.valid) return MQ_TEXT_ERR_FONT;
    *out_w = a.w; *out_h = a.h;
    const size_t need = a.rgba.size();
    if (!out_rgba || cap < need) return MQ_TEXT_ERR_CAP;
    std::memcpy(out_rgba, a.rgba.data(), need);
    return MQ_TEXT_OK;
}

extern "C" uint32_t mq_font_atlas_generation(mq_font* font, float size) {
    if (!font) return 0;
    Atlas& a = atlas_for(font, size);
    return a.generation;
}

extern "C" uint32_t mq_font_glyph_count(mq_font* font, float size) {
    if (!font) return 0;
    Atlas& a = atlas_for(font, size);
    return (uint32_t)a.glyphs.size();
}

extern "C" float mq_font_height(mq_font* font, float size) {
    if (!font) return 0.0F;
    if (font->builtin) {
        const int s = std::max(1, (int)(size / 8.0F + 0.5F));
        return (float)(9 * s);
    }
#if defined(MAQUE_HAVE_FREETYPE)
    if (font->face) {
        if (FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)((size > 0 ? size : 16.0F) + 0.5F)) == 0)
            return (float)(font->face->size->metrics.height >> 6);
    }
#endif
    return size > 0.0F ? size * 1.25F : 20.0F;
}

extern "C" float mq_font_advance(mq_font* font, uint32_t codepoint, float size) {
    if (!font) return 0.0F;
    if (font->builtin) {
        const int s = std::max(1, (int)((size > 0 ? size : 16.0F) / 8.0F + 0.5F));
        return (float)(6 * s);
    }
#if defined(MAQUE_HAVE_FREETYPE)
    if (font->face) {
        if (FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)((size > 0 ? size : 16.0F) + 0.5F)) == 0 &&
            FT_Load_Char(font->face, codepoint, FT_LOAD_DEFAULT) == 0)
            return (float)(font->face->glyph->advance.x >> 6);
    }
#endif
    return (size > 0 ? size : 16.0F) * 0.6F;
}

/* ============================ 富文本标记解析 ============================
 * 支持：<color=#rrggbb>…</color>、<size=NN>…</size>、<b>…</b>、<br/>
 * 规则：标签大小写不敏感；未知标签按纯文本输出（不报错）；属性缺失即忽略。
 * 解析是确定性的：单遍扫描，无回溯。 */

namespace {

struct Run {
    std::string text;
    float r = 1, g = 1, b = 1, a = 1;
    float size = 0.0F;      /* 0 = 用样式默认字号 */
    bool  bold = false;
    bool  line_break = false;
};

bool parse_hex_color(const std::string& s, float* r, float* g, float* b) {
    if (s.size() != 6 && s.size() != 7) return false;
    size_t i = (s[0] == '#') ? 1 : 0;
    if (s.size() - i != 6) return false;
    auto hex = [&](size_t p) -> int {
        char c = s[p];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int k = 0; k < 6; ++k) { v[k] = hex(i + (size_t)k); if (v[k] < 0) return false; }
    *r = (float)((v[0] << 4) | v[1]) / 255.0F;
    *g = (float)((v[2] << 4) | v[3]) / 255.0F;
    *b = (float)((v[4] << 4) | v[5]) / 255.0F;
    return true;
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

/* 解析富文本 → run 序列（栈式处理嵌套标签，最内层生效） */
void parse_runs(const std::string& in, const mq_text_style& st, std::vector<Run>& out) {
    Run cur;
    cur.r = st.r; cur.g = st.g; cur.b = st.b; cur.a = st.a;
    struct Saved { bool color = false; float r = 1, g = 1, b = 1; bool size = false; float sz = 0; bool bold = false; };
    std::vector<Saved> stack;
    auto flush = [&]() {
        if (!cur.text.empty()) { out.push_back(cur); cur.text.clear(); }
    };
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '<') {
            const size_t close = in.find('>', i);
            if (close == std::string::npos) { cur.text.push_back(in[i++]); continue; }
            const std::string tag = lower(in.substr(i + 1, close - i - 1));
            const bool closing = !tag.empty() && tag[0] == '/';
            const std::string body = closing ? tag.substr(1) : tag;
            if (body == "br" || body == "br/") {
                flush();
                Run br; br.line_break = true; br.size = cur.size; out.push_back(br);
                i = close + 1; continue;
            }
            if (body == "b") {
                flush();
                if (closing) { if (!stack.empty()) { cur.bold = stack.back().bold; stack.pop_back(); } }
                else { Saved s; s.color = true; s.r = cur.r; s.g = cur.g; s.b = cur.b; s.size = true; s.sz = cur.size; s.bold = cur.bold; stack.push_back(s); cur.bold = true; }
                i = close + 1; continue;
            }
            if (body.rfind("color", 0) == 0) {
                flush();
                if (closing) { if (!stack.empty()) { const Saved s = stack.back(); stack.pop_back(); cur.r = s.r; cur.g = s.g; cur.b = s.b; } }
                else {
                    Saved s; s.r = cur.r; s.g = cur.g; s.b = cur.b; s.color = true; s.sz = cur.size; s.size = true; s.bold = cur.bold;
                    stack.push_back(s);
                    const size_t eq = body.find('=');
                    if (eq != std::string::npos) {
                        float r = cur.r, g = cur.g, b = cur.b;
                        if (parse_hex_color(body.substr(eq + 1), &r, &g, &b)) { cur.r = r; cur.g = g; cur.b = b; }
                    }
                }
                i = close + 1; continue;
            }
            if (body.rfind("size", 0) == 0) {
                flush();
                if (closing) { if (!stack.empty()) { const Saved s = stack.back(); stack.pop_back(); cur.size = s.sz; } }
                else {
                    Saved s; s.r = cur.r; s.g = cur.g; s.b = cur.b; s.color = true; s.sz = cur.size; s.size = true; s.bold = cur.bold;
                    stack.push_back(s);
                    const size_t eq = body.find('=');
                    if (eq != std::string::npos) cur.size = (float)std::atof(body.c_str() + eq + 1);
                }
                i = close + 1; continue;
            }
            /* 未知标签：按纯文本输出 */
            cur.text.append(in, i, close - i + 1);
            i = close + 1;
            continue;
        }
        if (in[i] == '\n') {
            flush();
            Run br; br.line_break = true; out.push_back(br);
            ++i; continue;
        }
        cur.text.push_back(in[i++]);
    }
    flush();
}

} // namespace

extern "C" int mq_text_strip_markup(const char* text, char* out, uint32_t cap) {
    if (!text || !out) return MQ_TEXT_ERR_ARG;
    mq_text_style st{};
    st.struct_size = (uint32_t)sizeof st;
    st.font_size = 16.0F;
    st.r = st.g = st.b = st.a = 1.0F;
    std::vector<Run> runs;
    parse_runs(text, st, runs);
    std::string plain;
    for (const Run& r : runs) plain += r.line_break ? std::string("\n") : r.text;
    if (plain.size() + 1 > cap) return MQ_TEXT_ERR_CAP;
    std::memcpy(out, plain.c_str(), plain.size() + 1);
    return MQ_TEXT_OK;
}

/* ============================ 排版（换行 + 对齐） ============================ */

namespace {

struct PlacedGlyph {
    uint32_t cp;
    float x, y;                     /* 笔位置（排版空间） */
    float x0, y0;                   /* 位图左上角（相对文本框原点，设备像素，已对齐） */
    float size, device_size;        /* 逻辑字号 / 实际栅格化字号 */
    float r, g, b, a;
    int w, h, bearing_x, bearing_y; /* FreeType bitmap_left / bitmap_top */
    int ax = 0, ay = 0;             /* 图集内像素原点 */
    float u0, v0, u1, v1;
};

/* 逐字符测量 + 换行 + 对齐（确定性：单向扫描，不回溯） */
int layout_impl(mq_font* f, const mq_text_style& st, const char* text,
                std::vector<PlacedGlyph>* out, mq_text_metrics* metrics) {
    const float base = st.font_size > 0.0F ? st.font_size : 16.0F;
    std::vector<Run> runs;
    parse_runs(text ? text : "", st, runs);

    const float max_w = st.max_width;
    float pen_x = 0.0F, pen_y = 0.0F;
    float line_h = 0.0F;
    uint32_t line_count = 0;
    struct Line { float width; float y; float height; std::vector<PlacedGlyph> glyphs; };
    std::vector<Line> lines(1);
    lines.back().y = 0.0F;

    auto end_line = [&]() {
        lines.back().width = pen_x;
        pen_y += line_h + st.line_spacing;
        lines.emplace_back();
        lines.back().y = pen_y;
        pen_x = 0.0F;
        line_h = 0.0F;
        ++line_count;
    };

    float ascent = 0.0F;
    for (const Run& run : runs) {
        if (run.line_break) { end_line(); continue; }
        const float size = run.size > 0.0F ? run.size : base;
        const float dsize = device_size_of(st, size);        /* 设备像素字号（整数） */
        font_extents(f, dsize, &ascent);
        if (st.pixel_snap) pen_x = (float)(int)(pen_x + 0.5F);

        Atlas& atlas = atlas_for(f, dsize);
        std::vector<uint32_t> word;                          /* 词缓冲（码点） */

        /* 落一个字形：按需栅格化 → 位图矩形（含 bearing）→ 像素对齐 */
        auto place = [&](uint32_t cp) {
            PlacedGlyph g{};
            g.cp = cp; g.x = pen_x; g.y = pen_y; g.size = size; g.device_size = dsize;
            g.r = run.r; g.g = run.g; g.b = run.b; g.a = run.a;
            const Glyph* gl = ensure_glyph(f, atlas, cp, dsize);
            float adv = mq_font_advance(f, cp, dsize);
            if (gl) {
                g.w = gl->w; g.h = gl->h; g.bearing_x = gl->bearing_x; g.bearing_y = gl->bearing_y;
                g.ax = gl->ax; g.ay = gl->ay;
                if (gl->w > 0 && gl->h > 0) {
                    g.u0 = (float)gl->ax / (float)atlas.w; g.v0 = (float)gl->ay / (float)atlas.h;
                    g.u1 = (float)(gl->ax + gl->w) / (float)atlas.w;
                    g.v1 = (float)(gl->ay + gl->h) / (float)atlas.h;
                }
                if (gl->advance > 0.0F) adv = gl->advance;
            }
            g.x0 = g.x + (float)g.bearing_x;                 /* 位图左上角 = 笔位置 + bearing */
            g.y0 = g.y + ascent - (float)g.bearing_y;        /* y 向下：基线 − bitmap_top */
            if (st.pixel_snap) { g.x0 = (float)(int)(g.x0 + 0.5F); g.y0 = (float)(int)(g.y0 + 0.5F); }
            lines.back().glyphs.push_back(g);
            pen_x += adv;
            if (st.pixel_snap) pen_x = (float)(int)(pen_x + 0.5F);
            line_h = std::max(line_h, mq_font_height(f, dsize));
        };

        auto flush_word = [&]() {
            if (word.empty()) return;
            if (st.wrap == MQ_TEXT_WRAP_CHAR) {
                for (uint32_t cp : word) {
                    if (max_w > 0.0F && pen_x > 0.0F && pen_x + mq_font_advance(f, cp, dsize) > max_w)
                        end_line();
                    place(cp);
                }
            } else {
                float wsum = 0.0F;
                for (uint32_t cp : word) wsum += mq_font_advance(f, cp, dsize);
                if (max_w > 0.0F && pen_x > 0.0F && pen_x + wsum > max_w) end_line();
                for (uint32_t cp : word) place(cp);
            }
            word.clear();
        };

        const char* cur = run.text.c_str();
        while (*cur) {
            uint32_t cp = 0;
            const char* nx = utf8_next(cur, &cp);
            if (nx == cur) break;
            cur = nx;
            word.push_back(cp);
            /* WORD：空格后断行；CJK 逐字可断（无空格语言不会整段溢出） */
            if (st.wrap == MQ_TEXT_WRAP_WORD && (cp == ' ' || is_cjk(cp))) flush_word();
        }
        flush_word();                                        /* NONE/CHAR/WORD 统一收尾 */
    }
    lines.back().width = pen_x;
    lines.back().height = line_h;
    ++line_count;
    if (line_count == 0) line_count = 1;

    float max_line_w = 0.0F;
    for (const Line& l : lines) max_line_w = std::max(max_line_w, l.width);
    const float base_dev = device_size_of(st, base);
    float total_h = 0.0F;
    for (const Line& l : lines) total_h += (l.height > 0.0F ? l.height : mq_font_height(f, base_dev)) + st.line_spacing;
    if (total_h > 0.0F) total_h -= st.line_spacing;

    uint32_t count = 0;
    if (out) {
        out->clear();
        for (auto& l : lines) {
            const float box_w = max_w > 0.0F ? max_w : max_line_w;
            float dx = 0.0F;
            if (st.align == MQ_TEXT_ALIGN_CENTER) dx = (box_w - l.width) * 0.5F;
            else if (st.align == MQ_TEXT_ALIGN_END) dx = box_w - l.width;
            for (auto& g : l.glyphs) { g.x += dx; out->push_back(g); }
            count += (uint32_t)l.glyphs.size();
        }
    } else {
        for (auto& l : lines) count += (uint32_t)l.glyphs.size();
    }

    if (metrics) {
        metrics->width = max_w > 0.0F ? std::min(max_line_w, max_w) : max_line_w;
        metrics->height = total_h;
        metrics->line_count = line_count;
        metrics->glyph_count = count;
        metrics->baseline = mq_font_height(f, device_size_of(st, base));
        font_extents(f, device_size_of(st, base), &metrics->ascent);
        metrics->device_scale = st.device_scale > 0.0F ? st.device_scale : 1.0F;
    }
    return MQ_TEXT_OK;
}

} // namespace

extern "C" int mq_text_measure(mq_font* font, const mq_text_style* style, const char* text,
                               mq_text_metrics* out_metrics) {
    if (!font || !style || !out_metrics) return MQ_TEXT_ERR_ARG;
    return layout_impl(font, *style, text, nullptr, out_metrics);
}

extern "C" int mq_text_layout(mq_font* font, const mq_text_style* style, const char* text,
                              mq_text_glyph* out, uint32_t cap, uint32_t* out_count,
                              mq_text_metrics* out_metrics) {
    if (!font || !style) return MQ_TEXT_ERR_ARG;
    std::vector<PlacedGlyph> placed;
    const int rc = layout_impl(font, *style, text, &placed, out_metrics);
    if (rc != MQ_TEXT_OK) return rc;
    if (out_count) *out_count = (uint32_t)placed.size();
    if (!out || cap < placed.size()) return placed.empty() ? MQ_TEXT_OK : MQ_TEXT_ERR_CAP;
    for (size_t i = 0; i < placed.size(); ++i) {
        const PlacedGlyph& p = placed[i];
        mq_text_glyph& g = out[i];
        g.codepoint = p.cp;
        g.x = p.x; g.y = p.y;
        g.w = (float)p.w; g.h = (float)p.h;
        g.u0 = p.u0; g.v0 = p.v0; g.u1 = p.u1; g.v1 = p.v1;
        g.r = p.r; g.g = p.g; g.b = p.b; g.a = p.a;
        g.size = p.size;
        g.device_size = p.device_size;
        g.x0 = p.x0; g.y0 = p.y0;
        g.ax = p.ax; g.ay = p.ay;
        g.bearing_x = p.bearing_x; g.bearing_y = p.bearing_y;
    }
    return MQ_TEXT_OK;
}
