/* 文本渲染：排版 → 字形图元（docs/17/18；字号/对齐/换行/装饰/富文本）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S7，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

void Engine::text_setup() {
    if (font) return;
    const char* env = std::getenv("MAQUE_FONT");
    if (env && *env) {
        font = mq_font_load_file(env);
        if (font) font_path = env;
    }
    /* 项目字体：scenes/fonts/ 下按文件名排序取第一个 .ttf。
     * 搜索根顺序 = 场景目录 → CWD（与组件级 font 字段的解析规则一致）。 */
    if (!font) {
        std::vector<std::filesystem::path> roots;
        if (!scn_dir.empty()) roots.emplace_back(std::filesystem::path(scn_dir) / "fonts");
        roots.emplace_back(std::filesystem::path("scenes") / "fonts");
        roots.emplace_back(std::filesystem::path("fonts"));
        for (const auto& dir : roots) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            std::vector<std::filesystem::path> ttfs;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                const auto& p = it->path();
                if (p.extension() == ".ttf" || p.extension() == ".otf") ttfs.push_back(p);
            }
            std::sort(ttfs.begin(), ttfs.end());          // 稳定：同目录多字体时取文件名最小者
            for (const auto& t : ttfs) {
                font = mq_font_load_file(t.string().c_str());
                if (font) { font_path = t.string(); break; }
            }
            if (font) break;
        }
    }
    if (!font) {
        static const char* kCandidates[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/lato/Lato-Medium.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/tuffy/tuffy_bold.ttf",
            /* Windows 常见系统字体（TTC 也可被 FreeType 打开） */
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/msyh.ttc",
        };
        for (const char* c : kCandidates) {
            font = mq_font_load_file(c);
            if (font) { font_path = c; break; }
        }
    }
    if (!font) { font = mq_font_builtin(); font_path = "(builtin 5x7)"; }
    fonts.push_back({ font_path, font });
    std::print("runtime: text font '{}'\n", font_path);
}

/* 字体选择：标签的 font 字段（相对路径按 CWD 与场景目录解析）→ 缓存 */
mq_font* Engine::text_font_for(const std::string& path) {
    if (path.empty()) return font;
    for (const auto& fe : fonts) if (fe.path == path && fe.font) return fe.font;
    std::vector<std::string> tries = { path, scn_dir + "/" + path };
    for (const auto& t : tries) {
        if (mq_font* f = mq_font_load_file(t.c_str())) {
            fonts.push_back({ path, f });
            std::print("runtime: label font '{}'\n", t);
            return f;
        }
    }
    std::print(stderr, "runtime: label font '{}' not found (fallback: {})\n", path, font_path);
    fonts.push_back({ path, nullptr });
    return font;
}

/* 调试：导出图集为 PGM/PPM（MAQUE_TEXT_DUMP=<前缀>）——核查字形位图内容 */
static void dump_atlas(const char* prefix, float sz, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    const std::string path = std::string(prefix) + "_atlas_" + std::to_string((int)sz) + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::print(f, "P6\n{} {}\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint8_t a = rgba[i * 4 + 3];
        const uint8_t px[3] = { a, a, a };
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
    std::print("runtime: 图集已导出 {}\n", path);
}

/* 字形图集纹理：按 (字体, 字号) 缓存；图集增长（generation 变化）→ 重建纹理并释放旧的 */
int Engine::glyph_texture(const mq_font* f, float size) {
    const float sz = size > 0.0F ? size : 16.0F;
    if (!f || !rapi || !rself) return -1;
    const uint32_t gen = mq_font_atlas_generation(const_cast<mq_font*>(f), sz);
    for (auto& a : font_atlases) {
        if (a.font != f || a.size != sz) continue;
        if (a.generation == gen && a.texture >= 0) return a.texture;      /* 未变化 → 复用 */
        uint32_t w = 0, h = 0;
        if (mq_font_atlas(const_cast<mq_font*>(f), sz, nullptr, 0, &w, &h) != MQ_TEXT_ERR_CAP) return a.texture;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        if (mq_font_atlas(const_cast<mq_font*>(f), sz, rgba.data(), rgba.size(), &w, &h) != MQ_TEXT_OK) return a.texture;
        const int tex = rapi->create_texture(rself, rgba.data(), (int32_t)w, (int32_t)h);
        if (tex < 0) return a.texture;
        if (const char* dp = std::getenv("MAQUE_TEXT_DUMP"); dp && *dp) dump_atlas(dp, sz, rgba, w, h);
        if (a.texture >= 0) retired_textures.push_back(a.texture);      // 延迟释放（见成员注释）
        a.texture = tex; a.w = w; a.h = h; a.generation = gen;
        return tex;
    }
    uint32_t w = 0, h = 0;
    if (mq_font_atlas(const_cast<mq_font*>(f), sz, nullptr, 0, &w, &h) != MQ_TEXT_ERR_CAP) return -1;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    if (mq_font_atlas(const_cast<mq_font*>(f), sz, rgba.data(), rgba.size(), &w, &h) != MQ_TEXT_OK) return -1;
    if (const char* dp = std::getenv("MAQUE_TEXT_DUMP"); dp && *dp) dump_atlas(dp, sz, rgba, w, h);
    const int tex = rapi->create_texture(rself, rgba.data(), (int32_t)w, (int32_t)h);
    if (tex < 0) return -1;
    font_atlases.push_back({ f, sz, tex, w, h, gen });
    return tex;
}

/* 文本提取（docs/13 §8：只读）：排版后逐字形提交 quad。
 * 支持 scn.Label（扁平字段）与 ui.TextLabel / ui.RichText（字段）。 */
void Engine::extract_text(Engine& eng, int idx, SceneNode& e, const mq_type_desc* d,
                          std::vector<mq_renderer_item>& out, float inherit_alpha,
                          const ClipSet& clip) {
    std::string text, font_path_comp;
    float size = 16.0F, r = 1, g = 1, b = 1, a = 1, max_w = 0, max_h = 0, line_spacing = 0;
    int align = 0, wrap = 0, overflow = 0;
    int base_z = 100;                                  /* 文字默认在 UI 图元（z=0）之上 */
    bool bold = false, italic = false;
    float ox = 0.0F, oy = 0.0F;
    float outline_size = 0.0F, orr = 0.0F, ogg = 0.0F, obb = 0.0F, oaa = 1.0F;
    float shx = 0.0F, shy = 0.0F, shr = 0.0F, shg = 0.0F, shb = 0.0F, sha = 0.0F;
    const uint8_t* tb = component_slot(e, d).data();
    if (const auto* l = as_component<scn::Label>(d, tb)) {
        text = l->text; font_path_comp = l->font;
        size = l->font_size; max_w = l->max_width; max_h = l->max_height;
        line_spacing = l->line_spacing;
        r = l->r; g = l->g; b = l->b; a = l->a;
        align = (int)l->align; wrap = (int)l->wrap; overflow = (int)l->overflow;
        bold = l->bold; italic = l->italic;
        outline_size = l->outline_size;
        orr = l->outline_r; ogg = l->outline_g; obb = l->outline_b; oaa = l->outline_a;
        shx = l->shadow_x; shy = l->shadow_y;
        shr = l->shadow_r; shg = l->shadow_g; shb = l->shadow_b; sha = l->shadow_a;
        ox = l->x; oy = l->y;
        base_z = 100 + l->z;                            /* scn.Label.z 参与排序（此前被忽略） */
    } else if (const auto* tl = as_component<ui::TextLabel>(d, tb)) {
        text = tl->text; size = tl->font_size; line_spacing = tl->line_spacing;
        r = tl->color.x; g = tl->color.y; b = tl->color.z; a = tl->color.w;
        align = (int)tl->align_h; wrap = (int)tl->wrap_mode;
        outline_size = tl->outline_size;
        orr = tl->outline_color.x; ogg = tl->outline_color.y;
        obb = tl->outline_color.z; oaa = tl->outline_color.w;
    } else if (const auto* rt = as_component<render::RichText>(d, tb)) {
        text = rt->text; size = rt->font_size; max_w = rt->max_width;
        line_spacing = rt->line_spacing;
        r = rt->color.r; g = rt->color.g; b = rt->color.b; a = rt->color.a;
        align = (int)rt->align_h; wrap = (int)rt->wrap_mode;
        outline_size = rt->outline_size;
        orr = rt->outline_color.r; ogg = rt->outline_color.g;
        obb = rt->outline_color.b; oaa = rt->outline_color.a;
        if (!rt->parse_tags) {
            std::string esc;
            for (char c : text) { if (c == '<') esc += "&lt;"; else esc.push_back(c); }
            text = esc;
        }
    } else {
        return;                                          // role 文本组件必须是三个内置类型之一
    }
    if (text.empty()) return;

    mq_text_style st{};
    st.struct_size = (uint32_t)sizeof st;
    st.font_size = size;
    /* 设备缩放：窗口像素 / 目标像素（HiDPI 时按设备字号栅格化 → 1:1 呈现，不做二次缩放） */
    /* 字形栅格化倍率 = **渲染目标像素 / 设计像素**（=1：目标就是设计分辨率）。
     * 注意不是画布的窗口缩放：窗口放大由 present 做（整倍 → 最近邻，保持锐利）；
     * 若按窗口缩放栅格化（如 3x 出 36px 字形），再画进 320x240 的目标就是**降采样 3 倍** →
     * 笔画丢失、文字发虚/乱码（曾如此）。超采样目标（目标 > 设计）时才 > 1。 */
    st.device_scale = eng.ss;   /* 设计单位 → 当前渲染目标像素，仅影响字形栅格化 */
    st.pixel_snap = 1;                                  /* pixel-perfect：字形对齐整数像素 */
    st.line_spacing = line_spacing;
    st.wrap = (uint32_t)(wrap < 0 ? 0 : wrap);
    st.align = (uint32_t)(align < 0 ? 0 : align);
    st.max_width = max_w;
    st.bold = bold ? 1 : 0;
    st.italic = italic ? 1 : 0;
    st.r = r; st.g = g; st.b = b; st.a = a * inherit_alpha;

    /* 字体：标签的 font 字段优先（相对路径按 CWD/场景目录解析），否则默认字体 */
    mq_font* label_font = eng.text_font_for(font_path_comp);
    /* 排版度量为**设计像素**（text 模块内部与窗口/渲染倍率无关）；inv 只用于把字形位图
     * 的设备像素尺寸换算回设计像素（位图分辨率随窗口变化，布局不变）。 */
    const float inv = st.device_scale > 0.0F ? 1.0F / st.device_scale : 1.0F;
    const float req_size = size;

    /* 高度限制 + Shrink：度量已是设计像素，直接与 max_h 比较。
     * 先试请求字号；装不下则**二分**找「能装下的最大字号」——高度随字号总体单调增
     * （字号变小 → 每行更宽、折行更少 → 高度下降），比单步比例缩放更接近恰好装下
     * （比例缩放会因折行数突变而过缩，文字小到看不清）。16 轮二分 = 0.0004px 精度。 */
    const bool shrink_mode = max_h > 0.0F && overflow == (int)ui::TextOverflow::Shrink;
    mq_text_metrics m{};
    if (shrink_mode) {
        st.font_size = size;
        const bool ok0 = mq_text_measure(label_font, &st, text.c_str(), &m) == MQ_TEXT_OK;
        if (!ok0 || m.height > max_h + 0.01F) {
            float lo = 1.0F, hi = size;             /* lo = 假定装得下，hi = 装不下 */
            for (int it = 0; it < 16; ++it) {
                const float mid = (lo + hi) * 0.5F;
                st.font_size = mid;
                if (mq_text_measure(label_font, &st, text.c_str(), &m) != MQ_TEXT_OK) break;
                if (m.height <= max_h + 0.01F) lo = mid; else hi = mid;
            }
            size = std::floor(lo * 100.0F) / 100.0F;
            if (size < 1.0F) size = 1.0F;
        }
    }
    st.font_size = size;

    /* 先测量（写回 transient 字段，供脚本/控制面读取），再排版 */
    const int mrc = mq_text_measure(label_font, &st, text.c_str(), &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text] id={} type={} len={} size={:.1f}(req {:.1f}) maxw={:.0f} maxh={:.0f} overflow={} wrap={} rc={} lines={}\n",
                   idx, d->name, text.size(), st.font_size, req_size, max_w, max_h, overflow, st.wrap, mrc,
                   m.line_count);
    if (mrc != MQ_TEXT_OK) return;
    std::vector<mq_text_glyph> glyphs(m.glyph_count ? m.glyph_count : 1);
    uint32_t n = 0;
    const int lrc = mq_text_layout(label_font, &st, text.c_str(), glyphs.data(), (uint32_t)glyphs.size(), &n, &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text]   layout rc={} glyphs={} pos=({:.0f},{:.0f}) tex={} first=({:.1f},{:.1f},{:.1f}x{:.1f})\n",
                   lrc, n, e.world_x, e.world_y, n ? glyph_texture(label_font, glyphs[0].device_size) : -1,
                   n ? glyphs[0].x : 0.0F, n ? glyphs[0].y : 0.0F,
                   n ? glyphs[0].w : 0.0F, n ? glyphs[0].h : 0.0F);
    if (lrc != MQ_TEXT_OK) return;
    /* 度量写回：text 模块现在直接返回**设计像素**度量（与窗口/渲染倍率无关）。
     * 宽度/高度限制时夹到限制值（Clip 语义：盒子就是限制尺寸；Shrink 时通常已装下）。 */
    const float measured_w = m.width, measured_h = m.height;
    const float shown_w = max_w > 0.0F ? std::min(measured_w, max_w) : measured_w;
    const float shown_h = max_h > 0.0F ? std::min(measured_h, max_h) : measured_h;
    if (auto* l = as_component_mut<scn::Label>(d, component_slot(e, d).data())) {
        l->text_w = shown_w; l->text_h = shown_h;
        l->line_count = (int32_t)m.line_count;
        l->fit_font_size = shrink_mode ? st.font_size : 0.0F;
    } else if (auto* tl = as_component_mut<ui::TextLabel>(d, component_slot(e, d).data())) {
        tl->text_size = math::Vec2{ shown_w, shown_h };
    } else if (auto* rt = as_component_mut<render::RichText>(d, component_slot(e, d).data())) {
        rt->content_size = math::Vec2{ shown_w, shown_h };
        rt->line_count = (int32_t)m.line_count;
    }

    /* 锚点：scn.Label 的 x/y 为左上角偏移；ui.* 以节点位置为左上角 */

    /* 节点旋转（node_set_angle + 父链累积）：轴心 = **节点位置**（与 Sprite 一致，
     * x/y 偏移只做平移）。世界 y 向上、屏幕 y 向下 → 取负；旋转量同时作用于字形位置与
     * 字形自身朝向（整块文本刚体旋转）。angle=0 时 cos=1/sin=0 精确，走旧路径逐字节不变。 */
    float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
    accumulate_node_attrs(eng, idx, node_angle, node_scale, node_opacity);
    (void)node_scale; (void)node_opacity;
    const float rot = -node_angle * 3.14159265358979323846F / 180.0F;
    const float rot_cs = std::cos(rot), rot_sn = std::sin(rot);
    const bool rotated = rot_sn != 0.0F || rot_cs != 1.0F;
    const float pivot_x = e.world_x;
    const float pivot_y = (float)eng.H - e.world_y;
    /* 阶段 1：只收集（纹理与 UV 留到 text_flush，按**最终**图集计算）。
     * 每个字形按 阴影 → 描边 → 字形本体 的顺序入队（同 z，靠原始下标稳定排序）。 */
    const float base_a = st.a;                          /* 组件 alpha × 继承 alpha */
    auto push_quad = [&](const mq_text_glyph& gl, float dx, float dy,
                         float cr, float cg, float cb, float ca, int zi) {
        if (gl.w <= 0.0F || gl.h <= 0.0F) return;
        /* x0/y0 已是**设计像素**（text 模块布局空间）；只有字形位图 w/h 是设备像素
         * （按渲染倍率栅格化），换算回设计像素后落位 —— 布局与窗口大小无关。 */
        const float half_w = gl.w * 0.5F * inv, half_h = gl.h * 0.5F * inv;
        PendingQuad q{};
        q.font = label_font; q.size = gl.device_size;
        q.ax = gl.ax; q.ay = gl.ay;
        q.tw = (int)gl.w; q.th = (int)gl.h;
        q.cp = gl.codepoint;
        /* 锚点 = 位图左上角 (x0,y0)；排版 y 向下、屏幕 y 向下 → 基准翻转后相加 */
        const float lx = ox + gl.x0 + half_w + dx;
        const float ly = -oy + gl.y0 + half_h + dy;
        q.x = pivot_x + (rotated ? lx * rot_cs - ly * rot_sn : lx);
        q.y = pivot_y + (rotated ? lx * rot_sn + ly * rot_cs : ly);
        q.rotation = rot;
        q.snap = rotated ? 0 : 1;                        /* 旋转后不能再按轴对齐左上角吸附 */
        q.half_w = half_w; q.half_h = half_h;
        q.r = cr; q.g = cg; q.b = cb; q.a = ca;
        q.z = base_z + zi;
        /* 裁剪：reverse Mask/ListView 可能给出多个互不重叠的可见矩形 → 同一字形按矩形各推一份。
         * 先做可见性剔除：ListView 里滚出视口的行不再提交任何字形（scissor 只负责边缘硬切）。 */
        if (clip.active) {
            if (clip.rects.empty()) return;
            const float ex = q.half_w + q.half_h;              // 旋转保守包围盒
            bool visible = false;
            for (const ClipRect& r : clip.rects) {
                const float cx0 = r.x0, cy0 = (float)eng.H - r.y1;
                const float cx1 = r.x1, cy1 = (float)eng.H - r.y0;
                if (q.x - ex < cx1 && q.x + ex > cx0 && q.y - ex < cy1 && q.y + ex > cy0) {
                    visible = true; break;
                }
            }
            if (!visible) return;
            for (const ClipRect& r : clip.rects) {
                const float cx0 = r.x0, cy0 = (float)eng.H - r.y1;
                const float cx1 = r.x1, cy1 = (float)eng.H - r.y0;
                if (q.x + ex <= cx0 || q.x - ex >= cx1 || q.y + ex <= cy0 || q.y - ex >= cy1) continue;
                PendingQuad qc = q;
                qc.clip_x = r.x0;
                qc.clip_y = (float)eng.H - r.y1;
                qc.clip_w = r.x1 - r.x0;
                qc.clip_h = r.y1 - r.y0;
                eng.pending_quads.push_back(qc);
            }
        } else {
            eng.pending_quads.push_back(q);
        }
    };
    /* 描边：8 方向 × rings 环（ring 半径按 outline_size 均分；上限 8 环防止极端参数爆量）。
     * 用同一字形位图多次偏移绘制（描边色只取 alpha），确定性且与图集无关。 */
    const int outline_rings = outline_size > 0.0F
        ? std::clamp((int)std::ceil(outline_size), 1, 8) : 0;
    const bool has_shadow = sha > 0.0F && (shx != 0.0F || shy != 0.0F);
    for (uint32_t i = 0; i < n; ++i) {
        const mq_text_glyph& gl = glyphs[i];
        if (gl.w <= 0.0F || gl.h <= 0.0F) continue;
        /* 裁剪（设计像素）：
         *   · wrap=None + max_width：按宽度限制裁剪（文档语义：盒子就是限制尺寸）；
         *   · wrap=Word/Char：折行已由排版保证不越界；呈现用设备整数 advance 累计，
         *     可能比设计宽度多出 <1 设备像素 —— 不能逐字形裁掉，否则窗口非整数倍缩放时
         *     行尾字形会被误剪。高度超出 max_height 仍然整字形裁剪（Clip 语义）。 */
        const float gx = gl.x0, gy = gl.y0, gw = gl.w * inv, gh = gl.h * inv;
        if (max_w > 0.0F && wrap == 0 && gx + gw > max_w + 1.0F) continue;
        if (max_h > 0.0F && gy + gh > max_h + 0.5F) continue;
        if (has_shadow) push_quad(gl, shx, shy, shr, shg, shb, sha * base_a, (int)i);
        if (outline_rings > 0) {
            const float oa = std::clamp(oaa * base_a, 0.0F, 1.0F);
            for (int rr = 1; rr <= outline_rings; ++rr) {
                const float rad = outline_size * (float)rr / (float)outline_rings;
                for (int k = 0; k < 8; ++k) {
                    constexpr float kPi4 = 0.7853981633974483F;
                    const float ang = kPi4 * (float)k;
                    push_quad(gl, std::cos(ang) * rad, std::sin(ang) * rad, orr, ogg, obb, oa, (int)i);
                }
            }
        }
        push_quad(gl, 0.0F, 0.0F, gl.r, gl.g, gl.b, gl.a, (int)i);
    }
}

/* 阶段 2：刷新图集纹理（按最终尺寸）→ 计算 UV → 提交图元。
 * 必须在**所有**标签排版完成后调用（否则 UV 指向的图集尺寸会过期）。 */
void Engine::text_flush(std::vector<mq_renderer_item>& out) {
    if (pending_quads.empty()) return;
    for (const PendingQuad& q : pending_quads) {
        const int tex = glyph_texture(q.font, q.size);
        if (tex < 0) continue;
        /* 取该 (字体,字号) 的图集尺寸（= 纹理尺寸）→ 按最终尺寸换算 UV */
        uint32_t aw = 1, ah = 1;
        for (const auto& a : font_atlases)
            if (a.font == q.font && a.size == q.size) { aw = a.w ? a.w : 1; ah = a.h ? a.h : 1; break; }
        mq_renderer_item it{};
        it.x = q.x; it.y = q.y;
        it.half_w = q.half_w; it.half_h = q.half_h;
        it.size = q.half_w > q.half_h ? q.half_w : q.half_h;   /* 兼容字段 */
        it.filter = 1;                                          /* 1 纹素 = 1 像素 */
        it.rotation = q.rotation;
        it.snap = q.snap;                                       /* 未旋转 → 左上角吸附整数像素 */
        it.r = q.r; it.g = q.g; it.b = q.b; it.a = q.a;
        it.texture = tex;
        it.uv0 = (float)q.ax / (float)aw;
        it.uv1 = (float)q.ay / (float)ah;
        it.uv2 = (float)(q.ax + q.tw) / (float)aw;
        it.uv3 = (float)(q.ay + q.th) / (float)ah;
        it.layer = 1; it.z = q.z;
        it.clip_x = q.clip_x; it.clip_y = q.clip_y;
        it.clip_w = q.clip_w; it.clip_h = q.clip_h;
        out.push_back(it);
    }
    int traced = 0;
    if (std::getenv("MAQUE_TRACE_TEXT")) {
      for (const PendingQuad& tq : pending_quads) {
        if (tq.tw <= 0 || traced >= 80) continue;
        ++traced;
        uint32_t tw_ = 1, th_ = 1;
        for (const auto& fa : font_atlases)
            if (fa.font == tq.font && fa.size == tq.size) { tw_ = fa.w; th_ = fa.h; break; }
        /* 纹理实际尺寸 vs 图集当前尺寸：不一致 → UV 会指向纹理之外（表现为字形错乱） */
        uint32_t aw_now = 0, ah_now = 0;
        mq_font_atlas(const_cast<mq_font*>(tq.font), tq.size, nullptr, 0, &aw_now, &ah_now);
        std::print("[text]   quad U+{:04X} 纹理={}x{} 图集现={}x{} texel=({},{}) {}x{} uv=({:.4f},{:.4f},{:.4f},{:.4f})\n",
                   tq.cp, tw_, th_, aw_now, ah_now, tq.ax, tq.ay, tq.tw, tq.th,
                   (double)tq.ax / tw_, (double)tq.ay / th_,
                   (double)(tq.ax + tq.tw) / tw_, (double)(tq.ay + tq.th) / th_);
      }
    }
    if (std::getenv("MAQUE_TRACE_TEXT")) {
        const PendingQuad& q0 = pending_quads.front();
        uint32_t aw0 = 1, ah0 = 1;
        for (const auto& a : font_atlases)
            if (a.font == q0.font && a.size == q0.size) { aw0 = a.w; ah0 = a.h; break; }
        std::print("[text]   flush {} quad(s) 图集={}x{} 首个 texel=({},{}) {}x{} → uv=({:.4f},{:.4f},{:.4f},{:.4f})\n",
                   pending_quads.size(), aw0, ah0, q0.ax, q0.ay, q0.tw, q0.th,
                   (double)q0.ax / aw0, (double)q0.ay / ah0,
                   (double)(q0.ax + q0.tw) / aw0, (double)(q0.ay + q0.th) / ah0);
    }
    pending_quads.clear();
}


/* 读模拟器场景：先 VFS（资产路径，'/' 前缀兼容），失败再按宿主文件路径读（开发/CI 便利）。
 * 场景文件是**开发工具输入**而非游戏资产，因此允许宿主路径（docs/19）。 */
bool read_sim_scenario(mq_vfs* vfs, const char* path, std::string& out) {
    if (!path || !*path) return false;
    void* data = nullptr; size_t n = 0;
    std::string vp = path;
    if (vp[0] != '/') vp = "/" + vp;
    if (vfs && mq_vfs_read(vfs, vp.c_str(), &data, &n) == MQ_VFS_OK) {
        out.assign((const char*)data, n);
        mq_vfs_free(data);
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !out.empty();
}

}  // namespace maque::rt
