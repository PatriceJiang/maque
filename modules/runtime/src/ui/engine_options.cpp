/* F4 引擎选项面板（docs/28）：MSAA / 图层 / 太阳光 / 相机 near·far / 后处理
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S5，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- F4 引擎选项（docs/28）----------------------------------------------------
 * 「引擎选项」面板：渲染模式 / 游戏 3D、2D/UI 图层 / MSAA / 默认太阳光 / 主相机 near·far /
 * 后处理 PostEffect。
 *   · 纯视图层：不写场景组件、不进快照/校验和、不参与模拟（docs/10 确定性不受影响）；
 *   · 值在 extract_3d / 成帧时读取（Engine 的 opt_* 字段），切场景后仍保留；
 *   · 启动初值可用 `MAQUE_ENGINE_OPTIONS`（CI/截图/演示不必依赖鼠标拖控件）。
 * 环境变量格式（逗号分隔；向量分量用 ':'）：
 *   MAQUE_ENGINE_OPTIONS="mode=textured,3d=1,2d=0,msaa=4,rt_msaa=4,sun=0,near=0.01,far=500"
 * 键：mode(rendered|textured|solid|wireframe) / 3d(0|1) / 2d(0|1) / msaa(0|1|4) /
 * rt_msaa(0|1|4 = 相机离屏纹理的全局 MSAA，只作用于 3D 相机目标) /
 * sun(0|1) / sun_intensity / sun_dir(x:y:z) / near / far /
 * post(0|1) / tonemap(none|aces) / exposure / bloom(0|1) / bloom_intensity / bloom_threshold /
 * bloom_radius / vignette(0|1) / vignette_intensity / chromatic(0|1) / chromatic_strength /
 * grade(0|1) / saturation / contrast / gray / post_2d(0|1)
 * near 或 far 任一出场即启用主相机 near/far 覆盖；出现任何后处理键会自动打开总开关
 * （显式写 post=0 则在最后被尊重）。后处理**默认只叠加 3D 层**；post_2d=1 才含 2D/UI。 */

/* 后处理效果位（与 mq_renderer_post.effects / post/shader.wgsl 一致）。 */
enum : uint32_t {
    MQ_POST_BLOOM     = 1u << 0,
    MQ_POST_VIGNETTE  = 1u << 1,
    MQ_POST_CHROMATIC = 1u << 2,
    MQ_POST_GRADE     = 1u << 3,
};

/* opt_post_* → frame3d.post。关闭时只填 enabled=0（其余留 0）→ gfx 不走后处理通路。 */
void Engine::engine_options_fill_post(mq_renderer_post& out) const {
    std::memset(&out, 0, sizeof out);
    out.exposure = 1.0F;
    if (!opt_post_enabled) return;
    out.enabled = 1;
    out.tonemap = opt_post_tonemap;
    out.exposure = opt_post_exposure;
    out.effects = (opt_post_bloom ? MQ_POST_BLOOM : 0u) |
                  (opt_post_vignette ? MQ_POST_VIGNETTE : 0u) |
                  (opt_post_chromatic ? MQ_POST_CHROMATIC : 0u) |
                  (opt_post_grade ? MQ_POST_GRADE : 0u);
    out.bloom_intensity = opt_post_bloom_intensity;
    out.bloom_threshold = opt_post_bloom_threshold;
    out.bloom_radius = opt_post_bloom_radius;
    out.vignette = opt_post_vignette_intensity;
    out.aberration = opt_post_chromatic_strength;
    out.saturation = opt_post_saturation;
    out.contrast = opt_post_contrast;
    out.grayscale = opt_post_grayscale;
    out.include_2d = opt_post_include_2d ? 1 : 0;
}

/* 后处理状态变化时打印一行（面板调参/环境初值都可见，CI 可断言）。 */
void Engine::engine_options_log_post() {
    std::string fx;
    auto add = [&fx](bool on, const char* name) {
        if (!on) return;
        if (!fx.empty()) fx.push_back(',');
        fx += name;
    };
    add(opt_post_bloom, "bloom");
    add(opt_post_vignette, "vignette");
    add(opt_post_chromatic, "chromatic");
    add(opt_post_grade, "grade");
    std::print("runtime: engine option post={} effects={} exposure={:.2f} tonemap={} scope={}\n",
               opt_post_enabled ? "on" : "off", fx.empty() ? "-" : fx.c_str(),
               (double)opt_post_exposure,
               opt_post_tonemap ? "aces" : "none",
               opt_post_include_2d ? "all" : "3d");
}

void Engine::engine_options_parse_env() {
    if (opt_env_parsed) return;
    opt_env_parsed = true;
    const char* e = std::getenv("MAQUE_ENGINE_OPTIONS");
    if (!e || !*e) return;
    /* 出现任何后处理键 → 自动打开总开关（只要没显式写 post=0）。 */
    bool post_touched = false;
    bool post_explicit = false;
    std::string s(e), item;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = i < s.size() ? s[i] : ',';
        if (c != ',') { if (c != ' ' && c != '\t') item.push_back(c); continue; }
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        const std::string key = eq == std::string::npos ? item : item.substr(0, eq);
        const std::string val = eq == std::string::npos ? std::string() : item.substr(eq + 1);
        const bool on = !(val == "0" || val == "off" || val == "false");
        if (key == "mode") {
            opt_render_mode = val == "wireframe" ? 2 : (val == "solid" ? 1 : (val == "textured" ? 3 : 0));
        } else if (key == "3d") {
            opt_3d_enabled = on;
        } else if (key == "2d") {
            opt_2d_enabled = on;
        } else if (key == "msaa") {
            const int m = std::atoi(val.c_str());
            opt_msaa = m >= 4 ? 4 : (m == 1 ? 1 : 0);
        } else if (key == "rt_msaa") {
            /* 相机离屏纹理的全局 MSAA 覆盖（只作用于 3D 相机目标，不动主输出）。 */
            const int m = std::atoi(val.c_str());
            opt_rt_msaa = m >= 4 ? 4 : (m == 1 ? 1 : 0);
        } else if (key == "sun") {
            opt_sun_enabled = on;
        } else if (key == "sun_intensity") {
            opt_sun_intensity = std::clamp((float)std::atof(val.c_str()), 0.0F, 20.0F);
        } else if (key == "sun_dir") {
            float x = 0.0F, y = 0.0F, z = 0.0F;
            if (std::sscanf(val.c_str(), "%f:%f:%f", &x, &y, &z) == 3) {
                opt_sun_dir[0] = x; opt_sun_dir[1] = y; opt_sun_dir[2] = z;
            }
        } else if (key == "near") {
            opt_cam_near = std::max((float)std::atof(val.c_str()), 1e-4F);
            opt_cam_override = true;
        } else if (key == "far") {
            opt_cam_far = std::max((float)std::atof(val.c_str()), 1e-3F);
            opt_cam_override = true;
        } else if (key == "post") {
            opt_post_enabled = on;
            post_explicit = true;
        } else if (key == "tonemap") {
            opt_post_tonemap = (val == "aces") ? 1 : 0;
            post_touched = true;
        } else if (key == "exposure") {
            opt_post_exposure = std::clamp((float)std::atof(val.c_str()), 0.05F, 8.0F);
            post_touched = true;
        } else if (key == "bloom") {
            opt_post_bloom = on;
            post_touched = true;
        } else if (key == "bloom_intensity") {
            opt_post_bloom_intensity = std::clamp((float)std::atof(val.c_str()), 0.0F, 4.0F);
            post_touched = true;
        } else if (key == "bloom_threshold") {
            opt_post_bloom_threshold = std::clamp((float)std::atof(val.c_str()), 0.0F, 1.0F);
            post_touched = true;
        } else if (key == "bloom_radius") {
            opt_post_bloom_radius = std::clamp((float)std::atof(val.c_str()), 0.001F, 0.15F);
            post_touched = true;
        } else if (key == "vignette") {
            opt_post_vignette = on;
            post_touched = true;
        } else if (key == "vignette_intensity") {
            opt_post_vignette_intensity = std::clamp((float)std::atof(val.c_str()), 0.0F, 1.0F);
            post_touched = true;
        } else if (key == "chromatic") {
            opt_post_chromatic = on;
            post_touched = true;
        } else if (key == "chromatic_strength") {
            opt_post_chromatic_strength = std::clamp((float)std::atof(val.c_str()), 0.0F, 0.05F);
            post_touched = true;
        } else if (key == "grade") {
            opt_post_grade = on;
            post_touched = true;
        } else if (key == "saturation") {
            opt_post_saturation = std::clamp((float)std::atof(val.c_str()), 0.0F, 3.0F);
            post_touched = true;
        } else if (key == "contrast") {
            opt_post_contrast = std::clamp((float)std::atof(val.c_str()), 0.0F, 3.0F);
            post_touched = true;
        } else if (key == "gray") {
            opt_post_grayscale = std::clamp((float)std::atof(val.c_str()), 0.0F, 1.0F);
            post_touched = true;
        } else if (key == "post_2d") {
            /* 显式把 2D/UI 也纳入后处理（默认只叠加 3D 层）。 */
            opt_post_include_2d = on;
            post_touched = true;
        }
        item.clear();
    }
    if (post_touched && !post_explicit) opt_post_enabled = true;
    static const char* const kModeNames[4] = { "rendered", "solid", "wireframe", "textured" };
    std::print("runtime: engine options (env) mode={} 3d={} 2d={} msaa={} rt_msaa={} sun={} intensity={:.2f} cam={}",
               kModeNames[opt_render_mode], opt_3d_enabled ? "on" : "off", opt_2d_enabled ? "on" : "off",
               opt_msaa == 0 ? "scene" : (opt_msaa == 4 ? "4x" : "off"),
               opt_rt_msaa == 0 ? "scene" : (opt_rt_msaa == 4 ? "4x" : "off"),
               opt_sun_enabled ? "on" : "off", (double)opt_sun_intensity,
               opt_cam_override ? "override" : "scene");
    if (opt_cam_override)
        std::print(" near={:.3f} far={:.3f}", (double)opt_cam_near, (double)opt_cam_far);
    std::print("\n");
    engine_options_log_post();
}

/* F4 面板内容。调用方保证本帧 ImGui 已 begin（`imgui_frame_begin`）。
 * 布局：默认贴在 stats 面板右侧；窗口太窄时夹紧在窗口内。
 * 位置/尺寸用 FirstUseEver 语义（`mq_imgui_begin_at`）→ 用户拖动/缩放后不会被每帧复位。 */
void Engine::engine_options_panel(float show_w, float show_h) {
    const float pad = 8.0F * imgui_pixel_scale;
    const float gap = 8.0F * imgui_pixel_scale;
    const float w = std::min(imgui_panel_w * 1.30F, std::max(show_w * 0.90F, 120.0F));
    /* 高度：常用项 + 后处理折叠头一行；展开后靠 ImGui 滚动条（窗口太矮时夹到 85%）。 */
    const float h = std::min(imgui_panel_h * 4.05F, std::max(show_h * 0.85F, 100.0F));
    float x = imgui_panel_w > 0.0F ? pad + imgui_panel_w + gap : pad;
    if (x + w > show_w - pad) x = std::max(pad, show_w - w - pad);
    if (!mq_imgui_begin_at("engine options", x, pad, w, h)) { mq_imgui_end(); return; }
    mq_imgui_push_small_font();
    mq_imgui_push_item_width(std::min(w * 0.50F, 150.0F * imgui_pixel_scale));

    mq_imgui_text("选项：只影响本机画面，不改场景");
    mq_imgui_text_disabled("F4 开关本面板 / F2 开关 stats");
    mq_imgui_separator();

    /* ① 调试渲染与游戏图层：只改提交路径，不隐藏 F4 面板自身。 */
    const int mode_sel = opt_render_mode == 2 ? 0
                       : opt_render_mode == 1 ? 1
                       : opt_render_mode == 3 ? 2 : 3;
    const int new_mode_sel = (int)mq_imgui_combo("渲染模式", (float)mode_sel,
                                                  "wireframe,solid,textured,rendered");
    const int new_mode = new_mode_sel == 0 ? 2
                       : new_mode_sel == 1 ? 1
                       : new_mode_sel == 2 ? 3 : 0;
    if (new_mode != opt_render_mode) {
        opt_render_mode = new_mode;
        static const char* const kModeNames[4] = { "rendered", "solid", "wireframe", "textured" };
        std::print("runtime: engine option mode={}\n", kModeNames[opt_render_mode]);
    }
    const bool old_3d = opt_3d_enabled, old_2d = opt_2d_enabled;
    opt_3d_enabled = mq_imgui_checkbox("3D", opt_3d_enabled ? 1.0F : 0.0F) != 0.0F;
    opt_2d_enabled = mq_imgui_checkbox("2D / UI", opt_2d_enabled ? 1.0F : 0.0F) != 0.0F;
    if (old_3d != opt_3d_enabled)
        std::print("runtime: engine option 3d={}\n", opt_3d_enabled ? "on" : "off");
    if (old_2d != opt_2d_enabled)
        std::print("runtime: engine option 2d={}\n", opt_2d_enabled ? "on" : "off");
    mq_imgui_separator();

    /* ② MSAA（**主输出**）：0=跟随场景（与 MAQUE_MSAA），1=关，4=4x */
    const int msaa_sel = opt_msaa == 4 ? 2 : (opt_msaa == 1 ? 1 : 0);
    const int new_sel = (int)mq_imgui_combo("MSAA", (float)msaa_sel, "跟随场景,关闭,4x");
    const int new_msaa = new_sel == 2 ? 4 : (new_sel == 1 ? 1 : 0);
    if (new_msaa != opt_msaa) {
        opt_msaa = new_msaa;
        std::print("runtime: engine option msaa={}\n",
                   opt_msaa == 4 ? "4x" : (opt_msaa == 1 ? "off" : "scene"));
    }
    if (opt_last_msaa >= 4) mq_imgui_text("实际：4x MSAA");
    else mq_imgui_text("实际：关闭（1x）");
    /* ②b RenderTexture MSAA：只管**相机离屏目标**（render.RenderTexture），不动上面那个主输出开关。
     * 「跟随」= 交给 MAQUE_MSAA / 各目标的 msaa_samples 字段 / 场景字段决定。
     * 更专门的开关赢：这里选了 关/4x，就覆盖 ②的选择与场景字段（面板上直接说清楚，避免两处对着猜）。 */
    const int rt_msaa_sel = opt_rt_msaa == 4 ? 2 : (opt_rt_msaa == 1 ? 1 : 0);
    const int new_rt_sel = (int)mq_imgui_combo("RenderTexture MSAA", (float)rt_msaa_sel, "跟随,关闭,4x");
    const int new_rt_msaa = new_rt_sel == 2 ? 4 : (new_rt_sel == 1 ? 1 : 0);
    if (new_rt_msaa != opt_rt_msaa) {
        opt_rt_msaa = new_rt_msaa;
        std::print("runtime: engine option rt_msaa={}\n",
                   opt_rt_msaa == 4 ? "4x" : (opt_rt_msaa == 1 ? "off" : "scene"));
    }
    {
        char line[160];
        if (opt_last_rt_total <= 0)
            std::snprintf(line, sizeof line, "实际：无 3D 相机离屏目标（2D 目标不吃 MSAA）");
        else
            std::snprintf(line, sizeof line, "实际：%d/%d 张相机纹理 4x",
                          opt_last_rt_msaa4, opt_last_rt_total);
        mq_imgui_text(line);
    }
    mq_imgui_text_disabled("多采样只作用于 3D pass；离屏纹理本体仍单采样（pass 末 resolve）");
    mq_imgui_separator();

    /* ③ 默认太阳光：**只在场景没有任何光源时**补光（老场景的兜底行为可开关/调参） */
    mq_imgui_text("默认太阳光（场景无光源时补光）");
    opt_sun_enabled = mq_imgui_checkbox("启用", opt_sun_enabled ? 1.0F : 0.0F) != 0.0F;
    opt_sun_intensity = mq_imgui_slider_float("强度", opt_sun_intensity, 0.0F, 4.0F);
    for (int i = 0; i < 3; ++i) {
        /* 光的**传播方向**（shader 里 normalize(-dir) 得到入射方向）；分量为 0 会退化 → 夹一下 */
        static const char* kAxis[3] = { "方向 X", "方向 Y", "方向 Z" };
        opt_sun_dir[i] = std::clamp(mq_imgui_drag_float(kAxis[i], opt_sun_dir[i], 0.01F), -4.0F, 4.0F);
    }
    {
        const float len = std::fabs(opt_sun_dir[0]) + std::fabs(opt_sun_dir[1]) + std::fabs(opt_sun_dir[2]);
        if (len < 1e-4F) opt_sun_dir[2] = -1.0F;      // 全 0 → 给一个合法方向（避免 normalize(0)）
    }
    if (opt_last_sun) mq_imgui_text("实际：使用默认太阳光");
    else mq_imgui_text_disabled("实际：未使用（场景已有光源或已关闭）");
    mq_imgui_separator();

    /* ④ 主相机 near/far 覆盖（覆盖关闭时字段跟随场景实际值，勾选后不跳变） */
    mq_imgui_text("主相机 near / far");
    opt_cam_override = mq_imgui_checkbox("覆盖场景相机", opt_cam_override ? 1.0F : 0.0F) != 0.0F;
    if (opt_cam_override) {
        opt_cam_near = std::max(mq_imgui_drag_float("near", opt_cam_near, 0.005F), 1e-4F);
        opt_cam_far = mq_imgui_drag_float("far", opt_cam_far, 0.5F);
        if (opt_cam_far < opt_cam_near * 1.001F) opt_cam_far = opt_cam_near * 1.001F;
    } else {
        mq_imgui_text_disabled("跟随场景相机的 near/far");
    }
    {
        char line[160];
        std::snprintf(line, sizeof line, "实际：near=%.4f far=%.2f",
                      (double)opt_last_near, (double)opt_last_far);
        mq_imgui_text(line);
    }
    mq_imgui_separator();

    /* ⑤ 后处理 PostEffect（docs/13 §后处理）——视图层覆盖，不改场景。
     * 折叠头（默认收起）：面板高度留给常用项，展开后调参；总开关在折叠头内。
     * 默认只叠加 3D 层（2D/UI 不受影响），勾「包含 2D / UI」才整帧。
     * 执行顺序 = 采样（含色差）→ 曝光 → 色调映射 → Bloom → 暗角 → 色彩分级。 */
    if (mq_imgui_collapsing_header("后处理 PostEffect")) {
        const bool old_enabled = opt_post_enabled;
        /* label 带 ##post：与「默认太阳光」的「启用」同名会触发 ImGui ID 冲突（显示仍是「启用」）。 */
        opt_post_enabled = mq_imgui_checkbox("启用##post", opt_post_enabled ? 1.0F : 0.0F) != 0.0F;
        if (old_enabled != opt_post_enabled) engine_options_log_post();
        if (opt_post_enabled) {
            /* 作用范围：默认**只叠加 3D 层**（2D/UI 原样叠在 post 结果之上）。 */
            {
                const bool old = opt_post_include_2d;
                opt_post_include_2d = mq_imgui_checkbox("包含 2D / UI", opt_post_include_2d ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_include_2d) engine_options_log_post();
            }
            if (!opt_post_include_2d)
                mq_imgui_text_disabled("默认只叠加 3D 层（2D / UI 不受影响）");
            /* 曝光 + 色调映射 */
            {
                const int sel = opt_post_tonemap ? 1 : 0;
                const int nsel = (int)mq_imgui_combo("色调映射", (float)sel, "无,ACES");
                const int ntm = nsel == 1 ? 1 : 0;
                if (ntm != opt_post_tonemap) { opt_post_tonemap = ntm; engine_options_log_post(); }
                const float nexp = mq_imgui_slider_float("曝光", opt_post_exposure, 0.10F, 4.0F);
                if (nexp != opt_post_exposure) { opt_post_exposure = nexp; engine_options_log_post(); }
            }
            /* Bloom 泛光 */
            {
                const bool old = opt_post_bloom;
                opt_post_bloom = mq_imgui_checkbox("Bloom 泛光", opt_post_bloom ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_bloom) engine_options_log_post();
                if (opt_post_bloom) {
                    const float ni = mq_imgui_slider_float("强度##bloom", opt_post_bloom_intensity, 0.0F, 3.0F);
                    if (ni != opt_post_bloom_intensity) { opt_post_bloom_intensity = ni; engine_options_log_post(); }
                    const float nt = mq_imgui_slider_float("阈值##bloom", opt_post_bloom_threshold, 0.0F, 1.0F);
                    if (nt != opt_post_bloom_threshold) { opt_post_bloom_threshold = nt; engine_options_log_post(); }
                    const float nr = mq_imgui_drag_float("半径##bloom", opt_post_bloom_radius, 0.001F);
                    const float clamped = std::clamp(nr, 0.001F, 0.15F);
                    if (clamped != opt_post_bloom_radius) { opt_post_bloom_radius = clamped; engine_options_log_post(); }
                }
            }
            /* 暗角 */
            {
                const bool old = opt_post_vignette;
                opt_post_vignette = mq_imgui_checkbox("暗角", opt_post_vignette ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_vignette) engine_options_log_post();
                if (opt_post_vignette) {
                    const float nv = mq_imgui_slider_float("强度##vignette", opt_post_vignette_intensity, 0.0F, 1.0F);
                    if (nv != opt_post_vignette_intensity) { opt_post_vignette_intensity = nv; engine_options_log_post(); }
                }
            }
            /* 色差 */
            {
                const bool old = opt_post_chromatic;
                opt_post_chromatic = mq_imgui_checkbox("色差", opt_post_chromatic ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_chromatic) engine_options_log_post();
                if (opt_post_chromatic) {
                    const float na = mq_imgui_slider_float("强度##chromatic", opt_post_chromatic_strength, 0.0F, 0.02F);
                    if (na != opt_post_chromatic_strength) { opt_post_chromatic_strength = na; engine_options_log_post(); }
                }
            }
            /* 色彩分级（饱和度 / 对比度 / 灰度） */
            {
                const bool old = opt_post_grade;
                opt_post_grade = mq_imgui_checkbox("色彩分级", opt_post_grade ? 1.0F : 0.0F) != 0.0F;
                if (old != opt_post_grade) engine_options_log_post();
                if (opt_post_grade) {
                    const float ns = mq_imgui_slider_float("饱和度", opt_post_saturation, 0.0F, 2.0F);
                    if (ns != opt_post_saturation) { opt_post_saturation = ns; engine_options_log_post(); }
                    const float nc = mq_imgui_slider_float("对比度", opt_post_contrast, 0.25F, 2.0F);
                    if (nc != opt_post_contrast) { opt_post_contrast = nc; engine_options_log_post(); }
                    const float ng = mq_imgui_slider_float("灰度", opt_post_grayscale, 0.0F, 1.0F);
                    if (ng != opt_post_grayscale) { opt_post_grayscale = ng; engine_options_log_post(); }
                }
            }
        } else {
            mq_imgui_text_disabled("关闭（不改画面；开启后效果叠加在 3D 上层）");
        }
    }
    mq_imgui_separator();

    if (mq_imgui_button("重置为默认")) {
        opt_render_mode = 0;
        opt_3d_enabled = true;
        opt_2d_enabled = true;
        opt_msaa = 0;
        opt_rt_msaa = 0;
        opt_sun_enabled = true;
        opt_sun_intensity = 1.1F;
        opt_sun_dir[0] = 0.35F; opt_sun_dir[1] = -0.85F; opt_sun_dir[2] = -0.4F;
        opt_cam_override = false;            // near/far 下一帧自动跟随场景
        opt_post_enabled = false;
        opt_post_tonemap = 0;
        opt_post_exposure = 1.0F;
        opt_post_bloom = false;
        opt_post_bloom_intensity = 0.85F;
        opt_post_bloom_threshold = 0.62F;
        opt_post_bloom_radius = 0.018F;
        opt_post_vignette = false;
        opt_post_vignette_intensity = 0.55F;
        opt_post_chromatic = false;
        opt_post_chromatic_strength = 0.0022F;
        opt_post_grade = false;
        opt_post_saturation = 1.0F;
        opt_post_contrast = 1.0F;
        opt_post_grayscale = 0.0F;
        opt_post_include_2d = false;
        std::print("runtime: engine options reset\n");
    }

    mq_imgui_pop_item_width();
    mq_imgui_pop_small_font();
    mq_imgui_end();
}

/* 结束帧并把命令转成引擎网格图元（在 text_flush 之后、渲染之前调用）。
 * 几何拷进 Engine 自己的缓冲：C ABI 的指针只在"本帧 end_frame → 下帧 begin_frame"有效，
 * 而网格要到渲染时才被消费 —— 拷一份最稳，且避免跨模块生命周期耦合。 */
void Engine::imgui_extract(std::vector<mq_renderer_mesh>& out) {
    if (!imgui_on || !imgui_inited || !imgui_frame_active) return;
    imgui_frame_active = false;
    if (imgui_backend) { (void)out; return; }   // 官方后端：结束后由 mq_imgui_backend_render 画                  // 一帧只 end 一次（防脚本误关导致状态错乱）
    mq_imgui_frame f{};
    if (mq_imgui_end_frame(&f) != 0 || f.cmd_count == 0 || !f.cmds) {
        imgui_last_cmds = 0;
        return;
    }
    imgui_last_cmds = (int)f.cmd_count;
    ++imgui_frames;

    /* 一次算出总顶点/索引量 → reserve（避免边写边扩容） */
    size_t nv = 0, ni = 0;
    for (uint32_t i = 0; i < f.cmd_count; ++i) { nv += f.cmds[i].vertex_count; ni += f.cmds[i].index_count; }
    imgui_pos.clear(); imgui_uv.clear(); imgui_col.clear(); imgui_idx.clear();
    imgui_pos.reserve(nv * 2); imgui_uv.reserve(nv * 2); imgui_col.reserve(nv * 4);
    imgui_idx.reserve(ni);
    out.reserve(out.size() + f.cmd_count);

    for (uint32_t ci = 0; ci < f.cmd_count; ++ci) {
        const mq_imgui_cmd& c = f.cmds[ci];
        if (c.vertex_count == 0 || c.index_count == 0) continue;
        const uint32_t base = (uint32_t)(imgui_pos.size() / 2);
        imgui_pos.insert(imgui_pos.end(), c.positions, c.positions + (size_t)c.vertex_count * 2);
        imgui_uv.insert(imgui_uv.end(), c.uvs, c.uvs + (size_t)c.vertex_count * 2);
        imgui_col.insert(imgui_col.end(), c.colors, c.colors + (size_t)c.vertex_count * 4);
        const size_t idx_off = imgui_idx.size();
        /* 引用已是命令内 0 基；**不能**再加 base：gfx 画时会叠 baseVertex（=本网格顶点起点），
         * 再加一次就是双偏移 → 三角形拿到别的图元顶点（画面拉伸/模糊）。 */
        for (uint32_t k = 0; k < c.index_count; ++k) imgui_idx.push_back(c.indices[k]);

        /* 纹理：命令里的句柄 = gfx 纹理 id + 1（字体图集）。未上传 → -1（渲染器跳过该批）。
         * ⚠️ 判据是 `>= 1` 不是 `> 1`：gfx 的纹理 id 从 **0** 开始，
         * 句柄 1 对应 id 0（场景里没有别的纹理时字体图集正好是 0 号）——
         * 写成 `> 1` 会把**第一张纹理**当成"未上传"，面板整批被跳过。 */
        int tex = -1;
        if (c.texture >= 1) {
            const int tid = (int)(c.texture - 1);
            if (rapi && rapi->has_texture && rapi->has_texture(rself, tid)) tex = tid;
        }
        mq_renderer_mesh m{};
        m.vertices = imgui_pos.data() + (size_t)base * 2;
        m.uvs = imgui_uv.data() + (size_t)base * 2;
        m.colors = imgui_col.data() + (size_t)base * 4;
        m.dark_colors = nullptr;                 // 面板不参与 TintBlack（nullptr = 等价默认真黑）
        m.indices = imgui_idx.data() + idx_off;
        m.vertex_count = c.vertex_count;
        m.first_index = 0;                       // 本命令自带独立索引段
        m.index_count = c.index_count;
        m.base_vertex = 0;                       // 索引已含全局 base（上面的 +base）
        m.texture = tex;
        m.blend = 0;                             // normal（ImGui 默认 alpha 混合）
        m.pma = 0;                               // 图集是直通 RGBA（非预乘）
        m.layer = MQ_IMGUI_LAYER;                // ★ 在所有引擎图层之上
        m.z = (int32_t)ci;                       // 命令序 = ImGui 自身的绘制顺序
        /* 裁剪矩形：ImGui 的 scissor（子窗口/文本裁剪/滑块内部都靠它）。 */
        m.clip_x = c.clip_x;
        m.clip_y = c.clip_y;
        m.clip_w = c.clip_w;
        m.clip_h = c.clip_h;
        out.push_back(m);
    }
}

}  // namespace maque::rt
