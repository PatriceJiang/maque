/* 引擎装配（docs/12/20）：场景来源挂载、setup / load_scene_into / teardown
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

bool mount_scene_source(mq_vfs* vfs, const std::string& scn_path, std::string& vfs_path, std::string& dir_out) {
    if (!vfs || scn_path.empty()) return false;
    const bool remote = scn_path.rfind("http://", 0) == 0 || scn_path.rfind("https://", 0) == 0;
    const bool asset = scn_path.rfind("asset://", 0) == 0;
    std::string dir, file, source;
    if (remote) {
        const size_t slash = scn_path.find_last_of('/');
        if (slash == std::string::npos || slash + 1 >= scn_path.size()) return false;
        dir = scn_path.substr(0, slash + 1);                  // 含结尾 '/' —— URL 基址
        file = scn_path.substr(slash + 1);
        source = dir;
    } else if (asset) {
        std::string logical = scn_path.substr(8);
        while (!logical.empty() && logical.front() == '/') logical.erase(logical.begin());
        const size_t slash = logical.find_last_of('/');
        file = slash == std::string::npos ? logical : logical.substr(slash + 1);
        if (file.empty()) return false;
        const std::string base = slash == std::string::npos ? std::string() : logical.substr(0, slash + 1);
        dir = "asset://" + base;
        source = dir;
    } else {
        const std::filesystem::path scnp(scn_path);
        dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();
        file = scnp.filename().string();
        source = "file://" + dir;
    }
    const int rc = mq_vfs_mount(vfs, "/", source.c_str());
    if (rc != MQ_VFS_OK) {
        std::print(stderr, "runtime: mount scene source '{}' failed (rc={})\n", source, rc);
        return false;
    }
    dir_out = dir;
    vfs_path = "/" + file;
    return true;
}

/* MAQUE_MOUNT="/cdn=https://host/assets/,/extra=../shared"：额外挂载（后挂载优先，docs/04 §5）。
 * 场景目录挂载之后应用：本地开发时把资源指向 CDN，或从 URL 场景挂本地补丁目录。 */
void apply_env_mounts(mq_vfs* vfs) {
    const char* spec = std::getenv("MAQUE_MOUNT");
    if (!spec || !*spec || !vfs) return;
    const std::string s(spec);
    size_t i = 0;
    while (i <= s.size()) {
        const size_t comma = s.find(',', i);
        const std::string item = s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) {
            const std::string mp = item.substr(0, eq), src = item.substr(eq + 1);
            const int rc = mq_vfs_mount(vfs, mp.c_str(), src.c_str());
            if (rc == MQ_VFS_OK) std::print("runtime: mount '{}' <- {}\n", mp, src);
            else std::print(stderr, "runtime: mount '{}' <- {} failed (rc={})\n", mp, src, rc);
        } else {
            std::print(stderr, "runtime: MAQUE_MOUNT item '{}' ignored (expected <mountpoint>=<source>)\n", item);
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
}

/* 装配引擎：VFS → 反射 → 渲染插件 → 场景 → 平台/输入 → 脚本 → 控制面。
 * 两条入口（命令行 / 嵌入宿主）共用，保证行为一致。 */
bool Engine::setup(const std::string& scn_path, const char* renderer_override) {
    MQ_TRACY_ZONE_N(runtime, "engine.setup");   // Tracy：装配阶段（VFS/反射/插件/场景/平台/脚本）
    /* 优雅退出：**无条件**安装（宿主 kill / CI 超时清理）——
     * 注意 SDL_HINT_NO_SIGNAL_HANDLERS 已阻止 SDL 接管；这里必须在平台创建之后仍持有处理器。 */
    signal(SIGTERM, on_quit_signal);
    signal(SIGINT, on_quit_signal);
    if (const char* cr = std::getenv("MAQUE_CRASH_REPORT"); cr && *cr) {   // 两条入口共用
        signal(SIGSEGV, on_fatal_signal);
        signal(SIGABRT, on_fatal_signal);
        signal(SIGFPE, on_fatal_signal);
        signal(SIGINT, on_fatal_signal);
        if (g_crash_fd < 0) g_crash_fd = maque::compat::open_fd(cr, maque::compat::kOpenWriteCreate);
    }
    vfs = mq_vfs_create();
    if (const char* lim = std::getenv("MAQUE_SCENE_INSTANCE_LIMIT")) {
        const int v = std::atoi(lim);
        if (v > 0) instance_limit = v;              // 实例数预算（CI/诊断可调，docs/32 §8）
    }
    std::string scn_vfs;
    if (!mount_scene_source(vfs, scn_path, scn_vfs, scn_dir)) {
        std::print("runtime: bad scene source '{}'\n", scn_path);
        return false;
    }
    apply_env_mounts(vfs);

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, scn_vfs.c_str(), &d, &n) == MQ_VFS_OK) { text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) { std::print("runtime: cannot read scene '{}'\n", scn_path); return false; }

    reflect = mq_reflect_create();
    mq_reflect_load_schema(reflect);
    scene_type = mq_reflect_type_by_name(reflect, "scn.Scene");
    if (!scene_type) { std::print("runtime: no scn.Scene type\n"); return false; }

#ifdef MQ_RENDERER_STATIC_NULL
    (void)renderer_override;
    const char* plugin = "static:null";
    rapi = mq_renderer_null_api();
    if (!rapi) {
        std::print("runtime: static null renderer unavailable\n");
        return false;
    }
#elif defined(MQ_RENDERER_STATIC_GFX)
    (void)renderer_override;
    const char* plugin = "static:gfx";
    rapi = (const mq_renderer_v2*)mq_module_query(MQ_RENDERER_API_NAME, MQ_RENDERER_API_VERSION);
    if (!rapi) {
        std::print("runtime: static gfx renderer unavailable\n");
        return false;
    }
#else
    const char* plugin = renderer_override ? renderer_override : std::getenv("MAQUE_RENDERER");
#ifdef MQ_RENDERER_DEFAULT
    if (!plugin) plugin = MQ_RENDERER_DEFAULT;
#endif
    /* 平台默认插件文件名：Windows 是 maque_gfx.dll，类 UNIX 是 libmaque_gfx.so。 */
#if defined(_WIN32)
    if (!plugin) plugin = "maque_gfx.dll";
#else
    if (!plugin) plugin = "libmaque_gfx.so";
#endif
    host = mq_host_create();
    mq_loaded_module* rmod = nullptr;
    if (mq_host_load(host, plugin, &rmod) != 0 ||
        !(rapi = (const mq_renderer_v2*)mq_host_query(host, MQ_RENDERER_API_NAME, MQ_RENDERER_API_VERSION))) {
        std::print("runtime: cannot load renderer plugin '{}'\n", plugin);
        return false;
    }
#endif
    const char* backend = nullptr;
    rself = rapi->create(&backend);
    if (!rself) { std::print("runtime: renderer create failed\n"); return false; }
    std::print("runtime: renderer '{}' backend='{}'\n", plugin, backend ? backend : "?");

    audio_setup();                               // 音频实例先于场景：脚本 on_start 可 audio_play
    obs = mq_obs_create();                       // 场景装配期即可能产生事件（脚本错误等）
    if (!load_scene_into(scn_path)) return false;
    /* 窗口标题：MAQUE_TITLE 覆盖场景标题（默认用场景 title） */
    std::string win_title = scene->desc.title;
    if (const char* ts = std::getenv("MAQUE_TITLE"); ts && *ts) win_title = ts;
    platform = mq_platform_create(win_title.c_str(), win_w, win_h);
    window = mq_platform_has_window(platform) != 0;
    /* load_scene_into 发生在建窗前，只能用逻辑尺寸预估；现在用真实 drawable 校正 TW/TH。 */
    if (window) {
        mq_platform_drawable_size(platform, &drawable_last_w, &drawable_last_h);
        rebuild_render_target();
    }
    /* ---- 3D/PBR：能力自省 + 直接呈现探测（plans/18-pbr-3d.md M0）----
     *   · caps 总是调用（记录适配器能力，MAQUE_RENDER3D_INFO/wgpu 时打印）；
     *   · 呈现模式：`auto`（默认）| `sdl` | `wgpu` | `off`
     *       - auto：没有"必须读 renderer 后台缓冲像素"的消费者时**优先** wgpu 交换链直呈，
     *         省掉每帧 GPU→CPU 回读（回读是每帧一次 GPU 同步点，桌面/浏览器抢 GPU 就抖）；
     *         attach 失败自动回退 SDL。`MAQUE_CAPTURE`（SDL 平台抓帧）只能走 SDL。
     *       - sdl：强制 SDL_Renderer 呈现（老行为，像素可抓）。
     *       - wgpu：强制直呈（render3d_present 用例）。 */
    if (rapi->caps && rself) {
        const bool want_wgpu = [] {
            const char* p = std::getenv("MAQUE_PRESENT");
            if (p && (std::strcmp(p, "wgpu") == 0)) return true;
            if (p && *p && std::strcmp(p, "auto") != 0) return false;   // off/software/accelerated/sdl
            const char* cap = std::getenv("MAQUE_CAPTURE");             // SDL 抓帧：必须走 renderer
            return !(cap && *cap);
        }();
        const char* info = std::getenv("MAQUE_RENDER3D_INFO");
        mq_renderer_caps caps{};
        if (rapi->caps(rself, &caps) == 0 && (want_wgpu || (info && *info && *info != '0')))
            std::print("runtime: renderer caps tex={} lights={} target_fmt={} depth_fmt={} direct_present={} hdr={}\n",
                       caps.max_texture_size, caps.max_lights, caps.target_format, caps.depth_format,
                       caps.direct_present, caps.hdr);
        direct_requested = want_wgpu && window && rapi->attach_window;
        if (direct_requested) {
            mq_platform_native_window nw{};
            if (mq_platform_get_native_window(platform, &nw) != 0) {
                mq_renderer_window rw{};
                rw.kind = nw.kind;
                rw.display = nw.display;
                rw.handle = nw.handle;
                rw.extra = nw.extra;
                int32_t dw = 0, dh = 0;
                mq_platform_drawable_size(platform, &dw, &dh);
                rw.width = dw;
                rw.height = dh;
                const int rc = rapi->attach_window(rself, &rw);
                std::print("runtime: direct present attach_window kind={} size={}x{} rc={}（{}）\n",
                           rw.kind, rw.width, rw.height, rc, rc == 0 ? "wgpu swapchain" : "回退离屏+SDL");
                direct_present = (rc == 0);
                direct_window_handle = direct_present ? nw.handle : nullptr;
            } else {
                std::print("runtime: direct present 不可用（platform 未给出原生窗口句柄）\n");
            }
        }
    }
    input = mq_input_create();
    mq_input_set_hit_test(input, runtime_hit_test, this);
    mq_input_set_parent_fn(input, runtime_parent, this);
    mq_input_add_handler(input, runtime_input_handler, this);
    { const char* hs = std::getenv("MAQUE_HITS"); if (hs && *hs && *hs != '0') trace_hits = true; }
    { const char* ko = std::getenv("MAQUE_KEEP_OPEN"); if (ko && *ko && *ko != '0') keep_open = true; }
    if (const char* sp = std::getenv("MAQUE_SHOT"); sp && *sp) shot_path = sp;
    if (const char* sf = std::getenv("MAQUE_SHOT_FRAME"); sf && *sf) shot_frame = std::atoi(sf);
    /* 批量截图：一条命令拍多帧（动画/物理只拍首帧看不出对错）。 
     *   MAQUE_SHOT_SEQ   输出模板（`{}` → 帧号；无 `{}` 则插在扩展名前）
     *   MAQUE_SHOT_FRAMES 精确帧号，逗号分隔（如 "1,30,60"）
     *   MAQUE_SHOT_EVERY  每 N 帧拍一张；MAQUE_SHOT_COUNT 最多几张（默认到 shot_last 为止） */
    if (const char* sq = std::getenv("MAQUE_SHOT_SEQ"); sq && *sq) shot_seq = sq;
    if (const char* sl = std::getenv("MAQUE_SHOT_FRAMES"); sl && *sl) {
        std::string s(sl), cur;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ',') {
                if (!cur.empty()) shot_list.push_back(std::atoi(cur.c_str()));
                cur.clear();
            } else if (s[i] != ' ') cur.push_back(s[i]);
        }
    }
    if (const char* se = std::getenv("MAQUE_SHOT_EVERY"); se && *se) shot_every = std::atoi(se);
    if (const char* scn = std::getenv("MAQUE_SHOT_COUNT"); scn && *scn) shot_count = std::atoi(scn);
    snaps = mq_snapshot_pool_create(8, 1 << 16);
    { const char* cp = std::getenv("MAQUE_CHECKPOINTS"); if (cp && *cp && *cp != '0') cp_active = true; }
    /* 场景/脚本/资源/物理装配统一走 load_scene_into（与 director.loadScene 同一条路径） */
    control = mq_control_create();
    /* 控制面方法由 DSL（schema/ control_list）声明、codegen 生成注册表 —— 引擎零手写注册
     * （docs/21 §1/§5：公开接口只声明一次）。此处只需提供实例上下文。 */
    mq_control_register_generated(control, this);
    /* MCP 工具表（含类型化 inputSchema）同样是生成物（docs/21 §6.3） */
    mq_control_set_tools_json(control, [](void*) { return mq_control_tools_json(); }, nullptr);
    std::print("runtime: platform window={} input=on\n", window ? 1 : 0);
    return true;
}


/* 场景装配（setup 与 director.loadScene 共用）：解析 → 实例化 → 尺寸/视图 → 物理世界
 * → 脚本 VM/绑定 → 监视 → 资源预解析 → 物理组件建体。切场景同样走此处（docs/12/20）。 */
bool Engine::load_scene_into(const std::string& scn_path) {
    MQ_TRACY_ZONE_N(runtime, "scene.load");   // Tracy：场景加载（资源/实体/脚本装配）
    /* 场景来源挂载（本地目录 / http(s) URL 目录）——后挂载者优先 → 新场景来源生效 */
    std::string scn_vfs;
    if (!mount_scene_source(vfs, scn_path, scn_vfs, scn_dir)) return false;
    apply_env_mounts(vfs);        // MAQUE_MOUNT 覆盖目录保持最高优先级（幂等，不重复追加）

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, scn_vfs.c_str(), &d, &n) == MQ_VFS_OK) {
          text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) return false;

    /* docs/34 P0：包清单决定是否走"每语言一个 VM + 模块/实例"路径（在旧实例销毁前判定）。 */
    detect_package();

    /* ---- 世界重置（切场景：旧世界全部释放；脚本状态不跨场景保留，等价  onDestroy）---- */
    reset_instances();                   // 实例树/全部实例 VM（覆盖层是独立总线，不在此列）
    spine_release();                     // Spine 实例随世界一起释放（派生数据，重建即可）
    clear_asset_caches();                // ⚠️ 纹理/网格缓存键是相对路径，换场景挂载点后必须清空
    systems.agent_paths.clear();
    node_events.clear();
    physics_role_bodies.clear();
    systems.particles.clear();
    if (audio) {
        for (auto& kv : audio_clips) mq_audio_clip_release(kv.second);
        audio_clips.clear();
        last_listener_master = 1.0F;
    }
    director_paused = false;
    /* 实体 id 水位跨场景保留：旧场景的 id 永不复用（常驻脚本/快照引用的陈旧 id 必须
     * 保持无效，绝不能别名到新场景的同号实体）。 */
    const int preserved_entity_id = scene ? scene->next_id : 0;
    /* 热重载可能已把根场景换成堆上的 scene_owned；切场景/重装配必须释放它，
     * 否则旧的整棵 Scene 从 scene_store 之外泄漏（且下一个 reload 会再叠一层）。 */
    scene_owned.reset();
    scene_store = Scene();
    scene_store.next_id = preserved_entity_id;
    scene = &scene_store;
    scn_vfs_path = scn_vfs;              // 根实例 source（docs/32）
    scn_full_path = scn_path;
    /* 根场景 = 0 号实例：先建记录（实体归属/嵌套实例的父链都依赖它），再装配节点树。
     * 根场景的资源基准 = VFS 根（挂载点就是场景目录）；scene_chain 用于循环实例化检测。 */
    scene_chain.clear();
    scene_chain.push_back(scn_vfs);
    building_instance = 0;
    building_base.clear();
    if (create_instance(-1, -1, scn_vfs, 0) < 0) return false;   // 根实例固定 id=0
    if (SceneInstanceRuntime* root = instance_at(0)) { root->state = 1; }
    if (!scene->deserialize(*this, text)) return false;
    scene->instantiate(*this);
    scene_chain.pop_back();
    scene->update_world(*this);
    {
        SceneInstanceRuntime* root = instance_at(0);
        if (root) {
            for (int eid : scene->roots)
                if (entity_instance.find(eid) != entity_instance.end() && entity_instance[eid] == 0)
                    root->roots.push_back(eid);
        }
    }
    /* 窗口标题跟随场景（director.loadScene 切 demo 时标题栏同步；首次装配时 platform 尚未创建，
     * 起点标题由 setup 用场景 title 建窗）。MAQUE_TITLE 显式覆盖时不动。 */
    if (platform && !std::getenv("MAQUE_TITLE")) mq_platform_set_title(platform, scene->desc.title.c_str());

    W = scene->desc.width > 0 ? scene->desc.width : 320;
    H = scene->desc.height > 0 ? scene->desc.height : 240;
    nviews = scene->desc.views > 0 ? scene->desc.views : 1;
    /* 启动期覆盖（宿主/CI 用；docs/05、docs/22）：
     *   MAQUE_WINDOW=WxH  窗口尺寸（世界坐标不变，仅改可视区）
     *   MAQUE_VIEWS=n     视图数（1 = 单视图；>1 = 并排多视图/立体） */
    /* **默认窗口 800x650**（用户要求）：世界/渲染目标仍是场景尺寸（W×H），
     * 窗口内等比缩放 + 居中留边显示 —— 场景比窗口大就缩小，小就放大，都不裁剪。
     * 之前的默认是"窗口 = 场景尺寸"，大场景（如 spine 的 all.scn 1600x1300）
     * 在 1080p 屏上会超出显示器，必须改成固定小窗口。
     *
     * MAQUE_WINDOW=WxH 覆盖；`0x0` 表示**不建窗口**（CI/无显示环境用），
     * 所以解析成功时原样接受（含 0），不要被"必须 >0"的校验吃掉语义。
     * `director.loadScene` 切场景时窗口已存在 → 沿用**当前**窗口逻辑尺寸
     * （用户可能刚拖拽/最大化过；默认值/MAQUE_WINDOW 只决定首次建窗）。 */
    if (platform && window) {
        int32_t ww = 0, wh = 0;
        mq_platform_window_size(platform, &ww, &wh);
        if (ww > 0 && wh > 0) { win_w = ww; win_h = wh; }
        std::print("runtime: window {}x{} (current; world {}x{}, scale {:.2f}x)\n",
                   win_w, win_h, W, H,
                   std::min((float)win_w / (float)W, (float)win_h / (float)H));
    } else {
        win_w = 800; win_h = 650;
        if (const char* ws = std::getenv("MAQUE_WINDOW")) {
            int ow = 0, oh = 0;
            if (std::sscanf(ws, "%dx%d", &ow, &oh) == 2) {
                win_w = ow; win_h = oh;             // 只改窗口；世界/目标不变 → 窗口内等比缩放显示
                if (win_w > 0 && win_h > 0) {
                    std::print("runtime: window {}x{} (world {}x{}, scale {:.2f}x)\n",
                               win_w, win_h, W, H,
                               std::min((float)win_w / (float)W, (float)win_h / (float)H));
                } else {
                    std::print("runtime: window disabled (MAQUE_WINDOW={})\n", ws);
                }
            } else {
                std::print(stderr, "runtime: MAQUE_WINDOW='{}' 格式应为 WxH，已忽略\n", ws);
            }
        } else {
            std::print("runtime: window {}x{} (default; world {}x{}, scale {:.2f}x)\n",
                       win_w, win_h, W, H,
                       std::min((float)win_w / (float)W, (float)win_h / (float)H));
        }
    }
    if (const char* vs = std::getenv("MAQUE_VIEWS")) {
        const int v = std::atoi(vs);
        if (v >= 1 && v <= 4) nviews = v;
    }
    canvas_setup();                             // 先定位当前场景画布；render_scale 兼容入口依赖它
    resolve_render_scale();
    rebuild_render_target();
    win_last_w = win_w; win_last_h = win_h;   // resize 检测基线（首帧再按真实窗口校正）
    rng = scene->desc.seed * 2654435761u + 12345u;
    if (physics) { mq_physics_destroy(physics); physics = nullptr; }
    {
        /* 物理装配（坐标约定权威说明见 docs/15 §3.1）：
         *   · 物理世界**就是世界坐标（y 向上）**，`scn.Scene.gravity_y` 是该坐标系下的 y 分量
         *     → 直接透传，不取反；默认 -900（负 = 屏幕向下掉，正 = 向上加速）。
         *   · 边界 = 场景矩形在世界坐标下的四条平面：min=(0,0) 是**屏幕左下**、max=(W,H) 是右上。
         *     所以物体会被挡在 y ∈ [radius, H-radius]（这才是"屏幕底部的地面"）。 */
        mq_physics_config pcfg{};
        pcfg.struct_size = (uint32_t)sizeof pcfg;
        pcfg.gravity_y = scene->desc.gravity_y;
        pcfg.fixed_dt = 1.0F / 60.0F;
        pcfg.solver_iterations = 4;
        pcfg.max_bodies = 1024;
        pcfg.seed = scene->desc.seed;
        physics = mq_physics_create(&pcfg);
        mq_physics_set_bounds(physics, 0.0F, 0.0F, (float)W, (float)H);
    }
    if (nav) { mq_nav_destroy(nav); nav = nullptr; }
    std::print("runtime: scene '{}' title='{}' script='{}' scripts={} {}x{} views={}\n",
               scn_path, scene->desc.title, scene->desc.script, scene->desc.scripts.size(), W, H, nviews);

    /* 多脚本（多语言）：scripts 非空则按**声明顺序**逐个装载（顺序 = 事件投递顺序）。
     * 根场景 = 0 号实例；脚本装载走统一的实例 helper（与嵌套实例同一路径，docs/32）。 */
    std::vector<std::string> load_list;
    if (!scene->desc.scripts.empty()) load_list = scene->desc.scripts;
    else if (!scene->desc.script.empty()) load_list.push_back(scene->desc.script);
    SceneInstanceRuntime* root_inst = instance_at(0);
    if (!root_inst) return false;
    if (!load_instance_scripts(*root_inst, load_list, true)) return false;

    if (watcher) { mq_watcher_destroy(watcher); watcher = nullptr; }
    const bool remote_scene = scn_path.rfind("http://", 0) == 0 || scn_path.rfind("https://", 0) == 0;
    const char* watch = std::getenv("MAQUE_WATCH");
    if (watch && *watch && *watch != '0') {
        if (remote_scene) {
            // 远程场景没有本地文件可监视：热重载不可用（用 MAQUE_MOUNT 挂本地覆盖目录做开发迭代）
            std::print("runtime: hot reload disabled for remote scene '{}'\n", scn_path);
        } else {
            watcher = mq_watcher_create();
            mq_watcher_add(watcher, (fs::path(scn_dir) / scn_vfs_path.substr(1)).string().c_str());
            for (const std::string& sp : script_paths)          // 监视**全部**脚本（多脚本热重载）
                mq_watcher_add(watcher, (fs::path(scn_dir) / sp).string().c_str());
            /* 嵌套场景（docs/32 §5.3）：源 .scn + 各实例脚本都纳入监视（去重由 watcher 处理）。 */
            for (auto& [iid, up] : instances) {
                (void)iid;
                if (!up || up->id == 0 || up->source.empty()) continue;
                mq_watcher_add(watcher, (fs::path(scn_dir) / up->source.substr(1)).string().c_str());
                for (const std::string& sp : up->scripts)
                    mq_watcher_add(watcher, (fs::path(scn_dir) / vfs_join(up->base, sp).substr(1)).string().c_str());
            }
            std::print("runtime: hot reload watching {} file(s)\n", mq_watcher_count(watcher));
        }
    }
    setup_render_textures();                       /* 相机离屏目标先分配：消费者预载时 URI 已可解析 */
    preload_scene_assets();
    setup_3d();                                 /* 扫描 render.* 3D 组件 + 上传内建 cube（2D↔3D 切场景需重算） */
    setup_shader_passes();                      /* 场景资源全屏 shader（.wgsl，相对场景目录；docs/13） */
    setup_shader_graphs();                      /* 多通道 shader 图（program JSON，相对场景目录；docs/13） */
    spine_setup();                              // Spine：骨架/图集/页纹理装配（装配期做 IO，不在模拟帧内）
    text_setup();                               // 字体（MAQUE_FONT → 系统字体 → 内置 5x7）
    sim_setup();                                // 输入模拟器（MAQUE_SIM / MAQUE_SIM_CLICK）
    storage_setup();                            // 键值存储（docs/25：默认 <场景目录>/.maque）
    if (const char* sa = std::getenv("MAQUE_SAFE_AREA")) {   // "l,t,r,b"（设备像素；测试/刘海屏）
        float l = 0, t = 0, r = 0, b = 0;
        if (std::sscanf(sa, "%f,%f,%f,%f", &l, &t, &r, &b) == 4) {
            canvas_safe[0] = l; canvas_safe[1] = t; canvas_safe[2] = r; canvas_safe[3] = b;
            if (platform) mq_platform_set_safe_area(platform, l, t, r, b);
        }
    }
    /* 全局脚本 / 常驻覆盖层脚本：本场景声明了就激活/切换（同一路径复用已有 VM → 状态跨场景保留）；
     * 未声明则保持现有实例 —— director.loadScene 切场景时游戏服务/外壳不消失。 */
    if (!scene->desc.global_script.empty()) (void)ensure_global_script(scene->desc.global_script);
    if (!scene->desc.overlay_script.empty()) (void)ensure_overlay_script(scene->desc.overlay_script);
    physics_components_setup();
    /* 生命周期：on_start **后序**（子实例先 ready，根场景最后）；然后全局脚本，最后覆盖层
     * —— 全局服务在 HUD 之前拿到本场景状态（docs/32 §16.1）。 */
    const uint32_t err0 = instance_script_errors();
    (void)dispatch_instance_on_start(0);
    (void)call_global_event("on_start", 0.0, 0.0, 0.0, nullptr);
    (void)call_overlay_event("on_start", 0.0, 0.0, 0.0, nullptr);
    {
        char ebuf[512] = { 0 };
        if (instance_script_errors() != err0) {
            if (bus) mq_script_bus_last_error(bus, ebuf, (int)sizeof ebuf);
            if (!ebuf[0] && global_bus)
                mq_script_bus_last_error(global_bus, ebuf, (int)sizeof ebuf);
            if (!ebuf[0] && overlay_bus)
                mq_script_bus_last_error(overlay_bus, ebuf, (int)sizeof ebuf);
            if (!ebuf[0])
                std::snprintf(ebuf, sizeof ebuf, "scene instance script error");
            write_crash_context(ebuf);
        }
    }
    return true;
}

void Engine::teardown() {
    MQ_TRACY_ZONE_N(runtime, "engine.teardown");   // Tracy：退出清理
    /* ImGui：先关官方后端再销毁上下文（否则退出时断言 "Forgot to shutdown Renderer backend?"）。 */
    if (imgui_inited) {
        mq_imgui_shutdown();
        if (imgui_tex >= 0 && rapi && rself && rapi->destroy_texture) rapi->destroy_texture(rself, imgui_tex);
        imgui_tex = -1;
        imgui_inited = false;
        imgui_backend = false;
    }
    /* 控制端点：统一在此销毁（含 UDS socket 文件清理）——正常退出与 MAQUE_CONTROL 路径都经过这里 */
    if (ctl_server) { mq_control_server_destroy(ctl_server); ctl_server = nullptr; }
    /* 释放顺序：先释放"依赖 reflect 的资源"（clips 的字符串成员要经反射析构），再销毁 reflect。 */
    if (control) { mq_control_destroy(control); control = nullptr; }
    reset_instances();                                                      // 实例树 + 全部主脚本 VM
    unload_global();                                                        // 全局脚本 VM
    unload_overlay();                                                       // 常驻覆盖层 VM
    shutdown_module_vm();                                                   // docs/34：包脚本的单 JS VM
    if (obs) { mq_obs_destroy(obs); obs = nullptr; }
    if (reflect) {
        const mq_type_desc* ct = mq_reflect_type_by_name(reflect, "asset.AnimationClip");
        if (ct) for (auto& c : systems.clips) destroy_blob(*this, ct->type_id, c.blob);
        systems.clips.clear();
    } else {
        systems.clips.clear();
    }
    systems.agent_paths.clear();
    for (auto* s : checkpoints) mq_snapshot_destroy(s);
    checkpoints.clear();
    mq_snapshot_destroy(snap_manual); snap_manual = nullptr;
    mq_snapshot_pool_destroy(snaps); snaps = nullptr;
    mq_watcher_destroy(watcher); watcher = nullptr;
    /* 场景 blob 的**动态成员**（string/vector）析构要查反射类型表：必须在
     * mq_reflect_destroy 之前销毁根场景，否则 destroy_blob 取不到类型描述，
     * 只 clear() 掉 blob 而**跳过 std::string 析构** —— 组件里所有路径/名字的
     * 堆缓冲在退出时泄漏（scene_owned 里的热重载场景同理，整棵对象一起释放）。 */
    if (scene == &scene_store) scene_store = Scene();
    scene_owned.reset();
    scene = nullptr;
    if (reflect) { mq_reflect_destroy(reflect); reflect = nullptr; }
    if (sim) { mq_sim_destroy(sim); sim = nullptr; }
    if (font) { mq_font_destroy(font); font = nullptr; }
    font_atlases.clear();
    mq_nav_destroy(nav); nav = nullptr;
    mq_physics_destroy(physics); physics = nullptr;
    spine_release();                     // Spine 实例/骨架数据（纹理归 tex_cache → 随渲染器销毁）
    mq_input_destroy(input); input = nullptr;
    destroy_render_textures();           // 相机离屏目标（公共纹理句柄）
    destroy_shader_passes();             // 场景资源 shader pass（句柄归渲染器，先于销毁渲染器释放）
    destroy_shader_graphs();             // 多通道 shader 图（阶段句柄 + 中间缓冲纹理）
    audio_teardown();                    // 先停设备/join 回调，再拆平台（SDL）
    if (rapi && rself) { rapi->destroy(rself); rself = nullptr; }
    mq_platform_destroy(platform); platform = nullptr;
#if !defined(MQ_RENDERER_STATIC_NULL) && !defined(MQ_RENDERER_STATIC_GFX)
    mq_host_destroy(host);
#endif
    host = nullptr;
    mq_vfs_destroy(vfs); vfs = nullptr;
    if (storage) { mq_storage_destroy(storage); storage = nullptr; }
}


}  // namespace maque::rt
