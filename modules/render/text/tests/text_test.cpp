// 文本渲染单测：字体加载/回退、图集确定性、排版（换行/对齐/行距）、富文本标记解析。
#include "text/mq_text.h"

#include <print>
#include <vector>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static mq_text_style style(float size, uint32_t wrap, uint32_t align, float max_w) {
    mq_text_style st{};
    st.struct_size = (uint32_t)sizeof st;
    st.font_size = size;
    st.wrap = wrap;
    st.align = align;
    st.max_width = max_w;
    st.r = st.g = st.b = st.a = 1.0F;
    return st;
}

int main() {
    // ---- 1) 内置位图字体（无外部依赖，headless 可用）----
    mq_font* f = mq_font_builtin();
    CHECK(f != nullptr);
    CHECK(mq_font_height(f, 16.0F) > 0.0F);
    CHECK(mq_font_advance(f, 'A', 16.0F) > 0.0F);

    // 图集：确定性（两次构建逐字节一致）+ 尺寸合理
    uint32_t w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    CHECK(mq_font_atlas(f, 16.0F, nullptr, 0, &w1, &h1) == MQ_TEXT_ERR_CAP);
    CHECK(w1 > 0 && h1 > 0);
    std::vector<uint8_t> a1(w1 * h1 * 4), a2(w1 * h1 * 4);
    CHECK(mq_font_atlas(f, 16.0F, a1.data(), a1.size(), &w1, &h1) == MQ_TEXT_OK);
    CHECK(mq_font_atlas(f, 16.0F, a2.data(), a2.size(), &w2, &h2) == MQ_TEXT_OK);
    CHECK(w1 == w2 && h1 == h2);
    CHECK(a1 == a2);                                   // 逐字节确定性
    bool any_ink = false;
    for (size_t i = 3; i < a1.size(); i += 4) if (a1[i] > 0) { any_ink = true; break; }
    CHECK(any_ink);                                     // 图集里确有字形像素

    // ---- 2) 排版：单行测量与字形数 ----
    {
        mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
        mq_text_metrics m{};
        CHECK(mq_text_measure(f, &st, "Hello", &m) == MQ_TEXT_OK);
        CHECK(m.line_count == 1);
        CHECK(m.glyph_count == 5);
        CHECK(m.width > 0.0F && m.height > 0.0F);
        std::vector<mq_text_glyph> glyphs(64);
        uint32_t n = 0;
        CHECK(mq_text_layout(f, &st, "Hello", glyphs.data(), (uint32_t)glyphs.size(), &n, &m) == MQ_TEXT_OK);
        CHECK(n == 5);
        CHECK(glyphs[0].x < glyphs[1].x);                // 依次右移
        CHECK(glyphs[0].u1 > glyphs[0].u0);              // 有 UV 范围
        CHECK(glyphs[0].size == 16.0F);
    }

    // ---- 3) 换行：按词（超宽自动折行）与显式 \n ----
    {
        mq_text_style st = style(16.0F, MQ_TEXT_WRAP_WORD, MQ_TEXT_ALIGN_BEGIN, 60.0F);
        mq_text_metrics m{};
        CHECK(mq_text_measure(f, &st, "aaa bbb ccc ddd eee", &m) == MQ_TEXT_OK);
        CHECK(m.line_count >= 3);
        CHECK(m.width <= 60.0F + 1.0F);
        mq_text_style st2 = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
        mq_text_metrics m2{};
        CHECK(mq_text_measure(f, &st2, "one\ntwo\nthree", &m2) == MQ_TEXT_OK);
        CHECK(m2.line_count == 3);
        CHECK(m2.height > m.height * 0.5F);
    }

    // ---- 4) 对齐：Center/End 使首字形右移，End > Center > Begin ----
    {
        mq_text_style sb = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 200.0F);
        mq_text_style sc = sb; sc.align = MQ_TEXT_ALIGN_CENTER;
        mq_text_style se = sb; se.align = MQ_TEXT_ALIGN_END;
        std::vector<mq_text_glyph> gb(32), gc(32), ge(32);
        uint32_t nb = 0, nc = 0, ne = 0;
        mq_text_metrics m{};
        CHECK(mq_text_layout(f, &sb, "abc", gb.data(), 32, &nb, &m) == MQ_TEXT_OK);
        CHECK(mq_text_layout(f, &sc, "abc", gc.data(), 32, &nc, &m) == MQ_TEXT_OK);
        CHECK(mq_text_layout(f, &se, "abc", ge.data(), 32, &ne, &m) == MQ_TEXT_OK);
        CHECK(nb == nc && nc == ne);
        CHECK(gc[0].x > gb[0].x);
        CHECK(ge[0].x > gc[0].x);
    }

    // ---- 5) 富文本标记：颜色/字号生效，纯文本剥离正确 ----
    {
        const char* markup = "red <color=#FF0000>RED</color> big <size=32>BIG</size> end";
        char plain[256] = { 0 };
        CHECK(mq_text_strip_markup(markup, plain, sizeof plain) == MQ_TEXT_OK);
        CHECK(std::strcmp(plain, "red RED big BIG end") == 0);
        mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
        mq_text_metrics m{};
        CHECK(mq_text_measure(f, &st, markup, &m) == MQ_TEXT_OK);
        std::vector<mq_text_glyph> g(256);
        uint32_t n = 0;
        CHECK(mq_text_layout(f, &st, markup, g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
        CHECK(n == m.glyph_count);
        bool found_red = false, found_big = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (g[i].r > 0.9F && g[i].g < 0.1F && g[i].b < 0.1F) found_red = true;
            if (g[i].size > 30.0F) found_big = true;
        }
        CHECK(found_red);
        CHECK(found_big);
        // <br/> 换行
        mq_text_metrics mb{};
        CHECK(mq_text_measure(f, &st, "a<br/>b", &mb) == MQ_TEXT_OK);
        CHECK(mb.line_count == 2);
        // 未知标签按文本处理（不吞字）
        char plain2[128] = { 0 };
        CHECK(mq_text_strip_markup("<foo>x</foo>", plain2, sizeof plain2) == MQ_TEXT_OK);
        CHECK(std::strlen(plain2) > 0);
    }

    // ---- 6) 容量不足报错 + 所需数量（调用方可先测量再分配）----
    {
        mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
        mq_text_glyph one{};
        uint32_t n = 0;
        mq_text_metrics m{};
        CHECK(mq_text_layout(f, &st, "abcdef", &one, 1, &n, &m) == MQ_TEXT_ERR_CAP);
        CHECK(n == 6);
    }

    // ---- 8) UTF-8：多字节按**码点**排版（旧实现按字节会拆成多个伪字形）----
    {
        mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
        mq_text_metrics m{};
        CHECK(mq_text_measure(f, &st, "中A", &m) == MQ_TEXT_OK);
        CHECK(m.glyph_count == 2);                       // 不是 4（中 = 3 字节）
        CHECK(mq_text_measure(f, &st, "推箱子", &m) == MQ_TEXT_OK);
        CHECK(m.glyph_count == 3);
        char plain[64] = { 0 };
        CHECK(mq_text_strip_markup("中<color=#ff0000>文</color>", plain, sizeof plain) == MQ_TEXT_OK);
        CHECK(std::strcmp(plain, "中文") == 0);
        /* 注：CJK 断行需要含 CJK 字形的字体（内置 5x7 无 CJK → 推进为 1px），
         * 因此该用例放在下方"项目字体"段。 */
    }

    mq_font_destroy(f);

    // ---- 9) CJK 动态字形缓存（项目字体；缺失则跳过）----
    {
        const char* paths[] = {
            "scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
            "../../../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
            "../../../../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
        };
        mq_font* cjk = nullptr;
        for (const char* p : paths) { cjk = mq_font_load_file(p); if (cjk) break; }
        if (!cjk) {
            std::print("text_test: 项目字体不可用，跳过 CJK 用例\n");
        } else {
            mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
            /* ASCII 预热 + 按需增长：初次已含 95 个 ASCII 字形 */
            const uint32_t base = mq_font_glyph_count(cjk, 16.0F);
            CHECK(base >= 95);
            const uint32_t gen0 = mq_font_atlas_generation(cjk, 16.0F);
            mq_text_metrics m{};
            CHECK(mq_text_measure(cjk, &st, "推箱子中文渲染测试", &m) == MQ_TEXT_OK);
            CHECK(m.glyph_count == 9);
            std::vector<mq_text_glyph> g(m.glyph_count);
            uint32_t n = 0;
            CHECK(mq_text_layout(cjk, &st, "推箱子中文渲染测试", g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
            int drawn = 0;
            for (uint32_t i = 0; i < n; ++i) {
                CHECK(g[i].ax >= 0 && g[i].ay >= 0);      // 有图集像素原点
                if (g[i].w > 0.0F && g[i].h > 0.0F) ++drawn;
            }
            CHECK(drawn == 9);                            // 全部有字形（无缺失）
            CHECK(m.width > 100.0F);                      // CJK 宽度按全角推进
            CHECK(mq_font_atlas_generation(cjk, 16.0F) > gen0);      // 图集增长
            /* CJK 逐字可断行（无空格语言）：max_width 不足时应换行 */
            mq_text_style wst = style(16.0F, MQ_TEXT_WRAP_WORD, MQ_TEXT_ALIGN_BEGIN, 100.0F);
            mq_text_metrics wm{};
            CHECK(mq_text_measure(cjk, &wst, "推箱子中文渲染测试", &wm) == MQ_TEXT_OK);
            CHECK(wm.line_count >= 2);
            CHECK(mq_font_glyph_count(cjk, 16.0F) == base + 9);
            /* 图集确实变大（高度按需增长且缓冲随之扩容 —— 曾因未扩容导致越界写） */
            uint32_t aw = 0, ah = 0;
            CHECK(mq_font_atlas(cjk, 16.0F, nullptr, 0, &aw, &ah) == MQ_TEXT_ERR_CAP);
            CHECK(aw > 0 && ah > 2);
            std::vector<uint8_t> atlas((size_t)aw * ah * 4);
            CHECK(mq_font_atlas(cjk, 16.0F, atlas.data(), atlas.size(), &aw, &ah) == MQ_TEXT_OK);
            bool ink = false;
            for (size_t i = 3; i < atlas.size(); i += 4) if (atlas[i] > 0) { ink = true; break; }
            CHECK(ink);
            /* 大批量不同码点 → 图集持续增长且不越界（ASan 下同时验证） */
            std::string many;
            for (uint32_t cp = 0x4E00; cp < 0x4E00 + 300; ++cp) {
                many += (char)(0xE0 | (cp >> 12));
                many += (char)(0x80 | ((cp >> 6) & 0x3F));
                many += (char)(0x80 | (cp & 0x3F));
            }
            mq_text_style st2 = style(24.0F, MQ_TEXT_WRAP_CHAR, MQ_TEXT_ALIGN_BEGIN, 200.0F);
            mq_text_metrics m2{};
            CHECK(mq_text_measure(cjk, &st2, many.c_str(), &m2) == MQ_TEXT_OK);
            CHECK(mq_font_glyph_count(cjk, 24.0F) > 95);
            mq_font_destroy(cjk);
        }
    }

    // ---- 7) FreeType 路径（若可用）：加载系统字体并排版 ----
    {
        const char* paths[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/lato/Lato-Medium.ttf",
            "/usr/share/fonts/truetype/tuffy/tuffy_bold.ttf",
        };
        mq_font* ff = nullptr;
        for (const char* p : paths) { ff = mq_font_load_file(p); if (ff) break; }
        if (ff) {
            mq_text_style st = style(20.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
            mq_text_metrics m{};
            CHECK(mq_text_measure(ff, &st, "FreeType 123", &m) == MQ_TEXT_OK);
            CHECK(m.glyph_count == 12);
            CHECK(m.width > 40.0F);
            uint32_t aw = 0, ah = 0;
            CHECK(mq_font_atlas(ff, 20.0F, nullptr, 0, &aw, &ah) == MQ_TEXT_ERR_CAP);
            std::vector<uint8_t> atlas((size_t)aw * ah * 4);
            CHECK(mq_font_atlas(ff, 20.0F, atlas.data(), atlas.size(), &aw, &ah) == MQ_TEXT_OK);
            bool ink = false;
            for (size_t i = 3; i < atlas.size(); i += 4) if (atlas[i] > 0) { ink = true; break; }
            CHECK(ink);
            mq_font_destroy(ff);
        } else {
            std::print("text_test: FreeType 字体不可用，跳过真实字体用例\n");
        }
    }

    if (g_failures == 0) { std::print("text_test: all checks passed\n"); return 0; }
    std::print("text_test: {} failure(s)\n", g_failures);
    return 1;
}
