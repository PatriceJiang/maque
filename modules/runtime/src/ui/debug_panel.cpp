/* 引擎侧 ImGui 调试面板（docs/28）：帧管理 / stats 面板 / 命令 → 网格图元
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S5，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

#include <cctype>
#include <filesystem>

namespace maque::rt {

/* ---- ImGui 调试面板（docs/27 §ImGui）------------------------------------------------
 *   ① **不进确定性**：面板是视图层 → 不写任何实体/组件、不进快照/校验和（docs/10）。
 *   ② **画在所有引擎图层之上**：网格 layer = MQ_IMGUI_LAYER（引擎 UI 是 1），
 *      在**引擎渲染完（含 ui pass）、present 之前**追加 → 截图里也有面板（可自证）。
 *   ③ **不点穿**：ImGui 想要鼠标/键盘时，清空本帧设备事件，世界输入即被阻断。
 *
 * 未启用时（默认）**一次都不碰** ImGui 上下文 —— tank/flappy 等程序零开销。
 */
/* 固定 UI 度量（**一次算定**）：字号/面板尺寸只由**屏幕逻辑分辨率 + DPI + 渲染倍率**
 * 决定，与游戏窗口大小、场景分辨率、视图数无关。
 *
 * 为什么不用窗口/渲染目标比（旧做法 `ps = win_w/TW`）：同一个面板在不同窗口里会
 * 一会儿过大一会儿过小（用户反馈"标题太大、有些窗口又偏小"）。SDK 侧 SDL 对
 * DPI-unaware 进程报的是**逻辑分辨率**（2560x1600 @150% → 1707x1067），
 * 所以 `屏幕高/1080` 这一条公式同时覆盖了"4K 屏要放大"和"150% 缩放要放大"。
 * 可用 `MAQUE_IMGUI_SCALE` 显式覆盖（1.0 = 默认）。
 * 字号：正文 15 逻辑像素（可用 `MAQUE_IMGUI_FONT_SIZE` 覆盖）、标题 = 正文 × 4/3
 * —— 标题只比正文大一档，不会像之前 26px:15px 那样压过内容。 */
void Engine::imgui_resolve_metrics() {
    imgui_screen_w = imgui_screen_h = 0;
    /* 只有**真实窗口**才取屏幕尺寸；headless（dummy 驱动）没有有意义的"屏幕"，
     * 保持 ui_scale=1.0（截图/CI 的基准）。 */
    if (window && platform) mq_platform_screen_size(platform, &imgui_screen_w, &imgui_screen_h);
    float ui = 1.0F;
    if (imgui_screen_h > 0) ui = (float)imgui_screen_h / 1080.0F;
    if (const char* e = std::getenv("MAQUE_IMGUI_SCALE"); e && *e) {
        const float v = (float)std::atof(e);
        if (v > 0.0F) ui = v;                        // 显式覆盖（相对 1080p 的倍率）
    }
    if (ui < 0.7F) ui = 0.7F;
    if (ui > 2.5F) ui = 2.5F;
    imgui_ui_scale = ui;
    float body = 15.0F;
    if (const char* e = std::getenv("MAQUE_IMGUI_FONT_SIZE"); e && *e) {
        const float v = (float)std::atof(e);
        if (v > 0.0F) body = v;                      // 正文字号（逻辑像素）
    }
    imgui_body_size = body;
    imgui_title_size = std::round(body * 4.0F / 3.0F);
}

void Engine::imgui_setup() {
    if (imgui_inited) return;
    /* 字体：复用文本模块已解析的路径（MAQUE_FONT → scenes/fonts/*.ttf → 系统）。
     * **必须给 TTF**：ImGui 默认字体只有 ASCII，中文会渲染成 '?'（实测）。 */
    text_setup();                                // 确保 font_path 已解析（幂等）
    imgui_resolve_metrics();                     // 屏幕分辨率 → 固定 UI 度量（一次）
    /* 窗口 绘制区/逻辑区 比：HiDPI 下 ImGui 显示空间 = 设备像素（见 imgui_frame_begin），
     * 字号/样式与输入坐标都按这个比例换算。 */
    imgui_content_scale = (window && platform) ? mq_platform_content_scale(platform) : 1.0F;
    if (imgui_content_scale <= 0.0F) imgui_content_scale = 1.0F;
    /* 呈现通道 = SDL_Renderer（游戏帧与面板共用同一个）：能拿到 renderer 就把面板交给
     * **官方后端**（它自己管坐标/裁剪/字体纹理/多窗口，拖动/缩放是官方行为），
     * 与引擎帧一起在**同一次 present** 上屏（见 step_frame 呈现段）。
     * 拿不到 renderer（headless / MAQUE_PRESENT=off / 无窗口）→ 自研网格链，
     * 面板直接写进引擎像素缓冲（--shot 可验证）。
     * ⚠️ 必须在 init 之前声明 backend 模式：否则会先走静态图集，再切官方后端会撞
     *    "Build() before RendererHasTextures" 断言（实测）。 */
    void* pre_ren = (window && platform && !direct_present) ? mq_platform_ensure_renderer(platform) : nullptr;
    /* 直接呈现（plans/18 M1）：窗口归 wgpu 交换链所有，不能再建 SDL_Renderer（SDL2 不允许混用）→
     * ImGui 走 CPU 像素 → 2D 网格的既有回退链，随 render_frame3d 的 meshes 提交。 */
    if (pre_ren) mq_imgui_set_backend_mode(1);
    /* 像素倍率：官方后端工作在 drawable 像素；网格链还需乘主输出 render_scale。 */
    const float base_ps = imgui_ui_scale * imgui_content_scale *
                          (pre_ren ? 1.0F : std::max(render_scale_req, 0.25F));
    /* 允许 25%：低到亚像素字号时 ImGui 的 WindowBorderHoverPadding 会变成 0
     * 触发断言；在 imgui 初始化里把该样式值钳到 ≥1（不改变布局倍率）。 */
    const float f = std::clamp(opt_imgui_factor, 1.0F, 5.0F);
    imgui_effective_factor = f;
    const float ps = base_ps * f;
    imgui_pixel_scale = ps > 0.0F ? ps : 1.0F;
    /* "(builtin 5x7)" 不是可读文件；传给 AddFontFromFileTTF 只会触发断言/错误日志。
     * Android 上 text_setup 可能没有 FreeType（或项目 fonts/ 在 asset:// 里不可直接
     * 用 std::filesystem 打开），所以这里再给 ImGui 一次独立查找系统字体的机会；
     * 都找不到就退回内置 ASCII 字体，面板仍可打开（中文显示为 ?，但不崩溃）。 */
    std::string imgui_font_path = font_path;
    /* text_setup() 依赖 FreeType；某些移动端构建没编 FreeType（font_path=builtin），
     * 但 ImGui 的 stb_truetype 可以直接吃项目 TTF。优先用 MAQUE_FONT 的 TTF。 */
    if (const char* env_font = std::getenv("MAQUE_FONT"); env_font && *env_font) {
        std::string p = env_font;
        const size_t dot = p.find_last_of('.');
        std::string ext = dot == std::string::npos ? std::string() : p.substr(dot);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        std::error_code ec;
        if (ext == ".ttf" && std::filesystem::is_regular_file(p, ec)) imgui_font_path = p;
    }
    if (imgui_font_path.empty() || imgui_font_path == "(builtin 5x7)") {
        static const char* kSystemFonts[] = {
#if defined(__ANDROID__)
            "/system/fonts/NotoSansCJK-Regular.ttc",
            "/system/fonts/NotoSansSC-Regular.otf",
            "/system/fonts/DroidSansFallback.ttf",
            "/system/fonts/Roboto-Regular.ttf",
#else
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
        };
        for (const char* c : kSystemFonts) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(c, ec)) { imgui_font_path = c; break; }
        }
    }
    /* stb_truetype 只吃 TrueType 轮廓；CFF/OTF 与 TTC 会在 InitFont 阶段直接断言。
     * 桌面 Windows 的 msyh.ttc、Android 的 NotoSansCJK.ttc 都属于这种情况 → 交给内置字体。 */
    auto lower_ext = [](const std::string& p) {
        const size_t dot = p.find_last_of('.');
        std::string e = dot == std::string::npos ? std::string() : p.substr(dot);
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        return e;
    };
    const std::string fext = lower_ext(imgui_font_path);
    if (fext == ".ttc" || fext == ".otf") imgui_font_path.clear();
    const char* imgui_font = imgui_font_path.empty() ? nullptr : imgui_font_path.c_str();
    mq_imgui_set_compact_glyphs(opt_imgui_factor > 4.0F ? 1 : 0);
    if (mq_imgui_init_with_fonts(imgui_font,
                                 imgui_title_size, imgui_body_size, imgui_pixel_scale) != 0) {
        std::print(stderr, "runtime: imgui init failed\n");
        imgui_on = false;
        return;
    }
    if (pre_ren) {
        if (mq_imgui_backend_init(pre_ren) == 0) {
            imgui_backend = true;
            std::print("runtime: imgui uses official SDL_Renderer backend (shared present)\n");
        } else {
            /* 官方后端初始化失败 → 回退网格链：把模式复位，让图集走静态 Build 路径。
             * （字体已按"窗口像素"倍率烘好；网格链下略小于目标空间比例，可接受——
             *  这条路径只在 renderer 存在但官方后端初始化失败时触发。） */
            mq_imgui_set_backend_mode(0);
            imgui_backend = false;
            std::print(stderr, "runtime: imgui official backend init failed → mesh-chain fallback\n");
        }
    }
    imgui_inited = true;
    imgui_tex = -1;
    imgui_tex_gen = 0;
    stat_window_sized = false;       // 新上下文：stats 窗口按固定度量重新定位/定尺寸
    /* 面板固定尺寸：正文 16 字宽 × 10 行高（含标题/内边距）——屏幕比例固定，不随窗口变。 */
    imgui_panel_w = std::round(imgui_body_size * 16.0F * imgui_pixel_scale);
    imgui_panel_h = std::round(imgui_body_size * 10.0F * imgui_pixel_scale);
    std::print("runtime: imgui ui scale={:.2f} screen={}x{} title={:.0f} body={:.0f} "
               "pixel={:.2f} panel={:.0f}x{:.0f}\n",
               imgui_ui_scale, imgui_screen_w, imgui_screen_h,
               imgui_title_size, imgui_body_size, imgui_pixel_scale, imgui_panel_w, imgui_panel_h);
}

/* 开关面板：关掉时释放上下文 + 字体纹理（不占 GPU/输入）；再开会重新初始化。 */
void Engine::imgui_enable(bool on, bool show_stats) {
    imgui_stats = on && show_stats;                 // F4 可复用 ImGui 上下文而不显示 F2 stats
    if (on == imgui_on && (on ? imgui_inited : !imgui_inited)) return;   // 幂等
    if (!on) {
        /* F2/脚本关掉整个 ImGui 时**同时收起 F4 面板状态**：
         * 否则"F4 开着 → F2 关 → F2 再开"会把引擎选项面板又带回来（F2、F4 互相干扰）。 */
        const bool had_opts = opt_panel;
        opt_panel = false;
        opt_opened_imgui = false;
        if (imgui_inited) {
            mq_imgui_shutdown();
            if (imgui_tex >= 0 && rapi && rself && rapi->destroy_texture)
                rapi->destroy_texture(rself, imgui_tex);
            imgui_tex = -1;
            imgui_tex_gen = 0;
            imgui_inited = false;
            imgui_frame_active = false;
            imgui_meshes.clear();
        }
        imgui_on = false;
        imgui_ptr_down = false;
        imgui_pending_up = false;
        std::print("runtime: imgui panel OFF\n");
        if (had_opts) std::print("runtime: engine options panel OFF\n");
        return;
    }
    imgui_on = true;
    imgui_setup();
    std::print("runtime: imgui panel ON\n");
}

/* ImGui 的显示空间尺寸（= `mq_imgui_begin_frame` 的 show_w/show_h，也是导出顶点所在空间）。
 * 必须与**实际画到哪里**一致，否则鼠标命中会整体偏移（docs/bugs/010）：
 *   · 官方 SDL_Renderer 后端：绘制区**设备像素**（renderer 后台缓冲；HiDPI 下大于逻辑尺寸）；
 *   · 网格链（WGPU 直呈 / 无窗口 / present off）：渲染目标像素 TW×TH
 *     （gfx 对 MQ_IMGUI_LAYER 用恒等视图，顶点逐像素落在目标上）。 */
void Engine::imgui_show_size(float* w, float* h) const {
    float sw = (float)TW, sh = (float)TH;
    if (imgui_backend) {
        int dw = 0, dh = 0;
        if (platform) mq_platform_drawable_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) { sw = (float)dw; sh = (float)dh; }
        else if (win_w > 0 && win_h > 0) { sw = (float)win_w; sh = (float)win_h; }
    }
    /* ImGui 显示空间 = target（TW×TH）：输入映射与窗口位置都只用 target 坐标；
     * ImGuiFactor 只改字体/面板像素倍率（面板变小），不再改 show/scale ——
     * 否则缩放后面板位置与触控命中会错位。 */
    if (w) *w = sw;
    if (h) *h = sh;
}

/* 窗口**逻辑**像素 → ImGui 显示空间 的倍率（等比）。
 * 无窗口（headless/截图）时指针本来就是目标像素 → 1。
 * ⚠️ 与 Canvas 的 scale/offset 无关：面板是屏幕空间覆盖层，不参与"设计坐标 → 目标"映射
 *    （旧实现按画布反算 → WGPU 直呈下点击/拖动整体偏移，见 docs/bugs/010）。 */
float Engine::imgui_safe_top() const {
#if defined(__ANDROID__)
    /* 24dp × 实际像素倍率：避免 F2/F4 默认位置压住通知栏/刘海。
     * 位置一旦被用户拖动，会写进 ImGui ini（MAQUE_IMGUI_INI）并在下次恢复。 */
    return 24.0F * imgui_pixel_scale;
#else
    return 0.0F;
#endif
}

float Engine::imgui_show_scale() const {
    if (!window || !platform) return 1.0F;
    int ww = 0, wh = 0;
    mq_platform_window_size(platform, &ww, &wh);
    float sw = 0.0F, sh = 0.0F;
    imgui_show_size(&sw, &sh);
    if (ww <= 0 || !(sw > 0.0F)) return 1.0F;
    return sw / (float)ww;
}

/* 把本帧设备事件转发给 ImGui（**窗口逻辑像素坐标**；调用点在转成世界坐标**之前**）。
 * ImGui 的显示空间 = 渲染目标像素（网格链）或绘制区设备像素（官方后端）。Android 的
 * window 逻辑尺寸与真实 surface 可能**非等比**（889x722 → 2670x1200），因此 x/y 各用
 * 自己的倍率。**不能**再按 Canvas 的 offset/scale 反算：面板是屏幕空间覆盖层，
 * 不跟随游戏视图的 zoom/平移（docs/bugs/010）。 */
void Engine::imgui_input_feed(const mq_platform_pointer_event* ptrs, uint32_t np,
                              const mq_platform_key_event* keys, uint32_t nk) {
    if (!imgui_on || !imgui_inited) return;
    /* Android 上窗口逻辑尺寸（实测 889x722）与真实 surface（2670x1200）**不是等比**：
     * 只用一个 width 倍率会让 y 命中错位（面板点不中）。x/y 各用自己的倍率。 */
    int ww = 0, wh = 0;
    float sw = 0.0F, sh = 0.0F;
    if (platform) mq_platform_window_size(platform, &ww, &wh);
    imgui_show_size(&sw, &sh);
    const float kx = (ww > 0 && sw > 0.0F) ? sw / (float)ww : 1.0F;
    const float ky = (wh > 0 && sh > 0.0F) ? sh / (float)wh : 1.0F;
    /* 触摸 down+up 可能落在同一游戏帧（Android 低帧率）：ImGui 的 NewFrame 只在
     * 帧边界处理事件，同帧 down/up 最终状态是 up → 永远点不中控件。
     * 做法：down 立即入队；若同帧也出现 up，把 up 延到下一帧，保证至少一帧 MouseDown。 */
    bool saw_down = false, saw_up = false, saw_move = false;
    float last_x = imgui_ptr_x, last_y = imgui_ptr_y;
    for (uint32_t i = 0; i < np; ++i) {
        const mq_platform_pointer_event& p = ptrs[i];
        imgui_ptr_x = last_x = p.x;
        imgui_ptr_y = last_y = p.y;
        if (p.type == MQ_PLATFORM_POINTER_DOWN && p.button == 0) { saw_down = true; imgui_ptr_down = true; }
        else if (p.type == MQ_PLATFORM_POINTER_UP && p.button == 0) { saw_up = true; imgui_ptr_down = false; }
        else if (p.type == MQ_PLATFORM_POINTER_MOVE) { saw_move = true; }
        else if (p.type == MQ_PLATFORM_POINTER_WHEEL)
            mq_imgui_input_pointer(p.x * kx, p.y * ky, imgui_ptr_down ? 1 : 0,
                                   p.scroll_y != 0.0F ? p.scroll_y : p.dy);
    }
    if (imgui_pending_up) {
        mq_imgui_input_pointer(last_x * kx, last_y * ky, 0, 0.0F);
        imgui_ptr_down = false;
        imgui_pending_up = false;
    }
    if (saw_down) mq_imgui_input_pointer(last_x * kx, last_y * ky, 1, 0.0F);
    if (saw_move) mq_imgui_input_pointer(last_x * kx, last_y * ky, imgui_ptr_down ? 1 : 0, 0.0F);
    if (saw_up && saw_down) {
        imgui_pending_up = true;                 // 同帧 down+up：下一帧再抬
        imgui_ptr_down = true;
    } else if (saw_up) {
        mq_imgui_input_pointer(last_x * kx, last_y * ky, 0, 0.0F);
    }
    if (np == 0 && !imgui_pending_up)
        mq_imgui_input_pointer(imgui_ptr_x * kx, imgui_ptr_y * ky, imgui_ptr_down ? 1 : 0, 0.0F);
    for (uint32_t i = 0; i < nk; ++i) {
        const mq_platform_key_event& k = keys[i];
        mq_imgui_input_key((int)k.key, k.down ? 1 : 0);
        if (k.down && k.text[0]) mq_imgui_input_chars(k.text);
    }
}

/* CI/无真鼠标：把 MAQUE_IMGUI_POINTER 声明的**合成指针事件**压进本帧设备队列。
 * 走与真实鼠标完全相同的路径（iframe → imgui_input_feed → ImGui），用于 e2e 验证拖动。
 * 格式："frame:x:y:type[;frame:x:y:type...]"，type 0=move 1=down 2=up。 */
void Engine::imgui_fake_input(mq_platform_pointer_event* ptrs, uint32_t& count, uint32_t cap) {
    if (!imgui_fake_parsed) {
        imgui_fake_parsed = true;
        const char* spec = std::getenv("MAQUE_IMGUI_POINTER");
        if (spec && *spec) {
            std::string s(spec), item;
            for (size_t i = 0; i <= s.size(); ++i) {
                const char c = i < s.size() ? s[i] : ';';
                if (c != ';') { if (c != ' ') item.push_back(c); continue; }
                int fr = 0, ty = 0; float x = 0, y = 0;
                if (item.size() > 0 && std::sscanf(item.c_str(), "%d:%f:%f:%d", &fr, &x, &y, &ty) == 4)
                    imgui_fake_ptr.push_back({ fr, x, y, ty });
                item.clear();
            }
            std::print("runtime: imgui fake pointer events={}\n", imgui_fake_ptr.size());
        }
    }
    for (const ImguiFakePointer& ev : imgui_fake_ptr) {
        if ((uint64_t)ev.frame != frame || count >= cap) continue;
        mq_platform_pointer_event& e = ptrs[count++];
        std::memset(&e, 0, sizeof e);
        e.pointer_id = 0;
        e.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
        e.button = 0;
        e.type = ev.type == 1 ? MQ_PLATFORM_POINTER_DOWN
               : ev.type == 2 ? MQ_PLATFORM_POINTER_UP
                              : MQ_PLATFORM_POINTER_MOVE;
        e.x = ev.x;
        e.y = ev.y;
        e.pressure = ev.type == 1 ? 1.0F : 0.0F;
    }
}

/* ImGuiFactor 改变：字体图集/面板尺寸都绑定 pixel_scale，必须重建上下文。
 * 保留 stats/F4 面板开关；ini 里的窗口位置会随 shutdown/init 自动保存/恢复。 */
void Engine::rebuild_imgui_for_scale() {
    if (!imgui_inited || !imgui_on) { imgui_rescale_pending = false; return; }
    const bool stats = imgui_stats;
    const bool opts = opt_panel;
    const bool opened = opt_opened_imgui;
    imgui_enable(false, false);
    imgui_stats = stats;
    opt_panel = opts;
    opt_opened_imgui = opened;
    imgui_enable(true, stats);
}

void Engine::imgui_frame_begin(float dt) {
    if (imgui_rescale_pending) {
        imgui_rescale_pending = false;
        if (imgui_inited) rebuild_imgui_for_scale();
    }
    /* 首次调用时决定初始开关：**默认关**（ImGui 是开发工具，不默认覆盖游戏画面）。
     * 打开：启动期 `MAQUE_IMGUI=1`（含 headless/截图），或运行时 F2 / 脚本
     * `imgui_set_enabled(1)`；`MAQUE_IMGUI=0` 只是显式初值（F2 仍可再开）。 */
    if (!imgui_env_checked) {
        imgui_env_checked = true;
        const char* e = std::getenv("MAQUE_IMGUI");
        if (e && *e && std::strcmp(e, "0") != 0) {
            imgui_on = true;
            imgui_stats = true;
            std::print("runtime: imgui on (MAQUE_IMGUI) (F2 toggles)\n");
        }
    }
    if (!imgui_on) return;
    if (!imgui_inited) imgui_setup();
    if (!imgui_inited) return;
    const int target_w = TW;
    const int target_h = TH;
    if (target_w <= 0 || target_h <= 0) return;

    /* 字体图集（**静态路径**）：必须在 `mq_imgui_begin_frame` **之前** Build + 上传。
     * 为什么顺序反了就不行：ImGui 的 legacy 后端路径要求"NewFrame 时图集已 Build"
     * （imgui_draw.cpp: `IM_ASSERT_USER_ERROR(atlas->TexIsBuilt, ...)`），
     * 而 `mq_imgui_font_pixels()` 正是做 Build 的那一步。所以：
     *   ① 首次：取像素（内部 Build）→ 上传纹理 → `SetTexID` 绑定；
     *   ② 之后：图集内容不再变（静态），直接跳过。 */
    if (!imgui_backend && (imgui_tex < 0 || mq_imgui_font_dirty())) {
        int fw = 0, fh = 0;
        const uint8_t* px = mq_imgui_font_pixels(&fw, &fh);
        if (px && fw > 0 && fh > 0 && rapi && rself && rapi->create_texture) {
            if (imgui_tex >= 0 && rapi->destroy_texture) rapi->destroy_texture(rself, imgui_tex);
            imgui_tex = rapi->create_texture(rself, px, fw, fh);
            if (imgui_tex >= 0) mq_imgui_set_font_texture((uint64_t)(imgui_tex + 1));
            std::print("runtime: imgui font atlas {}x{} → texture {}\n", fw, fh, imgui_tex);
            /* MAQUE_IMGUI_DUMP_ATLAS=<file.raw>：把实际传给 GPU 的图集落盘（RGBA32），
             * 用来区分"图集里没字形"和"采不到字形"。 */
            if (const char* d = std::getenv("MAQUE_IMGUI_DUMP_ATLAS"); d && *d) {
                std::ofstream f(d, std::ios::binary);
                f.write(reinterpret_cast<const char*>(px), (std::streamsize)fw * fh * 4);
                std::print("runtime: imgui atlas dumped '{}' {}x{}\n", d, fw, fh);
            }
        }
    }

    /* 显示空间（docs/bugs/010）：见 `imgui_show_size()` —— 官方后端 = 绘制区设备像素，
     * 网格链 = 渲染目标像素。字号/面板尺寸都是**固定 UI 度量**（imgui_resolve_metrics），
     * 与窗口/目标大小无关。 */
    float show_w = 0.0F, show_h = 0.0F;
    imgui_show_size(&show_w, &show_h);
    const float scale = 1.0F;
    const float off_x = 0.0F, off_y = 0.0F;
    const float dpi = 1.0F;
    if (imgui_backend) mq_imgui_backend_new_frame();   // 官方后端的 NewFrame（须在 ImGui::NewFrame 之前）
    mq_imgui_begin_frame(show_w, show_h, (float)target_w, (float)target_h,
                        scale, off_x, off_y, dt, dpi);
    imgui_frame_active = true;

    /* ---- stats 面板（打开时）：所有游戏通用，左上角，无需脚本参与 ----
     * · 尺寸/字号 = **屏幕分辨率派生的固定值**（`imgui_panel_w/h`）——同一块屏上
     *   无论游戏窗口多大、场景分辨率多少，面板看起来都一样大（用户反馈"标题太大/
     *   有些窗口又偏小"的修复点）；只有窗口小到放不下时才按比例收缩。
     * · 位置/尺寸只在**首帧**设一次（FirstUseEver）——之后用户拖动/缩放不被复位。 */
    if (mq_imgui_ready()) {
        if (imgui_stats) {
            if (!stat_window_sized) {
                const float pad = 8.0F * imgui_pixel_scale;
                const float min_w = 80.0F * imgui_pixel_scale;
                const float min_h = 60.0F * imgui_pixel_scale;
                const float w = std::min(imgui_panel_w, show_w * 0.85F);
                const float h = std::min(imgui_panel_h, show_h * 0.75F);
                mq_imgui_set_pos(pad, pad + imgui_safe_top());
                mq_imgui_set_size(w > min_w ? w : min_w, h > min_h ? h : min_h);
                stat_window_sized = true;
            }
            if (mq_imgui_begin_overlay("stats")) {
                mq_imgui_push_small_font();     // 内容用正文字号（图集烘了标题/正文两档）
                char line[160];
                std::snprintf(line, sizeof line, "FPS %.0f  (%.1f ms)",
                              stat_fps, stat_fps > 0 ? 1000.0F / stat_fps : 0.0F);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "logic %.1f ms  render %.1f ms", stat_logic_ms, stat_render_ms);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "UI draw %d  3D draw %d", stat_ui_calls, stat_3d_calls);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "tris %d  verts %d", stat_tris, stat_verts);
                mq_imgui_text(line);
                if (stat_vram_bytes < 1024u * 1024u)
                    std::snprintf(line, sizeof line, "VRAM ~%.1f KiB", stat_vram_bytes / 1024.0);
                else
                    std::snprintf(line, sizeof line, "VRAM ~%.2f MiB", stat_vram_bytes / (1024.0 * 1024.0));
                mq_imgui_text(line);
                /* 内存域（docs/29 §10）：明细一行 + 分区（冷暖）一行；关闭时显示 "off"。 */
                {
                    char memline[224];
                    if (maque::tracy::memory_available() && maque::tracy::memory_enabled()) {
                        maque::tracy::memory_format(memline, sizeof memline);
                        std::snprintf(line, sizeof line, "MEM %s", memline);
                        mq_imgui_text(line);
                        maque::tracy::memory_area_format(memline, sizeof memline);
                        std::snprintf(line, sizeof line, "MEM %s", memline);
                        mq_imgui_text(line);
                    } else {
                        mq_imgui_text("MEM off");
                    }
                }
                std::snprintf(line, sizeof line, "frame %llu  target %dx%d",
                              (unsigned long long)frame, target_w, target_h);
                mq_imgui_text(line);
                std::snprintf(line, sizeof line, "3D %dx%d  low=%d",
                              scene3d_w, scene3d_h, scene3d_low_active ? 1 : 0);
                mq_imgui_text(line);
                mq_imgui_pop_small_font();
            }
            mq_imgui_end();
        }
        /* F4 引擎选项面板可独立显示；只有 F2 / 脚本 / MAQUE_IMGUI 才打开 stats。 */
        if (opt_panel) engine_options_panel(show_w, show_h);
    }

    /* 注：③ "不点穿"（清空本帧设备事件）放在调用点 —— iframe 是 step_frame 的局部量。 */
}

}  // namespace maque::rt
