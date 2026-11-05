/* 帧管线（docs/10 D1）：单帧推进的完整顺序（采样 → 输入 → 脚本 → 校验和 → 渲染 → 呈现）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- Engine ---- */

/* 单帧推进：① 采样 ② 输入帧 ③ 脚本 update+GC ④ 校验和/快照 ⑤ 渲染。
 * 主循环与控制面 Runtime.step 走**同一条帧序** → 行为一致（docs/10 D1、docs/11）。 */

void Engine::step_frame(bool render_frame) {
    MQ_TRACY_ZONE(runtime);                          // Tracy：一次模拟步（含渲染+present）总 zone
    tracy_tick();                                    // Tracy：MAQUE_TRACY 环境启动 + 1Hz CPU/内存采样
    profile_tick();                                  // perf：MAQUE_PROFILE 启动/到帧自动停（docs/35；未启用时零开销）
    engine_options_parse_env();                      // F4 引擎选项的启动初值（MAQUE_ENGINE_OPTIONS；只解析一次）
    /* 输入帧缓冲：**必须预分配**（平台按 pointer_cap/key_cap 写入；容量 0 = 静默丢弃全部设备事件）。
     * 这是"真实鼠标/键盘事件永远到不了脚本"的根因（docs/19 §3：宿主预分配语义）。 */
    static constexpr uint32_t kMaxPointers = 16, kMaxKeys = 32;
    static thread_local mq_input_frame iframe{};
    static thread_local mq_platform_pointer_event iframe_pointers[kMaxPointers]{};
    static thread_local mq_platform_key_event iframe_keys[kMaxKeys]{};
    iframe.pointers = iframe_pointers; iframe.pointer_cap = kMaxPointers;
    iframe.keys = iframe_keys;         iframe.key_cap = kMaxKeys;
    /* 帧起点 + FPS（按**上一次成帧**的总耗时；首帧为 0）。
     * 补步的中间步不上屏（主循环只让最后一步 render_frame）→ 这里也不能刷新 stat_t0，
     * 否则帧间隔会被算成"半步"，fps 曲线被补步拉成方波。 */
    if (render_frame) {
        const auto now = std::chrono::steady_clock::now();
        if (stat_t0.time_since_epoch().count() != 0) {
            const double total_ms = std::chrono::duration<double, std::milli>(now - stat_t0).count();
            if (total_ms > 0.0) {
                const float fps = (float)(1000.0 / total_ms);
                stat_fps = stat_fps > 0 ? (stat_fps * 0.9F + fps * 0.1F) : fps;
            }
        }
        stat_t0 = now;
    }
    // ① 采样（设备事件随时入队，固定处理点统一消费，docs/19 §3）
    /* 帧首清零设备队列：有窗口时 `mq_platform_poll` 会重填，但**无窗口/headless**
     * 没有平台轮询 → 不清零会让上一帧的事件永远滞留（表现为同一个按键每帧都触发）。 */
    iframe.pointer_count = 0;
    iframe.key_count = 0;
    if (platform) {
        MQ_TRACY_ZONE_N(runtime, "input.sample");    // Tracy：平台事件泵 + 原始事件（窗口像素）
        window_event_hooks();                        // CI/演示：MAQUE_WINDOW_EVENT_AT → 真实 SDL 窗口事件
        mq_platform_pump(platform);
        if (window) mq_platform_poll(platform, &iframe);
        /* 窗口 resize/最大化 → 自动重建渲染目标（FBO）。必须在 canvas_resolve / 渲染之前：
         * 画布映射、文本栅格化倍率（ss）、视图几何都读 TW/ss（下一段代码）。 */
        if (window) sync_window_size(iframe.width, iframe.height);
        /* 测试注入：MAQUE_IMGUI_POINTER（合成指针 → 本帧设备队列，与真实鼠标同路径） */
        imgui_fake_input(iframe.pointers, iframe.pointer_count, iframe.pointer_cap);
        /* ImGui 先吃**窗口像素**事件（它工作在窗口空间）；之后才把同一批转成世界坐标。
         * 顺序不能反：转成世界坐标后就没有原始窗口坐标了。 */
        imgui_input_feed(iframe.pointers, iframe.pointer_count, iframe.keys, iframe.key_count);
        /* 设备事件：窗口像素 → 世界坐标（只在此处转换一次；引擎内部一律世界坐标） */
        for (uint32_t i = 0; i < iframe.pointer_count; ++i)
            device_event_to_world(*this, &iframe.pointers[i]);
        if (std::getenv("MAQUE_TRACE_INPUT") && (iframe.pointer_count || iframe.key_count))
            std::print("runtime: polled pointers={} keys={} (frame {})\n",
                       iframe.pointer_count, iframe.key_count, frame);
    }
    // ① 帧首：控制端点轮询（inspector/MCP；非阻塞、单线程、帧边界一次 → 不改确定性）
    /* OrbitControls：鼠标左键拖拽环绕、滚轮缩放（3D 场景；与 ImGui 同帧时也生效，便于调试） */
    if (has_3d) {
        for (uint32_t i = 0; i < iframe.pointer_count; ++i) {
            const mq_platform_pointer_event& pe = iframe.pointers[i];
            if (pe.pointer_type != MQ_PLATFORM_POINTER_MOUSE) continue;
            if (pe.type == MQ_PLATFORM_POINTER_DOWN && pe.button == 1) {
                orbit_dragging = true;
                orbit_last_x = pe.x; orbit_last_y = pe.y;
            } else if (pe.type == MQ_PLATFORM_POINTER_UP || pe.type == MQ_PLATFORM_POINTER_CANCEL) {
                orbit_dragging = false;
            } else if (pe.type == MQ_PLATFORM_POINTER_MOVE && orbit_dragging) {
                orbit_enabled = true;
                orbit_yaw -= (pe.x - orbit_last_x) * 0.01F;
                orbit_pitch += (pe.y - orbit_last_y) * 0.01F;
                if (orbit_pitch > 1.45F) orbit_pitch = 1.45F;
                if (orbit_pitch < -1.45F) orbit_pitch = -1.45F;
                orbit_last_x = pe.x; orbit_last_y = pe.y;
            } else if (pe.type == MQ_PLATFORM_POINTER_WHEEL) {
                orbit_enabled = true;
                orbit_dist *= std::exp(-pe.scroll_y * 0.12F);
                if (orbit_dist < 0.05F) orbit_dist = 0.05F;
                if (orbit_dist > 500.0F) orbit_dist = 500.0F;
            }
        }
    }
    if (ctl_server) mq_control_server_poll(ctl_server);
    // ① 帧首：释放上一帧退役的字形图集纹理（此时已无图元引用旧 id）
    if (!retired_textures.empty()) {
        if (rapi && rself && rapi->destroy_texture)
            for (int t : retired_textures) rapi->destroy_texture(rself, t);
        retired_textures.clear();
    }
    // ①b 输入模拟器：把该帧到期的事件注入输入流（与设备事件同一条路径；回放时跳过）
    /* ①a 画布解析（docs/17）：每帧重算"设计 → 设备像素"映射。
     * 每帧而非"仅尺寸变化"：脚本/控制面可在运行时改 fit_mode / design_* / 安全区，
     * 必须立即生效；纯函数（几次乘法）开销可忽略。窗口/显示器变化同样被覆盖。 */
    canvas_resolve();
    if (platform) (void)mq_platform_display_changed(platform);
    if (direct_requested && rapi && rself && rapi->attach_window) {
        mq_platform_native_window nw{};
        const bool have_native = mq_platform_get_native_window(platform, &nw) != 0 && nw.handle;
        if (!have_native) {
            if (direct_present) {
                mq_renderer_window none{};
                (void)rapi->attach_window(rself, &none);
            }
            direct_present = false;
            direct_window_handle = nullptr;
        } else if (!direct_present || nw.handle != direct_window_handle) {
            mq_renderer_window rw{};
            rw.kind = nw.kind; rw.display = nw.display; rw.handle = nw.handle; rw.extra = nw.extra;
            mq_platform_drawable_size(platform, &rw.width, &rw.height);
            const int32_t rc = rapi->attach_window(rself, &rw);
            direct_present = rc == 0;
            direct_window_handle = direct_present ? nw.handle : nullptr;
            std::print("runtime: direct present reattach kind={} size={}x{} rc={}\n",
                       rw.kind, rw.width, rw.height, rc);
        }
    }

    sim_feed(frame);
    /* 测试钩子：`MAQUE_SIM_KEY_AT="<帧>:<键码>[,<帧>:<键码>...]"` —— 向**本帧设备事件队列**
     * 注入一次"按下"事件（走与真实键盘完全相同的路径：iframe → imgui_input_feed → 热键判断）。
     * headless/CI 没有真实键盘，这是回归"按 F2 开关面板"这条路径的唯一手段。
     * 例：MAQUE_SIM_KEY_AT="30:1002"（第 30 帧按下 F2）。 */
    if (const char* sk = std::getenv("MAQUE_SIM_KEY_AT"); sk && *sk) {
        std::string s(sk), item;
        for (size_t k = 0; k <= s.size(); ++k) {
            if (k < s.size() && s[k] != ',') { item.push_back(s[k]); continue; }
            const size_t colon = item.find(':');
            if (colon != std::string::npos && iframe.key_count < iframe.key_cap) {
                const int at = std::atoi(item.substr(0, colon).c_str());
                const int code = std::atoi(item.c_str() + colon + 1);
                if ((int)frame == at) {
                    mq_platform_key_event& ev = iframe.keys[iframe.key_count++];
                    std::memset(&ev, 0, sizeof ev);
                    ev.key = (uint32_t)code;
                    ev.down = 1;
                }
            }
            item.clear();
        }
    }
    /* **F2 引擎级热键**：开关 ImGui 调参面板（用户要求"引擎层面监听 F2，收到后打开"）。
     * 关键点：
     *   · **沿检测**（按住不放只切换一次）：SDL 在按住时会持续发 KEYDOWN（`repeat=1`），
     *     若"见到按下就切换"会变成每帧开关一次（实测日志 ON/OFF 刷屏、反复 init/shutdown）。
     *     这里维护 `imgui_f2_held`，只在 `未按住 → 按住` 的跳变时 toggle 一次，并忽略 repeat 事件。
     *   · 放在脚本 update 之前：无论脚本是否写过 imgui_*，都能开/关面板。 */
    {
        bool f2_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F2 || k.repeat) continue;      // 忽略键盘自动重复
            if (k.down) f2_down = true;
        }
        if (f2_down && !imgui_f2_held) imgui_toggle();          // 只有上升沿才切换
        imgui_f2_held = f2_down;
    }
    /* **F4 引擎级热键**：弹出/收起「引擎选项」面板；首次只开选项，不附带 F2 stats。
     * 与 F2 一样沿检测（按住只切一次）。与 F2 的**互不干扰**规则：
     *   · ImGui 本来关着 → F4 顺手打开它并记住"是 F4 开的"；再按 F4 收起面板时恢复到关
     *     （不会只关面板却把 stats 留在屏幕上）。
     *   · ImGui 本来就开着（F2 / MAQUE_IMGUI / 脚本）→ F4 只开关选项面板，**不动** stats 的开关。
     *   · F2 关整个 ImGui 时 `imgui_enable(false)` 会清掉选项面板状态 → F2 再开不会"复活"F4 面板。 */
    {
        bool f4_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F4 || k.repeat) continue;
            if (k.down) f4_down = true;
        }
        if (f4_down && !opt_f4_held) {
            if (opt_panel) {
                opt_panel = false;
                std::print("runtime: engine options panel OFF\n");
                if (opt_opened_imgui) {          // F4 自己开的 ImGui → 一并恢复关闭
                    opt_opened_imgui = false;
                    imgui_enable(false);
                }
            } else {
                opt_panel = true;
                if (!imgui_on) {
                    imgui_enable(true, false);   // F4 不附带打开 F2 stats
                    opt_opened_imgui = true;
                } else {
                    opt_opened_imgui = false;    // 已有 ImGui（F2/MAQUE_IMGUI/脚本）→ 收起时保持它开着
                }
                std::print("runtime: engine options panel ON\n");
            }
        }
        opt_f4_held = f4_down;
    }
    /* **F5 引擎级热键**：perf 录制开关（docs/35）。只在 profiling 已启用（MAQUE_PROFILE=1
     * 或控制面 start 过）时生效；沿检测（按住只触发一次）。默认不占用 F5，游戏可照常使用。 */
    if (profile_armed) {
        bool f5_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F5 || k.repeat) continue;
            if (k.down) f5_down = true;
        }
        if (f5_down && !profile_f5_held) profile_toggle();
        profile_f5_held = f5_down;
    }
    /* **F3 引擎级热键**：启动 Tracy（若未启动）+ 拉起 profiler GUI（docs/29）。
     * 沿检测同上（按住只触发一次）；已启动也允许再按 F3 —— 只补拉 GUI（不会重复弹窗）。 */
    {
        bool f3_down = false;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const mq_platform_key_event& k = iframe.keys[i];
            if (k.key != MQ_KEY_F3 || k.repeat) continue;
            if (k.down) f3_down = true;
        }
        if (f3_down && !tracy_f3_held) {
            tracy_start("F3");
            tracy_launch_gui();
        }
        tracy_f3_held = f3_down;
    }
    /* **引擎热键 F2/F4 从本帧输入里消费掉**：热键判断之后，同一批设备事件还会被 input 模块
     * 派发给脚本/玩法 —— 不消费的话"按 F2 开面板"会同时触发游戏自己的 F2 动作（双触发）。
     * 注意：ImGui 的输入转发在平台轮询阶段**更早**已经发生，这里只影响脚本可见输入。 */
    {
        uint32_t out = 0;
        for (uint32_t i = 0; i < iframe.key_count; ++i) {
            const uint32_t k = iframe.keys[i].key;
            if (k == MQ_KEY_F2 || k == MQ_KEY_F4 || (profile_armed && k == MQ_KEY_F5)) continue;   // 引擎热键（不传给游戏）
            if (out != i) iframe.keys[out] = iframe.keys[i];
            ++out;
        }
        iframe.key_count = out;
    }
    /* ImGui：开始一帧（**必须在脚本 update 之前** —— 脚本里要调 imgui_begin/控件/imgui_end）。
     * 内部还会按需初始化、上传字体图集纹理，并在 ImGui 要吃输入时清空本帧设备事件（不点穿）。 */
    imgui_frame_begin(1.0F / 60.0F);
    /* ③ 不点穿：ImGui 要吃鼠标/键盘 → 清空本帧设备事件，世界输入即被阻断。
     * 判据来自 ImGui 本帧 NewFrame（上一帧的面板状态），这是 ImGui 的标准用法。
     * 注：sim 注入走 input 模块内部路径，不受这里影响（CI 仍能驱动玩法）。 */
    if (imgui_on && imgui_inited) {
        if (mq_imgui_want_mouse()) iframe.pointer_count = 0;
        if (mq_imgui_want_keyboard()) iframe.key_count = 0;
    }
    // ② 输入帧：状态/事件（回放时忽略设备输入，只喂记录）
    if (input) {
        if (!mq_input_replaying(input) && window) mq_input_begin_frame(input, &iframe, frame);
        else mq_input_begin_frame(input, nullptr, frame);
        mq_input_dispatch(input);
        if (mq_input_quit(input)) quit = true;
    }
    // ②b 帧边界热重载：资源变化只在此处生效（docs/08/14）
    if (watcher && (frame % 6 == 0)) poll_watcher();
    // ②c 帧边界切场景（director.loadScene：帧内不加载资源，docs/10 D7）
    if (scene && !pending_scene.empty()) {
        const std::string path = pending_scene;
        pending_scene.clear();
        if (!load_scene_into(path)) {
            std::print(stderr, "runtime: director.loadScene('{}') failed\n", path);
            mq_obs_event(obs, "runtime", 3, "scene_load_failed");
        }
    }
    // ③ 脚本：update(dt) → 帧边界 GC（确定性，docs/14）
    std::vector<mq_renderer_item> draws;
    /* Spine 网格图元（骨骼动画）：与 draws 同一帧序提交，交给 render_mesh_views 统一排序 */
    std::vector<mq_renderer_mesh> spine_meshes;
    const float dt = 1.0F / 60.0F;
    if (scene && director_paused) {
        /* director.pause()：跳过模拟（脚本/动画/粒子/导航/物理/UI 布局），仅保留采样与渲染
         * （暂停语义；输入仍被采样，便于 resume 后继续）。 */
    } else if (scene) {
        MQ_TRACY_ZONE_N(runtime, "sim");             // Tracy：逻辑阶段总 zone（脚本+动画+物理+提取）
        ++script_frames;
        {
            MQ_TRACY_ZONE_N(anim, "tween");          // SystemsRuntime::Tween（补间）
            step_tweens(dt);
            step_schedules(dt);                      // 定时器
        }
        /* 脚本错误的判定：总线**错误计数**的增量（每个 VM 各自受保护调用 → 错误隔离但不静默）。
         * 注意不能拿 dispatch_event 的返回值判断：它返回的是命中数，不是错误码。 */
        {
            MQ_TRACY_ZONE_N(script, "update");       // Tracy：脚本入口（update 回调派发）
            const uint32_t err_before = instance_script_errors();
            dispatch_event(*this, MQ_EVENT_UPDATE, (double)dt, 0.0, 0.0, nullptr);
            if (instance_script_errors() != err_before) {
                char ebuf[512] = { 0 };
                if (bus) mq_script_bus_last_error(bus, ebuf, (int)sizeof ebuf);
                if (!ebuf[0] && overlay_bus)
                    mq_script_bus_last_error(overlay_bus, ebuf, (int)sizeof ebuf);
                mq_obs_event(obs, "script", 3, "update_failed");
                write_crash_context(ebuf[0] ? ebuf : "script update failed");      // 可精确重放（docs/09 §5）
            }
            gc_all_instance_scripts();               // 帧边界：逐实例逐 VM 推进 GC（docs/10 D7）
            /* 动态实例化/卸载：脚本阶段之后的固定边界执行（docs/32 §7.1）；
             * 本边界内 on_start 里再发的请求留到下一帧边界（FIFO，禁止递归构建）。 */
            apply_instance_ops();
        }
        { MQ_TRACY_ZONE_N(anim, "anim.step"); anim_step(dt); }           // 动画采样写回（docs/15 §2）
        { shader_pass_step(dt); }                                        // 场景 shader pass 时间推进（docs/13）
        { MQ_TRACY_ZONE_N(anim, "systems.particles"); particles_step(dt); }      // 粒子推进（docs/15 §4）
        { MQ_TRACY_ZONE_N(nav, "nav.step"); nav_step(); }                // 导航代理推进（A* + 追踪）
        { MQ_TRACY_ZONE_N(physics, "physics.step"); physics_components_step(dt); }   // 组件驱动物理
        flush_pending_destroy();                         // node.destroy()：帧边界统一销毁
        { MQ_TRACY_ZONE_N(ui, "ui.step"); ui_step(); }   // UI 组件语义（layout/progress/slider/...）
        { MQ_TRACY_ZONE_N(runtime, "flatten"); scene->update_world(*this); }
        /* 音频：组件同步（autoplay/参数/空间化）→ 逻辑 playhead/状态按模拟 dt 推进
         * （与设备是否出声无关，docs/30 §5）。 */
        {
            MQ_TRACY_ZONE_N(audio, "step");
            audio_step(dt);
            if (audio) mq_audio_update(audio, dt);
            if (audio_cap) audio_capture_step(dt);
        }
        /* Spine 事件记录的清空点：**脚本之后、step 之前**（见 Engine::spine_clear_events 注释：
         * 放错了会导致 pull 面永远读到 0 条）。 */
        spine_clear_events();
        {
            MQ_TRACY_ZONE_N(anim, "spine.step");         // Tracy：Spine 步进
            spine_step(dt);                              // Spine：步进 + 出绘制数据（依赖展平后的世界位置）
        }
        { MQ_TRACY_ZONE_N(runtime, "extract"); scene->extract(*this, draws); }
        { MQ_TRACY_ZONE_N(anim, "spine.extract"); spine_extract(spine_meshes); }
        { MQ_TRACY_ZONE_N(text, "text.flush"); text_flush(draws); }   // 文本阶段 2：图集纹理 + 最终 UV
    }
    if (scene) sync_render_textures();                  // 动态组件/尺寸/name 变化统一在帧边界提交
    /* 静态场景判定（RenderTexture 默认 EveryFrame，必须在视觉输入不变时跳过 RT pass）。
     * 哈希覆盖实体树/组件 blob/派生世界矩阵；动画/粒子/Spine/shader/ImGui/输入/截图一律视为动态。 */
    if (scene) {
        const bool visual_dynamic =
            gltf_dynamic || !spine_rt.items.empty() || !systems.particles.empty() || has_shader_pass || has_shader_graph ||
            imgui_on || imgui_backend || !shot_path.empty() || !shot_seq.empty() ||
            !frames_dir.empty() || !camera_captures.empty() || frame_capture_requested ||
            std::getenv("MAQUE_CAPTURE_WGPU") != nullptr;
        bool rt_dirty = false;
        for (const auto& rt : render_textures)
            if (rt.enabled && (rt.dirty || !rt.ready)) { rt_dirty = true; break; }
        /* 视觉哈希只服务于 RenderTexture 的“静态场景跳过 RT pass”（targets.cpp /
         * render_camera_textures）。没有 RenderTexture 时它不参与任何判定，
         * 而它要遍历整棵树 + 每个组件 —— profiler 上曾占 ~15%，这里直接跳过。 */
        const bool need_visual_hash = !render_textures.empty();
        const uint64_t svh = (visual_dynamic || !need_visual_hash) ? 0 : compute_scene_visual_hash();
        scene_visual_unchanged =
            !visual_dynamic && !rt_dirty && iframe.pointer_count == 0 &&
            iframe.key_count == 0 && !iframe.resized && svh == scene_visual_hash_value;
        scene_visual_hash_value = svh;
    } else {
        scene_visual_unchanged = false;
        scene_visual_hash_value = 0;
    }
    /* F4 的 2D/UI 开关只隐藏游戏层；随后提取的 ImGui 网格必须保留，否则调试面板也会消失。 */
    if (!opt_2d_enabled) { draws.clear(); spine_meshes.clear(); }
    /* stats：只计游戏每帧逻辑提交量，调试 ImGui 面板自身不计入。
     * UI drawcall = 图元 + Spine／网格批次；图元固定 4 顶点 / 2 三角形。 */
    stat_ui_calls = (int)(draws.size() + spine_meshes.size());
    stat_3d_calls = 0;
    stat_calls = stat_ui_calls;
    stat_verts = (int)draws.size() * 4;
    stat_tris = (int)draws.size() * 2;
    for (const auto& sm : spine_meshes) {
        stat_verts += (int)sm.vertex_count;
        stat_tris += (int)(sm.index_count / 3);
    }
    /* VRAM 是后端存活的纹理 + 网格缓冲估算；不是驱动/OS 的整进程显存使用量。 */
    stat_vram_bytes = (rapi && rself && rapi->resource_bytes) ? rapi->resource_bytes(rself) : 0;
    /* ImGui：结束帧 → 绘制命令转成网格图元（**grid 链模式**；官方后端模式下唤醒 Render）。
     * 放在这里（脚本/暂停分支之外）是刻意的：
     *   · 每帧都必须 end，暂停/无场景帧也不能漏（否则 ImGui 帧状态错乱、面板冻结）；
     *   · 引擎所有图层（含 ui pass 的 layer=1）都已提取完 → 面板以 MQ_IMGUI_LAYER 提交，
     *     天然画在最上层；且与其它图元一起进同一像素缓冲 → 截图里也有面板（可自证）。 */
    imgui_extract(spine_meshes);
    // ④ 世界校验和 + 周期快照（确定性验证 + 回滚点，docs/10 §4/§5）
    /* 逐帧 checksum 默认关闭（正常游玩不付这份 O(场景) 开销）；replay/录制/MAQUE_CHECKSUM=1 才打开。
     * 关闭时仍保留周期快照/检查点（它们有自己的 snapshot checksum）。 */
    checksum_init();
    if (scene) {
        if (checksum_track) {
            last_checksum = world_checksum();
            trail.emplace_back(frame, last_checksum);
        }
        if (snaps && (frame % 60 == 0)) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
        if (cp_active && (frame % cp_interval == 0)) checkpoint_push();
        if (frame % 30 == 0) refresh_crash_buffer();         // 预渲染崩溃上下文（docs/09 §5）
    }
    mq_obs_counter_add(obs, "frames", 1.0);
    mq_obs_gauge_set(obs, "entities", scene ? (double)scene->count() : 0.0);
    mq_obs_gauge_set(obs, "draws", (double)draws.size());
    if (std::getenv("MAQUE_TRACE_TEXT")) {
        int textured = 0, solid = 0;
        for (const auto& it : draws) (it.texture >= 0 ? textured : solid)++;
        std::print("[text] draws={} textured={} solid={}\n", draws.size(), textured, solid);
    }
    if (!render_frame) {
        ++frame;
        return;
    }
    // ⑤ 渲染（只读快照 → 渲染不回写，docs/13 §8）
    const int SHpx = TH;
    const auto stat_render0 = std::chrono::steady_clock::now();
    {
        MQ_TRACY_ZONE_N(render, "render_textures");
        render_camera_textures(draws, spine_meshes);
    }
    /* CPU 像素只为离线输出、按需控制面抓帧或 SDL 呈现分配；WGPU 直呈常态保持空。 */
    const bool explicit_readback = !shot_path.empty() || !shot_seq.empty() || !frames_dir.empty();
    /* 控制面只在**真的有人要像素**时才回读：Capture.screen 会置 frame_capture_requested。
     * 旧实现 ctl_server 非空就每帧回读 —— 开着 inspector 的直呈会话等于白丢一次 GPU 同步。 */
    const bool inspector_readback = ctl_server != nullptr && frame_capture_requested;
    const bool need_pixels = explicit_readback || inspector_readback || frame_capture_requested || !direct_present;
    std::vector<uint8_t> pixels;
    if (need_pixels) pixels.resize((size_t)TW * SHpx * 4);
    /* 后处理（F4）默认**只叠加 3D 层**：只有 3D/场景 shader pass 存在时才需要走 frame3d 通路；
     * 纯 2D 场景仅在显式勾选「包含 2D / UI」（post_2d=1）时才改走 frame3d（整帧后处理）。
     * 关闭后处理时条件与旧实现完全一致（零回归）。 */
    const bool post_needs_frame3d = opt_post_enabled && opt_post_include_2d;
    if ((has_3d || has_shader_pass || has_shader_graph || post_needs_frame3d) && rapi && rself && rapi->render_frame3d) {
        /* M2：3D 场景 = 3D pass + 2D 覆盖层同帧（直呈时 out=null → present；否则离屏回读）
         * docs/13：场景自带全屏 shader pass 也走这条通路（纯 shader 场景 view_count=0 同样支持）。 */
        MQ_TRACY_ZONE_N(render, "frame.3d");
        std::vector<mq_renderer_view3d> views3d;
        std::vector<mq_renderer_light> lights3d;
        std::vector<mq_renderer_pbr_material> mats3d;
        std::vector<mq_renderer_draw3d> draws3d;
        std::vector<mq_renderer_skin> skins3d;
        extract_3d(TW, SHpx, views3d, lights3d, mats3d, draws3d, skins3d);
        if (opt_3d_enabled) {
            stat_3d_calls = (int)draws3d.size();
            for (const auto& dr : draws3d) {
                auto it = mesh_stats.find(dr.mesh);
                if (it == mesh_stats.end()) continue;
                stat_verts += (int)it->second.vertex_count;
                stat_tris += (int)(it->second.index_count / 3);
            }
        }
        stat_calls = stat_ui_calls + stat_3d_calls;
        std::vector<mq_renderer_shader_pass> passes3d;
        extract_shader_passes(passes3d);
        /* 多通道 shader 图（render.ShaderGraph）：按顺序执行各阶段，target<0 的画到主目标。 */
        std::vector<mq_renderer_shader_graph_pass> graphs3d;
        extract_shader_graphs(graphs3d);
        /* 精灵形态（width/height>0）：各自一条阶段链，gfx 先渲染到离屏纹理再合成 2D 图元。 */
        std::vector<mq_renderer_shader_graph_sprite> graph_sprites3d;
        extract_shader_graph_sprites(graph_sprites3d);
        /* shader 精灵（docs/13）：2D 图元式记录，gfx 侧先离屏渲染再合成（与 item 同层排序）。 */
        std::vector<mq_renderer_shader_sprite> sprites3d;
        extract_shader_sprites(sprites3d);
        mq_renderer_frame3d fr{};
        fr.width = TW;
        fr.height = SHpx;
        fr.views = views3d.data(); fr.view_count = opt_3d_enabled ? (uint32_t)views3d.size() : 0;
        fr.lights = lights3d.data(); fr.light_count = (uint32_t)lights3d.size();
        fr.materials = mats3d.data(); fr.material_count = (uint32_t)mats3d.size();
        fr.draws = draws3d.data(); fr.draw_count = opt_3d_enabled ? (uint32_t)draws3d.size() : 0;
        fr.skins = skins3d.data(); fr.skin_count = (uint32_t)skins3d.size();
        fr.views2d = views.data(); fr.view2d_count = (uint32_t)views.size();
        fr.items = draws.data(); fr.item_count = (uint32_t)draws.size();
        fr.meshes = spine_meshes.data(); fr.mesh_count = (uint32_t)spine_meshes.size();
        fr.passes = passes3d.data(); fr.pass_count = (uint32_t)passes3d.size();
        fr.graph_passes = graphs3d.data(); fr.graph_pass_count = (uint32_t)graphs3d.size();
        fr.graph_sprites = graph_sprites3d.data(); fr.graph_sprite_count = (uint32_t)graph_sprites3d.size();
        fr.shader_sprites = sprites3d.data(); fr.shader_sprite_count = (uint32_t)sprites3d.size();
        fr.present_x = 0;
        fr.present_y = 0;
        fr.present_w = 0;                         /* 0 = 整个 surface；目标等于屏幕时可直接渲染 */
        fr.present_h = 0;
        fr.render_mode = opt_render_mode;
        /* 后处理 PostEffect：纯视图层（F4 / MAQUE_ENGINE_OPTIONS）。enabled=0 → gfx 不建中间
         * 目标、不跑 pass（旧画面逐字节不变）；参数只在开启时被读取。 */
        engine_options_fill_post(fr.post);
        for (int i = 0; i < 4; ++i) fr.bg[i] = bg[i];
        /* MSAA：与每张 RenderTexture 同一解析链（Engine::resolve_msaa）——
         * F4 引擎选项 > MAQUE_MSAA > scn.Scene.msaa_samples（0/1=关、4=4x）。
         * 默认关 → 148 模型批量预览与旧场景逐像素不变；gfx 不支持 4x 时自动回退。 */
        fr.msaa_samples = resolve_msaa(0, false);
        opt_last_msaa = fr.msaa_samples;
        /* 面板回显（上一帧实况）：与 render_camera_textures 同一套解析，不额外决定任何东西。
         * 只统计启用中的 3D 目标 —— 2D 图元层恒单采样，把 2D 目标算进「4x 张数」会误导。 */
        opt_last_rt_total = 0;
        opt_last_rt_msaa4 = 0;
        for (const auto& s : render_textures) {
            if (!s.enabled || !s.is_3d) continue;
            ++opt_last_rt_total;
            if (resolve_msaa(s.msaa_samples, true) > 1) ++opt_last_rt_msaa4;
        }
        fr.env.sky_mode = 1;
        /* 环境光：scn.Scene.env_color/env_intensity + analytic studio IBL 参数（plans/19）。
         * env_sky_color/env_ground_color 全 0 = 由 env_color 派生（旧场景不改也有半球环境）。 */
        float env_col[3] = { 0.27F, 0.28F, 0.31F };
        float env_int = 1.6F;
        float sky_col[3] = { 0.0F, 0.0F, 0.0F };
        float ground_col[3] = { 0.0F, 0.0F, 0.0F };
        float env_hi = 1.0F;
        float env_rot_deg = 0.0F;
        if (scene) {
            env_col[0] = scene->desc.env_color.r;
            env_col[1] = scene->desc.env_color.g;
            env_col[2] = scene->desc.env_color.b;
            env_int = scene->desc.env_intensity;
            sky_col[0] = scene->desc.env_sky_color.r;
            sky_col[1] = scene->desc.env_sky_color.g;
            sky_col[2] = scene->desc.env_sky_color.b;
            ground_col[0] = scene->desc.env_ground_color.r;
            ground_col[1] = scene->desc.env_ground_color.g;
            ground_col[2] = scene->desc.env_ground_color.b;
            env_hi = scene->desc.env_highlight;
            env_rot_deg = scene->desc.env_rotation;
        }
        const bool sky_zero = (sky_col[0] == 0.0F && sky_col[1] == 0.0F && sky_col[2] == 0.0F);
        const bool ground_zero = (ground_col[0] == 0.0F && ground_col[1] == 0.0F && ground_col[2] == 0.0F);
        for (int i = 0; i < 3; ++i) {
            if (sky_zero) sky_col[i] = env_col[i] * 1.15F;
            if (ground_zero) ground_col[i] = env_col[i] * 0.45F;
            fr.env.env_color[i] = env_col[i];
            fr.env.env_sky_color[i] = sky_col[i];
            fr.env.env_ground_color[i] = ground_col[i];
        }
        fr.env.env_sky_color[3] = 1.0F;
        fr.env.env_ground_color[3] = 1.0F;
        fr.env.env_color[3] = 1.0F;
        fr.env.intensity = env_int;
        fr.env.env_highlight = env_hi;
        fr.env.env_rotation = env_rot_deg * 3.14159265F / 180.0F;
    /* envmap/IBL：显式 scene.env_texture 才启用（默认保留 analytic studio IBL 基线）；
     * MAQUE_ENV_IBL=1 强制用内建 studio_env，MAQUE_ENV_IBL=0 强制关闭。 */
    int32_t env_tex_id = -1, env_irr_id = -1;
    std::string env_path = scene->desc.env_texture;
    const char* env_force = std::getenv("MAQUE_ENV_IBL");
    if (env_force && env_force[0] == '1') env_path = "builtin:/textures/studio_env";
    if (env_force && env_force[0] == '0') env_path.clear();
    if (!env_path.empty()) {
        load_env_textures(env_path, env_tex_id, env_irr_id);
        if (env_tex_id < 0) {
            std::print(stderr, "runtime: env_texture '{}' 加载失败，回退 analytic IBL\n", env_path);
            env_tex_id = -1; env_irr_id = -1;
        }
    }
    /* 立方体环境贴图（第 26 轮）：env_cubemap 非空 → 6 面 cube（优先于 equirect），sky_mode=2。 */
    int32_t env_cube_id = -1, env_cube_irr_id = -1;
    if (!scene->desc.env_cubemap.empty()) {
        load_env_cubemap(scene->desc.env_cubemap, env_cube_id, env_cube_irr_id);
        if (env_cube_id < 0)
            std::print(stderr, "runtime: env_cubemap '{}' 不可用，回退 {} IBL\n",
                       scene->desc.env_cubemap, env_tex_id >= 0 ? "equirect" : "analytic");
    }
    fr.env.env_texture = env_tex_id;
    fr.env.env_irradiance_texture = env_irr_id;
    fr.env.env_cubemap = env_cube_id;
    fr.env.env_cubemap_irradiance = env_cube_irr_id;
    if (env_cube_id >= 0)
        fr.env.sky_mode = scene->desc.env_background ? 2 : 1;
    else
        fr.env.sky_mode = (env_tex_id >= 0 && scene->desc.env_background) ? 3 : 1;
        /* 静态帧跳过（docs/29 / docs/bugs/009）：画面输入完全不变时不再提交 GPU。
         * 只对"没有逐帧动画/外部像素消费者/覆盖层"的 3D 直呈帧启用；脚本/物理仍每帧推进。
         * 每 60 帧强制重画一次，兜住窗口 expose/驱动 surface 变化。 */
        bool skip3d = false;
        {
            const bool idle_ok =
                direct_present && !need_pixels && !frame_capture_requested &&
                !std::getenv("MAQUE_CAPTURE_WGPU") &&
                !imgui_on && !imgui_backend && !has_shader_pass && !has_shader_graph &&
                !gltf_dynamic && spine_rt.items.empty() && systems.particles.empty() &&
                morph_meshes.empty() && skins3d.empty() &&
                (render_textures.empty() || scene_visual_unchanged) && spine_meshes.empty() &&
                iframe.pointer_count == 0 && iframe.key_count == 0 && !iframe.resized;
            if (idle_ok) {
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p;
                    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
                };
                auto mixv = [&](const auto& v) { mix(v.data(), v.size() * sizeof(v[0])); };
                mix(&TW, sizeof TW); mix(&SHpx, sizeof SHpx);
                mix(bg, sizeof bg); mix(&opt_render_mode, sizeof opt_render_mode);
                mix(&fr.msaa_samples, sizeof fr.msaa_samples);
                mix(&fr.env, sizeof fr.env);
                mix(&fr.post, sizeof fr.post);      /* 后处理参数变化必须重画（否则面板调参无效果） */
                mixv(views); mixv(draws);
                mixv(views3d); mixv(lights3d); mixv(mats3d); mixv(draws3d);
                mixv(skins3d); mixv(passes3d);
                if (h == idle_render_hash && idle_render_frames < 60) {
                    skip3d = true;
                    ++idle_render_frames;
                } else {
                    idle_render_hash = h;
                    idle_render_frames = 0;
                }
            } else {
                idle_render_hash = 0;
                idle_render_frames = 0;
            }
        }
        if (!skip3d) {
        const bool direct3d = direct_present && !need_pixels;
        fr.out = direct3d ? nullptr : pixels.data();
        fr.out_size = (uint32_t)pixels.size();
        fr.present_after_readback = direct_present && need_pixels ? 1 : 0;
        const bool present_frame = direct3d || fr.present_after_readback != 0;
        const int32_t rc = rapi->render_frame3d(rself, &fr);
        if (rc == 0 && present_frame) {
            if (rapi->present) rapi->present(rself);
        } else if (rc != 0 && !(present_frame && rc == -7)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::print(stderr, "runtime: 3D frame 渲染失败 rc={} → 本帧回退 2D\n", rc);
            }
            if (present_frame) {
                mq_renderer_window none{};
                (void)rapi->attach_window(rself, &none);
                direct_present = false;
                direct_requested = false;
                direct_window_handle = nullptr;
            } else {
                has_3d = false;
            }
        }
        }  /* !skip3d */
    } else if (direct_present && rapi && rself && rapi->render_frame3d) {
        MQ_TRACY_ZONE_N(render, "frame.direct");
        mq_renderer_frame3d f{};
        f.width = TW;
        f.height = SHpx;
        f.views2d = views.data();
        f.view2d_count = (uint32_t)views.size();
        f.items = draws.data();
        f.item_count = (uint32_t)draws.size();
        f.meshes = spine_meshes.data();
        f.mesh_count = (uint32_t)spine_meshes.size();
        f.present_x = 0;
        f.present_y = 0;
        f.present_w = 0;
        f.present_h = 0;
        f.out = need_pixels ? pixels.data() : nullptr;
        f.out_size = (uint32_t)pixels.size();
        f.present_after_readback = need_pixels ? 1 : 0;
        for (int i = 0; i < 4; ++i) f.bg[i] = bg[i];
        f.env.sky_mode = 1;                       /* M1：背景色直出（3D 天空盒在 M3） */
        f.env.env_texture = -1;                   /* 2D 直呈路径无 3D env（避免 0 号纹理被误当 env） */
        f.env.env_irradiance_texture = -1;
        for (int i = 0; i < 4; ++i) f.env.sky_color[i] = bg[i];
        bool skip2d = false;
        {
            const bool idle_ok =
                direct_present && !need_pixels && !frame_capture_requested &&
                !std::getenv("MAQUE_CAPTURE_WGPU") &&
                !imgui_on && !imgui_backend && !has_shader_pass && !has_shader_graph &&
                spine_meshes.empty() && systems.particles.empty() && render_textures.empty() &&
                iframe.pointer_count == 0 && iframe.key_count == 0 && !iframe.resized;
            if (idle_ok) {
                uint64_t h = 1099511628211ull;
                auto mix = [&](const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p;
                    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
                };
                auto mixv = [&](const auto& v) { mix(v.data(), v.size() * sizeof(v[0])); };
                mix(&TW, sizeof TW); mix(&SHpx, sizeof SHpx); mix(bg, sizeof bg);
                mixv(views); mixv(draws);
                if (h == idle_render_hash && idle_render_frames < 60) {
                    skip2d = true;
                    ++idle_render_frames;
                } else {
                    idle_render_hash = h;
                    idle_render_frames = 0;
                }
            } else {
                idle_render_hash = 0;
                idle_render_frames = 0;
            }
        }
        if (!skip2d) {
        const bool present_frame = !need_pixels || f.present_after_readback != 0;
        const int32_t rc = rapi->render_frame3d(rself, &f);
        if (rc == 0 && present_frame && rapi->present) rapi->present(rself);
        else if (rc != 0 && !(present_frame && rc == -7)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::print(stderr, "runtime: direct present 失败 rc={} → 回退 SDL 呈现\n", rc);
            }
            mq_renderer_window none{};
            (void)rapi->attach_window(rself, &none);
            direct_present = false;
            direct_requested = false;
            direct_window_handle = nullptr;
        }
        }  /* !skip2d */
    } else if (rapi && rself) {
        MQ_TRACY_ZONE_N(render, "frame");   // Tracy：渲染插件成帧 + 回读（颜色 = render 模块）
        /* 有网格（骨骼动画）时走 render_mesh_views（图元 + 网格一次成帧、统一 layer/z 排序）；
         * 没有网格时保持原路径（老插件/纯精灵场景零行为变化）。 */
        if (!spine_meshes.empty() && rapi->render_mesh_views)
            rapi->render_mesh_views(rself, TW, SHpx, views.data(), (uint32_t)views.size(),
                                    draws.data(), (uint32_t)draws.size(),
                                    spine_meshes.data(), (uint32_t)spine_meshes.size(),
                                    bg, pixels.data(), (uint32_t)pixels.size());
        else
            rapi->render_views(rself, TW, SHpx, views.data(), (uint32_t)views.size(),
                               draws.data(), (uint32_t)draws.size(), bg, pixels.data(), (uint32_t)pixels.size());
        if (std::getenv("MAQUE_TRACE_SPINE") && (frame % 20 == 0))
            std::print("[spine] render frame={} items={} meshes={}\n",
                       (long long)frame, draws.size(), spine_meshes.size());
    }
    /* draw 统计口径 = 渲染器**实际发出的 2D 批次**（实例化批次 + 网格）。
     * 合批/动态图集之后"图元数"不再等于 drawcall（tank: 119 图元 → 3 次绘制），
     * 旧面板把图元数当 UI draw 会看不出任何收益。老插件没有该函数 → 回退图元计数。 */
    if (MQ_RENDERER_HAS(rapi, last_2d_batches)) stat_ui_calls = (int)rapi->last_2d_batches(rself);
    stat_calls = stat_ui_calls + stat_3d_calls;
    /* stats：渲染耗时（到 present 之前）+ 整帧耗时 → FPS；EMA 平滑避免数字乱跳。 */
    {
        const auto now = std::chrono::steady_clock::now();
        const double rms = std::chrono::duration<double, std::milli>(now - stat_render0).count();
        const double lms = std::chrono::duration<double, std::milli>(now - stat_t0).count() - rms;
        stat_render_ms = stat_render_ms > 0 ? (float)(stat_render_ms * 0.9 + rms * 0.1) : (float)rms;
        stat_logic_ms = stat_logic_ms > 0 ? (float)(stat_logic_ms * 0.9 + lms * 0.1) : (float)lms;
    }
    const auto stat_present0 = std::chrono::steady_clock::now();
    /* 呈现（docs/05、M5）：**游戏帧与 ImGui 面板共用同一个 SDL_Renderer，只 present 一次**。
     *   ① mq_platform_draw_rect：引擎帧（CPU 像素）→ renderer 后台缓冲（游戏画面）
     *   ② mq_imgui_backend_render：ImGui 官方后端在同一后台缓冲上再画一趟（调试面板，
     *      独立 render pass —— ImGui 的所有指令在脚本阶段只是**录制**，到这里才真正渲染）
     *   ③ mq_platform_present_ui：抓帧（MAQUE_CAPTURE）+ 单次 SDL_RenderPresent → 一起上屏
     * 这样不会再出现「先写窗口表面、又被 renderer present 覆盖 → 游戏画面不上屏」。
     * 无窗口 / MAQUE_PRESENT=off 时各步骤均为安全空操作。 */
    if (!direct_present && platform && window && rapi && rself) {
        MQ_TRACY_ZONE_N(platform, "present");   // Tracy：上传纹理 + ImGui pass + 单次 present
        /* 渲染目标已经包含 Canvas viewport/留边；这里只做目标到 drawable 的最终输出缩放。 */
        const int dx = 0, dy = 0;
        const int dw = (int)(canvas.window_w + 0.5F), dh = (int)(canvas.window_h + 0.5F);
        mq_platform_draw_rect(platform, pixels.data(), TW, SHpx, dx, dy, dw, dh, -1);
        if (imgui_on && imgui_backend) mq_imgui_backend_render();   // ② ImGui 独立 pass（叠在游戏帧上）
        mq_platform_present_ui(platform);                            // ③ 单次 present（含抓帧）
    }
    /* 诊断：MAQUE_TRACE_STATS=1 时每 30 帧打印一次逐项耗时（logic / render / present）。
     * 三项相加 ≈ 每步成本，**屏幕 FPS ≈ 1000 / 该值**（主循环每个模拟步都渲染+present 一次）。
     * 用来回答"这个场景为什么只有 N fps"：先看是逻辑（脚本/Spine/文本）还是渲染/呈现。 */
    {
        const double pms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stat_present0).count();
        stat_present_ms = stat_present_ms > 0 ? (float)(stat_present_ms * 0.9 + pms * 0.1) : (float)pms;
        if (const char* ts = std::getenv("MAQUE_TRACE_STATS"); ts && *ts && *ts != '0' && frame % 30 == 0)
            std::print("[stats] frame={} fps={:.1f} total={:.1f}ms logic={:.1f} render={:.1f} present={:.1f} "
                       "draws={} ui_draws={} 3d_draws={} tris={} verts={} vram_bytes={} target={}x{} imgui={}\n",
                       (long long)frame, stat_fps,
                       stat_logic_ms + stat_render_ms + stat_present_ms,
                       stat_logic_ms, stat_render_ms, stat_present_ms,
                       stat_calls, stat_ui_calls, stat_3d_calls, stat_tris, stat_verts, stat_vram_bytes,
                       TW, SHpx, imgui_on ? 1 : 0);
    }
    /* Tracy：**基础数据**（帧时间/逻辑/渲染/呈现/FPS/draw/实体）+ 帧标记。
     * 未启动 Tracy 时全部 no-op（每次只是一个原子读 + 返回）。 */
    MQ_TRACY_FRAME();
    MQ_TRACY_PLOT("frametime", stat_logic_ms + stat_render_ms + stat_present_ms);
    MQ_TRACY_PLOT("logic", stat_logic_ms);
    MQ_TRACY_PLOT("render", stat_render_ms);
    MQ_TRACY_PLOT("present", stat_present_ms);
    MQ_TRACY_PLOT("fps", stat_fps);
    MQ_TRACY_PLOT("draws", stat_calls);
    MQ_TRACY_PLOT("entities", scene ? (double)scene->count() : 0.0);
    /* Tracy：内存域（docs/29 §10）。每 MAQUE_TRACY_MEM_INTERVAL（默认 1s）采样一次：
     * node / texture / gpu / font 是"读当前真相"的采样型域；script / heap 由分配器钩子实时记账。
     * memory_due 在关闭（MAQUE_TRACY_MEM=0）或未编入时恒 false → 连采样都不做。 */
    if (maque::tracy::memory_due(dt)) {
        memory_sample();
        maque::tracy::memory_publish();
    }
    if (!frames_dir.empty()) {
        char fn[64]; std::snprintf(fn, sizeof(fn), "/frame_%04d.ppm", (int)frame);
        write_ppm(frames_dir + fn, TW, SHpx, pixels);
    }
    /* 截图（--shot / MAQUE_SHOT）：渲染到指定帧后写一张图片并按扩展名选格式，随后退出 */
    if (!shot_path.empty() && (int)frame == shot_frame) {
        const bool png = shot_path.size() > 4 && shot_path.compare(shot_path.size() - 4, 4, ".png") == 0;
        const int rc = png ? mq_image_write_png(shot_path.c_str(), (uint32_t)TW, (uint32_t)SHpx,
                                                pixels.data(), (uint32_t)TW * 4u)
                           : (write_ppm(shot_path, TW, SHpx, pixels) ? 0 : 1);
        if (rc == 0) std::print("runtime: shot '{}' {}x{} frame={}\n", shot_path, TW, SHpx, (int)frame);
        else std::print(stderr, "runtime: shot 写入失败 '{}'\n", shot_path);
        quit = true;                                        // 单张截图为一次性任务（CI/预览：一条命令即得图）
    }
    /* 批量截图（--shot-seq）：按帧号列表 or 每 N 帧拍一张。写完**最后一张**才退出 ——
     * 这样一条命令就能拿到"第 1/30/60 帧"三张图，用来验证动画推进、物理轨迹、UI 变化。 */
    if (!shot_seq.empty() && !shot_list.empty()) {
        const bool due = std::find(shot_list.begin(), shot_list.end(), (int)frame) != shot_list.end();
        if (due) write_seq_shot(pixels, TW, SHpx);
    } else if (!shot_seq.empty() && shot_every > 0) {
        /* 从第 shot_every 帧开始，每 shot_every 帧一张（第 0 帧通常还没布局完）。 */
        if ((int)frame > 0 && (int)frame % shot_every == 0) write_seq_shot(pixels, TW, SHpx);
    }
    if ((ctl_server || frame_capture_requested) && !pixels.empty()) {
        frame_pixels = pixels;
        frame_w = TW;
        frame_h = SHpx;
        frame_capture_requested = false;
    }
    ++frame;
}

}  // namespace maque::rt
