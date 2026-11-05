/* Engine：运行时组合根（设备/VFS/脚本/场景/资源缓存/渲染目标）。
 * 只有 modules/runtime 的 .cpp 包含；成员分块见 struct 内的注释。 */
#pragma once

#include "runtime_scene.h"
#include "runtime_math3d.h"

namespace maque::rt {

struct SpineRuntime {
    struct Instance {
        int entity = -1;                       // 实体 id
        Engine* eng = nullptr;                 // 回指（事件回调要把事件投给脚本总线）
        uint32_t comp = 0;                     // 组件 type_id（定位 blob）
        mq_spine_data* data = nullptr;         // 骨架数据（装配期建，切场景/退出时销毁）
        mq_spine_instance* inst = nullptr;
        std::string skel_path;                 // VFS 路径（诊断/截图报告用）
        std::string atlas_path;
        std::string tex_dir;                   // 页纹理目录（含结尾 '/'）
        std::vector<std::string> anim_names;   // 动画清单快照（声明序，稳定）
        std::vector<std::string> skin_names;
        struct Page { std::string name; int width = 0, height = 0; uint64_t handle = 0; int pma = 0; };
        std::vector<Page> pages;
        /* 每帧 CPU 绘制缓冲（复用，避免逐帧分配；容量按骨架规模一次到位）
         * darks = 两色着色（TintBlack）的逐顶点**暗色**通道（4 分量/顶点，长度同 cols）。
         * 即使素材没启用 TintBlack 也照传：spine2d 会填官方默认 (0,0,0,1)，着色公式随即
         * 退化成"纹理 × light"，与不支持两色着色时逐字节一致（见 mesh shader 顶部注释）。 */
        std::vector<float> verts, uvs, cols, darks, tverts;
        std::vector<uint32_t> idxs;
        std::vector<mq_spine_batch> batches;
        SpineTexCtx tex_ctx;                   // 纹理回调 user（活到 data 销毁）
        mq_spine_draw draw{};
        int last_anim = -1;                    // 上一帧播放的动画下标（检测组件字段变化）
        int z = 0;                             // 本帧排序键（z_index*100000 + 实体 id）
        /* 运行时**合成**的皮肤（MixAndMatch：skin-base + nose/short + …）。
         * ABI 约定"分配者释放" → 引擎持有，切场景/退出时统一销毁（见 spine_release）。 */
        std::vector<mq_spine_skin*> own_skins;
        mq_spine_skin* composed = nullptr;     // 最近一次 spine_skin_compose 的结果（诊断用）
        /* 上一帧的事件计数（诊断/脚本 pull 的观测口；事件本体存在实例里） */
        int events_last = 0;
    };


    std::vector<Instance> items;   // 实例表（push_back 后定型：回调 user 指向元素）
};

struct SystemsRuntime {
    struct Tween { int entity; std::string path; float from, to, elapsed, duration; int easing; };
    std::vector<Tween> tweens;
    /* 定时器（定时器）：帧驱动，确定性 */
    struct Scheduled { std::string handler; float interval, remaining; bool repeat; };
    std::vector<Scheduled> schedules;
    std::vector<int> pending_destroy;              // node_destroy：帧边界统一销毁
    struct Particle { int owner = -1; float x = 0, y = 0, vx = 0, vy = 0, life = 0, max_life = 1; float size = 2; float r = 1, g = 1, b = 1; };
    std::vector<Particle> particles;
    std::vector<int> emitter_accum;                        // 每个发射器的发射累加器（按实体 id 排序使用）
    uint32_t max_particles = 2048;
    struct Clip { std::string name; float length = 0; bool loop = true; std::vector<std::string> paths; std::vector<uint8_t> blob; };
    std::vector<Clip> clips;
    int find_clip(const std::string& name) const {
        for (size_t i = 0; i < clips.size(); ++i) if (clips[i].name == name) return (int)i;
        return -1;
    }
    struct AgentPath { std::vector<float> path; uint32_t index = 0; };
    std::unordered_map<int, AgentPath> agent_paths;
};
struct Engine {
    mq_host*            host = nullptr;                   // 模块宿主（加载渲染插件）
    const mq_renderer_v2* rapi = nullptr;                 // 渲染器插件接口
    void*               rself = nullptr;                  // 渲染器实例
    mq_vfs*             vfs = nullptr;
    /* 多脚本/多语言（docs/14 §多 VM）：**事件总线**持有全部 VM 并负责按声明顺序投递。
     * `vm` 仅指第一个 VM（便捷/自省）；任何"调用脚本处理器"的地方都必须走 bus（跨语言广播）。 */
    mq_script_bus*      bus = nullptr;
    mq_script_vm*       vm = nullptr;
    std::vector<std::string> script_paths;   // 已装载脚本（声明顺序；自省/诊断用）
    std::vector<uint64_t> script_hashes;     // 每个脚本的内容哈希（热重载判定"哪个改了"）
    /* 常驻覆盖层脚本（scn.Scene.overlay_script）：独立 VM/总线，**跨 director_load_scene 保留**
     *（主场景脚本随世界重建，覆盖层不重建）。用于外壳/HUD/导航：每次场景装配完成后收 on_start，
     *  update/输入事件在主脚本之后同时派发给它。 */
    mq_script_bus*      overlay_bus = nullptr;
    std::string         overlay_rel;         // 声明路径（相对场景目录）
    std::string         overlay_abs;         // 本地文件路径（热重载/watch 用）
    uint64_t            overlay_hash = 0;
    /* 全局脚本（scn.Scene.global_script，docs/32 §16.1）：游戏级服务层 VM/总线。
     * 与 overlay 一样跨 director_load_scene 保留，但语义是"游戏服务"（进度/存档/跨场景规则），
     * 生命周期在实例之后、覆盖层之前；其他 VM 用 global_emit(event, value) 显式寻址。 */
    mq_script_bus*      global_bus = nullptr;
    std::string         global_rel;
    std::string         global_abs;
    uint64_t            global_hash = 0;
    /* 全局状态（docs/32 §16.3）：本 Engine 实例内唯一一份键值，所有 VM 共享。
     * 用有序 map（确定性遍历/校验和/快照）；数值与文本分开存，同键可同时存在两种值。 */
    std::map<std::string, double>      global_nums;
    std::map<std::string, std::string> global_texts;
    std::string         global_ret;          // global_get_text 的返回缓冲（绑定立即复制）
    mq_reflect_ctx*     reflect = nullptr;                 // per-instance
    mq_obs_ctx*         obs = nullptr;                     // 可观测（事件/指标）
    mq_control*         control = nullptr;                 // 控制平面（JSON-RPC）
    mq_control_server*  ctl_server = nullptr;              // 控制端点（inspector attach；plans/17 §3）
    std::vector<uint8_t> frame_pixels;                     // 最近一次按需回读的 RGBA（Capture.screen）
    int frame_w = 0, frame_h = 0;
    bool frame_capture_requested = false;                  // 控制面请求下一渲染帧回读
    bool                inspect_read_only = true;          // 默认只读（D2）；MAQUE_INSPECT_RW=1 开启编辑
    mq_platform*        platform = nullptr;                // 窗口/设备输入（headless 时 dummy）
    mq_input*           input = nullptr;                   // 确定性输入流（录制/回放）
    mq_snapshot_pool*   snaps = nullptr;                   // 快照池（回滚/防抖，docs/10 §4）
    mq_snapshot*        snap_last = nullptr;                // 最近一次周期快照（池内）
    mq_snapshot*        snap_manual = nullptr;              // World.snapshot 的显式快照（独立，不被周期覆盖）
    /* 检查点快照环（docs/10 §5 分歧定位）：每 30 帧一个，二分定位首个失步帧 */
    std::vector<mq_snapshot*> checkpoints;
    uint32_t cp_cursor = 0;
    uint32_t cp_interval = 30;
    uint32_t cp_count = 8;
    bool     cp_active = false;
    mq_watcher*         watcher = nullptr;                  // 资源热重载监视（docs/08）
    std::string         script_path;                        // 当前脚本（热重载用）
    uint64_t            script_hash = 0;
    /* ---- docs/34 P0：每语言一个 JS VM + 包/模块/实例 ---- */
    struct ModulePackage {
        bool active = false;
        std::string id, version, key;    // key = id@version
        std::string root_dir;            // 包根（本地绝对路径）
        std::string scene_rel;           // 场景目录相对包根（"" / "towers/arrow"）
    };
    ModulePackage       module_pkg;
    mq_script_vm*       module_vm = nullptr;             // Engine 级唯一 JS VM（shared）
    mq_script_instance* active_script_instance = nullptr;// 当前调用的脚本实例（node_on 归属）
    mq_script_instance* global_script_instance = nullptr;
    mq_script_instance* overlay_script_instance = nullptr;
    std::deque<std::string> module_src_pool;             // loader 读取缓冲（deque：嵌套读取指针稳定）
    std::deque<std::string> module_resolved_pool;        // resolver 结果（同上）
    uint32_t            module_errors = 0;
    std::string         scn_vfs_path;                       // 场景在 VFS 中的路径（/x.scn）
    std::string         scn_full_path;                      // 场景的原始路径（崩溃报告/重放用）
    std::string         scn_dir;                            // 场景所在目录（本地路径）
    mq_storage*         storage = nullptr;                  // 键值存储（docs/25；按场景目录/session 重建）
    std::string         script_ret;                         // 脚本返回值缓冲（绑定立即复制 → 单线程复用）
    std::string         shot_path;                          // MAQUE_SHOT：渲染到该帧后写一张图片（""=不写）
    int                 shot_frame = 1;                     // MAQUE_SHOT_FRAME（默认第 1 帧）
    /* 批量截图（--shot-seq / --shot-frames / --shot-every / --shot-count）：
     * 一条命令拍多帧 —— 动画/物理这类"随时间变化"的东西只拍第 1 帧看不出对错。
     * 模板里的 `{}` 会被替换成帧号（没有 `{}` 就自动插到扩展名前）。 */
    std::string         shot_seq;                           // MAQUE_SHOT_SEQ：输出路径模板
    std::vector<int>    shot_list;                          // MAQUE_SHOT_FRAMES：精确帧号（逗号分隔）
    int                 shot_every = 0;                     // MAQUE_SHOT_EVERY：每 N 帧拍一张
    int                 shot_count = 0;                     // MAQUE_SHOT_COUNT：最多拍几张（0=不限）
    int                 shots_taken = 0;                    // 已拍张数
    int                 shot_last_frame = -1;               // 最后一张的帧号（拍完即退出）
    /* 批量截图的落地：模板 → 文件名 → 写图；并判断"是不是最后一张"（是则退出）。 */
    void write_seq_shot(const std::vector<uint8_t>& px, int tw, int th);
    uint32_t            reloads = 0;
    uint64_t            frame = 0;                          // 模拟帧号（= sim time / dt）
    uint64_t            last_checksum = 0;
    std::vector<std::pair<uint64_t, uint64_t>> trail;       // (frame, checksum) —— 失步定位
    bool                window = false;                     // 是否真窗口（有显示环境）
    /* 墙钟限速（仅真窗口路径）：固定步长的缺省语义是"跑完一帧 = dt 秒游戏时间"，
     * 若完全不等待，游戏速度就变成"机器多快就多快"（实测窗口下 2.7 倍速）。
     * 两个方向都要处理：机器比实时快 → sleep 压到 1x；机器比实时慢（渲染重）→ 一轮补几步。 */
    bool                pace_wall_clock = false;            // 真窗口且非自动化时置 true
    bool                trace_hits = false;                 // MAQUE_HITS=1：命中轨迹打印（调试）
    /* ---- Tracy 性能分析（docs/29）----
     * 客户端**手动启动**：F3（并顺手拉起 profiler GUI）或启动期 MAQUE_TRACY=1（等外部连）。
     * 未启动时所有埋点宏 no-op → 普通运行/CI 零开销。 */
    bool                tracy_env_checked = false;          // 是否已按 MAQUE_TRACY 决定过初始启动
    bool                tracy_f3_held = false;              // F3 沿检测（按住只触发一次）
    bool                tracy_gui_launched = false;         // profiler GUI 是否已拉起（避免重复弹窗）
    void tracy_tick();                                      // 帧首：env 启动 + 1Hz CPU/内存采样
    void tracy_start(const char* why);                      // 启动 client + 配置基础数据曲线
    void tracy_launch_gui();                                // 拉起 tracy-profiler（F3）
    /* Tracy 内存域采样（docs/29 §10）：节点树 / GPU 纹理与缓冲 / 字图集。
     * 由 memory_due() 节流（默认 1s），delta 型域（script/heap）由分配器钩子维护。 */
    void memory_sample();
    /* ---- Linux 采样剖析（perf，docs/35）----
     * 与 Tracy 的分工：Tracy 看"一帧内各段耗时"（zone/内存域）；perf 看"几百帧里 CPU
     * 花在哪些函数/源码行"（统计采样）。perf 是**外部子进程**，不改变模拟行为。
     * 启用方式：MAQUE_PROFILE=1 自动录（可选 START/FRAMES），或控制面 Perf.profile_start；
     * MAQUE_PROFILE=1 时 F5 手动开关（引擎热键，不再下发给脚本）。 */
    bool                profile_env_checked = false;        // 是否已解析 MAQUE_PROFILE
    bool                profile_armed = false;              // F5 可用 / 控制面已介入
    bool                profile_auto_started = false;       // env 自动启动只尝试一次
    bool                profile_f5_held = false;            // F5 沿检测
    uint64_t            profile_auto_start_frame = 0;       // MAQUE_PROFILE_START
    uint64_t            profile_stop_frame = 0;             // 0=手动/退出停；否则到帧自动停
    void profile_tick();                                    // 帧首：env 启动 / 到点自动停 / 指标
    bool profile_start(const maque::profile::Options& opts, std::string* error);
    bool profile_stop(bool report, std::string* error, int timeout_ms = 5000);
    void profile_shutdown();                                // teardown 兜底 stop（写报告）
    void profile_toggle();                                  // F5：start/stop 切换
    std::string profile_context_json() const;               // meta.json 的 context
    int                 hover = -1;                         // 悬停实体（上帧命中）
    int                 capture = -1;                       // 指针捕获（拖拽中）
    float               mouse_x = 0, mouse_y = 0;
    const mq_input_event* ev = nullptr;                     // 正在分发的语义事件（脚本可读）
    struct Ball { int id; float vx, vy, radius; mq_physics_body body{}; };
    /* 动画（docs/15 §2）：反射驱动关键帧采样 → 写回目标组件字段 */
    /* 粒子（docs/15 §4）：CPU、确定性（引擎 RNG）、池化（docs/07 §8） */
    std::vector<Ball> balls;                                // 与物理世界对应的实体集合（按 id 升序）
    mq_physics_world* physics = nullptr;                    // 确定性 2D 求解器（docs/15 §3）
    mq_nav_world*     nav = nullptr;                        // 确定性 2D 网格寻路（docs/15 导航）
    /* 每个 NavAgent 的路径缓存（按实体 id；path 为 [x,y] 交错世界坐标） */
    /* 节点事件表（entity → [(event, handler, owner_instance, script_slot)]；注册序即派发序，确定性）。
     * owner_instance：注册该处理器的脚本所属实例（0=根，-2=常驻覆盖层）；派发只调它自己的 bus。
     * script_slot：注册时该 handler 函数所在 VM 在所属 bus 里的槽位（-1=无法判定）。
     * 热重载某个脚本槽位前必须按 (owner, slot) 摘掉旧注册 —— 否则 on_start 每次重载都
     * 再 add 一份：同一事件被调用 N 次，且处理器字符串随重载次数无界增长（内存泄漏）。 */
    struct NodeHandler {
        std::string event;
        std::string handler;
        int instance = 0;                    // 场景实例（旧路径/诊断）
        int slot = -1;
        mq_script_instance* script_inst = nullptr;   // docs/34：注册它的脚本实例（新路径）
    };
    std::unordered_map<int, std::vector<NodeHandler>> node_events;
    int  script_slot_for_handler(const char* handler) const;
    void purge_node_handlers(int instance, int slot, mq_script_vm* fresh, mq_script_bus* bus,
                             bool single_vm);
    bool director_paused = false;                  // director.pause()：跳过模拟，仅保留采样/渲染
    uint64_t script_frames = 0;                    // 脚本 update 实际执行次数（对照暂停）
    /* Tween（补间）：确定性固定步长推进；写入反射字段路径（与动画同一机制） */
    SystemsRuntime systems;
    std::string pending_scene;                     // director.loadScene()：帧边界切场景
    std::vector<std::pair<int, mq_physics_body>> physics_role_bodies;   // (entity, 世代句柄)
    /* 碰撞查询（docs/15 §3、docs/19）：把求解器本帧的接触对映射回实体 id，供脚本判玩法。
     * 内容是"帧内派生缓存"，**不进快照/校验和**（真相仍是物理体状态）。
     * 惰性刷新：物理 step 后首次查询时重建，之后复用；下一帧 step 置脏。 */
    std::vector<std::pair<int, int>> contact_entities;   // 与求解器 contacts 同序（a_index 升序）
    bool contacts_dirty = false;
    void refresh_contacts();                             // 重建 contact_entities（幂等）
    /* 输入模拟器（docs/19）：.sim.json 场景 → 帧索引输入事件流（确定性；与录制/回放一致） */
    /* 画布（分辨率策略，docs/17）：把设计分辨率适配到任意窗口/显示器 */
    mq_canvas_metrics canvas{};           // 当前解析结果（设计 → 设备像素）
    float canvas_safe[4] = { 0, 0, 0, 0 }; // 安全区内缩（设备像素；平台层可设置）
    int canvas_entity = -1;               // 场景里的 ui.Canvas（-1 = 无 → 用场景尺寸兜底）
    bool canvas_active = false;
    bool canvas_use_dpi = true;             // true: Canvas window 空间=drawable；false: 逻辑窗口像素
    bool canvas_design_warned = false;       // Canvas.design_* 与 Scene 设计尺寸冲突只报一次
    void canvas_setup();                  // 装配期：定位画布实体
    void storage_setup();                 // 装配期：建键值存储（默认 <场景目录>/.maque；MAQUE_STORAGE/UID/SESSION 覆盖）
    void canvas_resolve();                // 每帧/尺寸变化：重解析 + 写回 transient 字段
    /* ---- 窗口尺寸 / 渲染目标重建 ----
     * drawable 决定默认颜色附件尺寸；Canvas 只负责设计坐标到目标 viewport 的映射。 */
    void resolve_render_scale();          // Scene/Canvas/MAQUE_RENDER_SCALE → 相对屏幕输出倍率
    void rebuild_render_target();         // drawable × render_scale → TW/TH
    void update_main_views();             // Canvas 映射 → 2D viewport/scissor/zoom
    void sync_window_size(int32_t log_w, int32_t log_h);   // 窗口逻辑尺寸变化 → 重建渲染目标
    void window_event_hooks();            // CI/演示：MAQUE_WINDOW_EVENT_AT（真实 SDL 窗口路径）
    mq_sim* sim = nullptr;
    bool sim_active = false;
    std::string sim_path;                   // MAQUE_SIM 载入的场景文件（自省用）
    void sim_setup();                       // 装配期：MAQUE_SIM / MAQUE_SIM_CLICK → 载入场景
    void sim_feed(uint64_t frame);           // 每帧：把到期事件注入输入流（窗口坐标→世界）
    /* ---- Spine 2D（docs/27、组件 anim.SpineSkeleton）----
     * 分层：spine2d 只产出 CPU 侧绘制数据（顶点/UV/颜色/索引 + 页号），
     * 纹理由本引擎按页名去 VFS 读 → image 解码 → gfx 上传，再回填句柄。
     * 实例是**派生数据**（不进快照/校验和，docs/10）：唯一真相是组件字段 + 骨架资源。 */
    SpineRuntime spine_rt;         // 阶段 2：Spine 状态从 Engine 抽成子对象

    void spine_setup();                        // 装配期：按 role 找组件 → 建骨架数据/实例
    void spine_release();                      // 释放全部 spine 实例（切场景/退出；幂等）
    void spine_step(float dt);                 // 帧内：推进 + 出绘制数据 + 变换到屏幕像素
    void spine_extract(std::vector<mq_renderer_mesh>& out);   // 提取期：绘制数据 → 网格图元
    SpineRuntime::Instance* spine_of(int entity);               // 按实体 id 找实例（脚本 API 用）
    void spine_clear_events();                 // 帧边界（脚本之后、step 之前）：清事件记录

    /* ---- ImGui 调试面板（引擎侧；docs/27 §ImGui）----
     * 定位：**开发工具**，不是游戏 UI。游戏内 HUD 用声明式 ui.*（进场景/快照/可回放）；
     * 面板是视图层：**不进快照、不进校验和、不参与模拟**（docs/10 确定性不受影响）。
     * 绘制时机：脚本 update 之前 begin、**引擎所有图层（含 ui pass）之后** extract →
     * 网格以 layer=MQ_IMGUI_LAYER 提交，天然在最上层；且写进同一像素缓冲 → 截图可见。 */
    bool imgui_on = false;                     // 是否启用（MAQUE_IMGUI=1 / 脚本 imgui_set_enabled）
    bool imgui_stats = false;                  // F2 stats；F4 只开选项面板时不显示
    bool imgui_inited = false;
    int imgui_tex = -1;                        // 字体图集纹理（gfx 纹理 id；-1=未上传）
    uint32_t imgui_tex_gen = 0;                // 图集代数（变化 → 重传）
    uint64_t imgui_frames = 0;                 // 已渲染帧数（自省）
    int imgui_last_cmds = 0;                   // 上一帧导出的命令数（0 = 面板没画东西）
    std::vector<mq_renderer_mesh> imgui_meshes;
    /* 几何缓冲：每帧重建。命令里的指针指向这里 → 必须**先算总量再一次分配**，
     * 不能边 push_back 边取地址（扩容会让先前指针悬空）。 */
    std::vector<float> imgui_pos, imgui_uv, imgui_col;
    std::vector<uint32_t> imgui_idx;
    std::vector<mq_imgui_cmd> imgui_cmds;      // C ABI 导出的命令（拷贝一份，避免跨模块生命周期依赖）
    /* 脚本侧小状态 */
    std::unordered_map<std::string, std::string> imgui_text;    // input_text 的按键缓冲
    std::unordered_map<std::string, std::vector<float>> imgui_plots;   // plot_value 的环形历史
    std::vector<std::string> imgui_id_stack;                    // push_id 栈
    bool imgui_demo = false;                                   // show_demo 状态
    void imgui_setup();                        // 惰性初始化（首帧/首次脚本调用）
    /* 开关面板（脚本 `imgui_set_enabled` 与**引擎级 F2 热键**共用同一条实现）。
     * F4 传 show_stats=false：复用 ImGui 上下文，但不打开 F2 的 stats。 */
    void imgui_enable(bool on, bool show_stats = true);
    void imgui_toggle() { imgui_enable(!imgui_on); }
    /* 本帧输入转发：**窗口像素**原始事件（iframe 是 step_frame 的局部量 → 传参进来）。
     * 只记绝对位置/按键状态（ImGui 的 IO 需要持续状态，不是增量）。 */
    void imgui_input_feed(const mq_platform_pointer_event* ptrs, uint32_t np,
                          const mq_platform_key_event* keys, uint32_t nk);
    void imgui_frame_begin(float dt);          // 开始一帧（脚本 update 之前）
    void imgui_extract(std::vector<mq_renderer_mesh>& out);   // 结束帧 + 命令 → 网格图元
    float imgui_ptr_x = 0, imgui_ptr_y = 0;    // 最近指针位置（窗口**逻辑**像素）
    bool  imgui_ptr_down = false;              // 左键是否按住
    /* ImGui 的**显示空间**（= `mq_imgui_begin_frame` 的 show_w/show_h，也是导出顶点所在空间）：
     *   · 官方 SDL_Renderer 后端：绘制区**设备像素**（renderer 后台缓冲）；
     *   · 网格链（WGPU 直呈 / 无窗口 / present off）：**渲染目标像素** TW×TH
     *     —— gfx 对 MQ_IMGUI_LAYER 用恒等视图，顶点逐像素落在目标上。
     * 指针事件是窗口**逻辑**像素，与显示空间只差一个等比倍率（显示/逻辑）；
     * 无窗口时指针本来就是目标像素 → 1:1。 */
    void  imgui_show_size(float* w, float* h) const;
    float imgui_show_scale() const;
    bool  imgui_env_checked = false;           // 是否已按 MAQUE_IMGUI 决定过初始开关
    bool  imgui_frame_active = false;          // 本帧已 begin（保证 end 一定配对）
    bool  imgui_f2_held = false;               // F2 是否按住（热键沿检测：按住只切一次）
    /* ImGui 面板的**固定 UI 度量**：`imgui_resolve_metrics()` 一次算定
     *（屏幕逻辑分辨率 + DPI + 渲染倍率），**不随游戏窗口/渲染目标变化** ——
     * 同一块屏上 stats 字号/面板大小恒定；换显示器/换缩放才变。 */
    float imgui_ui_scale = 1.0F;               // 屏幕分辨率派生的 UI 缩放
    float imgui_pixel_scale = 1.0F;            // 实际像素倍率（字号 × 样式尺寸）
    float imgui_content_scale = 1.0F;          // 窗口 绘制区/逻辑区 比（HiDPI）
    int   imgui_screen_w = 0, imgui_screen_h = 0;
    float imgui_title_size = 0.0F;             // 标题字号（逻辑像素，未乘 pixel_scale）
    float imgui_body_size = 0.0F;              // 正文字号（逻辑像素，未乘 pixel_scale）
    float imgui_panel_w = 0, imgui_panel_h = 0;  // stats 面板固定尺寸（显示空间单位）
    void imgui_resolve_metrics();              // 屏幕分辨率 → 固定 UI 度量（一次）
    /* CI/无真鼠标环境验证「拖动 stats」：把合成指针事件压进**本帧设备事件队列**
     *（与真实鼠标同一条路径：iframe → imgui_input_feed → ImGui）。
     * 格式 MAQUE_IMGUI_POINTER="frame:x:y:type[;...]"，type 0=move 1=down 2=up。 */
    struct ImguiFakePointer { int frame; float x, y; int type; };
    std::vector<ImguiFakePointer> imgui_fake_ptr;
    bool imgui_fake_parsed = false;
    void imgui_fake_input(mq_platform_pointer_event* ptrs, uint32_t& count, uint32_t cap);
    /* stats 面板用的计量（显示上一帧的值：开帧时就要有数）。 */
    float stat_logic_ms = 0, stat_render_ms = 0, stat_present_ms = 0, stat_fps = 0;
    int   stat_calls = 0, stat_ui_calls = 0, stat_3d_calls = 0;
    int   stat_tris = 0, stat_verts = 0;
    uint64_t stat_vram_bytes = 0;
    /* 静态帧跳过：视觉输入（draws/views/3D 帧数据）不变时不再提交 GPU。
     * 只看"能让画面变化"的数据；脚本状态/物理等非视觉逻辑仍每帧推进。 */
    uint64_t idle_render_hash = 0;
    uint32_t idle_render_frames = 0;
    /* 场景组件/派生世界矩阵的视觉哈希：用于在 RenderTexture（默认 EveryFrame）场景里
     * 判断“整个场景没有变化”，从而跳过 RT pass 与主 pass（docs/bugs/009）。 */
    uint64_t scene_visual_hash_value = 0;
    bool     scene_visual_unchanged = false;
    uint64_t compute_scene_visual_hash() const;
    bool  stat_window_sized = false;
    bool  imgui_backend = false;   // 用官方 SDL_Renderer 后端（与游戏帧共用 renderer、单次 present）
    bool  direct_present = false;  // plans/18 M1：wgpu swapchain 直接呈现（attach_window 成功；不走 CPU readback→SDL）
    bool  direct_requested = false;
    void* direct_window_handle = nullptr;
    std::chrono::steady_clock::time_point stat_t0{};   // 本帧起点（算逻辑/整帧耗时）
    int spine_event_depth = 0;                 // 事件推送重入深度（护栏，见 spine_event_cb）
    /* 文本渲染（docs/13/17）：字体实例 + 每 (字体, 字号) 的图集纹理。
     * 图集**按需增长**（任意 Unicode/CJK）→ 用 generation 判断是否需要重传纹理。 */
    mq_font* font = nullptr;
    std::string font_path;
    struct FontEntry { std::string path; mq_font* font; };
    std::vector<FontEntry> fonts;           // 字体缓存（默认字体在 fonts[0]）
    struct FontAtlas { const mq_font* font; float size; int texture; uint32_t w, h, generation; };
    std::vector<FontAtlas> font_atlases;
    /* 文本两阶段（docs/13 §字形管线）：
     *   阶段 1（排版）：逐标签 measure+layout（**预热图集**），把字形收集为"待发图元"（无纹理/UV）
     *   阶段 2（收尾）：全部标签排版完成后，按 (字体,字号) 刷新一次图集纹理，再按**最终图集尺寸**
     *                   计算 UV 并提交图元 —— 否则先排的标签会用过期 UV 采样（错位/重叠）。
     *   同时避免"帧内重建纹理 + 图元仍引用旧 id"的悬空问题。 */
    struct PendingQuad {
        const mq_font* font; float size;            // 图集键
        int ax, ay, tw, th;                         // 图集内像素原点 + 字形位图尺寸（纹素）
        uint32_t cp;                                // 码点（跟踪用）
        float x, y, half_w, half_h;                 // 设计空间位置与半尺寸
        float r, g, b, a; int z;
        float rotation; int snap;                   // 节点旋转（屏幕空间弧度）与像素吸附
    };
    std::vector<PendingQuad> pending_quads;
    void text_flush(std::vector<mq_renderer_item>& out);        // 阶段 2
    /* 退役纹理：图集增长后替换下来的旧纹理**延迟到下一帧**释放 ——
     * 同一帧里先前已排队的字形图元仍引用旧 id（帧内释放会导致悬空/崩溃）。 */
    std::vector<int> retired_textures;
    mq_font* text_font_for(const std::string& path);   // 按标签的 font 字段选字体（缓存）
    int glyph_texture(const mq_font* f, float size);   // 取（必要时创建/刷新）字形图集纹理
    void text_setup();                      // 装配期：加载字体（MAQUE_FONT → 项目字体 → 系统 → 内置）
    uint64_t stop_frame = 0;                                // >0 时到该帧停止（CI 固定长度跑）
    /* `MAQUE_MAX_FRAMES` / `--frames` 到达上限属于**正常结束**（调用方要求的帧数跑完了），
     * 不能当成失败返回 1 —— 否则 `maque_player --frames 5 scene.scn` 在脚本/CI 里
     * 看起来像出错。单独记一个标志，退出码口径：quit || frames_capped → 0。 */
    bool frames_capped = false;
    std::string frames_dir;                                 // 每帧 PPM 输出目录（""=不输出）
    std::string name;
    const mq_type_desc* scene_type = nullptr;
    /* 组件存在性标志（装配/动态挂载时置位，切场景清零）：
     * 场景里没有 UI/音频/动画组件时，跳过对应的每帧全树遍历（ui_step/audio_step/anim_step）。
     * **只置位不清零**：多置位只多走一趟（正确性不变），漏置位才会漏更新 —— 所以三个写入点
     * （loader / 快照 decode / attach_component）必须都调 note_component()。 */
    bool has_ui_components = false;
    bool has_anim_components = false;
    bool has_audio_components = false;
    void note_component(const mq_type_desc* d);
    Scene               scene_store;                        // 场景状态（移动后失效）
    std::unique_ptr<Scene> scene_owned;                     // 宿主模式：堆上场景
    Scene*              scene = &scene_store;

    /* ---- 场景实例（docs/32）----
     * instances[id] = 实例；id 单调、永不复用（陈旧句柄可判定）；根场景 = 0。
     * **卸载即从表里移除**（不留墓碑）：历史实例不占内存、不占 instance_limit 预算。
     * 用有序 map（而不是 id=下标的 vector）：遍历顺序 = id 顺序 = 创建顺序，确定性不变；
     * 同时 id 可以继续单调增长，不必为了下标连续而保留已销毁的槽位。
     * entity_instance：实体 → 所属实例（运行期归属，不序列化；save/snapshot 用它过滤）。
     * active_instance / in_overlay：脚本上下文（派发期间设置、嵌套调用保存恢复）。 */
    std::map<int, std::unique_ptr<SceneInstanceRuntime>> instances;
    int next_instance_id = 1;                        // 根实例固定 0；其余从 1 起单调递增
    std::unordered_map<int, int> entity_instance;
    std::vector<std::string> scene_chain;            // 正在装配的 .scn 链（循环实例化检测）
    SceneInstanceRuntime* active_instance = nullptr; // 当前脚本所属实例（回调内有效）
    bool in_overlay = false;                         // 当前脚本是常驻覆盖层（node_on 归属 -2）
    bool in_global = false;                          // 当前脚本是全局脚本（node_on 归属 -3）
    int building_instance = 0;                       // 装配中的实例（load_entity 打归属）
    std::string building_base;                       // 装配中的资源基准
    struct InstanceOp {
        int kind = 0;                                // 0=instantiate 1=unload 2=reload
        int instance = -1;
        int parent = -1;                             // 挂载壳实体（-1 = 世界根）
        std::string source;                          // VFS 路径
    };
    std::vector<InstanceOp> instance_ops;            // 动态请求（帧边界 FIFO）
    std::string scene_last_error;                    // 最近一次实例化/卸载错误
    std::unordered_set<std::string> instance_warned; // 嵌套场景忽略世界级字段的警告去重
    int  instance_limit = 256;                       // 实例数预算（MAQUE_SCENE_INSTANCE_LIMIT 可调）
    /* 实例/事件辅助（实现见下方） */
    SceneInstanceRuntime* instance_at(int id);
    SceneInstanceRuntime* instance_of_entity(int entity);
    SceneInstanceRuntime* instance_by_attach(int entity);
    int   create_instance(int parent, int attach, const std::string& vfs_source, int forced_id = -1);
    bool  entity_in_instance_subtree(int instance, int entity) const;
    void  reset_instances();
    bool  build_instance(SceneInstanceRuntime& inst, int depth);
    bool  load_instance_scripts(SceneInstanceRuntime& inst, std::vector<std::string> rels, bool is_root);
    /* docs/34 P0：包清单 → 单 VM + 模块 + 每挂载点实例。 */
    bool  detect_package();
    bool  ensure_module_vm();
    bool  load_module_scripts(SceneInstanceRuntime& inst, const std::vector<std::string>& rels, bool is_root);
    int   call_module_event(mq_script_instance* instance, const char* name,
                            double a, double b, double c, const char* text);
    void  destroy_module_instances(SceneInstanceRuntime& inst);
    void  shutdown_module_vm();
    void  collect_instance_preorder(int id, std::vector<int>& out, bool skip_paused = false) const;
    void  collect_instance_postorder(int id, std::vector<int>& out) const;
    int   call_instance_event(SceneInstanceRuntime& inst, const char* name, double a, double b, double c, const char* text);
    int   call_overlay_event(const char* name, double a, double b, double c, const char* text);
    int   call_global_event(const char* name, double a, double b, double c, const char* text);
    int   dispatch_instance_on_start(int id);
    int   dispatch_instance_on_destroy(int id);
    void  gc_all_instance_scripts();
    uint32_t instance_script_errors() const;
    void  apply_instance_ops();
    void  destroy_instance_content(SceneInstanceRuntime& inst, bool fire_destroy);
    /* 移除 state==2 的记录（卸载/热重载后调用）。keep_id 用于"原地重建"：保留被重建的那条记录。 */
    void  prune_destroyed_instances(int keep_id = -1);
    bool  resolve_instance_path(const std::string& rel, const std::string& base, std::string& out_vfs) const;
    bool  reload_instance_script(int instance_id, const std::string& rel);
    void  rebase_instance_assets(int instance_id);
    void  ensure_instance_component(int shell, int instance_id);
    std::string serialized_instance_source(const SceneInstanceRuntime& inst);
    /* 快照扩展（docs/32 §6.3）：实例表 + 实体归属；旧快照无此块时保持兼容。
     * 返回 0=无实例表（旧快照）/1=重建实例 VM/2=结构相同→保留 VM 状态。 */
    void  encode_instances(SnapshotWriter& w) const;
    int   restore_instances(const uint8_t* data, size_t n, size_t& off);
    uint64_t instance_checksum() const;

    std::unordered_map<std::string, int> tex_cache;              // 装配期填充；提取期只读
    /* env PMREM 产物缓存：路径 → {specular(预过滤 mip 链), irradiance}；-1 = 加载失败 */
    std::unordered_map<std::string, std::pair<int32_t, int32_t>> env_tex_cache;
    /* 立方体 env（第 26 轮）：6 面基名 → {GGX cube, irradiance cube}；-1 = 加载失败 */
    std::unordered_map<std::string, std::pair<int32_t, int32_t>> env_cube_cache;
    std::unordered_map<std::string, scn::SpriteMaterial> material_cache;
    /* 音频（docs/30）：默认 **null 后端**（逻辑/脚本可用、CI 静音）；MAQUE_AUDIO=sdl|auto 才开设备。
     * 后端实现可替换（modules/world/audio/src/backend_*.cpp），runtime 只认 C ABI。 */
    mq_audio* audio = nullptr;
    std::unordered_map<std::string, mq_audio_clip*> audio_clips;   // 解码缓存（切场景清空）
    float last_listener_master = 1.0F;                             // 组件 master 只在变化时下发（避免覆盖脚本 audio_master_volume）
    void audio_setup();
    void audio_teardown();
    mq_audio_clip* audio_clip(const std::string& path);
    void audio_capture_start(const char* path);
    void audio_capture_step(float dt);
    void audio_capture_finish();
    /* 离线抓音（docs/30 §8）：MAQUE_AUDIO_CAPTURE=<wav> → 强制 null 后端，每模拟步
     * mq_audio_mix(dt*rate) 落盘。确定性、与设备/墙钟无关，CI 用它断言"确实有声音"。 */
    std::FILE* audio_cap = nullptr;
    std::string audio_cap_path;
    uint64_t audio_cap_frames = 0;
    uint32_t audio_cap_rate = 48000;
    uint32_t audio_cap_channels = 2;
    double audio_cap_frac = 0.0;
    std::vector<float> audio_cap_buf;
    uint32_t rng = 0;
    bool     quit = false;
    float    bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    int W = 0, H = 0, nviews = 1, TW = 0, TH = 0;
    /* 主输出倍率与设计坐标映射是两件事：
     *   render_scale_req = 渲染目标像素 / 屏幕 drawable 像素（默认 1）；
     *   ss = 渲染目标像素 / 设计坐标单位（由 Canvas 映射再乘输出倍率，仅用于 2D/文字栅格化）。 */
    float render_scale_req = 1.0F;
    float ss = 1.0F;
    /* 启动期是否存在可见输出；无窗口/截图以设计分辨率作为虚拟屏幕基准。 */
    bool  window_intent = false;
    int   win_last_w = 0, win_last_h = 0;         // 上次采样的窗口逻辑尺寸（0 = 尚未采样）
    int   drawable_last_w = 0, drawable_last_h = 0; // 上次 drawable 设备像素尺寸
    int win_w = 0, win_h = 0;                     // 窗口逻辑尺寸（MAQUE_WINDOW 可覆盖）
    std::vector<mq_renderer_view> views;
    /* 相机节点 RenderTexture：GPU 句柄仍在 renderer 的公共纹理命名空间；uri 进入 tex_cache，
     * 因而 Sprite/网格/PBR 不需要第二套纹理类型。状态是派生资源，不进快照/校验和。 */
    struct RenderTextureState {
        int entity = -1;
        int32_t handle = -1;
        int32_t width = 0, height = 0;
        int32_t filter = 1;
        int32_t update_mode = 0;
        int32_t msaa_samples = 0;   // 组件 render.RenderTexture.msaa_samples（0=跟随场景）
        std::string name;
        std::string uri;
        bool is_3d = false;
        bool include_ui = false;
        bool enabled = true;
        bool dirty = true;
        bool ready = false;
        uint64_t last_frame = 0;
        std::vector<uint8_t> capture_pixels;   /* Capture.camera 分块期间复用，避免每块重复 GPU readback */
        uint64_t capture_frame = UINT64_MAX;
    };
    struct CameraCaptureRequest { int entity = -1; std::string path; };
    std::vector<RenderTextureState> render_textures;
    std::vector<CameraCaptureRequest> camera_captures;
    RenderTextureState* render_texture_for(int entity);
    const RenderTextureState* render_texture_for(int entity) const;
    void setup_render_textures();
    void sync_render_textures();
    void destroy_render_textures();
    void render_camera_textures(const std::vector<mq_renderer_item>& draws,
                                const std::vector<mq_renderer_mesh>& meshes);
    bool read_camera_texture(RenderTextureState& rt, bool refresh);
    bool save_camera_texture(int entity, const std::string& path);
    bool make_camera_view2d(int entity, int width, int height, mq_renderer_view& view,
                            uint32_t* layer_mask = nullptr);
    void fill_frame_environment(mq_renderer_frame3d& fr);
    /* M2 3D（plans/18-pbr-3d.md）：场景含 render.* 3D 组件 → 走 mq_renderer_frame3d；
     * cube_mesh = 内建立方体网格句柄（M2 尚无 mesh 资产管线，M4 换成 glTF mesh 资产）。 */
    bool    has_3d = false;
    int32_t cube_mesh = -1;
    /* M4c：.mesh.json 资产缓存（路径 → 渲染器网格句柄；-1 = 加载失败，避免每帧重试） */
    std::unordered_map<std::string, int32_t> mesh_cache;
    struct MeshStats { uint32_t vertex_count = 0, index_count = 0; };
    std::unordered_map<int32_t, MeshStats> mesh_stats;   // 网格句柄 → stats 几何计数（含内建 cube / glTF）
    /* ---- 运行时 glTF/GLB 直接加载（plans/23）：mesh_path 指向 .gltf/.glb 时走这里 ----
     * 装配期解析一次并上传（网格/纹理句柄由本 Engine 持有，切场景统一销毁）。
     * plans/24：蒙皮/动画/morph/材质扩展/灯光相机全部在装配期解析，模拟帧内只读缓存。 */
    struct GltfGpuModel {
        bool failed = false;
        struct Node {
            int parent = -1;
            float t[3] = { 0.0F, 0.0F, 0.0F };
            float r[4] = { 0.0F, 0.0F, 0.0F, 1.0F };
            float s[3] = { 1.0F, 1.0F, 1.0F };
            bool has_matrix = false;
            float m[16] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                            0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
        };
        struct Skin {
            std::vector<int> joints;
            std::vector<float> inverse_bind;
        };
        struct AnimChannel {
            int node = -1;
            int path = 0;        /* 0=translation 1=rotation 2=scale 3=weights 4=pointer */
            int interp = 1;      /* 0=STEP 1=LINEAR 2=CUBICSPLINE */
            int comps = 0;
            int pointer_material = -1;
            int pointer_field = 0;
            std::vector<float> times;
            std::vector<float> values;
        };
        struct Anim {
            std::string name;
            float duration = 0.0F;
            std::vector<AnimChannel> channels;
        };
        struct Light {
            int node = -1;
            int type = 0;
            float color[3] = { 1.0F, 1.0F, 1.0F };
            float intensity = 1.0F;
            float range = 0.0F;
            float inner_cos = 1.0F;
            float outer_cos = 0.0F;
        };
        struct Camera {
            int node = -1;
            bool perspective = true;
            float yfov = 0.6F;
            float znear = 0.1F;
            float zfar = 1000.0F;
        };
        struct MorphTarget {
            std::vector<float> positions;
            std::vector<float> normals;
        };
        struct Prim {
            int32_t mesh = -1;
            int32_t material = -1;
            int32_t node = -1;
            int32_t skin = -1;
            int layout = 0;               /* 0/1 (无蒙皮) 或 2/3 (蒙皮) */
            uint32_t vertex_count = 0;
            uint32_t stride = 0;
            std::vector<uint8_t> base;    /* 打包后的基础顶点（morph 重算起点；无 morph 时为空） */
            std::vector<MorphTarget> morph_targets;
            std::vector<float> default_weights;
            std::vector<float> applied_weights;
            std::vector<int32_t> variant_materials; /* KHR_materials_variants 变体 → 材质 */
            int32_t morph_owner = -1;     /* 共享网格：首个驱动 morph 的实体 */
            bool warned_shared = false;
            bool warned_update = false;
        };
        std::vector<Prim> prims;
        std::vector<mq_renderer_pbr_material> mats;
        std::vector<int32_t> textures;              /* 本模型上传的纹理（clear 时销毁） */
        std::vector<Node> nodes;
        std::vector<int> order;                     /* 父先子 */
        std::vector<Skin> skins;
        std::vector<Anim> anims;
        std::vector<Light> lights;
        std::vector<Camera> cameras;
        std::vector<std::string> variants;
        float radius = 1.0F;                        /* 模型包围球半径（灯光观感归一化用） */
    };
    /* 每个实体的 glTF 动画状态（按实体，而不是按模型：同模型多实例可各自播放）。 */
    struct GltfEntityState {
        std::string path;
        int clip = 0;
        float time = 0.0F;
        float duration = 0.0F;
        bool inited = false;
        bool warned_clip = false;
        std::vector<float> node_local;              /* 10 / 节点：t3 r4 s3 */
        std::vector<uint8_t> node_animated;         /* 该节点本帧是否被动画覆盖 */
        std::vector<float> node_world;              /* 16 / 节点 */
        std::vector<std::vector<float>> node_weights; /* weights 通道：每个节点的 morph 权重 */
        std::vector<std::vector<float>> palettes;   /* 每个 skin 一份 palette */
        std::vector<std::vector<float>> morph_weights; /* 每个 primitive 的当前权重 */
        bool logged = false;
    };
    std::unordered_map<std::string, GltfGpuModel> gltf_models;
    std::unordered_map<int, GltfEntityState> gltf_states;
    /* 运行期 glTF 是否含逐帧变化（动画/蒙皮/morph）；静态 glTF 不阻塞 idle 跳帧。 */
    bool gltf_dynamic = false;
    GltfGpuModel* load_gltf_model(const std::string& path);
    bool read_vfs_file(const std::string& path, std::vector<uint8_t>& out);
    int32_t upload_texture_rgba(const uint8_t* rgba, int w, int h, bool srgb,
                                int filter, int wrap_u, int wrap_v);
    void gltf_state_ensure(int entity, const std::string& path, const GltfGpuModel& gm);
    void gltf_anim_step(float dt);
    bool apply_gltf_morph(int entity, GltfGpuModel& gm, size_t prim_idx,
                          const std::vector<float>& weights);
    /* ---- glTF 动画 / 蒙皮 / morph（plans/22 §6；全部派生缓存，不进快照/校验和）---- */
    std::unordered_map<std::string, GClip> gclips;             /* .clip.json 路径 → 片段 */
    std::unordered_map<std::string, Skeleton> skeletons;       /* .skel.json 路径 → 骨架 */
    std::unordered_map<std::string, MorphMesh> morph_meshes;   /* mesh_path → morph 数据（CPU 重算用） */
    std::unordered_map<std::string, int32_t> mesh_layout;      /* mesh_path → vertex_layout（-1=失败） */
    std::unordered_map<int, AnimPose> anim_pose;               /* 实体 id → 本帧派生姿态（clip 采样） */
    std::unordered_map<int, std::vector<float>> morph_weight_override;  /* morph mesh 实体 → 动画权重 */
    std::unordered_map<int, std::string> entity_morph_mesh;    /* morph mesh 实体 → mesh_path（装配期建） */
    std::unordered_map<std::string, int> entity_by_name;       /* 唯一实体名 → 最小 id（装配期建） */
    std::vector<std::vector<float>> skin_palettes;             /* 本帧调色板（out_skins.palette 指向这里） */
    std::unordered_set<std::string> gltf_warned;               /* 一次性告警去重（skin/joint 解析失败等） */
    bool anim_enabled = true;             /* MAQUE_ANIM=0 → 跳过片段采样（保持 bind pose，A/B 对照） */
    bool skin_enabled = true;             /* MAQUE_SKIN=0 → 调色板填 identity（bind pose 不变量） */
    bool morph_enabled = true;            /* MAQUE_MORPH=0 → 不应用 morph（基础网格，A/B 对照） */
    bool render3d_info = false;           /* MAQUE_RENDER3D_INFO=1 → 装配/首帧诊断 */
    bool gltf_env_read = false;
    bool gltf_frame_info = false;         /* extract_3d 首帧诊断是否已打印 */
    bool gltf_anim_info = false;          /* anim_step 首帧姿态诊断是否已打印 */
    void read_gltf_env();                 /* 一次性读 MAQUE_ANIM / MAQUE_SKIN / MAQUE_RENDER3D_INFO */
    void setup_gltf_assets();             /* 装配期：名字表 / 骨架 / 片段 / morph 预解析（IO 不在模拟帧内） */
    bool load_skeleton(const std::string& path);   /* .skel.json → skeletons[path]（失败缓存 failed） */
    bool load_gclip(const std::string& path);      /* .clip.json → gclips[path]（失败缓存 failed） */
    void anim_step_gclip(int entity, const mq_type_desc* d, std::vector<uint8_t>& blob, float dt);
    bool apply_morph(const std::string& path, const std::vector<float>& weights);
    /* 场景资源 shader（docs/13）：shader = 场景目录里的 .wgsl（**不是** builtin shader）。
     * sprite=false → render.ShaderPass（全屏背景层）；sprite=true → render.ShaderSprite（2D 精灵）。
     * 每个实体最多一个组件；顺序 = 实体 id 序（确定性）。 */
    struct LoadedShaderPass { int entity = -1; int32_t handle = -1; bool sprite = false; };
    /* 任一 render.ShaderPass / render.ShaderSprite：决定走 frame3d 通路、并视为"画面动态"。 */
    bool has_shader_pass = false;
    std::vector<LoadedShaderPass> loaded_shader_passes;
    /* 场景资源多通道 shader 图（render.ShaderGraph；docs/13 §多通道 shader）：
     * program JSON 声明 buffers（中间缓冲）与 passes（shader + 通道 + 输出目标）。
     * feedback 缓冲有两张纹理：本帧读上一帧那张、写另一张（ping-pong，swap 每模拟帧一次）。 */
    struct ShaderGraphBuffer {
        std::string name;
        int32_t tex[2] = { -1, -1 };
        int32_t write = 0;                 /* 本帧写 tex[write]、读 tex[1-write] */
        int32_t w = 0, h = 0;
    };
    struct ShaderGraphPass {
        std::string name;
        std::string path;
        int32_t shader = -1;
        int32_t target_buffer = -1;        /* -1 = 主目标（背景层） */
        int32_t channels[4] = { -1, -1, -1, -1 };   /* 通道 → buffer 下标；-1 = 未绑定 */
    };
    struct LoadedShaderGraph {
        int entity = -1;
        std::string program_path;
        std::vector<ShaderGraphBuffer> buffers;
        std::vector<ShaderGraphPass> passes;
        bool sprite = false;           /* true = render.ShaderSprite 引用 program JSON（画在节点矩形里） */
        uint64_t start_frame = 0;      /* 装配时的全局帧号：各阶段的 frame 从 0 起算（重载后自反馈缓冲从头初始化） */
    };
    bool has_shader_graph = false;
    std::vector<LoadedShaderGraph> loaded_shader_graphs;
    /* 精灵形态的阶段暂存：所有精灵的阶段按顺序拼在这里，记录里的 passes 指向它的区间。 */
    std::vector<mq_renderer_shader_graph_pass> graph_sprite_stage_scratch;
    uint64_t shader_graph_swap_frame = ~0ull;   /* 每模拟帧只切换一次 ping-pong（一帧可能多次成帧） */
    bool shift_down = false;                    /* shader 图 keys.y：Shift 按住（Shadertoy 风格） */
    void setup_shader_graphs();
    void destroy_shader_graphs();
    void shader_graph_step(float dt);
    void build_shader_graph_stages(const LoadedShaderGraph& g, float time, float frame_value,
                                   float mx, float my, bool pressed, const float keys[4],
                                   float screen_w, float screen_h,
                                   std::vector<mq_renderer_shader_graph_pass>& out);
    void extract_shader_graphs(std::vector<mq_renderer_shader_graph_pass>& out);
    void extract_shader_graph_sprites(std::vector<mq_renderer_shader_graph_sprite>& out);
    void shader_graph_swap_targets();
    /* 3D OrbitControls（鼠标拖拽环绕 + 滚轮缩放；在场景相机之上覆盖；MAQUE_ORBIT=yaw,pitch,dist 供 CI） */
    bool  orbit_enabled = false;
    bool  orbit_dragging = false;
    bool  orbit_inited = false;
    float orbit_yaw = 0.0F, orbit_pitch = 0.3F, orbit_dist = 1.0F;
    float orbit_last_x = 0.0F, orbit_last_y = 0.0F;

    /* ---- 引擎选项（F4 ImGui 面板；docs/28）----
     * 运行期可热调的纯视图选项：
     *   · rendered / textured / solid / wireframe 渲染模式，以及游戏 3D、2D/UI 图层开关
     *   · MSAA（主输出）与 RenderTexture MSAA（相机离屏目标）：各自 0=跟随，1=关，4=4x
     *   · 默认太阳光（场景无光源时补的方向光：开关 / 强度 / 传播方向）
     *   · 主相机 near/far 覆盖
     *   · 后处理 PostEffect（曝光/ACES/Bloom/暗角/色差/色彩分级；默认只叠加 3D 层）
     * 只在 extract_3d / 成帧时生效，**不写回场景组件/不进快照/校验和**（视图层调试覆盖，docs/10）。
     * 同一组值也可用 `MAQUE_ENGINE_OPTIONS` 给启动初值（见 engine_options_parse_env）。 */
    bool  opt_panel = false;                   // F4 面板开关
    bool  opt_f4_held = false;                 // F4 沿检测（按住只切换一次）
    bool  opt_opened_imgui = false;            // F4 自己打开的 ImGui（收起面板时恢复"关"→ 不残留 stats）
    int   opt_msaa = 0;                        // 0=跟随场景(MAQUE_MSAA 次之) 1=关 4=4x
    /* RenderTexture MSAA 全局开关（F4 / MAQUE_ENGINE_OPTIONS 的 rt_msaa）：只管**相机离屏目标**，
     * 不动主输出。0=跟随（上面的 MSAA / MAQUE_MSAA / 各 RT 字段 / 场景），1=全部强制关，
     * 4=全部强制 4x。因为它是更专门的那个开关，对 RT 而言赢过 opt_msaa。 */
    int   opt_rt_msaa = 0;
    /* MAQUE_MSAA 只读一次（"出现过"即生效，即使值为 0/1）；主目标与每张 RenderTexture
     * 共用同一条解析链，见 Engine::resolve_msaa()。 */
    bool  msaa_env_loaded = false;
    bool  msaa_env_set = false;
    int   msaa_env_value = 0;
    /* MSAA 样本数解析（唯一出口）：
     *   主目标（is_render_texture=false）：opt_msaa > MAQUE_MSAA > scn.Scene.msaa_samples
     *   相机离屏目标（is_render_texture=true）：opt_rt_msaa > opt_msaa > MAQUE_MSAA >
     *       render.RenderTexture.msaa_samples > scn.Scene.msaa_samples
     * target_override = 目标自己的字段值（0 = 跟随场景、1 = 强制关、4 = 4x）。
     * 只返回 1/4：gfx 只为这两个 sampleCount 建管线（docs/31 §MSAA）。 */
    int   resolve_msaa(int target_override, bool is_render_texture);
    int   opt_render_mode = 0;                 // 0=rendered 1=solid 2=wireframe 3=textured
    bool  opt_3d_enabled = true;               // 游戏 3D 层（不影响 F4 面板）
    bool  opt_2d_enabled = true;               // 游戏 2D/UI 层（不影响 F4 面板）
    bool  opt_sun_enabled = true;              // 场景无光源时是否补默认太阳光
    float opt_sun_intensity = 1.1F;
    float opt_sun_dir[3] = { 0.35F, -0.85F, -0.4F };   // 光的**传播方向**（shader 内 normalize）
    bool  opt_cam_override = false;            // 覆盖主相机 near/far
    float opt_cam_near = 0.1F, opt_cam_far = 100.0F;   // 未覆盖时跟随场景实际值（面板默认值随之同步）
    /* 后处理 PostEffect（docs/13 §后处理）：纯视图层，默认**关闭**（旧画面逐字节不变）；
     * 默认**只叠加 3D 层**，2D/UI 在 post 结果之上叠加（include_2d=1 才整帧）。
     * 效果由位掩码 + 参数组成；tonemap=0 表示只做曝光，不额外压 ACES。 */
    bool  opt_post_enabled = false;
    int   opt_post_tonemap = 0;                // 0=none 1=ACES
    float opt_post_exposure = 1.0F;
    bool  opt_post_bloom = false;
    float opt_post_bloom_intensity = 0.85F;
    float opt_post_bloom_threshold = 0.62F;
    float opt_post_bloom_radius = 0.018F;      // 相对目标高度的采样半径
    bool  opt_post_vignette = false;
    float opt_post_vignette_intensity = 0.55F;
    bool  opt_post_chromatic = false;
    float opt_post_chromatic_strength = 0.0022F;   // 沿径向的 UV 偏移比例
    bool  opt_post_grade = false;
    float opt_post_saturation = 1.0F;
    float opt_post_contrast = 1.0F;
    float opt_post_grayscale = 0.0F;
    /* 作用范围：false（默认）= 只叠加 3D 层（shader pass + 3D + 天空），2D/UI 原样叠在上面；
     * true = 整帧（含 2D/UI）。纯 2D 场景只有 true 时才有后处理。 */
    bool  opt_post_include_2d = false;
    /* 上一帧实际生效值（面板只读回显） */
    int   opt_last_msaa = 0;
    /* 上一帧相机离屏目标的 MSAA 实况（面板回显）：只统计 3D 且启用的目标，
     * 2D 目标不计 —— 多采样对 2D 图元层无效，列进去只会误导。 */
    int   opt_last_rt_total = 0;
    int   opt_last_rt_msaa4 = 0;
    bool  opt_last_sun = false;
    float opt_last_near = 0.1F, opt_last_far = 100.0F;
    bool  opt_env_parsed = false;
    void engine_options_parse_env();           // 启动初值 MAQUE_ENGINE_OPTIONS（只解析一次）
    void engine_options_panel(float show_w, float show_h);   // F4 面板内容（本帧 ImGui 已 begin）
    void engine_options_fill_post(mq_renderer_post& out) const;  // opt_post_* → frame3d.post
    void engine_options_log_post();            // 变化时打印一行（CI 可断言）

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
    int load_texture(const std::string& path);                 // assets（VFS 编码文件）
    int load_texture_ref(const std::string& ref);              // builtin: 或 assets
    std::string scoped_texture_ref(int entity, const std::string& ref) const;
    /* envmap（PMREM）：.hdr(RGBE) / PNG/JPEG / builtin studio → GGX 粗糙度预过滤 mip 链 +
     * 漫反射 irradiance 贴图；两项都是 RGBA16F，失败时输出 -1（shader 回退 analytic）。 */
    void load_env_textures(const std::string& path, int32_t& spec_id, int32_t& irr_id);
    /* 第 26 轮：6 面 cubemap env（基名 → _px/_nx/... + .hdr/.exr/.png/.jpg） */
    void load_env_cubemap(const std::string& prefix, int32_t& spec_id, int32_t& irr_id);
    bool load_env_image_linear(const std::string& path, int& w, int& h, std::vector<float>& base);
    int32_t load_mesh(const std::string& path);                // M4c：glTF 导入的 .mesh.json → 网格句柄
    int load_texture_srgb(const std::string& path);            // M4g：albedo 贴图按 sRGB 采样（RGBA8_SRGB）
    int load_texture_mips(const std::string& path);            // plans/19：线性数据贴图 + CPU mip 链（抗锯齿）
    bool load_material(const std::string& ref, scn::SpriteMaterial& out);
    void main_loop(const std::string& frames_dir, const std::string& name);
    /* M2 3D：扫描 render.* 组件 + 上传内建 cube；每帧提取 frame3d 数据（只读）。 */
    void setup_3d();
    /* 场景资源 shader（docs/13）：加载 .wgsl → create_shader_pass；每帧推进 time/提取。
     * 全屏 pass（render.ShaderPass）→ 背景层；精灵（render.ShaderSprite）→ 2D 图层矩形。 */
    void setup_shader_passes();
    void destroy_shader_passes();
    void shader_pass_step(float dt);
    void extract_shader_passes(std::vector<mq_renderer_shader_pass>& out);
    void extract_shader_sprites(std::vector<mq_renderer_shader_sprite>& out);
    void extract_3d(int32_t tw, int32_t th,
                    std::vector<mq_renderer_view3d>& out_views,
                    std::vector<mq_renderer_light>& out_lights,
                    std::vector<mq_renderer_pbr_material>& out_mats,
                    std::vector<mq_renderer_draw3d>& out_draws,
                    std::vector<mq_renderer_skin>& out_skins,
                    int camera_entity = -1, bool allow_orbit = true);
    bool prepare_replay();                                  // 载入回放日志并设定期望校验和（docs/10 §3）
    bool load_clip(const std::string& ref);                 // 载入动画片段（反射验证轨道路径）
    void nav_step();                                        // 导航代理推进（寻路 + 追踪）
    void ui_step();                                         // UI 组件语义（layout/progress/slider/check/safearea/scroll）
    void extract_text(Engine& eng, int idx, SceneNode& e, const mq_type_desc* d,
                      std::vector<mq_renderer_item>& out, float inherit_alpha);
    void step_tweens(float dt);                             // Tween 推进（补间）
    void step_schedules(float dt);                          // 定时器推进（定时器）
    void flush_pending_destroy();                           // 延迟销毁（帧末生效）
    void physics_components_setup();                        // 装配期：按 role 建物理体（刚体/碰撞体组件）
    void physics_components_step(float dt);                 // 帧内：步进并回写变换
    void anim_step(float dt);                               // 关键帧采样 → 写回字段（docs/15 §2）
    void audio_step(float dt);                              // 音频组件同步（docs/30 §6）
    void particles_step(float dt);                          // 确定性粒子推进（docs/15 §4）
    bool setup(const std::string& scn_path, const char* renderer_override);   // 加载并装配引擎
    void teardown();                                        // 释放全部实例资源（幂等）
         void step_frame(bool render_frame = true);                // 推进一个模拟帧（单一帧序，docs/10 D1）                                      // 推进一个模拟帧（单一帧序，docs/10 D1）
    bool run_frames(uint64_t n);                            // 推进 n 帧（控制面 Runtime.step）
    void preload_scene_assets();                            // 装配期预解析纹理/材质（extract 只读，docs/13 §8）
    void clear_asset_caches();                              // 切场景：释放按**相对路径**缓存的纹理/网格（挂载点已换）
    bool reload_script(std::string* err = nullptr, const std::string* only = nullptr);   // 重载脚本（多脚本：按路径）
    /* 常驻覆盖层脚本：激活/切换（同一路径复用已有 VM，状态保留）、卸载、热重载。 */
    bool ensure_overlay_script(const std::string& rel);
    void unload_overlay();
    bool reload_overlay_script();
    /* 全局脚本（docs/32 §16.1）：与覆盖层同形，但语义是游戏级服务（先于覆盖层派发）。 */
    bool ensure_global_script(const std::string& rel);
    void unload_global();
    bool reload_global_script();
    bool reload_scene();                                    // 重载场景（重建世界，docs/08）
    bool load_scene_into(const std::string& scn_path);       // 场景装配（setup / director.loadScene 共用）
    uint32_t poll_watcher();                                // 帧边界检查资源变化
    void checkpoint_push();                                 // 记录一个检查点（周期性）
    void write_crash_context(const char* why);              // 崩溃/异常上下文（docs/09 §5、docs/10 §7）
    void refresh_crash_buffer();                            // 帧边界预渲染（信号安全落盘）
    void export_final_snapshot();                           // 导出世界快照（MAQUE_SNAPSHOT）
    void finalize_run();                                    // 导出快照 + 录制日志（含 frames/trail 元数据）
    bool world_snapshot(mq_snapshot* snap);                 // 世界 → 快照（反射驱动，docs/10 §4）
    void world_restore(const mq_snapshot* snap);            // 快照 → 世界
    uint64_t world_checksum();                              // 世界校验和：场景 + 物理 + 粒子（docs/10 §5）
    uint64_t global_checksum();                             // 全局状态校验和（空 → 中立值）
    void encode_globals(SnapshotWriter& w) const;           // 快照块：全局状态（docs/32 §16.3）
    int  restore_globals(const uint8_t* data, size_t n, size_t& off);
    uint64_t world_trail() const;                           // 校验和轨迹（失步定位）
};

}  // namespace maque::rt
