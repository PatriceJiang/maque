/* 文本渲染基础设施（docs/13/17）：FreeType 字形栅格化 → 图集 → 文本排版。
 *
 * 设计要点（docs/10 确定性）：
 *   · 纯函数式：所有状态在 mq_font 实例内；无全局可变状态（ADR-0014）；
 *   · 排版只做整数/浮点算术，遍历按码点升序 → 逐位可复现；
 *   · 图集按码点升序做行式打包（确定性），输出 RGBA8（白字 + alpha）；
 *   · 无 FreeType 时回退到内置 5x7 位图字体（保证 headless/CI 亦可用）。
 * 富文本标记：<color=#rrggbb>…</color>、<size=NN>…</size>、<b>…</b>、<i>…</i>、<br/>（大小写不敏感）。
 * 合成样式（<b>/<i> 或 style.bold/italic）：图集按 (字号, 码点, bold, italic) 缓存，**同一字号共用一份
 * Atlas**（带样式字形只是作为额外条目追加，generation 递增）；普通样式路径的像素/推进/换行完全不变。 */
#include "text/mq_text.h"
#include "core/tracy/mq_tracy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#if defined(MAQUE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#endif
#if defined(MAQUE_HAVE_HARFBUZZ)
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#endif
#if defined(MAQUE_HAVE_LIBUNIBREAK)
#include "graphemebreak.h"
#include "linebreak.h"
#endif

namespace {

constexpr uint32_t kFirstCp = 32;
constexpr uint32_t kLastCp = 126;          /* 内置位图字体覆盖 ASCII 可见字符 */
constexpr uint32_t kGlyphCount = kLastCp - kFirstCp + 1;

/* 读环境变量（避开 MSVC 对 std::getenv 的 deprecation；值只在初始化/诊断用）。 */
std::string env_value(const char* name) {
#if defined(_WIN32)
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) == 0 && v) {
        std::string s(v);
        std::free(v);
        return s;
    }
    return {};
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}


struct Glyph {
    uint32_t cp = 0;              /* 码点（连字/多码点 cluster 取首码点；仅调试/公开 API 展示） */
    uint32_t gid = 0;             /* HarfBuzz/FreeType 字形索引（图集查找键；内置字体 = cp） */
    int      ax = 0, ay = 0;      /* 图集位置 */
    int      w = 0, h = 0;        /* 位图尺寸 */
    int      bearing_x = 0, bearing_y = 0;
    float    advance = 0.0F;
    /* 合成样式位：参与查找键（同码点的普通/粗体/斜体字形各自独立缓存） */
    bool     bold = false;
    bool     italic = false;
};

/* 内置位图字体的放大倍率（与旧实现逐位一致：16px → 2 倍） */
int builtin_scale_of(float size) {
    const int scale = (int)(size / 8.0F + 0.5F);
    return scale < 1 ? 1 : scale;
}

/* FreeType 合成加粗的笔画加宽量（像素）：约 round(device_size / 16)，至少 1 */
int bold_strength_of(float device_size) {
    const int s = (int)(device_size / 16.0F + 0.5F);
    return s < 1 ? 1 : s;
}

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
#if defined(MAQUE_HAVE_HARFBUZZ)
    /* 按设计 ppem 缓存 hb-ft font（创建时必须 face 尺寸已就绪，否则 scale=0）。 */
    std::vector<std::pair<float, hb_font_t*>> hb_fonts;
#endif
    std::vector<uint8_t> builtin_bits;      /* 内置字体：kGlyphCount × 5 字节 */
};

/* ============================ 内置位图字体 ============================ */

namespace {
#include "builtin5x7.inc"


/* 内置字体：把 5x7 位图按字号放大为 RGBA8 字形（最近邻，确定性）。
 * 合成样式（纯几何变换，不依赖 FreeType）：
 *   · bold   ：每个点亮像素块向右膨胀 1*s 像素（= 一个源像素的设备宽度），推进同步 +s；
 *   · italic ：逐行水平剪切（顶行右移最多约 1/4 字高，底行不动），推进不变。
 * 普通样式（bold/italic 均 false）走与旧实现完全相同的写入路径 → 图集逐字节不变。 */
void builtin_glyph_bitmap(const mq_font* f, uint32_t cp, float size, bool bold, bool italic,
                          std::vector<uint8_t>& out, int* out_w, int* out_h) {
    const int s = builtin_scale_of(size);
    if (cp < kFirstCp || cp > kLastCp) { *out_w = 0; *out_h = 0; out.clear(); return; }
    const uint8_t* col = f->builtin_bits.data() + (size_t)(cp - kFirstCp) * 5;
    const int base_w = 5 * s, h = 7 * s;
    /* 斜体：顶行最大右移 max(1, (h-1)/4) 像素（整数、确定性；小字号也至少有 1px 倾斜） */
    const int shear = italic ? std::max(1, (h - 1) / 4) : 0;
    const int dilate = bold ? s : 0;                 /* 水平膨胀：1*s 像素 */
    const int w = base_w + shear + dilate;
    out.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 5; ++x) {
            const bool on = (col[x] >> y) & 1u;
            if (!on) continue;
            for (int dy = 0; dy < s; ++dy) {
                const int py = y * s + dy;
                /* 逐设备行错位：顶行右移最多 shear，底行 0（整数除法 → 确定性） */
                const int row_dx = shear > 0 ? (shear * (h - 1 - py)) / (h - 1) : 0;
                for (int dx = 0; dx < s; ++dx) {
                    const int px = x * s + dx + row_dx;
                    for (int bx = 0; bx <= dilate; ++bx) {
                        uint8_t* p = out.data() + ((size_t)py * w + (size_t)(px + bx)) * 4;
                        p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
                    }
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
#if defined(MAQUE_HAVE_HARFBUZZ)
    for (auto& e : font->hb_fonts) if (e.second) hb_font_destroy(e.second);
    font->hb_fonts.clear();
#endif
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

const Glyph* ensure_glyph_gid(mq_font* f, Atlas& a, uint32_t gid, uint32_t cp, float size,
                              bool bold, bool italic);
const Glyph* ensure_glyph(mq_font* f, Atlas& a, uint32_t cp, float size, bool bold, bool italic) {
#if defined(MAQUE_HAVE_FREETYPE)
    if (f && f->face && !f->builtin)
        return ensure_glyph_gid(f, a, FT_Get_Char_Index(f->face, cp), cp, size, bold, italic);
#endif
    return ensure_glyph_gid(f, a, cp, cp, size, bold, italic);   /* 内置位图字体：gid = cp */
}

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
    for (uint32_t cp = kFirstCp; cp <= kLastCp; ++cp) (void)ensure_glyph(f, out, cp, sz, false, false);
    return out;
}

/* 按需栅格化一个字形并打包：查找键 = (gid, bold, italic)（内置字体用 cp 作 gid）。
 * 字形索引来自 HarfBuzz；`FT_Load_Char` 只在 codepoint 布局路径里先映射成 gid。
 * 普通样式（bold/italic 均为 false）的栅格化与打包路径与旧实现**完全相同**（逐字节不变）。 */
const Glyph* ensure_glyph_gid(mq_font* f, Atlas& a, uint32_t gid, uint32_t cp, float size,
                              bool bold, bool italic) {
    for (const auto& g : a.glyphs) {
        if (f->builtin ? (g.cp == cp && g.bold == bold && g.italic == italic)
                       : (g.gid == gid && g.bold == bold && g.italic == italic))
            return &g;
    }
    if (a.full) return nullptr;

    Glyph g;
    g.cp = cp;
    g.gid = gid;
    g.bold = bold;
    g.italic = italic;
    std::vector<uint8_t> bmp;                 /* RGBA（白字 + alpha） */
    int bw = 0, bh = 0;
    if (f->builtin) {
        builtin_glyph_bitmap(f, cp, size, bold, italic, bmp, &bw, &bh);
        /* 推进：普通/斜体为位图宽 +1（旧行为）；粗体额外 +s（膨胀像素数） */
        const int s = builtin_scale_of(size);
        g.advance = (bw > 0 && bh > 0) ? (float)(5 * s + 1 + (bold ? s : 0)) : 1.0F;
        g.bearing_y = bh;
    }
#if defined(MAQUE_HAVE_FREETYPE)
    else if (f->face) {
        if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)(size + 0.5F)) != 0) return nullptr;
        /* 防御性复位：保证普通字形不会被之前样式字形残留的变换污染 */
        FT_Set_Transform(f->face, nullptr, nullptr);
        if (bold) {
            /* 粗体：加载轮廓（不渲染）→ 轮廓膨胀 → 再渲染；strength 单位 26.6 像素 */
            if (italic) {
                FT_Matrix m;
                m.xx = 0x10000; m.xy = 0x4000;      /* x' = x + 0.25*y（16.16 定点剪切） */
                m.yx = 0;       m.yy = 0x10000;
                FT_Set_Transform(f->face, &m, nullptr);
            }
            const FT_Error err = FT_Load_Glyph(f->face, gid, FT_LOAD_DEFAULT);
            if (italic) FT_Set_Transform(f->face, nullptr, nullptr);   /* 立即复位，避免污染后续字形 */
            if (err != 0) return nullptr;
            FT_GlyphSlot slot = f->face->glyph;
            if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
                (void)FT_Outline_Embolden(&slot->outline, (FT_Pos)(bold_strength_of(size) << 6));
            if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return nullptr;
        } else if (italic) {
            FT_Matrix m;
            m.xx = 0x10000; m.xy = 0x4000;
            m.yx = 0;       m.yy = 0x10000;
            FT_Set_Transform(f->face, &m, nullptr);
            const FT_Error err = FT_Load_Glyph(f->face, gid, FT_LOAD_RENDER);
            FT_Set_Transform(f->face, nullptr, nullptr);               /* 立即复位 */
            if (err != 0) return nullptr;
        } else {
            if (FT_Load_Glyph(f->face, gid, FT_LOAD_RENDER) != 0) return nullptr;
        }
        FT_GlyphSlot sl = f->face->glyph;
        bw = (int)sl->bitmap.width;
        bh = (int)sl->bitmap.rows;
        g.bearing_x = sl->bitmap_left;
        g.bearing_y = sl->bitmap_top;
        g.advance = (float)(sl->advance.x >> 6);
        if (bold) g.advance += (float)bold_strength_of(size);      /* 粗体推进加宽（约 +strength） */
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
            py = a.cur_y + a.row_h + kAtlasPad;      /* 从**当前行底**起新行（cur_y 必须随之推进） */
            a.cur_y = py;
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

/* ============================ UAX #14 换行 / UAX #29 字素边界 ============================
 * 由 vendored libunibreak 提供，作为 WORD wrap 的断行机会来源（空格/CJK/hyphen/禁则/组合字都覆盖）。
 * `MAQUE_TEXT_LINEBREAK=0` 可回退旧的"空格 + CJK 逐字"规则；libunibreak 缺席时同样回退。 */
struct UaxBreakMap {
    std::vector<uint32_t> cps;        /* 码点（回退路径也用它，避免重复解码） */
    std::vector<uint32_t> byte_off;   /* 每个码点的 UTF-8 起始字节 */
    std::vector<char> lb;             /* set_linebreaks_utf32：lb[i] = 第 i 个码点之后能否断行 */
    std::vector<char> gb;             /* set_graphemebreaks_utf32：gb[i] = 第 i 个码点之后是否字素边界 */
    bool valid = false;
};

bool uax_linebreak_enabled() {
    const std::string e = env_value("MAQUE_TEXT_LINEBREAK");
    return !(e == "0" || e == "off");
}

void build_uax_breaks(const std::string& text, UaxBreakMap& m) {
    m = UaxBreakMap{};
#if defined(MAQUE_HAVE_LIBUNIBREAK)
    if (!uax_linebreak_enabled() || text.empty()) return;
    std::vector<utf32_t> cps;
    m.cps.reserve(text.size());
    m.byte_off.reserve(text.size());
    for (const char* cur = text.c_str(); *cur;) {
        uint32_t cp = 0;
        const char* nx = utf8_next(cur, &cp);
        if (nx == cur) return;                     /* 非法 UTF-8：回退 */
        m.byte_off.push_back((uint32_t)(cur - text.data()));
        m.cps.push_back(cp);
        cps.push_back((utf32_t)cp);
        cur = nx;
    }
    const size_t n = cps.size();
    if (!n) return;
    m.lb.assign(n, LINEBREAK_NOBREAK);
    m.gb.assign(n, GRAPHEMEBREAK_NOBREAK);
    init_linebreak();
    init_graphemebreak();
    set_linebreaks_utf32(cps.data(), n, nullptr, m.lb.data());
    set_graphemebreaks_utf32(cps.data(), n, nullptr, m.gb.data());
    m.valid = true;
#else
    (void)text;
#endif
}

/* 第 i 个码点之后（码点 i 与 i+1 之间）是否是合法的 WORD 断行点。 */
bool uax_break_after_cp(const UaxBreakMap& m, size_t i) {
    if (!m.valid || i + 1 >= m.lb.size()) return false;
    const char lb = m.lb[i];
    return (lb == LINEBREAK_ALLOWBREAK || lb == LINEBREAK_MUSTBREAK) &&
           m.gb[i] == GRAPHEMEBREAK_BREAK;
}

/* 字节偏移 byte_end（某个 cluster 的结束）对应的码点边界之后能否断行。 */
bool uax_break_after_byte(const UaxBreakMap& m, uint32_t byte_end) {
    if (!m.valid) return false;
    const auto it = std::lower_bound(m.byte_off.begin(), m.byte_off.end(), byte_end);
    if (it == m.byte_off.end()) return false;      /* 文本末尾：由 flush 收尾 */
    const size_t i = (size_t)(it - m.byte_off.begin());
    if (i == 0) return false;
    return uax_break_after_cp(m, i - 1);
}

/* 字节偏移 byte_end 对应的码点边界之后是否是字素边界（CHAR wrap 用：可任意断，但不能拆字素）。 */
bool uax_grapheme_after_byte(const UaxBreakMap& m, uint32_t byte_end) {
    if (!m.valid) return true;
    const auto it = std::lower_bound(m.byte_off.begin(), m.byte_off.end(), byte_end);
    if (it == m.byte_off.end()) return true;       /* 文本末尾 */
    const size_t i = (size_t)(it - m.byte_off.begin());
    if (i == 0) return false;
    return m.gb[i - 1] == GRAPHEMEBREAK_BREAK;
}

/* 布局 ppem：逻辑字号 → **设计像素**整数 ppem（FreeType 只接受整数 ppem）。
 * 排版（advance / 行高 / 基线 / 折行）只用它，因此与窗口大小、渲染倍率无关（docs/13）。 */
float layout_px_of(float logical) {
    float px = logical > 0.0F ? logical : 16.0F;
    if (px < 1.0F) px = 1.0F;
    return (float)(int)(px + 0.5F);
}

/* 栅格化倍率：device px / design px（>=1 为 HiDPI/窗口放大；<=0 视为 1）。 */
float raster_scale_of(const mq_text_style& st) {
    return st.device_scale > 0.0F ? st.device_scale : 1.0F;
}

/* 栅格化 ppem：逻辑字号 × 渲染倍率。只决定字形位图分辨率/清晰度，**不参与排版**
 * → 改窗口大小（渲染倍率变化）不会改变折行、行数与设计像素度量。 */
float raster_px_of(float logical, float raster_scale) {
    float px = (logical > 0.0F ? logical : 16.0F) * (raster_scale > 0.0F ? raster_scale : 1.0F);
    if (px < 1.0F) px = 1.0F;
    return (float)(int)(px + 0.5F);
}

/* 字体 ascent（**设计像素**；入参为设计 px 字号）。 */
void font_extents(mq_font* f, float design_size, float* ascent) {
    *ascent = design_size * 0.8F;                     /* 内置位图字体缺省 */
#if defined(MAQUE_HAVE_FREETYPE)
    if (f && !f->builtin && f->face) {
        if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)(design_size + 0.5F)) == 0)
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

extern "C" int mq_font_glyph_at(mq_font* font, float size, uint32_t index, mq_text_glyph* out) {
    if (!font || !out) return MQ_TEXT_ERR_ARG;
    Atlas& a = atlas_for(font, size);
    if (index >= a.glyphs.size()) return MQ_TEXT_ERR_ARG;
    const Glyph& g = a.glyphs[index];
    std::memset(out, 0, sizeof *out);
    out->codepoint = g.cp;
    out->w = (float)g.w;
    out->h = (float)g.h;
    out->ax = g.ax; out->ay = g.ay;
    out->bearing_x = g.bearing_x;
    out->bearing_y = g.bearing_y;
    out->device_size = a.size;
    if (g.w > 0 && g.h > 0) {
        out->u0 = (float)g.ax / (float)a.w;   out->v0 = (float)g.ay / (float)a.h;
        out->u1 = (float)(g.ax + g.w) / (float)a.w;
        out->v1 = (float)(g.ay + g.h) / (float)a.h;
    }
    return MQ_TEXT_OK;
}

extern "C" int mq_font_atlas_size(mq_font* font, float size, uint32_t* out_w, uint32_t* out_h) {
    if (!font) return MQ_TEXT_ERR_ARG;
    Atlas& a = atlas_for(font, size);
    if (out_w) *out_w = a.w;
    if (out_h) *out_h = a.h;
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
 * 支持：<color=#rrggbb>…</color>、<size=NN>…</size>、<b>…</b>、<i>…</i>、<br/>
 * 规则：标签大小写不敏感；未知标签按纯文本输出（不报错）；属性缺失即忽略；可嵌套（栈式恢复）。
 * 解析是确定性的：单遍扫描，无回溯。 */

namespace {

struct Run {
    std::string text;
    float r = 1, g = 1, b = 1, a = 1;
    float size = 0.0F;      /* 0 = 用样式默认字号 */
    bool  bold = false;
    bool  italic = false;
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

/* 解析富文本 → run 序列（栈式处理嵌套标签，最内层生效）
 * 样式级 st.bold / st.italic 作为初始状态：富文本 <b>/<i> 在 run 上叠加。 */
void parse_runs(const std::string& in, const mq_text_style& st, std::vector<Run>& out) {
    Run cur;
    cur.r = st.r; cur.g = st.g; cur.b = st.b; cur.a = st.a;
    cur.bold = st.bold != 0;
    cur.italic = st.italic != 0;
    /* 开标签保存**完整状态**，关闭时整体恢复 → 支持任意嵌套顺序（<b><i>…</i></b> 等） */
    struct Saved { float r = 1, g = 1, b = 1; float sz = 0; bool bold = false; bool italic = false; };
    std::vector<Saved> stack;
    auto snapshot = [&]() {
        Saved s; s.r = cur.r; s.g = cur.g; s.b = cur.b; s.sz = cur.size;
        s.bold = cur.bold; s.italic = cur.italic;
        return s;
    };
    auto restore = [&]() {
        if (stack.empty()) return;
        const Saved s = stack.back();
        stack.pop_back();
        cur.r = s.r; cur.g = s.g; cur.b = s.b; cur.size = s.sz;
        cur.bold = s.bold; cur.italic = s.italic;
    };
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
                if (closing) restore();
                else { stack.push_back(snapshot()); cur.bold = true; }
                i = close + 1; continue;
            }
            if (body == "i") {
                flush();
                if (closing) restore();
                else { stack.push_back(snapshot()); cur.italic = true; }
                i = close + 1; continue;
            }
            if (body.rfind("color", 0) == 0) {
                flush();
                if (closing) restore();
                else {
                    stack.push_back(snapshot());
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
                if (closing) restore();
                else {
                    stack.push_back(snapshot());
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
    float x, y;                     /* 笔位置 / 行顶（**设计像素**） */
    float x0, y0;                   /* 位图左上角（设计像素；已按设备像素网格吸附） */
    float size, device_size;        /* 逻辑字号 / 栅格化 ppem（设备像素，只影响位图） */
    float ascent = 0.0F;            /* 所属 run 的基线（设计像素，相对行顶；行内取 max 对齐） */
    float raster_advance = 0.0F;    /* 呈现推进（**设备像素**；普通字形整数，kerning 增量可带小数） */
    float x_off = 0.0F;             /* shaping 的 x 偏移（设计像素） */
    float r, g, b, a;
    int w, h, bearing_x, bearing_y; /* 位图宽高 / FreeType bearing（**设备像素**） */
    int ax = 0, ay = 0;             /* 图集内像素原点 */
    float u0, v0, u1, v1;
};

/* 样式化推进（换行/排版测量用）：单位 = **设计像素**。
 * 普通样式 == mq_font_advance（旧行为不变）；粗体按布局字号加宽（与位图加粗同源）。 */
float styled_advance(mq_font* f, uint32_t cp, float layout_size, bool bold) {
    const float a = mq_font_advance(f, cp, layout_size);
    if (!bold) return a;
    return a + (float)(f->builtin ? builtin_scale_of(layout_size) : bold_strength_of(layout_size));
}

/* ============================ HarfBuzz shaping ============================
 * 每个排版单元 = 一个 shaped glyph（连字/多码点 cluster 合并为一个单元）。
 * 这层只产出 **设计像素** 的推进/偏移；位图仍由 FreeType 按 (gid, ppem, 合成样式) 栅格化。
 * `MAQUE_TEXT_SHAPING=0` 或没有 HarfBuzz 时走旧逐码点路径（逐字节不变）。 */
bool text_shaping_enabled() {
#if defined(MAQUE_HAVE_HARFBUZZ)
    const std::string e = env_value("MAQUE_TEXT_SHAPING");
    if (e == "0" || e == "off") return false;
    return true;
#else
    return false;
#endif
}

struct ShapedCell {
    uint32_t gid = 0;
    uint32_t cp = 0;            /* cluster 首码点（公开 API 的 codepoint） */
    float advance = 0.0F;       /* 设计像素（含 kerning / bold 加宽） */
    float base_advance = 0.0F;  /* 设计像素：无 kerning 的逐码点推进（连字 = advance） */
    float x_offset = 0.0F;      /* 设计像素 mark/连写偏移 */
    bool  break_after = false;  /* 空格 / CJK / UAX14：WORD 模式的断行机会 */
    bool  grapheme_break = true;/* UAX29：本单元之后是否字素边界（CHAR wrap 用） */
    bool  ligature = false;     /* 多码点 cluster 或 gid != 首码点 gid */
    bool  shaped = false;       /* true = 来自 HarfBuzz（gid 有效）；false = 码点回退 */
};

/* UTF-8 码点跨度（cluster 字节区间 [begin,end) 内的码点数 + 首码点） */
void cluster_info(const std::string& s, uint32_t begin, uint32_t end, uint32_t* first_cp, int* cp_count) {
    *first_cp = 0;
    *cp_count = 0;
    uint32_t p = begin;
    while (p < end) {
        uint32_t cp = 0;
        const char* cur = s.data() + p;
        const char* nx = utf8_next(cur, &cp);
        if (nx == cur) break;
        if (*cp_count == 0) *first_cp = cp;
        ++*cp_count;
        p = (uint32_t)(nx - s.data());
    }
}

bool shape_run(mq_font* f, const std::string& text, float lsize, bool bold,
               const UaxBreakMap& uax, std::vector<ShapedCell>& out) {
#if defined(MAQUE_HAVE_HARFBUZZ)
    if (!f || f->builtin || text.empty() || !text_shaping_enabled()) return false;
    if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)(lsize + 0.5F)) != 0) return false;
    /* hb-ft font 必须在 face 尺寸设置**之后**创建：创建时若 face->size 为空，scale 会是 0，
     * positions 单位会错（实测 Segoe UI 的 adv 只有一半）。按设计 ppem 缓存。 */
    hb_font_t* hbf = nullptr;
    for (auto& e : f->hb_fonts) if (e.first == lsize) { hbf = e.second; break; }
    if (!hbf) {
        hbf = hb_ft_font_create_referenced(f->face);
        if (!hbf) return false;
        hb_ft_font_set_load_flags(hbf, FT_LOAD_DEFAULT);
        hb_font_set_scale(hbf, (int)std::lround(lsize * 64.0F), (int)std::lround(lsize * 64.0F));
        f->hb_fonts.emplace_back(lsize, hbf);
    }
    hb_buffer_t* buf = hb_buffer_create();
    if (!buf) return false;
    hb_buffer_add_utf8(buf, text.data(), (int)text.size(), 0, (int)text.size());
    hb_buffer_guess_segment_properties(buf);
    hb_shape(hbf, buf, nullptr, 0);
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    if (!info || !pos || n == 0) { hb_buffer_destroy(buf); return false; }
    const float bold_add = bold ? (float)bold_strength_of(lsize) : 0.0F;
    out.clear();
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        ShapedCell c;
        c.gid = info[i].codepoint;
        c.shaped = true;
        const uint32_t begin = info[i].cluster;
        uint32_t end = (i + 1 < n) ? info[i + 1].cluster : (uint32_t)text.size();
        if (end < begin) end = begin;                     /* RTL（cluster 逆序）时保守处理 */
        uint32_t cp = 0;
        int cp_count = 0;
        cluster_info(text, begin, end, &cp, &cp_count);
        c.cp = cp;
        c.advance = (float)pos[i].x_advance / 64.0F + bold_add;
        c.x_offset = (float)pos[i].x_offset / 64.0F;
        c.break_after = false;
        c.grapheme_break = true;
        c.ligature = (cp_count > 1);
        if (uax.valid) {
            c.break_after = uax_break_after_byte(uax, end);
            c.grapheme_break = uax_grapheme_after_byte(uax, end);
        } else {
            /* 回退（MAQUE_TEXT_LINEBREAK=0 或无 libunibreak）：空格/CJK 断行 */
            for (uint32_t p = begin; p < end;) {
                uint32_t t = 0;
                const char* cur = text.data() + p;
                const char* nx = utf8_next(cur, &t);
                if (nx == cur) break;
                if (t == ' ' || is_cjk(t)) c.break_after = true;
                p = (uint32_t)(nx - text.data());
            }
        }
        c.base_advance = c.advance;
        if (!c.ligature && cp) {
            const float plain = mq_font_advance(f, cp, lsize) + bold_add;
            const FT_UInt cpg = FT_Get_Char_Index(f->face, cp);
            if (c.gid != cpg) c.ligature = true;         /* 单码点被替换成其它字形（如竖排/替代形） */
            c.base_advance = plain;
        }
        out.push_back(c);
    }
    const int raw0 = n ? pos[0].x_advance : 0;
    hb_buffer_destroy(buf);
    if (!env_value("MAQUE_TRACE_SHAPE").empty()) {
        int xs = 0, ys = 0;
        hb_font_get_scale(hbf, &xs, &ys);
        float sum = 0.0F;
        for (const ShapedCell& c : out) sum += c.advance;
        std::print(stderr, "[shape] '{}' lsize={} n={} sum={:.1f} hbscale={},{} face_px={} raw0={} first:",
                   text, lsize, out.size(), sum, xs, ys,
                   f->face->size ? f->face->size->metrics.height : -1,
                   raw0);
        for (size_t i = 0; i < out.size() && i < 12; ++i)
            std::print(stderr, " [{} cp={} adv={:.2f} base={:.2f} brk={}]",
                       out[i].gid, out[i].cp, out[i].advance, out[i].base_advance, out[i].break_after);
        std::print(stderr, "\n");
    }
    return !out.empty();
#else
    (void)f; (void)text; (void)lsize; (void)bold; (void)out;
    return false;
#endif
}

/* 码点回退：与旧布局路径等价的排版单元（gid 由 ensure_glyph 内部映射）。 */
void build_fallback_cells(mq_font* f, const std::string& text, float lsize, bool bold,
                          const UaxBreakMap& uax, std::vector<ShapedCell>& out) {
    out.clear();
    if (uax.valid && uax.cps.size() == uax.byte_off.size()) {
        for (size_t i = 0; i < uax.cps.size(); ++i) {
            ShapedCell c;
            c.cp = uax.cps[i];
            c.advance = styled_advance(f, c.cp, lsize, bold);
            c.base_advance = c.advance;
            c.break_after = uax_break_after_cp(uax, i);
            c.grapheme_break = (i + 1 >= uax.gb.size()) ? true
                                                        : uax.gb[i] == GRAPHEMEBREAK_BREAK;
            out.push_back(c);
        }
        return;
    }
    for (const char* cur = text.c_str(); *cur;) {
        uint32_t cp = 0;
        const char* nx = utf8_next(cur, &cp);
        if (nx == cur) break;
        cur = nx;
        ShapedCell c;
        c.cp = cp;
        c.advance = styled_advance(f, cp, lsize, bold);
        c.base_advance = c.advance;
        c.break_after = (cp == ' ' || is_cjk(cp));
        out.push_back(c);
    }
}

/* 逐字符排版 + 换行 + 对齐（确定性：单向扫描，不回溯）。
 * 坐标/度量一律在**设计像素**空间 —— 与窗口大小/渲染倍率无关（窗口 resize 只改字形位图分辨率）。
 * 行内混排不同字号时：行基线 = 行顶 + max(run ascent)，各字号字形按 `bitmap_top` 回落到**同一基线**。 */
int layout_impl(mq_font* f, const mq_text_style& st, const char* text,
                std::vector<PlacedGlyph>* out, mq_text_metrics* metrics) {
    const float base = st.font_size > 0.0F ? st.font_size : 16.0F;
    const float raster_scale = raster_scale_of(st);
    const float base_layout = layout_px_of(base);
    const float base_line_h = mq_font_height(f, base_layout);
    std::vector<Run> runs;
    parse_runs(text ? text : "", st, runs);

    const float max_w = st.max_width;                    /* 设计像素 */
    float pen_x = 0.0F, pen_y = 0.0F;
    float line_h = 0.0F;
    uint32_t line_count = 0;
    struct Line {
        float width = 0.0F, y = 0.0F, height = 0.0F, ascent = 0.0F;
        std::vector<PlacedGlyph> glyphs;
    };
    std::vector<Line> lines(1);
    lines.back().y = 0.0F;

    auto end_line = [&]() {
        lines.back().width = pen_x;
        lines.back().height = line_h > 0.0F ? line_h : base_line_h;
        pen_y += lines.back().height + st.line_spacing;
        lines.emplace_back();
        lines.back().y = pen_y;
        pen_x = 0.0F;
        line_h = 0.0F;
        ++line_count;
    };

    for (const Run& run : runs) {
        if (run.line_break) { end_line(); continue; }
        const float size = run.size > 0.0F ? run.size : base;
        const float lsize = layout_px_of(size);                /* 布局 ppem（设计像素） */
        const float rsize = raster_px_of(size, raster_scale);  /* 栅格化 ppem（设备像素） */
        float run_ascent = 0.0F;
        font_extents(f, lsize, &run_ascent);
        Atlas& atlas = atlas_for(f, rsize);
        /* HarfBuzz：整个 run 一次 shape（GPOS kerning / GSUB 连字）；失败/关闭时回退逐码点。
         * 断行机会统一由 UAX #14（libunibreak）+ UAX #29 字素边界给出。 */
        UaxBreakMap uax;
        build_uax_breaks(run.text, uax);
        std::vector<ShapedCell> cells;
        if (!shape_run(f, run.text, lsize, run.bold, uax, cells))
            build_fallback_cells(f, run.text, lsize, run.bold, uax, cells);
        std::vector<ShapedCell> word;                          /* 词缓冲（shaped cell） */

        /* 落一个排版单元：记录设备像素位图 + 设计空间笔位置；x0/y0 留到行结束后按**行基线/设备推进**确定 */
        auto place = [&](const ShapedCell& c) {
            PlacedGlyph g{};
            g.cp = c.cp; g.x = pen_x + c.x_offset; g.y = pen_y; g.size = size; g.device_size = rsize;
            g.ascent = run_ascent;
            g.x_off = c.x_offset;
            g.r = run.r; g.g = run.g; g.b = run.b; g.a = run.a;
            /* run 级样式（parse_runs 已把样式级 st.bold/st.italic 折叠进 run 初始状态） */
            const Glyph* gl = c.shaped
                ? ensure_glyph_gid(f, atlas, c.gid, c.cp, rsize, run.bold, run.italic)
                : ensure_glyph(f, atlas, c.cp, rsize, run.bold, run.italic);
            /* 呈现推进 = 图集设备 advance（普通字形） + shaping 的 kerning 增量（设计→设备）。
             * 不能把设计 advance 直接乘 scale 再逐字吸附 —— 非整数倍缩放会得到 8/9/8/9…
             * 的字距抖动；普通字形的图集 advance 已是栅格化后的整数，增量只来自 kerning。 */
            const float bold_dev = run.bold
                ? (float)(f->builtin ? builtin_scale_of(rsize) : bold_strength_of(rsize)) : 0.0F;
            const float base_raster = (gl && gl->advance > 0.0F)
                ? gl->advance : mq_font_advance(f, c.cp, rsize) + bold_dev;
            const float delta_design = (c.shaped && !c.ligature) ? (c.advance - c.base_advance) : 0.0F;
            g.raster_advance = base_raster + delta_design * raster_scale;
            if (gl) {
                g.w = gl->w; g.h = gl->h; g.bearing_x = gl->bearing_x; g.bearing_y = gl->bearing_y;
                g.ax = gl->ax; g.ay = gl->ay;
                if (gl->w > 0 && gl->h > 0) {
                    g.u0 = (float)gl->ax / (float)atlas.w; g.v0 = (float)gl->ay / (float)atlas.h;
                    g.u1 = (float)(gl->ax + gl->w) / (float)atlas.w;
                    g.v1 = (float)(gl->ay + gl->h) / (float)atlas.h;
                }
            }
            lines.back().glyphs.push_back(g);
            pen_x += c.advance;                                  /* 设计像素推进（含 kerning/连字） */
            line_h = std::max(line_h, mq_font_height(f, lsize));
        };

        auto flush_word = [&]() {
            if (word.empty()) return;
            if (st.wrap == MQ_TEXT_WRAP_CHAR) {
                bool prev_grapheme = true;               /* 当前 cell 之前是否字素边界 */
                for (const ShapedCell& c : word) {
                    if (max_w > 0.0F && pen_x > 0.0F && pen_x + c.advance > max_w && prev_grapheme)
                        end_line();
                    place(c);
                    prev_grapheme = c.grapheme_break;
                }
            } else {
                float wsum = 0.0F;
                for (const ShapedCell& c : word) wsum += c.advance;
                if (max_w > 0.0F && pen_x > 0.0F && pen_x + wsum > max_w) end_line();
                for (const ShapedCell& c : word) place(c);
            }
            word.clear();
        };

        for (const ShapedCell& c : cells) {
            word.push_back(c);
            /* WORD：空格后断行；CJK 逐字可断（无空格语言不会整段溢出） */
            if (st.wrap == MQ_TEXT_WRAP_WORD && c.break_after) flush_word();
        }
        flush_word();                                          /* NONE/CHAR/WORD 统一收尾 */
    }
    lines.back().width = pen_x;
    lines.back().height = line_h > 0.0F ? line_h : base_line_h;
    ++line_count;
    if (line_count == 0) line_count = 1;

    float max_line_w = 0.0F;
    for (Line& l : lines) {
        max_line_w = std::max(max_line_w, l.width);
        /* 行基线 = 行内最大 run ascent（同屏不同字号共享基线，修此前逐 run 用自身 ascent 的错位） */
        l.ascent = 0.0F;
        for (const PlacedGlyph& g : l.glyphs) l.ascent = std::max(l.ascent, g.ascent);
        if (l.ascent <= 0.0F) {
            float a = 0.0F;
            font_extents(f, base_layout, &a);
            l.ascent = a;
        }
    }
    float total_h = 0.0F;
    for (const Line& l : lines) total_h += l.height + st.line_spacing;
    if (total_h > 0.0F) total_h -= st.line_spacing;

    uint32_t count = 0;
    if (out) {
        out->clear();
        /* 呈现用的**设备行顶**：逐行按设备整数行高累计（与横向设备 advance 同理），
         * 否则 0.833× 等倍率下「每行各自取整」会让行基线间距 17/16/17 抖动（整体看"波浪状"）。 */
        int line_top_dev = 0;
        for (Line& l : lines) {
            const float box_w = max_w > 0.0F ? max_w : max_line_w;
            float dx = 0.0F;
            if (st.align == MQ_TEXT_ALIGN_CENTER) dx = (box_w - l.width) * 0.5F;
            else if (st.align == MQ_TEXT_ALIGN_END) dx = box_w - l.width;
            /* 呈现坐标（pixel_snap）：
             *   · 行基线只吸附**一次** → 行内所有字号/字形共享同一设备像素基线；
             *   · 行与行之间按**设备整数行高**累计 → 行距均匀，不受非整数倍 ss 的取整抖动影响；
             *   · 横向用**设备整数 advance** 累计 → 字距均匀（1.25×/0.83× 不会 14/14/13/14）。
             * 设计语义的 g.x/y 仍保持与渲染倍率无关（g.x += dx 供排版/测试读取）。 */
            const int baseline_dev = (raster_scale == 1.0F)
                ? (int)std::lround(l.y + l.ascent)            /* 1×：与旧行为逐像素一致 */
                : line_top_dev + (int)std::lround(l.ascent * raster_scale);
            /* 设备笔位置用 float 累计：普通字形 advance 是整数（字距均匀），只有 shaping 的
             * kerning 增量带小数（按 shaping 语义落位）；逐字取整仍只在最终 x0 做一次。 */
            float pen_dev = std::floor(dx * raster_scale + 0.5F);
            for (PlacedGlyph& g : l.glyphs) {
                g.x += dx;                                   /* 笔位置（排版语义） */
                if (st.pixel_snap) {
                    const int x_dev = (int)std::floor(pen_dev + g.x_off * raster_scale + 0.5F);
                    g.x0 = (float)(x_dev + g.bearing_x) / raster_scale;
                    g.y0 = (float)(baseline_dev - g.bearing_y) / raster_scale;
                    pen_dev += g.raster_advance;
                } else {
                    /* 不吸附：直接设计像素落位（开发者可关 pixel_snap 做几何精确对照） */
                    g.x0 = g.x + (float)g.bearing_x / raster_scale;
                    g.y0 = l.y + l.ascent - (float)g.bearing_y / raster_scale;
                }
                out->push_back(g);
            }
            if (raster_scale != 1.0F)
                line_top_dev += (int)std::lround(l.height * raster_scale)
                              + (int)std::lround(st.line_spacing * raster_scale);
            count += (uint32_t)l.glyphs.size();
        }
    } else {
        for (const Line& l : lines) count += (uint32_t)l.glyphs.size();
    }

    if (metrics) {
        /* width/height/baseline/ascent 均为**设计像素**（与窗口/渲染倍率无关）。 */
        metrics->width = max_w > 0.0F ? std::min(max_line_w, max_w) : max_line_w;
        metrics->height = total_h;
        metrics->line_count = line_count;
        metrics->glyph_count = count;
        metrics->baseline = lines.empty() ? 0.0F : lines.front().ascent;   /* 首行基线（行顶→基线前距离） */
        float base_ascent = 0.0F;
        font_extents(f, base_layout, &base_ascent);
        metrics->ascent = base_ascent;
        metrics->device_scale = raster_scale;              /* 仅报告栅格化倍率（不参与排版） */
    }
    return MQ_TEXT_OK;
}

} // namespace

extern "C" int mq_text_measure(mq_font* font, const mq_text_style* style, const char* text,
                               mq_text_metrics* out_metrics) {
    MQ_TRACY_ZONE_N(text, "measure");
    if (!font || !style || !out_metrics) return MQ_TEXT_ERR_ARG;
    return layout_impl(font, *style, text, nullptr, out_metrics);
}

extern "C" int mq_text_layout(mq_font* font, const mq_text_style* style, const char* text,
                              mq_text_glyph* out, uint32_t cap, uint32_t* out_count,
                              mq_text_metrics* out_metrics) {
    MQ_TRACY_ZONE_N(text, "layout");
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
