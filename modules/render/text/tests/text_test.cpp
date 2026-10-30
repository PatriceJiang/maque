// 文本渲染单测：字体加载/回退、图集确定性、排版（换行/对齐/行距）、富文本标记解析。
#include "text/mq_text.h"

#include <print>
#include <array>
#include <cmath>
#include <vector>
#if defined(MAQUE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif
#include <cstring>
#include <cstdlib>
#include <string>

/* HarfBuzz shaping 测试开关（Windows/MSVC 用 _putenv_s；其它平台 setenv）。 */
static void set_shaping_env(bool on) {
#if defined(_WIN32)
    _putenv_s("MAQUE_TEXT_SHAPING", on ? "1" : "0");
#else
    setenv("MAQUE_TEXT_SHAPING", on ? "1" : "0", 1);
#endif
}

/* UAX #14 换行测试开关（关闭 = 旧的“空格 + CJK”贪心规则）。 */
static void set_linebreak_env(bool on) {
#if defined(_WIN32)
    _putenv_s("MAQUE_TEXT_LINEBREAK", on ? "1" : "0");
#else
    setenv("MAQUE_TEXT_LINEBREAK", on ? "1" : "0", 1);
#endif
}

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

/* 项目自带 TTF（含中文）；相对路径覆盖两种测试工作目录（build 模块目录 / 仓库根）。 */
static mq_font* load_project_font(const char** out_path = nullptr) {
    static const char* const kPaths[] = {
        "scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
        "../../../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
        "../../../../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
    };
    for (const char* p : kPaths) {
        if (mq_font* f = mq_font_load_file(p)) {
            if (out_path) *out_path = p;
            return f;
        }
    }
    return nullptr;
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

    // 字体内存域（docs/29 §10.5）：只读统计；图集创建后必须非零，且不会因查询而增长
    const uint64_t font_mem0 = mq_font_memory_bytes(f);
    CHECK(font_mem0 > 0);
    CHECK(mq_font_memory_bytes(f) == font_mem0);
    CHECK(mq_font_memory_bytes(nullptr) == 0);

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
        /* 对齐必须同时平移位图矩形 x0（runtime 落 quad 用的是 x0；只平移 x 对齐对渲染完全无效 ——
         * 曾因此让 Begin/Center/End 三个 Label 的墨迹 bbox 完全重合）。 */
        CHECK(gc[0].x0 > gb[0].x0);
        CHECK(ge[0].x0 > gc[0].x0);
        CHECK(std::abs((gc[0].x0 - gb[0].x0) - (gc[0].x - gb[0].x)) < 1e-4F);
        CHECK(std::abs((ge[0].x0 - gb[0].x0) - (ge[0].x - gb[0].x)) < 1e-4F);
        /* pixel_snap：对齐平移后 x0 重新吸附回整数像素 */
        mq_text_style sp = sc; sp.pixel_snap = 1;
        std::vector<mq_text_glyph> gp(32);
        uint32_t np = 0;
        CHECK(mq_text_layout(f, &sp, "abc", gp.data(), (uint32_t)gp.size(), &np, &m) == MQ_TEXT_OK);
        CHECK(np == nb);
        for (uint32_t i = 0; i < np; ++i) CHECK(gp[i].x0 == std::floor(gp[i].x0));
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

    // ---- 10) 合成粗体/斜体：<b>/<i> 标记 + style.bold/italic（内置位图字体路径）----
    {
        // 10a) 标记剥离：<b>/<i> 剥离、大小写不敏感、可嵌套
        char pi[128] = { 0 };
        CHECK(mq_text_strip_markup("<i>it</i><B>bo</B>", pi, sizeof pi) == MQ_TEXT_OK);
        CHECK(std::strcmp(pi, "itbo") == 0);
        char pn[128] = { 0 };
        CHECK(mq_text_strip_markup("<b><i>ni</i> x</b>", pn, sizeof pn) == MQ_TEXT_OK);
        CHECK(std::strcmp(pn, "ni x") == 0);

        const mq_text_style sn = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
        mq_text_style sb = sn; sb.bold = 1;
        mq_text_style si = sn; si.italic = 1;

        // 10b) 粗体推进加宽；斜体推进不变
        mq_text_metrics mn{}, mb{}, mi{};
        CHECK(mq_text_measure(f, &sn, "Bold", &mn) == MQ_TEXT_OK);
        CHECK(mq_text_measure(f, &sb, "Bold", &mb) == MQ_TEXT_OK);
        CHECK(mq_text_measure(f, &si, "Bold", &mi) == MQ_TEXT_OK);
        CHECK(mn.glyph_count == 4 && mb.glyph_count == 4 && mi.glyph_count == 4);
        CHECK(mb.width > mn.width);                        // 粗体布局更宽
        CHECK(mi.width == mn.width);                       // 斜体推进不变
        // style 级与标记级同源：<b>/<i> 与 st.bold/st.italic 结果一致
        mq_text_metrics mmb{}, mmi{};
        CHECK(mq_text_measure(f, &sn, "<b>Bold</b>", &mmb) == MQ_TEXT_OK);
        CHECK(mq_text_measure(f, &sn, "<i>Bold</i>", &mmi) == MQ_TEXT_OK);
        CHECK(mmb.width == mb.width);
        CHECK(mmi.width == mi.width);

        std::vector<mq_text_glyph> gn(16), gb(16), gi(16);
        uint32_t nn = 0, nb = 0, ni = 0;
        CHECK(mq_text_layout(f, &sn, "Bold", gn.data(), 16, &nn, &mn) == MQ_TEXT_OK);
        CHECK(mq_text_layout(f, &sb, "Bold", gb.data(), 16, &nb, &mb) == MQ_TEXT_OK);
        CHECK(mq_text_layout(f, &si, "Bold", gi.data(), 16, &ni, &mi) == MQ_TEXT_OK);
        CHECK(nn == 4 && nb == 4 && ni == 4);
        CHECK(gb[1].x - gb[0].x > gn[1].x - gn[0].x);      // 粗体 advance 更宽
        CHECK(gi[1].x - gi[0].x == gn[1].x - gn[0].x);     // 斜体 advance 不变
        CHECK(gb[0].w > gn[0].w);                          // 粗体位图水平膨胀
        CHECK(gi[0].w > gn[0].w);                          // 斜体位图因剪切变宽

        // 10c) 斜体位图内容与普通不同（宽/高/bearing/x0/像素任一不同 → 判定生效且稳定）
        {
            uint32_t aw = 0, ah = 0;
            CHECK(mq_font_atlas(f, 16.0F, nullptr, 0, &aw, &ah) == MQ_TEXT_ERR_CAP);
            std::vector<uint8_t> atlas((size_t)aw * ah * 4);
            CHECK(mq_font_atlas(f, 16.0F, atlas.data(), atlas.size(), &aw, &ah) == MQ_TEXT_OK);
            auto glyph_bits = [&](const mq_text_glyph& g) {
                std::vector<uint8_t> v((size_t)g.w * (size_t)g.h);
                for (int y = 0; y < (int)g.h; ++y)
                    for (int x = 0; x < (int)g.w; ++x)
                        v[(size_t)y * (size_t)g.w + (size_t)x] =
                            atlas[(((size_t)(g.ay + y) * aw) + (size_t)(g.ax + x)) * 4 + 3];
                return v;
            };
            const std::vector<uint8_t> bn = glyph_bits(gn[0]), bi = glyph_bits(gi[0]);
            const bool italic_diff = gi[0].w != gn[0].w || gi[0].h != gn[0].h ||
                                     gi[0].bearing_x != gn[0].bearing_x || gi[0].x0 != gn[0].x0 ||
                                     bn != bi;
            CHECK(italic_diff);                            // 斜体确实不是普通字形
        }

        // 10d) 带样式字形作为额外条目追加进同一图集 → generation / glyph_count 增长
        const uint32_t gen0 = mq_font_atlas_generation(f, 16.0F);
        const uint32_t cnt0 = mq_font_glyph_count(f, 16.0F);
        mq_text_style sbi = sn; sbi.bold = 1; sbi.italic = 1;
        mq_text_metrics mbi{};
        CHECK(mq_text_measure(f, &sbi, "Zz", &mbi) == MQ_TEXT_OK);
        CHECK(mq_font_glyph_count(f, 16.0F) > cnt0);
        CHECK(mq_font_atlas_generation(f, 16.0F) > gen0);
        CHECK(mq_font_memory_bytes(f) >= font_mem0);       // 字形/图集增长不会让统计变小
        mq_text_metrics mbi2{};
        CHECK(mq_text_measure(f, &sn, "<b><i>Zz</i></b>", &mbi2) == MQ_TEXT_OK);
        CHECK(mbi2.width == mbi.width);                    // 嵌套 <b><i> == style 双置位

        // 10e) 确定性：同一输入两次测量/排版结果一致（styled 路径不引入状态漂移）
        mq_text_metrics d1{}, d2{};
        const char* styled = "<b><i>Ab</i></b>";
        CHECK(mq_text_measure(f, &sbi, styled, &d1) == MQ_TEXT_OK);
        CHECK(mq_text_measure(f, &sbi, styled, &d2) == MQ_TEXT_OK);
        CHECK(d1.width == d2.width && d1.height == d2.height && d1.glyph_count == d2.glyph_count);
        std::vector<mq_text_glyph> q1(8), q2(8);
        uint32_t qn1 = 0, qn2 = 0;
        CHECK(mq_text_layout(f, &sbi, styled, q1.data(), 8, &qn1, &d1) == MQ_TEXT_OK);
        CHECK(mq_text_layout(f, &sbi, styled, q2.data(), 8, &qn2, &d2) == MQ_TEXT_OK);
        CHECK(qn1 == 2 && qn2 == 2);
        for (uint32_t i = 0; i < qn1 && i < qn2; ++i) {
            CHECK(q1[i].codepoint == q2[i].codepoint);
            CHECK(q1[i].x == q2[i].x && q1[i].x0 == q2[i].x0);
            CHECK(q1[i].w == q2[i].w && q1[i].h == q2[i].h);
            CHECK(q1[i].ax == q2[i].ax && q1[i].ay == q2[i].ay);
        }
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
        const char* cjk_path = nullptr;
        for (const char* p : paths) { cjk = mq_font_load_file(p); if (cjk) { cjk_path = p; break; } }
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
            /* ⑨a 字形矩形**两两不重叠**且在图集范围内：
             *     曾因换行未推进打包游标 cur_y，导致后续行覆盖写到前面的行上（字形互相覆盖 →
             *     文字乱码；ASCII 单行时看不出，加入 CJK 后才暴露）。
             *     注意：像素级测试比对的是"渲染 vs 图集"，图集本身错了它抓不到 —— 必须在这里查。 */
            {
                uint32_t aw = 0, ahh = 0;
                mq_font_atlas(cjk, 16.0F, nullptr, 0, &aw, &ahh);
                mq_text_style st3 = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                const char* sample = "ABCabc0123推箱子中文渲染测试，。！";
                mq_text_metrics mm{};
                mq_text_measure(cjk, &st3, sample, &mm);
                std::vector<mq_text_glyph> gg(mm.glyph_count ? mm.glyph_count : 1);
                uint32_t nn = 0;
                mq_text_layout(cjk, &st3, sample, gg.data(), (uint32_t)gg.size(), &nn, &mm);
                std::vector<std::array<int, 4>> rects;      // x, y, w, h
                for (uint32_t i = 0; i < nn; ++i) {
                    if (gg[i].w <= 0.0F || gg[i].h <= 0.0F) continue;
                    CHECK(gg[i].ax >= 0 && gg[i].ay >= 0);
                    CHECK((uint32_t)(gg[i].ax + (int)gg[i].w) <= aw);          // 不越界
                    CHECK((uint32_t)(gg[i].ay + (int)gg[i].h) <= ahh);
                    rects.push_back({ gg[i].ax, gg[i].ay, (int)gg[i].w, (int)gg[i].h });
                }
                CHECK(rects.size() >= 20);
                int overlap = 0;
                for (size_t i = 0; i < rects.size(); ++i)
                    for (size_t j = i + 1; j < rects.size(); ++j) {
                        const bool sep = rects[i][0] + rects[i][2] <= rects[j][0] ||
                                         rects[j][0] + rects[j][2] <= rects[i][0] ||
                                         rects[i][1] + rects[i][3] <= rects[j][1] ||
                                         rects[j][1] + rects[j][3] <= rects[i][1];
                        if (!sep) ++overlap;
                    }
                CHECK(overlap == 0);
                std::vector<uint8_t> at((size_t)aw * ahh * 4);
                CHECK(mq_font_atlas(cjk, 16.0F, at.data(), at.size(), &aw, &ahh) == MQ_TEXT_OK);
                /* ⑨b 图集内容 == FreeType 直接栅格化的位图（**逐字节**）：
                 *      能抓住"写错位置 / 被后续字形覆盖"这类图集损坏（渲染 vs 图集的像素测试抓不到）。 */
#if defined(MAQUE_HAVE_FREETYPE)
                FT_Library lib = nullptr;
                FT_Face face = nullptr;
                int ft_ok = (cjk_path && FT_Init_FreeType(&lib) == 0 &&
                             FT_New_Face(lib, cjk_path, 0, &face) == 0) ? 1 : 0;
                int checked = 0, mismatch = 0;
                if (ft_ok) {
                    for (uint32_t i = 0; i < nn && checked < 6; ++i) {
                        const mq_text_glyph& q = gg[i];
                        if (q.w <= 0.0F || q.h <= 0.0F) continue;
                        if (FT_Set_Pixel_Sizes(face, 0, 16) != 0) continue;
                        if (FT_Load_Char(face, q.codepoint, FT_LOAD_RENDER) != 0) continue;
                        const FT_Bitmap& bm = face->glyph->bitmap;
                        if ((int)bm.width != (int)q.w || (int)bm.rows != (int)q.h) { ++mismatch; break; }
                        ++checked;
                        for (int y = 0; y < (int)q.h && !mismatch; ++y) {
                            const uint8_t* row = bm.buffer + (size_t)y * bm.pitch;
                            for (int x = 0; x < (int)q.w; ++x) {
                                const uint8_t a = at[(((size_t)(q.ay + y) * aw) + (uint32_t)(q.ax + x)) * 4 + 3];
                                if (a != row[x]) { ++mismatch; break; }
                            }
                        }
                    }
                }
                CHECK(ft_ok == 1);
                CHECK(checked >= 6);
                CHECK(mismatch == 0);          // 图集里该字形的像素必须与字体引擎输出完全一致
                if (face) FT_Done_Face(face);
                if (lib) FT_Done_FreeType(lib);
#endif

            }

            mq_text_style st2 = style(24.0F, MQ_TEXT_WRAP_CHAR, MQ_TEXT_ALIGN_BEGIN, 200.0F);
            mq_text_metrics m2{};
            CHECK(mq_text_measure(cjk, &st2, many.c_str(), &m2) == MQ_TEXT_OK);
            CHECK(mq_font_glyph_count(cjk, 24.0F) > 95);
            /* ⑩ 合成粗体/斜体（FreeType 路径）：粗体推进加宽、斜体位图不同；
             *    并且样式加载后必须复位 transform —— 之后栅格化的**新普通字形**要与
             *    一个干净字体实例逐位一致（否则斜体剪切会污染后续所有普通字形）。 */
            {
                const mq_text_style fsn = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                mq_text_style fsb = fsn; fsb.bold = 1;
                mq_text_style fsi = fsn; fsi.italic = 1;
                mq_text_metrics fmn{}, fmb{}, fmi{};
                CHECK(mq_text_measure(cjk, &fsn, "Bold", &fmn) == MQ_TEXT_OK);
                CHECK(mq_text_measure(cjk, &fsb, "Bold", &fmb) == MQ_TEXT_OK);
                CHECK(mq_text_measure(cjk, &fsi, "Bold", &fmi) == MQ_TEXT_OK);
                CHECK(fmb.width > fmn.width);                    // FT 合成粗体：推进加宽
                CHECK(fmi.width == fmn.width);                   // FT 合成斜体：推进不变
                std::vector<mq_text_glyph> fn(8), fb(8), fi(8);
                uint32_t cnn = 0, cnb = 0, cni = 0;
                CHECK(mq_text_layout(cjk, &fsn, "Bold", fn.data(), 8, &cnn, &fmn) == MQ_TEXT_OK);
                CHECK(mq_text_layout(cjk, &fsb, "Bold", fb.data(), 8, &cnb, &fmb) == MQ_TEXT_OK);
                CHECK(mq_text_layout(cjk, &fsi, "Bold", fi.data(), 8, &cni, &fmi) == MQ_TEXT_OK);
                CHECK(cnn == 4 && cnb == 4 && cni == 4);
                CHECK(fb[1].x - fb[0].x > fn[1].x - fn[0].x);    // 粗体 advance 更宽
                CHECK(fi[1].x - fi[0].x == fn[1].x - fn[0].x);   // 斜体 advance 不变
                /* 斜体位图/定位与普通不同（剪切变换生效） */
                const bool italic_diff = fi[0].w != fn[0].w || fi[0].h != fn[0].h ||
                                         fi[0].bearing_x != fn[0].bearing_x || fi[0].x0 != fn[0].x0;
                CHECK(italic_diff);
                /* transform 复位：样式字形之后再栅格化新码点 'W'（普通），结果必须与干净实例一致 */
                mq_font* clean = mq_font_load_file(cjk_path);
                CHECK(clean != nullptr);
                if (clean) {
                    std::vector<mq_text_glyph> fw(8), cw(8);
                    uint32_t nfw = 0, ncw = 0;
                    mq_text_metrics fmw{}, cmw{};
                    CHECK(mq_text_layout(cjk, &fsn, "W", fw.data(), 8, &nfw, &fmw) == MQ_TEXT_OK);
                    CHECK(mq_text_layout(clean, &fsn, "W", cw.data(), 8, &ncw, &cmw) == MQ_TEXT_OK);
                    CHECK(nfw == 1 && ncw == 1);
                    CHECK(fw[0].w == cw[0].w && fw[0].h == cw[0].h);
                    CHECK(fw[0].bearing_x == cw[0].bearing_x && fw[0].bearing_y == cw[0].bearing_y);
                    CHECK(fw[0].x0 == cw[0].x0 && fw[0].y0 == cw[0].y0);
                    CHECK(fmw.width == cmw.width);
                    mq_font_destroy(clean);
                }
            }

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

    // ---- 11) 布局空间不变性：device_scale 只是字形栅格化倍率，不参与折行/行高/metrics ----
    {
        mq_font* pf = load_project_font();
        if (!pf) {
            std::print("text_test: 项目字体不可用，跳过布局不变性/混排基线用例\n");
        } else {
            const char* kText = "the quick brown fox jumps over the lazy dog";
            const float kScales[] = { 1.0F, 1.25F, 2.0F, 2.5F };
            constexpr size_t kScaleN = sizeof(kScales) / sizeof(kScales[0]);

            /* scale=1 排版为参考；其余倍率的 metrics 与字形 x/y 必须与之**严格相等**（设计像素空间）。 */
            auto check_invariance = [&](mq_text_style ref_st, const char* label) {
                ref_st.device_scale = kScales[0];
                mq_text_metrics ref{};
                CHECK(mq_text_measure(pf, &ref_st, kText, &ref) == MQ_TEXT_OK);
                std::vector<mq_text_glyph> ref_g(ref.glyph_count ? ref.glyph_count : 1);
                uint32_t ref_n = 0;
                CHECK(mq_text_layout(pf, &ref_st, kText, ref_g.data(), (uint32_t)ref_g.size(), &ref_n,
                                     &ref) == MQ_TEXT_OK);
                CHECK(ref_n == ref.glyph_count);
                std::vector<mq_text_glyph> prev = ref_g;
                for (size_t k = 0; k < kScaleN; ++k) {
                    const float sc = kScales[k];
                    mq_text_style st = ref_st;
                    st.device_scale = sc;
                    mq_text_metrics m{};
                    CHECK(mq_text_measure(pf, &st, kText, &m) == MQ_TEXT_OK);
                    /* metrics 全是设计像素 → 与栅格化倍率完全无关（用 ==，不是近似） */
                    CHECK(m.width == ref.width);
                    CHECK(m.height == ref.height);
                    CHECK(m.line_count == ref.line_count);
                    CHECK(m.baseline == ref.baseline);
                    CHECK(m.ascent == ref.ascent);
                    CHECK(m.glyph_count == ref.glyph_count);
                    CHECK(m.device_scale == sc);               /* 仅报告栅格化倍率 */
                    std::vector<mq_text_glyph> g(m.glyph_count ? m.glyph_count : 1);
                    uint32_t n = 0;
                    CHECK(mq_text_layout(pf, &st, kText, g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
                    CHECK(n == ref_n);
                    int w_bad = 0, ratio_bad = 0, x0_bad = 0, xy_bad = 0, y0_bad = 0;
                    for (uint32_t i = 0; i < n && i < ref_n; ++i) {
                        /* 设计像素排版位置严格不变；只有设备位图（w/h/bearing）与 x0/y0 吸附随倍率变 */
                        if (!(g[i].x == ref_g[i].x && g[i].y == ref_g[i].y)) ++xy_bad;
                        /* x0 与参考倍率的差只应来自设备像素吸附：<= 1 设备像素 = 1/scale 设计像素 */
                        if (std::abs(g[i].x0 - ref_g[i].x0) > 1.0F / sc + 1e-4F) ++x0_bad;
                        /* y0 另受 bitmap_top 的 hinting 量化影响（每个 ppem 独立取整），放宽到 2 设计像素 */
                        if (std::abs(g[i].y0 - ref_g[i].y0) > 2.0F) ++y0_bad;
                        if (ref_g[i].w > 0 && ref_g[i].h > 0) {
                            /* 位图分辨率随倍率单调增大、约 1:scale（TrueType hinting ⇒ 非严格比例） */
                            if (g[i].w < prev[i].w || g[i].h < prev[i].h) ++w_bad;
                            const float wr = std::abs((float)g[i].w - (float)ref_g[i].w * sc);
                            const float hr = std::abs((float)g[i].h - (float)ref_g[i].h * sc);
                            /* 约 1:scale：相对 50% + 2px 绝对余量（小字形 hinting 量化占比大） */
                            if (wr > 0.5F * (float)ref_g[i].w * sc + 2.0F ||
                                hr > 0.5F * (float)ref_g[i].h * sc + 2.0F) ++ratio_bad;
                        }
                    }
                    CHECK(xy_bad == 0);                        /* 字形 x/y 与倍率无关 */
                    CHECK(x0_bad == 0);                        /* x0 只差吸附网格 */
                    CHECK(y0_bad == 0);                        /* y0 另含 bitmap_top 量化 */
                    CHECK(w_bad == 0);                         /* w/h 随倍率单调不减 */
                    CHECK(ratio_bad == 0);                     /* w/h ≈ 1:scale */
                    prev = g;
                }
                std::print("text_test[invariance] {}: lines={} glyphs={} w={:.3f} h={:.3f} baseline={:.3f} "
                           "ascent={:.3f}\n",
                           label, ref.line_count, ref.glyph_count, ref.width, ref.height, ref.baseline,
                           ref.ascent);
            };

            /* A) 多行：wrap=Word + max_width=120（折行结果必须与倍率无关） */
            mq_text_style sa = style(12.0F, MQ_TEXT_WRAP_WORD, MQ_TEXT_ALIGN_BEGIN, 120.0F);
            {
                mq_text_style t = sa;
                t.device_scale = 1.0F;
                mq_text_metrics ma{};
                CHECK(mq_text_measure(pf, &t, kText, &ma) == MQ_TEXT_OK);
                CHECK(ma.line_count >= 2);                     /* 确实发生折行，否则用例没意义 */
            }
            check_invariance(sa, "wrap=Word/max_width=120");

            /* B) 单行：max_width=0 + align Center */
            check_invariance(style(12.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_CENTER, 0.0F),
                             "wrap=None/Center/max_width=0");

            /* C) line_spacing=8：height 同样与倍率无关；且 = 多行无行距高 + 8×(行数-1) */
            {
                mq_text_style t = sa;
                t.device_scale = 1.0F;
                mq_text_metrics ma{};
                CHECK(mq_text_measure(pf, &t, kText, &ma) == MQ_TEXT_OK);
                mq_text_style sl = sa;
                sl.line_spacing = 8.0F;
                float h_ref = 0.0F;
                for (size_t k = 0; k < kScaleN; ++k) {
                    mq_text_style st = sl;
                    st.device_scale = kScales[k];
                    mq_text_metrics m{};
                    CHECK(mq_text_measure(pf, &st, kText, &m) == MQ_TEXT_OK);
                    if (k == 0) h_ref = m.height;
                    else CHECK(m.height == h_ref);
                }
                CHECK(std::abs(h_ref - (ma.height + 8.0F * (float)(ma.line_count - 1))) <= 1e-3F);
                std::print("text_test[invariance+spacing]: lines={} h(no spacing)={:.3f} h(spacing=8)={:.3f}\n",
                           ma.line_count, ma.height, h_ref);
            }
            mq_font_destroy(pf);
        }
    }

    // ---- 12) 行内混排字号共享基线（修复前：大字号按自身 ascent 定位 → 整体下移、基线错位）----
    {
        mq_font* pf = load_project_font();
        if (!pf) {
            std::print("text_test: 项目字体不可用，跳过混排基线用例\n");
        } else {
            const char* kMixed = "HH<size=32>H</size>HH";
            const float kScales[] = { 1.0F, 2.5F };
            for (float sc : kScales) {
                mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                st.device_scale = sc;
                mq_text_metrics m{};
                CHECK(mq_text_measure(pf, &st, kMixed, &m) == MQ_TEXT_OK);
                CHECK(m.glyph_count == 5);
                std::vector<mq_text_glyph> g(m.glyph_count);
                uint32_t n = 0;
                CHECK(mq_text_layout(pf, &st, kMixed, g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
                CHECK(n == 5);
                /* 基线（设计像素）= y0 + bearing_y/scale；行内所有字号必须共享同一基线 */
                auto baseline_of = [&](uint32_t i) {
                    return g[i].y0 + (float)g[i].bearing_y / sc;
                };
                const float b0 = baseline_of(0);
                float dmax = 0.0F;
                for (uint32_t i = 1; i < n; ++i)
                    dmax = std::max(dmax, std::abs(baseline_of(i) - b0));
                CHECK(dmax <= 1e-4F);                          /* 行内共享基线 */
                /* 32px 字（index 2）：字形顶更高（y0 更小）、位图 h 约 2 倍 */
                CHECK(g[2].y0 < g[0].y0);
                CHECK(g[2].h >= g[0].h * 1.6F);
                CHECK(g[2].h <= g[0].h * 2.4F);
                /* metrics.baseline = 首行 ascent（取行内最大 run ascent） */
                CHECK(m.baseline > 0.0F && m.baseline < m.height);
                mq_text_metrics mp{};
                CHECK(mq_text_measure(pf, &st, "HHHH", &mp) == MQ_TEXT_OK);
                CHECK(mp.baseline == m.ascent);                /* 纯基础字号：baseline == metrics.ascent */
                CHECK(m.baseline > mp.baseline);               /* 混排行 ascent 取 max（32px） */
                CHECK(m.baseline > mp.baseline + 4.0F);        /* 明显更大，而非差一点 */
                CHECK(std::abs(m.baseline - b0) <= 1e-4F);     /* 首行基线 == metrics.baseline */
                std::print("text_test[baseline] scale={:.2f}: baseline={:.3f} ascent16={:.3f} h(big)={} "
                           "h(small)={} max_inline_diff={:.6f}\n",
                           sc, m.baseline, mp.baseline, g[2].h, g[0].h, dmax);
            }
            mq_font_destroy(pf);
        }
    }

    // ---- 13) 显式换行 + 混排：每行独立共享基线；行高由该行最大字号决定（行高栈）----
    {
        mq_font* pf = load_project_font();
        if (!pf) {
            std::print("text_test: 项目字体不可用，跳过换行混排用例\n");
        } else {
            const char* kLines = "A<size=32>B</size>\nC<size=8>D</size>";
            const float kScales[] = { 1.0F, 2.5F };
            mq_text_style s16 = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
            mq_text_style s32 = s16;
            s32.font_size = 32.0F;
            mq_text_metrics m32{}, m16{};
            CHECK(mq_text_measure(pf, &s32, "H", &m32) == MQ_TEXT_OK);   /* 行高栈：32px 行 */
            CHECK(mq_text_measure(pf, &s16, "H", &m16) == MQ_TEXT_OK);
            for (float sc : kScales) {
                mq_text_style st = s16;
                st.device_scale = sc;
                mq_text_metrics m{};
                CHECK(mq_text_measure(pf, &st, kLines, &m) == MQ_TEXT_OK);
                CHECK(m.line_count == 2);
                CHECK(m.glyph_count == 4);
                std::vector<mq_text_glyph> g(4);
                uint32_t n = 0;
                CHECK(mq_text_layout(pf, &st, kLines, g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
                CHECK(n == 4);
                auto baseline_of = [&](uint32_t i) {
                    return g[i].y0 + (float)g[i].bearing_y / sc;
                };
                /* 行划分：y = 行顶（设计像素），同 y 即同一行 */
                CHECK(g[0].y == g[1].y);
                CHECK(g[2].y == g[3].y);
                const float line1_top = g[0].y, line2_top = g[2].y;
                const float b1 = baseline_of(0), b1b = baseline_of(1);
                const float b2 = baseline_of(2), b2b = baseline_of(3);
                CHECK(std::abs(b1 - b1b) <= 1e-4F);            /* 第一行共享基线 */
                CHECK(std::abs(b2 - b2b) <= 1e-4F);            /* 第二行共享基线 */
                /* 行高栈：第二行行顶 − 第一行行顶 = 第一行高 = 该行最大字号的行高 */
                const float stack = line2_top - line1_top;
                CHECK(std::abs(stack - m32.height) <= 1e-4F);
                /* 两行基线差 = 行高栈 + (第二行 ascent − 第一行 ascent)。
                 * 第一行 ascent = max(ascent16, ascent32) = m32.baseline；第二行 = max(ascent16, ascent8) = m16.baseline。
                 * 注：spec 的「基线差 == 第一行高」只在两行 max ascent 相同时成立；此处第一行含 32px、第二行最大 16px，
                 * 故必须带上 ascent 差（否则大字号行会整体下移 —— 正是本次修的 bug）。 */
                CHECK(std::abs((b2 - b1) - (stack + m16.baseline - m32.baseline)) <= 1e-4F);
                CHECK(b2 > b1);                                 /* y 向下：第二行基线更低 */
                CHECK((b2 - b1) < stack);                       /* 且小于行高（ascent 差为负贡献） */
                CHECK(std::abs(m.baseline - (b1 - line1_top)) <= 1e-4F);   /* metrics.baseline = 首行 ascent */
                std::print("text_test[newline] scale={:.2f}: line_tops=({:.3f},{:.3f}) stack={:.3f} "
                           "baselines=({:.3f},{:.3f}) diff={:.3f}\n",
                           sc, line1_top, line2_top, stack, b1, b2, b2 - b1);
            }
            mq_font_destroy(pf);
        }
    }

    // ---- 14) 非整数倍缩放的**呈现网格**：设备字距均匀 + 行基线一致（修「歪歪扭扭」）----
    // 设计 advance 在 1.25×/0.83× 等倍率下不是整数设备像素；若逐字把"设计位置×倍率"取整，
    // 会出现 8/9/8/9 的字距抖动与逐字基线抖动。修复后：pixel_snap 下横向用设备整数 advance
    // 累计、行基线只吸附一次。这里直接断言设备网格性质。
    {
        mq_font* pf = load_project_font();
        if (!pf) {
            std::print("text_test: 项目字体不可用，跳过呈现网格用例\n");
        } else {
            for (float sc : { 1.0F, 1.1F, 1.25F, 0.833F, 2.5F }) {
                mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                st.device_scale = sc;
                st.pixel_snap = 1;
                mq_text_metrics m{};
                CHECK(mq_text_measure(pf, &st, "HHHHHHHH", &m) == MQ_TEXT_OK);
                std::vector<mq_text_glyph> g(m.glyph_count ? m.glyph_count : 1);
                uint32_t n = 0;
                CHECK(mq_text_layout(pf, &st, "HHHHHHHH", g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
                CHECK(n == 8);
                /* x0 已落在设备网格：x0×scale 为整数；同字形相邻设备起点差 = 设备 advance（常数） */
                float gap0 = 0.0F;
                for (uint32_t i = 0; i < n; ++i) {
                    const float dx_dev = g[i].x0 * sc;
                    CHECK(std::abs(dx_dev - (float)std::lround(dx_dev)) <= 1e-3F);
                    if (i == 1) gap0 = dx_dev - g[0].x0 * sc;
                    if (i >= 1) {
                        const float gap = dx_dev - g[i - 1].x0 * sc;
                        CHECK(gap > 0.0F);
                        if (i >= 2) CHECK(std::abs(gap - gap0) <= 1e-3F);   /* 字距均匀（不抖动） */
                    }
                }
                /* 同字形基线（设备坐标）= y0×scale + bitmap_top，必须一致 */
                const float base_dev0 = g[0].y0 * sc + (float)g[0].bearing_y;
                for (uint32_t i = 1; i < n; ++i)
                    CHECK(std::abs((g[i].y0 * sc + (float)g[i].bearing_y) - base_dev0) <= 1e-3F);
                std::print("text_test[grid] scale={:.3f}: device_gap={:.1f} baseline_dev={:.1f}\n",
                           sc, gap0, base_dev0);
            }
            /* 行内混排字号：设备基线也一致（行基线只吸附一次，而不是逐字吸附） */
            for (float sc : { 1.25F, 2.5F }) {
                mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                st.device_scale = sc;
                st.pixel_snap = 1;
                const char* kMixed = "HH<size=32>H</size>HH";
                mq_text_metrics m{};
                CHECK(mq_text_measure(pf, &st, kMixed, &m) == MQ_TEXT_OK);
                std::vector<mq_text_glyph> g(m.glyph_count ? m.glyph_count : 1);
                uint32_t n = 0;
                CHECK(mq_text_layout(pf, &st, kMixed, g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
                CHECK(n == 5);
                const float b0 = g[0].y0 * sc + (float)g[0].bearing_y;
                bool same = true;
                for (uint32_t i = 1; i < n; ++i)
                    same = same && std::abs((g[i].y0 * sc + (float)g[i].bearing_y) - b0) <= 1e-3F;
                CHECK(same);
            }
            /* 多行行距：非整数倍 ss 下相邻行**设备基线间距**必须恒定
             * （呈现按设备整数行高累计；若每行各自 round(行顶×ss)，0.833× 会出现 17/16/17 的"波浪"）。 */
            {
                const char* kLines = "HH\nHH\nHH\nHH";
                for (float sc : { 1.25F, 0.833F, 2.5F }) {
                    mq_text_style st = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                    st.device_scale = sc;
                    st.pixel_snap = 1;
                    mq_text_metrics m{};
                    CHECK(mq_text_measure(pf, &st, kLines, &m) == MQ_TEXT_OK);
                    std::vector<mq_text_glyph> g(m.glyph_count ? m.glyph_count : 1);
                    uint32_t n = 0;
                    CHECK(mq_text_layout(pf, &st, kLines, g.data(), (uint32_t)g.size(), &n, &m) == MQ_TEXT_OK);
                    CHECK(m.line_count == 4);
                    /* 每行取一个代表字形（g.y = 设计行顶；同一行的字号一致） */
                    std::vector<float> line_y, line_base;
                    for (uint32_t i = 0; i < n; ++i) {
                        if (!line_y.empty() && g[i].y == line_y.back()) continue;
                        line_y.push_back(g[i].y);
                        line_base.push_back(g[i].y0 * sc + (float)g[i].bearing_y);
                    }
                    CHECK(line_y.size() == 4);
                    bool uniform = true;
                    float gap0 = 0.0F;
                    for (size_t i = 1; i < line_base.size(); ++i) {
                        const float gap = line_base[i] - line_base[i - 1];
                        if (i == 1) gap0 = gap;
                        else uniform = uniform && std::abs(gap - gap0) <= 1e-3F;
                    }
                    CHECK(uniform);
                    std::print("text_test[linegrid] scale={:.3f}: line_baselines=({:.1f},{:.1f},{:.1f},{:.1f}) "
                               "device_line_gap={:.1f}\n",
                               sc, line_base[0], line_base[1], line_base[2], line_base[3], gap0);
                }
            }
            /* HarfBuzz shaping：GPOS kerning 与 GSUB 连字必须生效，关掉时退回逐码点等价布局。 */
#if defined(MAQUE_HAVE_HARFBUZZ)
            {
                mq_text_style st = style(32.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_BEGIN, 0.0F);
                st.device_scale = 1.0F;
                st.pixel_snap = 1;
                set_shaping_env(true);
                mq_text_metrics fi{};
                CHECK(mq_text_measure(pf, &st, "fi", &fi) == MQ_TEXT_OK);
                mq_text_metrics av_on{};
                CHECK(mq_text_measure(pf, &st, "AV", &av_on) == MQ_TEXT_OK);
                std::vector<mq_text_glyph> sg(fi.glyph_count ? fi.glyph_count : 1);
                uint32_t sn = 0;
                CHECK(mq_text_layout(pf, &st, "fi", sg.data(), (uint32_t)sg.size(), &sn, &fi) == MQ_TEXT_OK);
                CHECK(sn == 1);                                  /* Alibaba 字体有 fi 连字 */
                if (sn == 1) {
                    CHECK(sg[0].w > 0.0F && sg[0].h > 0.0F);
                    CHECK(sg[0].codepoint == 'f');               /* cluster 首码点 */
                }
                /* kerning：AV 的宽度应小于逐码点之和（GPOS pair adjustment） */
                mq_text_metrics a{}, v{};
                CHECK(mq_text_measure(pf, &st, "A", &a) == MQ_TEXT_OK);
                CHECK(mq_text_measure(pf, &st, "V", &v) == MQ_TEXT_OK);
                CHECK(av_on.width + 0.05F < a.width + v.width);
                std::print("text_test[shape] fi_glyphs={} AV_on={:.2f} A+V={:.2f}\n",
                           sn, av_on.width, a.width + v.width);

                set_shaping_env(false);                         /* 关闭 shaping：逐码点回退 */
                mq_text_metrics fi_off{}, av_off{};
                CHECK(mq_text_measure(pf, &st, "fi", &fi_off) == MQ_TEXT_OK);
                CHECK(mq_text_measure(pf, &st, "AV", &av_off) == MQ_TEXT_OK);
                CHECK(fi_off.glyph_count == 2);
                CHECK(av_off.width == a.width + v.width);       /* 无 kerning：逐字 advance 之和 */
                std::print("text_test[shape] off: fi_glyphs={} AV_off={:.2f}\n",
                           fi_off.glyph_count, av_off.width);
                set_shaping_env(true);
            }
#endif
            /* UAX #14 换行 + UAX #29 字素边界：hyphen/ZWSP 可断、NBSP/括号前/组合字不可断。 */
#if defined(MAQUE_HAVE_LIBUNIBREAK)
            {
                mq_text_style ux = style(16.0F, MQ_TEXT_WRAP_WORD, MQ_TEXT_ALIGN_BEGIN, 10.0F);
                ux.device_scale = 1.0F;
                auto lines_of = [&](const char* t) {
                    mq_text_metrics mm{};
                    CHECK(mq_text_measure(pf, &ux, t, &mm) == MQ_TEXT_OK);
                    return mm.line_count;
                };
                set_linebreak_env(true);
                const uint32_t l_hyphen = lines_of("a-b");
                const uint32_t l_zwsp = lines_of("a\u200Bb");
                const uint32_t l_nbsp = lines_of("a\u00A0b");
                const uint32_t l_paren = lines_of("a(b");
                const uint32_t l_cjk = lines_of("日本語");
                const uint32_t l_comb = lines_of("e\u0301");
                CHECK(l_hyphen >= 2);                /* HY：连字符后可断 */
                CHECK(l_zwsp >= 2);                  /* ZWSP 是可断点 */
                CHECK(l_nbsp == 1);                  /* NBSP 禁止断行 */
                CHECK(l_paren == 1);                 /* 开括号前不拆 */
                CHECK(l_cjk >= 3);                   /* CJK 逐字可断 */
                CHECK(l_comb == 1);                  /* 组合字素不拆 */
                std::print("text_test[uax14] lines: hyphen={} zwsp={} nbsp={} paren={} cjk={} comb={}\n",
                           l_hyphen, l_zwsp, l_nbsp, l_paren, l_cjk, l_comb);
                set_linebreak_env(false);            /* 回退旧规则：只在空格/CJK 断 */
                const uint32_t o_hyphen = lines_of("a-b");
                const uint32_t o_zwsp = lines_of("a\u200Bb");
                CHECK(o_hyphen == 1);
                CHECK(o_zwsp == 1);
                std::print("text_test[uax14] off: hyphen={} zwsp={}\n", o_hyphen, o_zwsp);
                set_linebreak_env(true);
                /* CHAR wrap：可以逐 cell 断，但不能拆组合字素（e + combining acute 必须同线）。 */
                mq_text_style cs = style(16.0F, MQ_TEXT_WRAP_CHAR, MQ_TEXT_ALIGN_BEGIN, 1.0F);
                cs.device_scale = 1.0F;
                set_shaping_env(false);              /* 强制逐码点 cell，验证 grapheme 保护 */
                mq_text_metrics cm{};
                CHECK(mq_text_measure(pf, &cs, "e\u0301x", &cm) == MQ_TEXT_OK);
                CHECK(cm.line_count == 2);
                set_shaping_env(true);
                std::print("text_test[uax14] char-wrap combining lines={}\n", cm.line_count);
            }
#endif
            /* 布局：行尾空格不计入行宽/对齐；富文本标签切开的词不能跨 run 断开。 */
            {
                mq_text_style cst = style(16.0F, MQ_TEXT_WRAP_NONE, MQ_TEXT_ALIGN_CENTER, 100.0F);
                cst.device_scale = 1.0F;
                mq_text_metrics a{}, as{};
                CHECK(mq_text_measure(pf, &cst, "A", &a) == MQ_TEXT_OK);
                CHECK(mq_text_measure(pf, &cst, "A ", &as) == MQ_TEXT_OK);
                CHECK(as.width == a.width);                    /* 行尾空格不增加行宽 */
                std::vector<mq_text_glyph> ga(4), gas(4);
                uint32_t na = 0, nas = 0;
                CHECK(mq_text_layout(pf, &cst, "A", ga.data(), 4, &na, &a) == MQ_TEXT_OK);
                CHECK(mq_text_layout(pf, &cst, "A ", gas.data(), 4, &nas, &as) == MQ_TEXT_OK);
                CHECK(na == 1 && nas == 2);
                CHECK(std::abs(ga[0].x0 - gas[0].x0) <= 1e-3F); /* 尾空格不改变居中位置 */

                mq_text_style wst = style(16.0F, MQ_TEXT_WRAP_WORD, MQ_TEXT_ALIGN_BEGIN, 48.0F);
                wst.device_scale = 1.0F;
                std::vector<mq_text_glyph> gp(16), gr(16);
                uint32_t np = 0, nr = 0;
                mq_text_metrics mp{}, mr{};
                CHECK(mq_text_layout(pf, &wst, "x hello", gp.data(), 16, &np, &mp) == MQ_TEXT_OK);
                CHECK(mq_text_layout(pf, &wst, "x <b>hel</b>lo", gr.data(), 16, &nr, &mr) == MQ_TEXT_OK);
                CHECK(mp.line_count == 2 && mr.line_count == 2);
                auto second_line_count = [](const std::vector<mq_text_glyph>& g, uint32_t n) {
                    if (!n) return uint32_t(0);
                    float maxy = 0.0F;
                    for (uint32_t i = 0; i < n; ++i) maxy = std::max(maxy, g[i].y);
                    uint32_t c = 0;
                    for (uint32_t i = 0; i < n; ++i) if (g[i].y == maxy) ++c;
                    return c;
                };
                CHECK(second_line_count(gp, np) == 5);         /* 普通文本第二行 hello */
                CHECK(second_line_count(gr, nr) == 5);         /* 富文本也不在词中断行 */
                bool first_line2_h = false;
                for (uint32_t i = 0; i < nr; ++i)
                    if (gr[i].y > 0.0F) { first_line2_h = (gr[i].codepoint == 'h'); break; }
                CHECK(first_line2_h);

                mq_text_style tst = style(16.0F, MQ_TEXT_WRAP_WORD, MQ_TEXT_ALIGN_BEGIN, 62.0F);
                tst.device_scale = 1.0F;
                mq_text_metrics tm{};
                CHECK(mq_text_measure(pf, &tst, "aaa bbb", &tm) == MQ_TEXT_OK);
                CHECK(tm.width == 30.0F);                      /* 第一行 “aaa+尾空格” 行宽 = aaa */
                std::print("text_test[layout] center x_equal={} rich_line2={} width_trim={}\n",
                           std::abs(ga[0].x0 - gas[0].x0) <= 1e-3F,
                           second_line_count(gr, nr), tm.width);
            }
#if defined(MAQUE_HAVE_FREETYPE)
            /* baseline 归一：平底字母的墨迹底行不得比 baseline-1 再高（否则肉眼基线不齐）。 */
            {
                for (float sz : { 16.0F, 18.0F, 24.0F, 31.0F }) {
                    uint32_t aw = 0, ah = 0;
                    CHECK(mq_font_atlas_size(pf, sz, &aw, &ah) == MQ_TEXT_OK);
                    std::vector<uint8_t> rgba((size_t)aw * ah * 4);
                    CHECK(mq_font_atlas(pf, sz, rgba.data(), rgba.size(), &aw, &ah) == MQ_TEXT_OK);
                    const uint32_t gc = mq_font_glyph_count(pf, sz);
                    int bad = 0;
                    auto is_flat_letter = [](uint32_t cp) {
                        return cp == 'K' || cp == 'h' || cp == 'r' || cp == 'o' || cp == 'n' ||
                               cp == 'e' || cp == 'a' || cp == 'm' || cp == 's' || cp == 'd' ||
                               cp == 'H' || cp == 'M' || cp == 'A' || cp == 'x' || cp == 'u' ||
                               cp == 'c' || (cp >= '0' && cp <= '9');
                    };
                    for (uint32_t i = 0; i < gc; ++i) {
                        mq_text_glyph gl{};
                        if (mq_font_glyph_at(pf, sz, i, &gl) != MQ_TEXT_OK) continue;
                        if (!is_flat_letter(gl.codepoint)) continue;
                        const int gw = (int)gl.w, gh = (int)gl.h;
                        if (gw <= 0 || gh <= 0) continue;
                        int ink_bottom = -1;
                        for (int y = gh - 1; y >= 0 && ink_bottom < 0; --y)
                            for (int x = 0; x < gw; ++x)
                                if (rgba[(((size_t)(gl.ay + y) * aw) + (uint32_t)(gl.ax + x)) * 4 + 3]) {
                                    ink_bottom = y; break;
                                }
                        if (ink_bottom >= 0 && ink_bottom - gl.bearing_y < -1) ++bad;
                    }
                    CHECK(bad == 0);
                    std::print("text_test[baseline-norm] size={:.0f}: glyphs={} off_baseline={}\n", sz, gc, bad);
                }
            }
#endif
            mq_font_destroy(pf);
        }
    }

    if (g_failures == 0) { std::print("text_test: all checks passed\n"); return 0; }
    std::print("text_test: {} failure(s)\n", g_failures);
    return 1;
}
