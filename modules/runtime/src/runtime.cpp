/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */
#include "runtime/mq_runtime.h"
#include "core/storage/mq_storage.h"   // 键值存储（docs/25）
#include "runtime/mq_host.h"

#include "core/persist/mq_persist.h"
#include "core/persist/mq_persist_binary.h"
#include "core/reflect/mq_reflect.h"
#include "core/script/mq_script.h"
#include "core/handle/mq_handle.h"
#include "core/vfs/mq_vfs.h"
#include "core/image/mq_image.h"
#include "core/observability/mq_obs.h"
#include "core/platform/mq_platform.h"
#include "core/input/mq_input.h"
#include "core/watcher/mq_watcher.h"
#include "physics/mq_physics.h"
#include "nav/mq_nav.h"
#include "text/mq_text.h"
#include "sim/mq_sim.h"
#include "canvas/mq_canvas.h"
#include "anim/mq_spine2d.h"          // Spine 2D 官方运行时（docs/27）：骨架/实例/绘制数据
#include "ui/mq_imgui.h"              // 引擎侧 ImGui 调试面板：帧管理/绘制数据导出
#include "ui/mq_imgui_widgets.h"      // ImGui 控件 C ABI（runtime 不直接依赖 ImGui 头）

#include <fstream>
#include "core/control/mq_control.h"
#include "core/api/mq_api.h"
#include "maque_api.h"                 // 生成：host:: 声明（api_list）
#include "maque_ifaces.h"
#include "maque_control.h"
#include "maque_events.h"                 // 生成：mq_renderer_v1（插件接口 ABI）
#include "core/module/mq_module.h"          // mq_host（dlopen 加载插件）
#include "maque_types.h"

#include <print>
#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <thread>
#include <utility>
#include <functional>
#include <csignal>
#include <algorithm>
#include "core/compat/mq_posix_io.h"   // 崩溃路径的低层 fd I/O（POSIX open/write/pwrite ↔ Windows _open/_write）

namespace {

struct Entity {
    std::string name;
    int parent = -1;
    /* 世界坐标：**每帧由 flatten_world() 从节点树展平写到这里的派生缓存**（不是位置真相！
     * 位置真相 = Transform 组件里的局部坐标；改位置请经 node/entity API 写组件，
     * 直接改 world_x/y 会在下一帧被覆盖）。 */
    float world_x = 0, world_y = 0;
    /* 节点属性（active/scale/angle/opacity）：active/scale/angle/opacity 影响
     * 自身与子树；z_index 参与 UI/绘制稳定排序。全部进快照与校验和（docs/10）。 */
    bool  active = true;
    float scale = 1.0F;
    float angle = 0.0F;                                    // 角度（度）
    float opacity = 1.0F;                                  // 0..1
    int   z_index = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> comps;   // type_id → blob
};

struct Engine;
struct Scene;
struct SnapshotWriter;

/* Spine 图集页纹理回调的上下文：真正的"读文件 + 解码 + 上传"由 Engine 做
 * （只有它知道 VFS 挂载点与渲染器句柄）。生命周期：装配期创建骨架数据时使用，
 * 但 spine 会**保存**这个 user 指针 → 随实例一起活到数据销毁（见 Engine::spine_release）。 */
struct SpineTexCtx {
    Engine* eng = nullptr;
    std::string dir;          // 页纹理所在 VFS 目录（含结尾 '/'）
};

/* 引擎内辅助（定义见 Engine 之后；Scene 与 Engine 互相引用）。 */
void write_json_string_pub(std::string& out, const std::string& s);
/* MAQUE_KEEP_OPEN：忽略脚本 quit()（演示用；见 host::quit） */
bool keep_open = false;
/* 组件语义辅助（定义见 host API 之前）：供输入回调使用。 */
std::vector<uint8_t>* comp_by_role(Engine& eng, int id, uint32_t role);
std::vector<uint8_t>* comp_by_name(Engine& eng, int id, const char* type_name, const mq_type_desc** out_desc);
float read_num(const mq_type_desc* d, const void* base, const char* field, float def);
bool  write_num(const mq_type_desc* d, void* base, const char* field, double v);
void  slider_apply_pointer(Engine& eng, int id, const mq_type_desc* d, void* base, float px);
void  window_to_world(const Engine& eng, float* x, float* y);   // 窗口像素 → 世界坐标（含 y 翻转）
void  device_event_to_world(const Engine& eng, mq_platform_pointer_event* p);  // 位置 + 方向量
/* node_emit_number / node_width 由生成的 maque_api.h 在 ::host 中声明（api_list）。 */
void attach_component(Engine& eng, int idx, const char* type_name);
void construct_blob(Engine& eng, const mq_type_desc* d, std::vector<uint8_t>& blob);
void destroy_blob(Engine& eng, uint32_t type_id, std::vector<uint8_t>& blob);

struct Scene {
    scn::Scene desc;
    std::string text_;
    /* 实体 id 采用**单调递增**（不复用槽位）：docs/02 R4/docs/07 §3 要求陈旧句柄不得
     * 静默指向新对象；不复用即从根上消除"销毁后 id 指到别人"的风险。
     * （世代号在等价意义上内化于 id：同一个 id 永恒指向同一个实体或不存在。） */
    int next_id = 0;
    Engine* eng = nullptr;                  // 组件析构需要反射 ctx（remove/重建时）
    std::unordered_map<int, Entity> ents;

    Scene() = default;
    ~Scene() { destroy_all(); }
    Scene(const Scene&) = delete; Scene& operator=(const Scene&) = delete;
    Scene(Scene&& o) noexcept
        : desc(std::move(o.desc)), text_(std::move(o.text_)), next_id(o.next_id), eng(o.eng),
          ents(std::move(o.ents)) {
        o.eng = nullptr;             // 所有权转移：源对象不得再析构已移交的资源
    }
    Scene& operator=(Scene&& o) noexcept {
        if (this == &o) return *this;
        destroy_all();
        desc = std::move(o.desc);
        text_ = std::move(o.text_);
        next_id = o.next_id;
        eng = o.eng; o.eng = nullptr;
        ents = std::move(o.ents);
        return *this;
    }
    /* 析构全部组件 blob（含动态成员）。 */
    void destroy_all() {
        if (!eng) { ents.clear(); return; }
        for (auto& [k, ent] : ents)
            for (auto& [t, b] : ent.comps) destroy_blob(*eng, t, b);
        ents.clear();
    }

    bool deserialize(Engine& eng, const std::string& text);
    void instantiate(Engine& eng);
    int  spawn();
    Entity* at(int index);
    void remove(int index);
    int  count() const { return (int)ents.size(); }
    /* 每帧从**节点树**展平出世界坐标（写进 Entity::world_x/y）；树是唯一模型，这里是派生缓存。 */
    void flatten_world(Engine& eng);
    uint64_t checksum(Engine& eng);     // 世界状态校验和（确定性；反射驱动）
    void encode(Engine& eng, SnapshotWriter& w);   // 世界 → 字节（二进制 codec）
    size_t decode(Engine& eng, const uint8_t* in, size_t n);
    void extract(Engine& eng, std::vector<mq_renderer_item>& out);
};

/* ---- 快照写入器（Scene::encode 的字节汇；无反射依赖） ---- */
struct SnapshotWriter {
    std::vector<uint8_t> buf;
    void bytes(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        buf.insert(buf.end(), b, b + n);
    }
    template <class T> void pod(T v) { bytes(&v, sizeof(T)); }
};

struct Engine {
    mq_host*            host = nullptr;                   // 模块宿主（加载渲染插件）
    const mq_renderer_v1* rapi = nullptr;                 // 渲染器插件接口
    void*               rself = nullptr;                  // 渲染器实例
    mq_vfs*             vfs = nullptr;
    /* 多脚本/多语言（docs/14 §多 VM）：**事件总线**持有全部 VM 并负责按声明顺序投递。
     * `vm` 仅指第一个 VM（便捷/自省）；任何"调用脚本处理器"的地方都必须走 bus（跨语言广播）。 */
    mq_script_bus*      bus = nullptr;
    mq_script_vm*       vm = nullptr;
    std::vector<std::string> script_paths;   // 已装载脚本（声明顺序；自省/诊断用）
    std::vector<uint64_t> script_hashes;     // 每个脚本的内容哈希（热重载判定"哪个改了"）
    mq_reflect_ctx*     reflect = nullptr;                 // per-instance
    mq_obs_ctx*         obs = nullptr;                     // 可观测（事件/指标）
    mq_control*         control = nullptr;                 // 控制平面（JSON-RPC）
    mq_control_server*  ctl_server = nullptr;              // 控制端点（inspector attach；plans/17 §3）
    std::vector<uint8_t> frame_pixels;                     // 最近一帧 RGBA（Capture.screen 用）
    int frame_w = 0, frame_h = 0;
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
    double              pace_accum = 0.0;                   // 已累计但尚未消费的墙钟秒数
    bool                trace_hits = false;                 // MAQUE_HITS=1：命中轨迹打印（调试）
    int                 hover = -1;                         // 悬停实体（上帧命中）
    int                 capture = -1;                       // 指针捕获（拖拽中）
    float               mouse_x = 0, mouse_y = 0;
    const mq_input_event* ev = nullptr;                     // 正在分发的语义事件（脚本可读）
    struct Ball { int id; float vx, vy, radius; mq_physics_body body{}; };
    /* 动画（docs/15 §2）：反射驱动关键帧采样 → 写回目标组件字段 */
    struct Clip { std::string name; float length = 0; bool loop = true; std::vector<std::string> paths; std::vector<uint8_t> blob; };
    std::vector<Clip> clips;
    int find_clip(const std::string& name) const {
        for (size_t i = 0; i < clips.size(); ++i) if (clips[i].name == name) return (int)i;
        return -1;
    }
    /* 粒子（docs/15 §4）：CPU、确定性（引擎 RNG）、池化（docs/07 §8） */
    struct Particle { int owner = -1; float x = 0, y = 0, vx = 0, vy = 0, life = 0, max_life = 1; float size = 2; float r = 1, g = 1, b = 1; };
    std::vector<Particle> particles;
    std::vector<int> emitter_accum;                        // 每个发射器的发射累加器（按实体 id 排序使用）
    uint32_t max_particles = 2048;
    std::vector<Ball> balls;                                // 与物理世界对应的实体集合（按 id 升序）
    mq_physics_world* physics = nullptr;                    // 确定性 2D 求解器（docs/15 §3）
    mq_nav_world*     nav = nullptr;                        // 确定性 2D 网格寻路（docs/15 导航）
    /* 每个 NavAgent 的路径缓存（按实体 id；path 为 [x,y] 交错世界坐标） */
    struct AgentPath { std::vector<float> path; uint32_t index = 0; };
    std::unordered_map<int, AgentPath> agent_paths;
    /* 节点事件表（entity → [(event, handler)]；注册序即派发序，确定性） */
    struct NodeHandler { std::string event; std::string handler; };
    std::unordered_map<int, std::vector<NodeHandler>> node_events;
    bool director_paused = false;                  // director.pause()：跳过模拟，仅保留采样/渲染
    uint64_t script_frames = 0;                    // 脚本 update 实际执行次数（对照暂停）
    /* Tween（补间）：确定性固定步长推进；写入反射字段路径（与动画同一机制） */
    struct Tween { int entity; std::string path; float from, to, elapsed, duration; int easing; };
    std::vector<Tween> tweens;
    /* 定时器（定时器）：帧驱动，确定性 */
    struct Scheduled { std::string handler; float interval, remaining; bool repeat; };
    std::vector<Scheduled> schedules;
    std::vector<int> pending_destroy;              // node_destroy：帧边界统一销毁
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
    void canvas_setup();                  // 装配期：定位画布实体
    void storage_setup();                 // 装配期：建键值存储（默认 <场景目录>/.maque；MAQUE_STORAGE/UID/SESSION 覆盖）
    void canvas_resolve();                // 每帧/尺寸变化：重解析 + 写回 transient 字段
    mq_sim* sim = nullptr;
    bool sim_active = false;
    std::string sim_path;                   // MAQUE_SIM 载入的场景文件（自省用）
    void sim_setup();                       // 装配期：MAQUE_SIM / MAQUE_SIM_CLICK → 载入场景
    void sim_feed(uint64_t frame);           // 每帧：把到期事件注入输入流（窗口坐标→世界）
    /* ---- Spine 2D（docs/27、组件 anim.SpineSkeleton）----
     * 分层：spine2d 只产出 CPU 侧绘制数据（顶点/UV/颜色/索引 + 页号），
     * 纹理由本引擎按页名去 VFS 读 → image 解码 → gfx 上传，再回填句柄。
     * 实例是**派生数据**（不进快照/校验和，docs/10）：唯一真相是组件字段 + 骨架资源。 */
    struct Spine {
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
    std::vector<Spine> spines;
    void spine_setup();                        // 装配期：按 role 找组件 → 建骨架数据/实例
    void spine_release();                      // 释放全部 spine 实例（切场景/退出；幂等）
    void spine_step(float dt);                 // 帧内：推进 + 出绘制数据 + 变换到屏幕像素
    void spine_extract(std::vector<mq_renderer_mesh>& out);   // 提取期：绘制数据 → 网格图元
    Spine* spine_of(int entity);               // 按实体 id 找实例（脚本 API 用）
    void spine_clear_events();                 // 帧边界（脚本之后、step 之前）：清事件记录

    /* ---- ImGui 调试面板（引擎侧；docs/27 §ImGui）----
     * 定位：**开发工具**，不是游戏 UI。游戏内 HUD 用声明式 ui.*（进场景/快照/可回放）；
     * 面板是视图层：**不进快照、不进校验和、不参与模拟**（docs/10 确定性不受影响）。
     * 绘制时机：脚本 update 之前 begin、**引擎所有图层（含 ui pass）之后** extract →
     * 网格以 layer=MQ_IMGUI_LAYER 提交，天然在最上层；且写进同一像素缓冲 → 截图可见。 */
    bool imgui_on = false;                     // 是否启用（MAQUE_IMGUI=1 / 脚本 imgui_set_enabled）
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
    /* 开关面板（脚本 `imgui_set_enabled` 与**引擎级 F2 热键**共用同一条实现）。 */
    void imgui_enable(bool on);
    void imgui_toggle() { imgui_enable(!imgui_on); }
    /* 本帧输入转发：**窗口像素**原始事件（iframe 是 step_frame 的局部量 → 传参进来）。
     * 只记绝对位置/按键状态（ImGui 的 IO 需要持续状态，不是增量）。 */
    void imgui_input_feed(const mq_platform_pointer_event* ptrs, uint32_t np,
                          const mq_platform_key_event* keys, uint32_t nk);
    void imgui_frame_begin(float dt);          // 开始一帧（脚本 update 之前）
    void imgui_extract(std::vector<mq_renderer_mesh>& out);   // 结束帧 + 命令 → 网格图元
    float imgui_ptr_x = 0, imgui_ptr_y = 0;    // 最近指针位置（窗口像素）
    bool  imgui_ptr_down = false;              // 左键是否按住
    /* 指针的"窗口像素 → 目标/设计像素"换算（ImGui 显示空间 = 目标空间）：
     * 显示 = 目标*scale + offset → 目标 = (显示 - offset) / scale。
     * 无窗口时 scale=1/offset=0，等价于直接用。 */
    float ptr_target_x() const {
        if (!window) return imgui_ptr_x;
        const float s = canvas.scale > 0.0F ? canvas.scale : 1.0F;
        return (imgui_ptr_x - canvas.offset_x) / s;
    }
    float ptr_target_y() const {
        if (!window) return imgui_ptr_y;
        const float s = canvas.scale > 0.0F ? canvas.scale : 1.0F;
        return (imgui_ptr_y - canvas.offset_y) / s;
    }
    bool  imgui_env_checked = false;           // 是否已按 MAQUE_IMGUI 决定过初始开关
    bool  imgui_frame_active = false;          // 本帧已 begin（保证 end 一定配对）
    bool  imgui_f2_held = false;               // F2 是否按住（热键沿检测：按住只切一次）
    /* 默认 stats 面板用的计量（显示上一帧的值：开帧时就要有数）。 */
    float stat_logic_ms = 0, stat_render_ms = 0, stat_fps = 0;
    int   stat_calls = 0, stat_tris = 0;
    bool  stat_window_sized = false;
    bool  imgui_backend = false;   // 用官方 SDL_Renderer 后端直接画到窗口（绕开网格导出链）   // stats 窗口只在首帧定位/定尺寸（之后可自由拖动）
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
    Scene               scene_store;                        // 场景状态（移动后失效）
    std::unique_ptr<Scene> scene_owned;                     // 宿主模式：堆上场景
    Scene*              scene = &scene_store;

    std::unordered_map<std::string, int> tex_cache;              // 装配期填充；提取期只读
    std::unordered_map<std::string, scn::SpriteMaterial> material_cache;
    uint32_t rng = 0;
    bool     quit = false;
    float    bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    int W = 0, H = 0, nviews = 1, TW = 0;
    /* 渲染分辨率倍率（超采样，docs/17 §渲染分辨率）：渲染目标 = 设计分辨率 × ss。
     * ss = 0 → 自动跟随窗口像素（上限 4）；1 → 设计分辨率（像素风）；N → 固定 N 倍。
     * 文字按目标像素栅格化（device_scale = ss）→ ss 越接近"窗口/设计"，文字越平滑（无锯齿）。 */
    float ss = 1.0F;
    int win_w = 0, win_h = 0;                               // 窗口尺寸（MAQUE_WINDOW 可覆盖；世界等比缩放显示）
    std::vector<mq_renderer_view> views;

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
    int load_texture(const std::string& path);                 // assets（VFS 编码文件）
    int load_texture_ref(const std::string& ref);              // builtin: 或 assets
    bool load_material(const std::string& ref, scn::SpriteMaterial& out);
    void main_loop(const std::string& frames_dir, const std::string& name);
    bool prepare_replay();                                  // 载入回放日志并设定期望校验和（docs/10 §3）
    bool load_clip(const std::string& ref);                 // 载入动画片段（反射验证轨道路径）
    void nav_step();                                        // 导航代理推进（寻路 + 追踪）
    void ui_step();                                         // UI 组件语义（layout/progress/slider/check/safearea/scroll）
    void extract_text(Engine& eng, int idx, Entity& e, const mq_type_desc* d,
                      std::vector<mq_renderer_item>& out, float inherit_alpha);
    void step_tweens(float dt);                             // Tween 推进（补间）
    void step_schedules(float dt);                          // 定时器推进（定时器）
    void flush_pending_destroy();                           // 延迟销毁（帧末生效）
    void physics_components_setup();                        // 装配期：按 role 建物理体（刚体/碰撞体组件）
    void physics_components_step(float dt);                 // 帧内：步进并回写变换
    void anim_step(float dt);                               // 关键帧采样 → 写回字段（docs/15 §2）
    void particles_step(float dt);                          // 确定性粒子推进（docs/15 §4）
    bool setup(const std::string& scn_path, const char* renderer_override);   // 加载并装配引擎
    void teardown();                                        // 释放全部实例资源（幂等）
    void step_frame();                                      // 推进一个模拟帧（单一帧序，docs/10 D1）
    bool run_frames(uint64_t n);                            // 推进 n 帧（控制面 Runtime.step）
    void preload_scene_assets();                            // 装配期预解析纹理/材质（extract 只读，docs/13 §8）
    bool reload_script(std::string* err = nullptr, const std::string* only = nullptr);   // 重载脚本（多脚本：按路径）
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
    uint64_t world_trail() const;                           // 校验和轨迹（失步定位）
};

/* 事件 kind → 处理器名 → **总线广播**（多脚本/多语言的唯一事件入口；定义见下方） */
int dispatch_event(Engine& eng, int kind, double a, double b, double c, const char* text);

/* JSON 字符串转义（与 persist 规则一致；runtime 侧轻量实现）。 */
void write_json_string_pub(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    out += '"';
}

/* 组件 blob 生命周期（关键）：组件含 std::string/容器时，裸字节缓冲**不是有效对象**；
 * 必须显式构造（默认构造 = 反射默认值）后再反序列化，覆盖前显式析构。 */
void destroy_blob(Engine& eng, uint32_t type_id, std::vector<uint8_t>& blob) {
    const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, type_id);
    if (!d || blob.size() < d->size) { blob.clear(); return; }
    if (d->flags & MQ_TYPE_DYNAMIC) {
        /* 动态成员按字段类型逐个析构：string 显式析构；vector 通过容器算子清空（不释放容量，
         * 缓冲随即被释放 → 内存由 vector 自身管理）。 */
        for (uint32_t i = 0; i < d->field_count; ++i) {
            const mq_field_desc* f = &d->fields[i];
            if (f->kind == MQ_TK_STRING) {
                reinterpret_cast<std::string*>(blob.data() + f->offset)->~basic_string();
            } else if (f->kind == MQ_TK_ARRAY_DYN && f->container_clear) {
                f->container_clear(blob.data() + f->offset);
            }
        }
    }
    blob.clear();
}

/* 默认构造组件 blob（走"空 JSON"反序列化，schema 默认值生效）。 */
void construct_blob(Engine& eng, const mq_type_desc* d, std::vector<uint8_t>& blob) {
    (void)eng;
    blob.assign(d->size, 0);
    /* 默认构造由描述符提供（codegen 生成 init_defaults → placement-new 生成的 C++ 类型）：
     *   · 应用 schema 里的字段默认值（此前零填充不会应用默认值 —— 曾导致 RichText 颜色全透明等）；
     *   · 正确构造 string / vector 等动态成员（零填充缓冲不是合法对象）。 */
    if (d->init_defaults) {
        d->init_defaults(blob.data());
    } else if (d->flags & MQ_TYPE_DYNAMIC) {
        for (uint32_t i = 0; i < d->field_count; ++i) {
            const mq_field_desc* f = &d->fields[i];
            if (f->kind == MQ_TK_STRING) new (blob.data() + f->offset) std::string();
            else if (f->kind == MQ_TK_ARRAY_DYN && f->container_clear) f->container_clear(blob.data() + f->offset);
        }
    }
}

/* 组件挂载：由**描述符**构造默认实例（schema 默认值生效）→ 引擎不硬编码组件类型。 */
void attach_component(Engine& eng, int idx, const char* type_name) {
    Entity* e = eng.scene->at(idx);
    const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, type_name);
    if (!e || !d) return;
    auto& blob = e->comps[d->type_id];
    if (!blob.empty()) destroy_blob(eng, d->type_id, blob);
    construct_blob(eng, d, blob);        // 含动态成员（string/vector）的合法构造 + schema 默认值
}

/* ---- 终止信号（docs/09 §5）：处理器只置位并恢复默认行为，写盘在帧边界 ---- */
volatile sig_atomic_t g_signal_hit = 0;
const char* g_signal_name = nullptr;
/* 信号安全崩溃落盘（docs/09 §5）：处理器内不得分配/加锁/调用 iostream。
 * 因此引擎在每个帧边界把"崩溃上下文"预渲染进 g_crash_buf，并预打开报告 fd；
 * 处理器只做 write(fd, buf, len) 后恢复默认行为再 raise。 */
int  g_crash_fd = -1;
char g_crash_buf[16384];
volatile sig_atomic_t g_crash_len = 0;
/* 优雅退出标志（宿主 kill / CI 超时清理用）。
 * 注意：SDL2 会**接管 SIGTERM/SIGINT 并只投递 SDL_QUIT 事件** —— 无窗口（headless）时没人处理该事件，
 * 进程就杀不掉了。这里自己装处理器置标志，主循环据此走正常退出路径（teardown/端点清理）。 */
volatile sig_atomic_t g_quit_requested = 0;
inline void on_quit_signal(int) { g_quit_requested = 1; }

inline void on_fatal_signal(int sig) {
    g_signal_hit = sig;
    g_signal_name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT"
                  : sig == SIGFPE ? "SIGFPE" : sig == SIGINT ? "SIGINT" : "signal";
    if (g_crash_fd >= 0 && g_crash_len > 0) {
        const maque::compat::ssize_t ignored =
            maque::compat::write_fd(g_crash_fd, g_crash_buf, (size_t)g_crash_len);
        (void)ignored;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- 反射读取辅助（不依赖具体类型，仅按字段名） ---- */
bool read_f32(const mq_type_desc* d, const void* blob, const char* name, float* out) {
    const mq_field_desc* f = mq_reflect_field(d, name);
    if (!f || f->kind != MQ_TK_F32) return false;
    *out = *(const float*)((const uint8_t*)blob + f->offset);
    return true;
}
const std::string* read_str(const mq_type_desc* d, const void* blob, const char* name) {
    const mq_field_desc* f = mq_reflect_field(d, name);
    if (!f || f->kind != MQ_TK_STRING) return nullptr;
    return (const std::string*)((const uint8_t*)blob + f->offset);
}

bool write_ppm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::print(f, "P6\n{} {}\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

/* ---- Engine ---- */
int Engine::load_texture(const std::string& path) {
    auto it = tex_cache.find(path);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int id = rapi->create_texture(rself, rgba, (int)w, (int)h);
    mq_image_free(rgba);
    tex_cache[path] = id;
    return id;
}

/* builtin:/ → 引擎内建（此处程序化生成）；否则 assets（VFS）。 */
int Engine::load_texture_ref(const std::string& ref) {
    if (ref.rfind("builtin:", 0) == 0) {
        auto it = tex_cache.find(ref);
        if (it != tex_cache.end()) return it->second;
        int w = 1, h = 1;
        std::vector<uint8_t> px;
        if (ref == "builtin:/textures/checker") {
            w = h = 16; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                bool alt = (((x / 4) + (y / 4)) & 1) != 0;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = alt ? 255 : 230; c[1] = alt ? 255 : 140; c[2] = alt ? 255 : 60; c[3] = 255;
            }
        } else { px = { 255, 255, 255, 255 }; }
        int id = rapi->create_texture(rself, px.data(), w, h);
        tex_cache[ref] = id;
        return id;
    }
    return load_texture(ref);   // assets
}

/* 材质资源：builtin:/ 或 assets（JSON，反射反序列化 scn.SpriteMaterial）。 */
bool Engine::load_material(const std::string& ref, scn::SpriteMaterial& out) {
    if (ref.rfind("builtin:", 0) == 0) {
        out.base_color = math::Vec4{ 1, 1, 1, 1 };
        out.texture = (ref == "builtin:/materials/checker") ? "builtin:/textures/checker"
                                                            : "builtin:/textures/white";
        return true;
    }
    void* d = nullptr; size_t n = 0;
    std::string path = "/" + ref;
    if (mq_vfs_read(vfs, path.c_str(), &d, &n) != 0) { std::print("runtime: material not found '{}'\n", ref); return false; }
    mq_json* dom = mq_json_parse((const char*)d, n);
    mq_vfs_free(d);
    if (!dom) return false;
    const mq_type_desc* mt = mq_reflect_type_by_name(reflect, "scn.SpriteMaterial");
    int rc = mt ? mq_persist_apply_json(reflect, mt, dom, &out) : 1;
    mq_json_free(dom);
    return rc == 0;
}

/* 世界 → 快照（反射驱动；进快照池避免抖动，docs/07 §8、docs/10 §4）。 */
bool Engine::world_snapshot(mq_snapshot* snap) {
    if (!scene || !snap) return false;
    SnapshotWriter w;
    w.buf.reserve(1 << 16);
    scene->encode(*this, w);                            // 先前置容量（快照池缓冲复用，避免抖动）
    /* 物理状态（docs/10 §4）：物理是回放最易漂移环节 → 必须进快照 */
    const size_t phys_size = physics ? mq_physics_state_size(physics) : 0;
    w.pod((uint64_t)phys_size);
    if (phys_size) {
        std::vector<uint8_t> pstate(phys_size);
        const size_t n = mq_physics_save_state(physics, pstate.data(), pstate.size());
        w.bytes(pstate.data(), n);
    }
    /* 粒子状态（docs/15 §4）：也是模拟状态 → 必须进快照，否则回滚后世界不一致 */
    w.pod(frame);                                     // 模拟帧号（时间位置；回滚必须一并回退）
    w.pod(rng);                                       // 引擎 RNG 状态（D2：回滚后随机序列延续）
    w.pod((uint32_t)emitter_accum.size());
    for (int acc : emitter_accum) w.pod((int32_t)acc);
    w.pod((uint32_t)particles.size());
    for (auto& p : particles) {
        w.pod((int32_t)p.owner);
        w.pod(p.x); w.pod(p.y); w.pod(p.vx); w.pod(p.vy);
        w.pod(p.life); w.pod(p.max_life); w.pod(p.size);
        w.pod(p.r); w.pod(p.g); w.pod(p.b);
    }
    void* out = mq_snapshot_begin(snap, w.buf.size() ? w.buf.size() : 1);
    if (!out) return false;
    if (!w.buf.empty()) std::memcpy(out, w.buf.data(), w.buf.size());
    mq_snapshot_commit(snap, w.buf.size());
    return true;
}

/* 快照 → 世界（道具/子节点/组件逐字段恢复；prefab 就地实例化，docs/12 §5）。 */
void Engine::world_restore(const mq_snapshot* snap) {
    if (!scene || !snap) return;
    size_t n = 0;
    const void* bytes = mq_snapshot_bytes(const_cast<mq_snapshot*>(snap), &n);
    if (!bytes || !n) return;
    const size_t used = scene->decode(*this, (const uint8_t*)bytes, n);
    if (used && used + 8 <= n) {
        uint64_t phys_size = 0;
        std::memcpy(&phys_size, (const uint8_t*)bytes + used, 8);
        const uint8_t* ps = (const uint8_t*)bytes + used + 8;
        size_t off = used + 8;
        if (physics && phys_size && off + phys_size <= n) {
            mq_physics_load_state(physics, ps, phys_size);
            off += phys_size;
        }
        particles.clear();
        if (off + 8 <= n) {                                  // 模拟帧号
            uint64_t fr = 0;
            std::memcpy(&fr, (const uint8_t*)bytes + off, 8);
            off += 8;
            frame = fr;
        }
        if (off + 4 <= n) {                                  // 引擎 RNG 状态
            uint32_t rng_state = 0;
            std::memcpy(&rng_state, (const uint8_t*)bytes + off, 4);
            off += 4;
            rng = rng_state;
        }
        if (off + 4 <= n) {                                  // 发射累加器（发射相位，确定性）
            uint32_t acount = 0;
            std::memcpy(&acount, (const uint8_t*)bytes + off, 4);
            off += 4;
            emitter_accum.clear();
            for (uint32_t i = 0; i < acount && off + 4 <= n; ++i) {
                int32_t v = 0;
                std::memcpy(&v, (const uint8_t*)bytes + off, 4);
                off += 4;
                emitter_accum.push_back(v);
            }
        }
        if (off + 4 <= n) {
            uint32_t pcount = 0;
            std::memcpy(&pcount, (const uint8_t*)bytes + off, 4);
            off += 4;
            if (pcount < 100000) {
                particles.reserve(pcount);
                for (uint32_t i = 0; i < pcount; ++i) {
                    if (off + sizeof(Particle) - sizeof(int) > n) break;
                    Particle p{};
                    std::memcpy(&p.owner, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.x, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.y, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.vx, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.vy, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.life, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.max_life, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.size, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.r, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.g, (const uint8_t*)bytes + off, 4); off += 4;
                    std::memcpy(&p.b, (const uint8_t*)bytes + off, 4); off += 4;
                    particles.push_back(p);
                }
            }
        }
    }
    scene->flatten_world(*this);
}

/* 世界校验和：场景（实体/组件标量）+ 物理求解器状态 + 粒子（都是模拟状态，docs/10 §5）。 */
uint64_t Engine::world_checksum() {
    uint64_t h = scene ? scene->checksum(*this) : 1469598103934665603ull;
    if (physics) h ^= mq_physics_checksum(physics) * 1099511628211ull;
    for (auto& p : particles) {
        h ^= (uint64_t)(int64_t)(p.x * 4096.0F); h *= 1099511628211ull;
        h ^= (uint64_t)(int64_t)(p.y * 4096.0F); h *= 1099511628211ull;
        h ^= (uint64_t)(int64_t)(p.life * 4096.0F); h *= 1099511628211ull;
    }
    return h;
}

/* 校验和轨迹（逐帧；用于失步定位与回放对比，docs/10 §5/§7）。 */
uint64_t Engine::world_trail() const {
    uint64_t h = 1469598103934665603ull;
    for (auto& [f, c] : trail) {
        h ^= f; h *= 1099511628211ull;
        h ^= c; h *= 1099511628211ull;
    }
    return h;
}

/* 单帧推进：① 采样 ② 输入帧 ③ 脚本 update+GC ④ 校验和/快照 ⑤ 渲染。
 * 主循环与控制面 Runtime.step 走**同一条帧序** → 行为一致（docs/10 D1、docs/11）。 */
void Engine::step_frame() {
    /* 输入帧缓冲：**必须预分配**（平台按 pointer_cap/key_cap 写入；容量 0 = 静默丢弃全部设备事件）。
     * 这是"真实鼠标/键盘事件永远到不了脚本"的根因（docs/19 §3：宿主预分配语义）。 */
    static constexpr uint32_t kMaxPointers = 16, kMaxKeys = 32;
    static thread_local mq_input_frame iframe{};
    static thread_local mq_platform_pointer_event iframe_pointers[kMaxPointers]{};
    static thread_local mq_platform_key_event iframe_keys[kMaxKeys]{};
    iframe.pointers = iframe_pointers; iframe.pointer_cap = kMaxPointers;
    iframe.keys = iframe_keys;         iframe.key_cap = kMaxKeys;
    /* 帧起点 + FPS（按上一帧总耗时；首帧为 0）*/
    {
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
        mq_platform_pump(platform);
        if (window) mq_platform_poll(platform, &iframe);
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
    (void)mq_platform_display_changed;   /* 尺寸变化由每帧解析自然覆盖（保留 API 供宿主查询） */

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
        ++script_frames;
        step_tweens(dt);                             // Tween（补间）
        step_schedules(dt);                          // 定时器（定时器）
        /* 脚本错误的判定：总线**错误计数**的增量（每个 VM 各自受保护调用 → 错误隔离但不静默）。
         * 注意不能拿 dispatch_event 的返回值判断：它返回的是命中数，不是错误码。 */
        const uint32_t err_before = bus ? mq_script_bus_error_count(bus) : 0;
        dispatch_event(*this, MQ_EVENT_UPDATE, (double)dt, 0.0, 0.0, nullptr);
        if (bus && mq_script_bus_error_count(bus) != err_before) {
            char ebuf[512] = { 0 };
            mq_script_bus_last_error(bus, ebuf, (int)sizeof ebuf);
            mq_obs_event(obs, "script", 3, "update_failed");
            write_crash_context(ebuf[0] ? ebuf : "script update failed");      // 可精确重放（docs/09 §5）
        }
        if (bus) mq_script_bus_gc(bus);             // 帧边界：逐 VM 推进 GC（docs/10 D7）
        anim_step(dt);                                   // 动画采样写回（docs/15 §2）
        particles_step(dt);                              // 粒子推进（docs/15 §4）
        nav_step();                                      // 导航代理推进（A* + 追踪）
        physics_components_step(dt);                     // 组件驱动物理（RigidBody2D）
        flush_pending_destroy();                         // node.destroy()：帧边界统一销毁
        ui_step();                                       // UI 组件语义（layout/progress/slider/check/safearea/scroll）
        scene->flatten_world(*this);
        /* Spine 事件记录的清空点：**脚本之后、step 之前**（见 Engine::spine_clear_events 注释：
         * 放错了会导致 pull 面永远读到 0 条）。 */
        spine_clear_events();
        spine_step(dt);                                  // Spine：步进 + 出绘制数据（依赖展平后的世界位置）
        scene->extract(*this, draws);
        spine_extract(spine_meshes);                     // Spine：绘制数据 → 网格图元（与图元同序渲染）
    text_flush(draws);            // 文本阶段 2：刷新图集纹理并按最终尺寸计算 UV
    /* ImGui：结束帧 → 绘制命令转成网格图元。**放在这里**是刻意的：
     * 引擎所有图层（含 ui pass 的 layer=1）都已提取完 → 面板以 MQ_IMGUI_LAYER 提交，
     * 天然画在最上层；且与其它图元一起进同一像素缓冲 → 截图里也有面板（可自证）。 */
    imgui_extract(spine_meshes);
    }
    // ④ 世界校验和 + 周期快照（确定性验证 + 回滚点，docs/10 §4/§5）
    if (scene) {
        last_checksum = world_checksum();
        trail.emplace_back(frame, last_checksum);
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
    // ⑤ 渲染（只读快照 → 渲染不回写，docs/13 §8）
    const int SWpx = (int)(W * ss + 0.5F), SHpx = (int)(H * ss + 0.5F);
    /* stats：drawcall = 提交项数（图元 + 网格批次）；三角形 = 图元 2 个/项 + 网格索引/3。 */
    stat_calls = (int)(draws.size() + spine_meshes.size());
    stat_tris = (int)draws.size() * 2;
    for (const auto& sm : spine_meshes) stat_tris += (int)(sm.index_count / 3);
    const auto stat_render0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> pixels((size_t)TW * SHpx * 4);
    if (rapi && rself) {
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
    /* stats：渲染耗时（到 present 之前）+ 整帧耗时 → FPS；EMA 平滑避免数字乱跳。 */
    {
        const auto now = std::chrono::steady_clock::now();
        const double rms = std::chrono::duration<double, std::milli>(now - stat_render0).count();
        const double lms = std::chrono::duration<double, std::milli>(now - stat_t0).count() - rms;
        stat_render_ms = stat_render_ms > 0 ? (float)(stat_render_ms * 0.9 + rms * 0.1) : (float)rms;
        stat_logic_ms = stat_logic_ms > 0 ? (float)(stat_logic_ms * 0.9 + lms * 0.1) : (float)lms;
    }
    /* 呈现到窗口（docs/05、M5）：无窗口/dummy 驱动时为安全空操作 */
    if (platform && window && rapi && rself) {
        /* 呈现按**画布策略**给出的目标矩形（缩放/留边由分辨率策略决定；整倍放大用最近邻） */
        const int dx = (int)(canvas.offset_x + 0.5F), dy = (int)(canvas.offset_y + 0.5F);
        const int dw = (int)(canvas.viewport_w + 0.5F), dh = (int)(canvas.viewport_h + 0.5F);
        mq_platform_present_rect(platform, pixels.data(), TW, SHpx, dx, dy, dw, dh, -1);
    }
    /* 官方后端模式：**present 之后**把面板直接画到窗口（在窗口像素空间，不受引擎缩放影响）。 */
    if (imgui_on && imgui_backend) {
        mq_imgui_backend_render();
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
    if (ctl_server) {                     // 预览：保留最近一帧（仅在开了端点时）
        frame_pixels = pixels;
        frame_w = TW;
        frame_h = SHpx;
    }
    ++frame;
}

/* ---- 动画（docs/15 §2）：关键帧在模拟步内采样并写回反射字段 ---- */

/* 路径解析：先按组件类型名定位（"scn.Sprite.radius"），再按叶子字段名定位（"radius"）。
 * 返回目标类型/字段与所在实体；全部按 role 与反射，不硬编码组件类型。 */
bool resolve_field_path(Engine& eng, int entity, const std::string& path,
                        const mq_type_desc** out_type, const mq_field_desc** out_field, void** out_base) {
    Entity* e = eng.scene->at(entity);
    if (!e) return false;
    const size_t dot = path.rfind('.');
    const std::string type_part = dot == std::string::npos ? std::string() : path.substr(0, dot);
    const std::string field_part = dot == std::string::npos ? path : path.substr(dot + 1);
    /* 允许 "Type.field"（跨组件）或裸 "field"（本实体任一组件的字段） */
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
        if (!type_part.empty() && type_part != d->name) continue;
        const mq_field_desc* f = mq_reflect_field(d, field_part.c_str());
        if (!f) continue;
        *out_type = d; *out_field = f; *out_base = blob.data();
        return true;
    }
    return false;
}

bool Engine::load_clip(const std::string& ref) {
    if (ref.empty() || find_clip(ref) >= 0) return find_clip(ref) >= 0;
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + ref;
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK) {
        std::print(stderr, "runtime: clip not found '{}'\n", ref);
        return false;
    }
    const mq_type_desc* ct = mq_reflect_type_by_name(reflect, "asset.AnimationClip");
    if (!ct) { mq_vfs_free(d); return false; }
    Clip clip;
    clip.blob.assign((uint8_t*)d, (uint8_t*)d + ct->size);
    mq_vfs_free(d);
    std::string text;
    { void* t = nullptr; size_t tn = 0;
      if (mq_vfs_read(vfs, vpath.c_str(), &t, &tn) == MQ_VFS_OK) { text.assign((char*)t, tn); mq_vfs_free(t); } }
    if (text.empty()) return false;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    construct_blob(*this, ct, clip.blob);
    const int rc = mq_persist_apply_json(reflect, ct, dom, clip.blob.data());
    mq_json_free(dom);
    if (rc != 0) return false;
    const asset::AnimationClip* ac = (const asset::AnimationClip*)clip.blob.data();
    clip.name = ref;
    clip.length = ac->length;
    clip.loop = ac->loop == anim::LoopMode::Loop;
    if (ac->tracks.empty()) { std::print("runtime: clip '{}' has no tracks\n", ref); return false; }
    /* 校验每条轨道至少能解析（角色/实体在采样时确定） */
    for (auto& t : ac->tracks) clip.paths.push_back(t.field_path);
    clips.push_back(std::move(clip));
    std::print("runtime: clip '{}' loaded ({} track(s), length={:.2f}s)\n",
               ref, (int)ac->tracks.size(), (double)ac->length);
    return true;
}

void Engine::anim_step(float dt) {
    if (!scene) return;                                   // 片段首次使用时按需载入（见下）
    std::vector<int> ids;
    for (auto& [idx, e] : scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_ANIM_PLAYER)) continue;
            bool playing = true;
            if (const mq_field_desc* f = mq_reflect_field(d, "playing")) playing = *(const bool*)(blob.data() + f->offset);
            std::string clipref;
            if (const std::string* cp = read_str(d, blob.data(), "clip")) clipref = *cp;
            float speed = 1.0F;
            read_f32(d, blob.data(), "speed", &speed);
            if (!playing || clipref.empty()) continue;
            if (find_clip(clipref) < 0 && !load_clip(clipref)) continue;
            const int ci = find_clip(clipref);
            if (ci < 0) continue;
            const asset::AnimationClip* ac = (const asset::AnimationClip*)clips[ci].blob.data();
            const float length = ac->length;
            /* 播放头推进（确定性：dt 来自固定步长，无墙钟，docs/10 D1/D3） */
            float time = 0.0F;
            if (const mq_field_desc* f = mq_reflect_field(d, "time")) time = *(const float*)(blob.data() + f->offset);
            time += dt * speed;
            const bool loop = clips[ci].loop;
            if (length > 0.0F) {
                if (loop) { while (time >= length) time -= length; }
                else if (time > length) time = length;
            }
            if (const mq_field_desc* f = mq_reflect_field(d, "time")) *(float*)(blob.data() + f->offset) = time;
            if (const mq_field_desc* f = mq_reflect_field(d, "duration")) *(float*)(blob.data() + f->offset) = length;
            /* 采样 + 写回：对每条轨道做线性插值（值型 Vec4 前 n 分量按字段类型写标量） */
            for (auto& track : ac->tracks) {
                if (track.keys.empty() || track.field_path.empty()) continue;
                const mq_type_desc* tt = nullptr; const mq_field_desc* tf = nullptr; void* base = nullptr;
                if (!resolve_field_path(*this, id, track.field_path, &tt, &tf, &base)) continue;
                /* 定位关键帧区间 */
                size_t k = 0;
                while (k + 1 < track.keys.size() && track.keys[k + 1].time <= time) ++k;
                const asset::Key& a = track.keys[k];
                const asset::Key& b = track.keys[k + 1 < track.keys.size() ? k + 1 : k];
                float alpha = 0.0F;
                if (b.time > a.time) alpha = (time - a.time) / (b.time - a.time);
                if (alpha < 0.0F) alpha = 0.0F;
                if (alpha > 1.0F) alpha = 1.0F;
                const float v[4] = {
                    a.value.x + (b.value.x - a.value.x) * alpha,
                    a.value.y + (b.value.y - a.value.y) * alpha,
                    a.value.z + (b.value.z - a.value.z) * alpha,
                    a.value.w + (b.value.w - a.value.w) * alpha,
                };
                void* fp = (uint8_t*)base + tf->offset;
                if (tt == nullptr || tf == nullptr) continue;
                if (tf->kind == MQ_TK_F32) { *(float*)fp = v[0]; continue; }
                if (tf->kind == MQ_TK_STRUCT) {
                    const mq_type_desc* vt = mq_reflect_type_by_id(reflect, tf->type_id);
                    if (!vt) continue;
                    for (uint32_t fi = 0, vi = 0; fi < vt->field_count && vi < 4; ++fi, ++vi) {
                        const mq_field_desc* vf = &vt->fields[fi];
                        if (vf->kind == MQ_TK_F32) *(float*)((uint8_t*)fp + vf->offset) = v[vi];
                    }
                }
            }
        }
    }
}

/* ---- 粒子（docs/15 §4）：CPU 确定性粒子；池上限 + 引擎 RNG（参与回放） ---- */
void Engine::particles_step(float dt) {
    if (!scene) return;
    /* 推进已有粒子（按生成顺序；与实体无关，顺序稳定） */
    size_t w = 0;
    for (size_t i = 0; i < particles.size(); ++i) {
        Particle& p = particles[i];
        p.life += dt;
        if (p.life >= p.max_life) continue;
        p.vy += 0.0F;                                  // 重力在下面按发射器参数施加
        p.x += p.vx * dt; p.y += p.vy * dt;
        particles[w++] = p;
    }
    particles.resize(w);

    /* 发射（按实体 id 升序 → 确定性；RNG 为引擎 RNG） */
    std::vector<int> ids;
    for (auto& [idx, e] : scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_PARTICLE)) continue;
            bool enabled = true;
            if (const mq_field_desc* f = mq_reflect_field(d, "enabled")) enabled = *(const bool*)(blob.data() + f->offset);
            if (!enabled) continue;
            float rate = 0, life = 1, speed = 0, spread = 0.5F, gravity = 0, size = 2;
            float r = 1, g = 1, b = 1;
            uint32_t max_count = 256;
            read_f32(d, blob.data(), "rate", &rate);
            read_f32(d, blob.data(), "lifetime", &life);
            read_f32(d, blob.data(), "speed", &speed);
            read_f32(d, blob.data(), "spread", &spread);
            read_f32(d, blob.data(), "gravity", &gravity);
            read_f32(d, blob.data(), "size", &size);
            read_f32(d, blob.data(), "r", &r);
            read_f32(d, blob.data(), "g", &g);
            read_f32(d, blob.data(), "b", &b);
            if (const mq_field_desc* f = mq_reflect_field(d, "max_count")) max_count = *(const uint32_t*)(blob.data() + f->offset);
            /* 其它粒子的重力（按各自发射器；用 owner 记录） */
            for (auto& p : particles) if (p.owner == id) p.vy += gravity * dt;
            /* 累加器（每实体一份，存在 emitter_accum；按 id 定位） */
            while ((int)emitter_accum.size() <= id) emitter_accum.push_back(0);
            emitter_accum[id] += (int)(rate * dt * 1024.0F);        // 定点累加（确定性，避免浮点漂移）
            const int per_emit = emitter_accum[id] / 1024;
            emitter_accum[id] -= per_emit * 1024;
            for (int i = 0; i < per_emit; ++i) {
                if (particles.size() >= max_particles) break;
                uint32_t alive_owner = 0;
                for (auto& p : particles) if (p.owner == id) ++alive_owner;
                if (alive_owner >= max_count) break;
                const float ang = (next_rng() / 4294967296.0F) * 6.28318530718F;
                const float mag = speed * (1.0F - spread * (next_rng() / 4294967296.0F));
                Particle p;
                p.owner = id;
                p.x = e->world_x; p.y = e->world_y;
                p.vx = std::cos(ang) * mag;
                p.vy = std::sin(ang) * mag;
                p.max_life = life > 0 ? life : 1.0F;
                p.size = size; p.r = r; p.g = g; p.b = b;
                particles.push_back(p);
            }
        }
    }
}

/* ---- 导航（docs/15 导航）：A* 在帧内确定性执行，代理沿路径追踪 ---- */
void Engine::nav_step() {
    if (!scene || !nav) return;
    std::vector<int> ids;
    for (auto& [idx, e] : scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());                      // 升序 → 更新顺序确定（docs/10 D4）
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_NAV_AGENT)) continue;
            bool has_target = false;
            if (const mq_field_desc* f = mq_reflect_field(d, "has_target")) has_target = *(const bool*)(blob.data() + f->offset);
            const mq_field_desc* fstate = mq_reflect_field(d, "state");
            if (!has_target) { if (fstate) *(int32_t*)(blob.data() + fstate->offset) = 0; continue; }
            float tx = 0, ty = 0, speed = 60, arrive = 2;
            read_f32(d, blob.data(), "tx", &tx);
            read_f32(d, blob.data(), "ty", &ty);
            read_f32(d, blob.data(), "speed", &speed);
            read_f32(d, blob.data(), "arrive_dist", &arrive);
            bool repath = true;
            if (const mq_field_desc* f = mq_reflect_field(d, "repath")) repath = *(const bool*)(blob.data() + f->offset);

            AgentPath& ap = agent_paths[id];
            /* 路径缺失/目标变化 → 重新寻路（确定性：与实体 id 顺序无关的单点查询） */
            const bool need_path = ap.path.empty() || ap.index >= ap.path.size() / 2;
            if (need_path || (repath && !ap.path.empty())) {
                const uint32_t idx_last = ap.path.empty() ? 0 : (uint32_t)(ap.path.size() / 2 - 1);
                const float goal_x = ap.path.empty() ? tx : ap.path[idx_last * 2];
                const float goal_y = ap.path.empty() ? ty : ap.path[idx_last * 2 + 1];
                if (need_path || (repath && (goal_x != tx || goal_y != ty))) {
                    std::vector<float> buf(2 * 4096);
                    uint32_t n = 0, need = 0;
                    const int rc = mq_nav_find_path(nav, e->world_x, e->world_y, tx, ty, buf.data(),
                                                   (uint32_t)buf.size() / 2, &n, &need);
                    if (rc == MQ_NAV_ERR_CAPACITY) {
                        buf.resize(need);
                        mq_nav_find_path(nav, e->world_x, e->world_y, tx, ty, buf.data(), need, &n, &need);
                    }
                    if (rc == MQ_NAV_OK) {
                        n = mq_nav_smooth(nav, buf.data(), n);
                        ap.path.assign(buf.begin(), buf.begin() + (size_t)n * 2);
                        ap.index = 0;
                        if (fstate) *(int32_t*)(blob.data() + fstate->offset) = 1;   // Moving
                    } else {
                        ap.path.clear();
                        ap.index = 0;
                        if (fstate) *(int32_t*)(blob.data() + fstate->offset) = 3;   // NoPath
                        continue;
                    }
                }
            }
            if (ap.path.empty()) continue;
            float x = e->world_x, y = e->world_y;
            const float dt = 1.0F / 60.0F;
            const int rc = mq_nav_step_towards(nav, &x, &y, ap.path.data(),
                                               (uint32_t)(ap.path.size() / 2), &ap.index,
                                               speed, dt, arrive);
            host::entity_set_pos(this, (float)id, x, y);
            e->world_x = x; e->world_y = y;
            if (rc == 1 && fstate) *(int32_t*)(blob.data() + fstate->offset) = 2;    // Arrived
        }
    }
}

/* 装配期预解析（docs/13 §8：渲染/提取只读）：把所有实体的纹理与材质引用解析进缓存。
 * 引擎在装配阶段做 IO（合法：不在模拟帧内，docs/10 D7）。 */
void Engine::preload_scene_assets() {
    if (!scene) return;
    std::vector<int> ids;
    for (auto& [idx, e] : scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d) continue;
            if (d->role & MQ_ROLE_SPRITE) {
                if (const std::string* tp = read_str(d, blob.data(), "texture"); tp && !tp->empty())
                    load_texture_ref(*tp);
                if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty()) {
                    scn::SpriteMaterial m{};
                    if (load_material(*mp, m)) {
                        material_cache[*mp] = m;
                        if (!m.texture.empty()) load_texture_ref(m.texture);
                    }
                }
            }
        }
    }
}

/* ---- 热重载（docs/08 §热重载、docs/14）：只在帧边界执行 ---- */

bool Engine::reload_script(std::string* err, const std::string* only) {
    if (script_paths.empty() || !bus) { if (err) *err = "no script"; return false; }
    bool any = false;
    for (size_t i = 0; i < script_paths.size(); ++i) {
        const std::string& sp = script_paths[i];
        if (only && !only->empty() && *only != sp) continue;      // 只重载改动的那个脚本
        void* d = nullptr; size_t n = 0;
        if (mq_vfs_read(vfs, ("/" + sp).c_str(), &d, &n) != MQ_VFS_OK) continue;
        std::string src((char*)d, n);
        mq_vfs_free(d);
        const uint64_t h = mq_vfs_hash(src.data(), src.size());
        if (i < script_hashes.size() && h == script_hashes[i]) continue;   // 内容未变 → 跳过
        /* 原子热重载：先建**全新 VM** 并 eval；成功才换入槽位（顺序不变，其它脚本状态保留）。
         * 失败则销毁新 VM、保留旧的 —— 写坏一个脚本不会让正在跑的游戏失去该逻辑。 */
        const char* lang = mq_script_lang_for_path(sp.c_str());
        mq_script_vm* fresh = mq_script_vm_create_lang(lang);
        if (!fresh) { if (err) *err = "backend unavailable"; return false; }
        mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
        mq_bind_generated(fresh, this);
        if (mq_script_eval(fresh, src.c_str(), sp.c_str()) != 0) {
            char ebuf[512] = { 0 };
            std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
            mq_script_vm_destroy(fresh);                       // 保留旧 VM：游戏继续用旧逻辑
            if (err) *err = e;
            std::print(stderr, "runtime: hot reload failed for '{}': {} (旧脚本保留)\n", sp, e);
            return false;
        }
        mq_script_bus_swap(bus, (uint32_t)i, fresh, sp.c_str());
        /* 换入后**只对新 VM** 派发 on_start：脚本重新初始化并重新注册它的事件处理器。
         * （不加这一步，热更后的脚本会失去 node_on 注册 → 收不到任何事件，功能静默失效。） */
        if (mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr) != 0) {
            char ebuf[512] = { 0 };
            std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "on_start failed";
            std::print(stderr, "runtime: hot reload on_start failed for '{}': {}\n", sp, e);
        }
        if (script_hashes.size() <= i) script_hashes.resize(i + 1, 0);
        script_hashes[i] = h;
        if (script_paths.size() == 1) { script_hash = h; vm = fresh; }
        any = true;
        mq_obs_event(obs, "runtime", 1, "script_reloaded");
        std::print("runtime: hot reload script '{}' ({} bytes, slot {})\n", sp, src.size(), i);
    }
    if (any) ++reloads;
    return any;
}

bool Engine::reload_scene() {
    if (scn_vfs_path.empty() || !scene) return false;
    void* d = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, scn_vfs_path.c_str(), &d, &n) != MQ_VFS_OK) return false;
    std::string text((char*)d, n);
    mq_vfs_free(d);
    Scene* fresh = new Scene();
    fresh->deserialize(*this, text);
    fresh->instantiate(*this);
    delete scene;                       // 重建世界（编辑器/AI 场景迭代路径）
    scene = fresh;
    scene->flatten_world(*this);
    ++reloads;
    mq_obs_event(obs, "runtime", 1, "scene_reloaded");
    std::print("runtime: hot reload scene '{}'\n", scn_vfs_path);
    return true;
}

uint32_t Engine::poll_watcher() {
    if (!watcher) return 0;
    struct Ctx { Engine* eng; };
    Ctx cx{ this };
    const uint32_t changed = mq_watcher_poll(watcher, [](void* ud, const char* path, mq_watch_kind kind) {
        auto* c = (Ctx*)ud;
        if (kind == MQ_WATCH_REMOVED) return 0;
        const std::string p = path ? path : "";
        bool is_script = false;
        for (const std::string& sp : c->eng->script_paths) {          // 多脚本：按路径定位改的是哪一个
            if (p.size() >= sp.size() && p.compare(p.size() - sp.size(), sp.size(), sp) == 0) {
                c->eng->reload_script(nullptr, &sp);
                is_script = true;
                break;
            }
        }
        if (is_script) { /* 已处理 */ }
        else if (!c->eng->scn_vfs_path.empty() &&
                 p.size() >= c->eng->scn_vfs_path.size() &&
                 p.compare(p.size() - c->eng->scn_vfs_path.size(), c->eng->scn_vfs_path.size(), c->eng->scn_vfs_path) == 0)
            c->eng->reload_scene();
        return 0;
    }, &cx);
    return changed;
}

/* 载入回放日志（MAQUE_REPLAY）：此后所有步进都只消费记录（设备输入被忽略）。 */
bool Engine::prepare_replay() {
    const char* rp = std::getenv("MAQUE_REPLAY");
    if (!rp || !input) return false;
    std::ifstream f(rp, std::ios::binary);
    if (!f) { std::print("runtime: cannot open replay log '{}'\n", rp); return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty() || mq_input_log_load_json(input, text.c_str()) != 0) {
        std::print("runtime: cannot load replay log '{}'\n", rp);
        return false;
    }
    const char* mf = mq_input_log_get_meta(input, "frames");
    std::print("runtime: replay '{}' ({} frames expected)\n", rp, mf ? mf : "?");
    return true;
}

/* 控制面 Runtime.step：确定性推进 n 帧（AI/CI 驱动同一帧序）。 */
bool Engine::run_frames(uint64_t n) {
    for (uint64_t i = 0; i < n && !quit; ++i) step_frame();
    return !quit;
}

/* 检查点快照（docs/10 §5 分歧定位）：环状保存最近 N 个（默认每 30 帧），
 * 回放失步时可用 Replay.seek 跳回任一检查点做二分/逐步定位。 */
void Engine::checkpoint_push() {
    if (checkpoints.empty()) {
        checkpoints.reserve(cp_count);
        for (uint32_t i = 0; i < cp_count; ++i) checkpoints.push_back(mq_snapshot_create());
    }
    mq_snapshot* s = checkpoints[cp_cursor % checkpoints.size()];
    cp_cursor++;
    if (!world_snapshot(s)) return;
    mq_snapshot_set_frame(s, frame);
    mq_snapshot_set_desc(s, ("checkpoint@" + std::to_string(frame)).c_str());
}

/* 崩溃/异常上下文（docs/09 §5）：把"精确重放所需的一切"落盘为 JSON —— 
 * seed、场景、帧号、最近检查点、输入日志（若在录制）、世界校验和、指标。
 * 触发点：① 脚本异常（可重放定位）；② 信号（abort/段错误，见 install_crash_handler）。 */
/* 把崩溃上下文预渲染进信号安全缓冲并确保报告 fd 已打开（每帧边界调用）。 */
void Engine::refresh_crash_buffer() {
    if (g_crash_fd < 0) {
        const char* out = std::getenv("MAQUE_CRASH_REPORT");
        if (!out || !*out) return;
        g_crash_fd = maque::compat::open_fd(out, maque::compat::kOpenWriteCreate);
        if (g_crash_fd < 0) return;
    }
    uint64_t cp_frame = 0, cp_sum = 0;
    for (auto* s : checkpoints)
        if (s && mq_snapshot_frame(s) > cp_frame) { cp_frame = mq_snapshot_frame(s); cp_sum = mq_snapshot_checksum(s); }
    const int n = std::snprintf(g_crash_buf, sizeof g_crash_buf,
        "{\"reason\":\"fatal signal\",\"scene\":\"%s\",\"frame\":%llu,\"seed\":%u,"
        "\"world_trail\":\"%llu\",\"checkpoint\":{\"frame\":%llu,\"checksum\":\"%llu\"},\n"
        " \"replay\":{\"command\":\"MAQUE_REPLAY=<log> %s\"},\n \"metrics\":%s}\n",
        (scn_full_path.empty() ? scn_vfs_path : scn_full_path).c_str(),
        (unsigned long long)frame, scene ? scene->desc.seed : 0u,
        (unsigned long long)world_trail(),
        (unsigned long long)cp_frame, (unsigned long long)cp_sum,
        (scn_full_path.empty() ? scn_vfs_path : scn_full_path).c_str(),
        mq_obs_dump_json(obs));
    g_crash_len = n > 0 ? (n < (int)sizeof g_crash_buf ? n : (int)sizeof g_crash_buf - 1) : 0;
    if (g_crash_fd >= 0 && g_crash_len > 0) {          // 同步落盘（保证即使不崩也有当前快照）
        const maque::compat::ssize_t ig =
            maque::compat::pwrite_fd(g_crash_fd, g_crash_buf, (size_t)g_crash_len, 0);
        (void)ig;
    }
}

void Engine::write_crash_context(const char* why) {
    const char* out = std::getenv("MAQUE_CRASH_REPORT");
    if (!out || !*out) return;
    std::ofstream f(out, std::ios::binary);
    if (!f) return;
    uint64_t trail = world_trail();
    /* 最近检查点 */
    uint64_t cp_frame = 0, cp_sum = 0;
    for (auto* s : checkpoints)
        if (s && mq_snapshot_frame(s) > cp_frame) { cp_frame = mq_snapshot_frame(s); cp_sum = mq_snapshot_checksum(s); }
    std::string json = "{\n";
    json += "  \"reason\": ";
    write_json_string_pub(json, why ? why : "unknown");
    json += ",\n  \"scene\": ";
    write_json_string_pub(json, scn_full_path.empty() ? scn_vfs_path : scn_full_path);
    json += ",\n  \"frame\": " + std::to_string(frame);
    json += ",\n  \"seed\": " + std::to_string(scene ? scene->desc.seed : 0);
    json += ",\n  \"world_trail\": \"" + std::to_string(trail) + "\"";
    json += ",\n  \"last_checksum\": \"" + std::to_string(last_checksum) + "\"";
    json += ",\n  \"checkpoint\": {\"frame\": " + std::to_string(cp_frame) +
            ", \"checksum\": \"" + std::to_string(cp_sum) + "\"}";
    json += ",\n  \"replay\": {\"command\": \"MAQUE_REPLAY=<log> ";
    json += scn_vfs_path + "\", \"recorded\": ";
    json += (input && mq_input_recording(input)) ? "true" : "false";
    json += "}";
    json += ",\n  \"metrics\": " + std::string(mq_obs_dump_json(obs)) + "\n}\n";
    f << json;
    std::print(stderr, "runtime: crash context → '{}' (frame={} reason={})\n", out, frame, why ? why : "?");
}

/* 导出世界快照（崩溃复现/AI undo 的落点，docs/10 §7）。控制面每次 step 后也会调用。 */
void Engine::export_final_snapshot() {
    const char* sp = std::getenv("MAQUE_SNAPSHOT");
    if (!sp || !snaps) return;
    if (!snap_last) snap_last = mq_snapshot_pool_acquire(snaps);
    if (!world_snapshot(snap_last)) return;
    size_t n = 0;
    const void* b = mq_snapshot_bytes(snap_last, &n);
    std::ofstream f(sp, std::ios::binary);
    if (f && b && n) {
        mq_snapshot_set_frame(snap_last, frame);
        f.write((const char*)b, (std::streamsize)n);
        std::print("runtime: snapshot → '{}' ({} bytes, frame {})\n", sp, n, frame);
    }
}

/* 终局导出：快照 + 录制日志（frames/trail 元数据 → 回放失步检测的基准）。 */
void Engine::finalize_run() {
    if (const char* td = std::getenv("MAQUE_TRAIL_OUT"); td && *td) {   // 失步定位：逐帧校验和（docs/10 §5）
        std::ofstream f(td, std::ios::binary);
        for (auto& [fr, sum] : trail) f << fr << " " << sum << "\n";
    }
    if (snaps) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
    export_final_snapshot();
    if (input && mq_input_recording(input)) {
        mq_input_log_set_meta(input, "frames", std::to_string(frame).c_str());
        char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)world_trail());
        mq_input_log_set_meta(input, "trail", tb);
        if (const char* rp = std::getenv("MAQUE_RECORD")) {
            std::ofstream f(rp, std::ios::binary);
            const char* json = mq_input_log_json(input);
            if (f && json) f << json;
            std::print("runtime: recorded input log → '{}' ({} frames)\n", rp, frame);
        }
        mq_input_record_end(input);
    }
}

void Engine::main_loop(const std::string& frames_dir, const std::string& name) {
    this->frames_dir = frames_dir;
    this->name = name;
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);
    uint64_t cap = 60 * 60;
    if (const char* mf = std::getenv("MAQUE_MAX_FRAMES")) { long v = std::strtol(mf, nullptr, 10); if (v > 0) cap = (uint64_t)v; }
    if (const char* sf = std::getenv("MAQUE_STOP_FRAME")) { long v = std::strtol(sf, nullptr, 10); if (v > 0) { cap = (uint64_t)v + 1; stop_frame = (uint64_t)v; } }

    /* 回放（docs/10 §3/§5）：输入是数据 → 同一日志应逐帧复现 */
    std::string replay_trail;
    if (prepare_replay()) {
        if (const char* mt = input ? mq_input_log_get_meta(input, "trail") : nullptr) replay_trail = mt;
        /* 日志元数据 frames = **已执行帧数**（finalize_run 写入 loop 计数）→ 回放跑同样多帧；
         * 曾经 +1（误当"末帧索引"）导致回放多跑一帧、末尾校验和必然不等（docs/10 §5）。 */
        if (const char* mf = input ? mq_input_log_get_meta(input, "frames") : nullptr) {
            const uint64_t n = std::strtoull(mf, nullptr, 10);
            if (n > 0) cap = n;
        }
    }
    if (input && std::getenv("MAQUE_RECORD")) mq_input_record_begin(input);

    mq_obs_event(obs, "runtime", 1, "loop_start");
    frame = 0;

    /* 墙钟限速（见 pace_wall_clock 注释）：只在"真窗口 + 非自动化"时启用。
     * 判定为自动化、因此**不限速**（保持"尽快出结果"的既有语义）的情形：
     *   · 无窗口（headless / MAQUE_WINDOW=0x0 / 无显示环境）
     *   · --shot / MAQUE_SHOT（只求一张图，帧数与时间无关）
     *   · 录制/回放（docs/10 §3/§5：要求逐帧复现，不该受墙钟影响）
     * 注意 `MAQUE_MAX_FRAMES`/`MAQUE_STOP_FRAME` **不算**自动化信号 —— 它们只是"跑多少帧"
     * 的上限，与"要不要按真实时间跑"无关（无头跑它们本来就是全速的）。 */
    const bool shot_mode = std::getenv("MAQUE_SHOT") != nullptr;
    const bool recorded = std::getenv("MAQUE_RECORD") != nullptr || !replay_trail.empty();
    pace_wall_clock = window && !shot_mode && !recorded;
    if (const char* pc = std::getenv("MAQUE_PACE")) {          // 显式覆盖：0/off 关闭，1/on 强制开启
        const std::string v(pc);
        if (v == "0" || v == "off") pace_wall_clock = false;
        else if (v == "1" || v == "on") pace_wall_clock = true;
    }
    pace_accum = 0.0;
    /* 墙钟限速用"下一个应执行的时刻"模型（比累加器直观且不会双重计时）：
     * 每步把目标时刻推后 dt，然后睡到该时刻。若落后太多（掉帧），重新锚定到"现在 + dt"，
     * 丢弃积压 —— 宁可让游戏时间变慢，也不要越睡越久追不上（死亡螺旋）。 */
    auto pace_next = std::chrono::steady_clock::now();
    auto pace_last = pace_next;
    const auto pace_start = pace_next;
    double pace_sleep = 0.0;
    uint64_t pace_reanchor = 0;
    uint64_t pace_steps = 0;

    for (; frame < cap && !quit; ) {
        if (stop_frame && frame > stop_frame) break;
        if (g_quit_requested) {                          // SIGTERM/SIGINT → 优雅退出（正常收尾 + 清理端点）
            quit = true;
            mq_obs_event(obs, "runtime", 1, "terminated");
            break;
        }
        /* 墙钟 → 固定步数（docs/10 D1 的确定性不变：**步长仍恒为 1/60**，变的只是
         * "这一轮屏幕刷新要跑几步"）。两个方向都要处理：
         *   · 机器比实时快 → 只跑 1 步会让动画跟着快（实测 2.7x）→ 用 sleep 压到 1x；
         *   · 机器比实时慢（渲染每帧 ~29ms 实测）→ 只跑 1 步会让动画跟着慢（实测 0.58x）
         *     → 这一轮多补几步，让**游戏时间始终跟墙钟走**。
         * 上限 8 步：超出的部分直接丢弃（宁可让游戏时间变慢，也不要越补越久追不上）。 */
        int steps_this_frame = 1;
        if (pace_wall_clock) {
            constexpr auto kStep = std::chrono::milliseconds(1000 / 60);
            const auto now = std::chrono::steady_clock::now();
            pace_accum += std::chrono::duration<double>(now - pace_last).count();
            pace_last = now;
            if (pace_accum > 8.0 / 60.0) { pace_accum = 8.0 / 60.0; ++pace_reanchor; }
            steps_this_frame = static_cast<int>(pace_accum / (1.0 / 60.0));
            if (steps_this_frame < 1) steps_this_frame = 1;
            if (steps_this_frame > 8) steps_this_frame = 8;
            pace_accum -= steps_this_frame * (1.0 / 60.0);
            if (pace_accum < 0.0) pace_accum = 0.0;
        }
        for (int s = 0; s < steps_this_frame && frame < cap && !quit; ++s) {
            if (stop_frame && frame > stop_frame) break;
            step_frame();
            ++pace_steps;
        }
        if (pace_wall_clock) {
            /* 还有余量说明比实时快 → 睡到下一帧的时间点（下轮重算，避免睡多）。 */
            constexpr auto kStep = std::chrono::milliseconds(1000 / 60);
            const auto now = std::chrono::steady_clock::now();
            if (pace_next > now) {
                const auto d = pace_next - now;
                pace_sleep += std::chrono::duration<double>(d).count();
                std::this_thread::sleep_for(d);
            }
            pace_next = std::chrono::steady_clock::now() + kStep;
            pace_last = std::chrono::steady_clock::now();
        }
        if (g_signal_hit) {                              // 收到终止信号 → 落盘上下文后退出
            write_crash_context(g_signal_name ? g_signal_name : "signal");
            mq_obs_event(obs, "runtime", 3, g_signal_name ? g_signal_name : "signal");
            break;
        }
    }

    /* 循环是因为"到帧数上限"退出的 → 记下来（退出码按成功算，见 frames_capped 注释）。 */
    if (!quit && (frame >= cap || (stop_frame && frame > stop_frame))) frames_capped = true;

    finalize_run();

    if (const char* td = std::getenv("MAQUE_TRAIL_OUT"); td && *td) {   // 失步定位（docs/10 §5）
        std::ofstream f(td, std::ios::binary);
        for (auto& [fr, sum] : trail) f << fr << " " << sum << "\n";
    }
    // 失步检测：回放必须有记录里的校验和（docs/10 §5）
    if (!replay_trail.empty()) {
        char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)world_trail());
        if (replay_trail == tb) std::print("runtime: replay matches recorded trail ({})\n", tb);
        else std::print("runtime: DESYNC — replay trail {} != recorded {}\n", tb, replay_trail);
    }
    mq_obs_gauge_set(obs, "frames_total", (double)frame);
    if (const char* pt = std::getenv("MAQUE_TRACE_PACE")) {
        (void)pt;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - pace_start).count();
        std::print("[pace] frames={} 墙钟={:.3f}s 仿真={:.3f}s 速率={:.2f}x wall_clock={} 睡觉={:.3f}s 重锚定={} 次\n",
                   (long long)frame, wall, (double)frame / 60.0, wall > 0 ? (frame / 60.0) / wall : 0.0,
                   pace_wall_clock ? 1 : 0, pace_sleep, (long long)pace_reanchor);
    }
    mq_obs_event(obs, "runtime", 1, quit ? "quit" : "timeout");
    std::print("runtime: '{}' exited after {} frames (quit={}) world_trail=0x{:016x}\n",
               name, frame, quit ? 1 : 0, world_trail());
}

/* ---- Scene ---- */
int Scene::spawn() {
    const int idx = next_id++;                   // 单调：永不复用（陈旧 id 不会别名新实体）
    ents.emplace(idx, Entity{});
    return idx;
}
Entity* Scene::at(int index) {
    if (index < 0) return nullptr;
    auto it = ents.find(index);
    return it == ents.end() ? nullptr : &it->second;
}
void Scene::remove(int index) {
    auto it = ents.find(index);
    if (it != ents.end()) {
        for (auto& [t, b] : it->second.comps) if (eng) destroy_blob(*eng, t, b);
        ents.erase(it);
    }
    /* 不回收 id（单调分配）→ 之后 entity_valid(旧 id) 恒为 false */
}

bool Scene::deserialize(Engine& eng, const std::string& text) {
    text_ = text;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    int rc = mq_persist_apply_json(eng.reflect, eng.scene_type, dom, &desc);
    mq_json_free(dom);
    return rc == 0;
}

/* 加载实体树；prefab 就地展开。
 * docs/12 §7：**禁止循环实例化** —— 用 chain（当前展开链上的 prefab 路径）+ 深度上限检测，
 * 命中则报结构化错误并跳过该实例（绝不递归到栈溢出）。 */
static int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent,
                       std::vector<std::string>* chain, int depth);

/* 载入并展开一个 prefab：把它的根实体挂到 `parent` 下（-1 = 场景根）。
 * 供两处使用：
 *   · `scn.PrefabInstance` 组件 → 展开为**该实体的子实体**（包一层壳，便于整体变换/显隐）；
 *   · 实体的 `source` 简写 → **平级展开**（prefab 的根直接成为兄弟，不套壳）。
 * `name_prefix` 非空时给展开出的**根实体**名加前缀 —— 同一个 prefab 实例多次时，
 * 名字必须能区分，否则场景里会出现一堆同名实体（脚本按名查找就只会命中第一个）。
 * `chain` 为当前展开链（循环引用检测，docs/12 §7）；`depth` 限制递归层数。 */
static void expand_prefab(Engine& eng, Scene& sc, const std::string& src, int parent,
                          std::vector<std::string>* chain, int depth,
                          const std::string& name_prefix = std::string()) {
    if (chain) {
        for (const auto& c : *chain)
            if (c == src) {
                std::print(stderr, "runtime: cyclic prefab reference '{}' — skipped\n", src);
                mq_obs_event(eng.obs, "runtime", 3, "prefab_cycle");
                return;
            }
    }
    void* pd = nullptr; size_t pn = 0;
    const std::string ppath = "/" + src;
    if (mq_vfs_read(eng.vfs, ppath.c_str(), &pd, &pn) != MQ_VFS_OK) {
        std::print(stderr, "runtime: prefab not found '{}'\n", src);
        return;
    }
    std::string ptext((char*)pd, pn);
    mq_vfs_free(pd);
    mq_json* pdom = mq_json_parse(ptext.c_str(), ptext.size());
    if (!pdom) return;
    std::vector<std::string> child_chain = chain ? *chain : std::vector<std::string>{};
    child_chain.push_back(src);
    const mq_json* pents = mq_json_get(pdom, "entities");
    const uint32_t pcount = mq_json_len(pents);
    for (uint32_t i = 0; i < pcount; ++i) {
        int child = load_entity(eng, sc, mq_json_at(pents, i), parent, &child_chain, depth + 1);
        if (child >= 0 && !name_prefix.empty())
            if (Entity* ce = sc.at(child); ce && !ce->name.empty()) ce->name = name_prefix + ce->name;
    }
    mq_json_free(pdom);
    std::print("runtime: prefab '{}' instantiated under entity {}\n", src, parent);
}

static int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent,
                       std::vector<std::string>* chain = nullptr, int depth = 0) {
    if (depth > 64) {
        std::print(stderr, "runtime: prefab nesting too deep (>64), aborted at depth {}\n", depth);
        return -1;
    }
    const char* nm = mq_json_str(mq_json_get(e, "name"), nullptr);
    const mq_json* comps = mq_json_get(e, "components");
    uint32_t cn = mq_json_len(comps);
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
    const char* inline_src = mq_json_str(mq_json_get(e, "source"), nullptr);

    /* **平级展开**（`source` 简写，且自身无组件/无子节点）：纯展开指令，**不 spawn 壳实体**。
     * 用于"重复的平级对象"——坦克的 20 个履带印、5 个残骸本来是各自独立的根级实体，
     * 用 PrefabInstance 会多套一层壳（改变层级与 entity_find 语义，还白增 25 个空实体）。
     * 带 components/children 时仍走下面的"壳 + 子实体"路径（那种情况需要一个实体挂它们）。 */
    if (inline_src && *inline_src && cn == 0 && kn == 0) {
        const char* pfx = mq_json_str(mq_json_get(e, "name_prefix"), nullptr);
        expand_prefab(eng, sc, inline_src, parent, chain, depth, pfx ? pfx : "");
        return parent;                                  // 未创建实体 → 返回父（调用方按 <0 判错）
    }

    int idx = sc.spawn();
    if (idx < 0) return -1;
    Entity* ent = sc.at(idx);
    ent->parent = parent;
    if (nm) ent->name = nm;

    for (uint32_t i = 0; i < cn; ++i) {
        const mq_json* c = mq_json_at(comps, i);
        const char* tname = mq_json_str(mq_json_get(c, "type"), nullptr);
        if (!tname) continue;
        const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, tname);
        if (!d) { std::print("runtime: unknown component '{}'\n", tname); continue; }
        std::vector<uint8_t>& blob = ent->comps[d->type_id];
        if (!blob.empty()) destroy_blob(eng, d->type_id, blob);
        construct_blob(eng, d, blob);
        mq_persist_apply_json(eng.reflect, d, c, blob.data());
    }
    /* Prefab 实例（docs/12 §5）：按 source 载入 .prefab.json，就地展开为子实体。
     * detached=true 表示**已展开**（内容已是本场景的一部分），不再展开。 */
    for (auto& [tid, blob] : ent->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
        const std::string* src = read_str(d, blob.data(), "source");
        if (!src || src->empty()) continue;
        bool detached = false;
        if (const mq_field_desc* fd = mq_reflect_field(d, "detached")) detached = *(const bool*)(blob.data() + fd->offset);
        if (detached) continue;
        expand_prefab(eng, sc, *src, idx, chain, depth);
    }

    /* 实体的 `source` 简写（平级展开）：prefab 的**根实体直接成为本实体的兄弟**，不套壳。
     * `source` 与自身组件同时出现属于**误用**（组件会挂在一个多余的空壳上）：
     * 这里明确警告，但仍保留壳（否则那些组件无处安放）。 */
    if (inline_src && *inline_src) {
        std::print(stderr, "runtime: entity '{}' 同时给了 source 与 components/children —— "
                           "source 本意是平级展开、不套壳；此处为了容纳组件仍创建了壳实体\n",
                   nm ? nm : "?");
        const char* pfx = mq_json_str(mq_json_get(e, "name_prefix"), nullptr);
        expand_prefab(eng, sc, inline_src, idx, chain, depth, pfx ? pfx : "");
    }
    for (uint32_t i = 0; i < kn; ++i) load_entity(eng, sc, mq_json_at(ch, i), idx, chain, depth);
    return idx;
}

void Scene::instantiate(Engine& eng) {
    this->eng = &eng;
    mq_json* dom = mq_json_parse(text_.c_str(), text_.size());
    if (!dom) return;
    const mq_json* ents_json = mq_json_get(dom, "entities");
    uint32_t n = mq_json_len(ents_json);
    for (uint32_t i = 0; i < n; ++i) load_entity(eng, *this, mq_json_at(ents_json, i), -1);
    mq_json_free(dom);
    std::print("runtime: instantiated {} root entities\n", n);
}

/* ---- 世界快照（docs/10 §4）：实体/组件用二进制 codec 逐字段序列化。
 * 布局：u32 实体数；每实体 {i32 index, i32 parent, u32 name_len+name,
 *        u32 组件数, 每组件 {u32 type_id, u32 bytes, payload}}。
 * 转换按 index 排序 → 不依赖哈希迭代顺序（docs/10 D4）。 ---- */
void Scene::encode(Engine& eng, SnapshotWriter& w) {
    std::vector<int> idx;
    for (auto& [k, v] : ents) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    w.pod((uint32_t)idx.size());
    for (int k : idx) {
        Entity& e = ents[k];
        w.pod((int32_t)k);
        w.pod((int32_t)e.parent);
        w.pod((uint8_t)(e.active ? 1 : 0));
        w.pod(e.scale);
        w.pod(e.angle);
        w.pod(e.opacity);
        w.pod((int32_t)e.z_index);
        w.pod((uint32_t)e.name.size());
        w.bytes(e.name.data(), e.name.size());
        std::vector<uint32_t> tids;
        for (auto& [t, b] : e.comps) tids.push_back(t);
        std::sort(tids.begin(), tids.end());
        w.pod((uint32_t)tids.size());
        for (uint32_t t : tids) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, t);
            auto& blob = e.comps[t];
            std::vector<uint8_t> tmp(blob.size() + 64);
            size_t n = d ? mq_persist_encode(eng.reflect, d, blob.data(), tmp.data(), tmp.size()) : 0;
            w.pod(t);
            w.pod((uint32_t)n);
            if (n) w.bytes(tmp.data(), n);
        }
    }
}

size_t Scene::decode(Engine& eng, const uint8_t* in, size_t n) {
    this->eng = &eng;
    size_t pos = 0;
    auto take = [&](void* dst, size_t bytes) -> bool {
        if (pos + bytes > n) return false;
        std::memcpy(dst, in + pos, bytes);
        pos += bytes;
        return true;
    };
    uint32_t count = 0;
    if (!take(&count, 4) || count > (1u << 20)) return 0;
    for (auto& [k, ent] : ents)
        for (auto& [t, b] : ent.comps) destroy_blob(eng, t, b);
    ents.clear();
    for (uint32_t i = 0; i < count; ++i) {
        int32_t k = 0, parent = -1;
        uint32_t name_len = 0;
        uint8_t active = 1;
        float scale = 1.0F, angle = 0.0F, opacity = 1.0F;
        int32_t z_index = 0;
        if (!take(&k, 4) || !take(&parent, 4) || !take(&active, 1)
            || !take(&scale, 4) || !take(&angle, 4) || !take(&opacity, 4) || !take(&z_index, 4)
            || !take(&name_len, 4)) return 0;
        if (name_len > 4096 || pos + name_len > n) return 0;
        std::string nm((const char*)in + pos, name_len);
        pos += name_len;
        uint32_t comps = 0;
        if (!take(&comps, 4) || comps > 4096) return 0;
        Entity e;
        e.name = std::move(nm);
        e.parent = parent;
        e.active = active != 0;
        e.scale = scale;
        e.angle = angle;
        e.opacity = opacity;
        e.z_index = (int)z_index;
        if (k < 0) return 0;              // 实体索引按快照重建（脚本 id 稳定，docs/10 D4）
        for (uint32_t c = 0; c < comps; ++c) {
            uint32_t tid = 0, len = 0;
            if (!take(&tid, 4) || !take(&len, 4) || pos + len > n) return 0;
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            std::vector<uint8_t> blob;
            if (d) {
                construct_blob(eng, d, blob);          // 先构造（析构由 destroy_blob 负责）
                if (len && mq_persist_decode(eng.reflect, d, in + pos, len, blob.data()) < 0) {
                    destroy_blob(eng, tid, blob);
                    return 0;
                }
                e.comps[tid] = std::move(blob);
            }
            pos += len;
        }
        ents[k] = std::move(e);
        if (k + 1 > next_id) next_id = k + 1;
    }
    return pos;
}

/* 世界校验和：按 id/type 排序，哈希各组件的标量字段（不确定性来源如字符串/动态容器跳过）。
 * 反射驱动，与具体组件无关。用于确定性验证（docs/10）。 */
uint64_t Scene::checksum(Engine& eng) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    std::vector<int> idx;
    for (auto& [k, v] : ents) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    for (int k : idx) {
        Entity& e = ents[k];
        mix(&k, sizeof k); mix(&e.parent, sizeof e.parent); mix(&e.world_x, sizeof e.world_x); mix(&e.world_y, sizeof e.world_y);
        mix(&e.active, sizeof e.active); mix(&e.scale, sizeof e.scale); mix(&e.angle, sizeof e.angle);
        mix(&e.opacity, sizeof e.opacity); mix(&e.z_index, sizeof e.z_index);
        std::vector<uint32_t> tids;
        for (auto& [t, b] : e.comps) tids.push_back(t);
        std::sort(tids.begin(), tids.end());
        for (uint32_t t : tids) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, t);
            auto& blob = e.comps[t];
            mix(&t, sizeof t);
            if (!d) continue;
            for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                const mq_field_desc* f = &d->fields[fi];
                /* transient 字段是派生/装饰状态（排版度量、控件 phase、rect…）：
                 * 不进快照（docs/10 §4）→ 也不进校验和，否则回滚后必然不等 */
                if (f->attrs_flags & MQ_ATTR_TRANSIENT) continue;
                switch (f->kind) {
                    case MQ_TK_BOOL: case MQ_TK_I8: case MQ_TK_U8:
                    case MQ_TK_I16: case MQ_TK_U16: case MQ_TK_I32: case MQ_TK_U32:
                    case MQ_TK_I64: case MQ_TK_U64: case MQ_TK_F32: case MQ_TK_F64:
                        mix(blob.data() + f->offset, f->kind == MQ_TK_F32 ? 4 :
                            (f->kind == MQ_TK_F64 || f->kind == MQ_TK_I64 || f->kind == MQ_TK_U64) ? 8 :
                            (f->kind == MQ_TK_I16 || f->kind == MQ_TK_U16) ? 2 : 1);
                        break;
                    default: break;   // 字符串/容器/引用：跳过（非确定性来源）
                }
            }
        }
    }
    return h;
}

/* 组件语义由 roles 决定：transform2d / sprite —— 引擎不认具体组件类型。 */
/* 组件局部位置：按 role 读；兼容两种声明形态（扁平 x/y 或 math.Transform2D.local.position）。
 * 引擎不认具体组件类型 —— 只看 role 与标准字段路径（docs/12/17）。 */
static void read_local_pos(Engine& eng, const Entity& e, float* lx, float* ly) {
    *lx = e.world_x; *ly = e.world_y;                      /* 默认：实体自身（无 Transform2D 的纯 UI 节点） */
    for (auto& [tid, blob] : e.comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d || !(d->role & MQ_ROLE_TRANSFORM2D)) continue;
        { float tx = 0, ty = 0;
          if (read_f32(d, blob.data(), "x", &tx) && read_f32(d, blob.data(), "y", &ty)) { *lx = tx; *ly = ty; return; } }
        if (const mq_field_desc* fl = mq_reflect_field(d, "local")) {
            const mq_type_desc* lt = mq_reflect_type_by_id(eng.reflect, fl->type_id);
            if (lt) {
                const void* lp = blob.data() + fl->offset;
                if (const mq_field_desc* fp = mq_reflect_field(lt, "position")) {
                    const mq_type_desc* pt = mq_reflect_type_by_id(eng.reflect, fp->type_id);
                    if (pt) {
                        read_f32(pt, (const uint8_t*)lp + fp->offset, "x", lx);
                        read_f32(pt, (const uint8_t*)lp + fp->offset, "y", ly);
                    }
                }
            }
        }
    }
}

/* 世界矩阵自根向叶累积（确定性：父先于子；迭代顺序不影响结果，docs/10 D4）。 */
/* 展平（派生）：局部坐标在 Transform 组件里，世界坐标每帧重算到 Entity::world_x/y。
 * 顺序确定：根按 id 升序、子节点按 id 升序（栈序反转）→ 与快照/校验和的遍历顺序一致。 */
void Scene::flatten_world(Engine& eng) {
    std::vector<int> roots;
    for (auto& [idx, e] : ents) if (e.parent < 0 || !at(e.parent)) roots.push_back(idx);
    std::sort(roots.begin(), roots.end());
    std::vector<std::pair<int, int>> stack;      // (index, parent index)
    for (int r : roots) stack.emplace_back(r, -1);
    while (!stack.empty()) {
        auto [idx, pidx] = stack.back();
        stack.pop_back();
        Entity* e = at(idx);
        if (!e) continue;
        float lx = 0, ly = 0;
        read_local_pos(eng, *e, &lx, &ly);
        if (pidx >= 0) { if (Entity* pe = at(pidx)) { e->world_x = pe->world_x + lx; e->world_y = pe->world_y + ly; } }
        else { e->world_x = lx; e->world_y = ly; }
        std::vector<int> kids;
        for (auto& [k, c] : ents) if (c.parent == idx) kids.push_back(k);
        std::sort(kids.begin(), kids.end(), std::greater<int>());   // 栈序 → 最小 id 先处理（稳定）
        for (int k : kids) stack.emplace_back(k, idx);
    }
}

/* 节点属性沿父链累积（angle/scale/opacity）。
 *
 * 为什么需要它：`flatten_world` 只累积**位置**（父子坐标相加），而 `Entity` 上的
 * angle/scale/opacity 是"影响自身与子树"的节点属性（见 Entity 定义处注释）。
 * 渲染提取若不累积，子节点的朝向/缩放就不会跟随父节点 —— 炮塔装在车体上、
 * 挂点、动作部件这类层级结构就摆不对。
 *
 * 语义与 2D 直觉一致：
 *   · 角度**相加**（每级旋转都是相对父级的局部旋转）；
 *   · 缩放**相乘**；
 *   · 透明度**相乘**（父级半透明则整棵子树都更透明），并夹到 [0,1]；
 *   · active 由调用方判断（子节点不会"复活"父节点隐藏的内容）。
 * 自父向子累积的顺序与 flatten_world 的遍历一致（最小 id 先），结果稳定（docs/10 D4）。 */
void accumulate_node_attrs(Engine& eng, int idx, float& angle_deg, float& scale, float& opacity) {
    angle_deg = 0.0F; scale = 1.0F; opacity = 1.0F;
    if (!eng.scene) return;
    int guard = 0;
    for (int cur = idx; cur >= 0 && guard < 4096; ++guard) {
        Entity* e = eng.scene->at(cur);
        if (!e) break;
        angle_deg += e->angle;
        scale *= e->scale;
        opacity *= e->opacity;
        if (e->parent == cur) break;             // 自环保护（脏数据不应造成死循环）
        cur = e->parent;
    }
    if (opacity < 0.0F) opacity = 0.0F;
    if (opacity > 1.0F) opacity = 1.0F;
}

/* active 沿父链判定（docs/18：`node_set_active` 影响自身与子树）。
 * 之前只查了 `e.active` 自身 —— 父节点隐藏后子节点照常绘制（实测）。
 * 带自环保护：脏数据不应造成死循环。 */
bool node_active_effective(Engine& eng, int idx) {
    if (!eng.scene) return true;
    int guard = 0;
    for (int cur = idx; cur >= 0 && guard < 4096; ++guard) {
        Entity* e = eng.scene->at(cur);
        if (!e) break;
        if (!e->active) return false;
        if (e->parent == cur) break;                 // 自环保护
        cur = e->parent;
    }
    return true;
}

void Scene::extract(Engine& eng, std::vector<mq_renderer_item>& out) {
    /* 粒子（docs/15 §4）：只读提取，alpha 随寿命衰减；在实体图元之后按稳定顺序输出 */
    for (auto& p : eng.particles) {
        const float t = p.max_life > 0 ? p.life / p.max_life : 1.0F;
        mq_renderer_item it{};
        it.x = p.x; it.y = (float)eng.H - p.y; it.size = p.size;
        it.r = p.r; it.g = p.g; it.b = p.b;
        it.a = t < 1.0F ? 1.0F - t : 0.0F;
        it.texture = -1; it.layer = 0; it.z = 100000;      // 粒子在世界层最上
        out.push_back(it);
    }
    for (auto& [idx, e] : ents) {
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (!d) continue;
            /* UI 控件：overlay 层（layer=1）——屏幕空间绘制，不受相机影响（docs/13 §分层） */
            if (d->role & MQ_ROLE_UI_BUTTON) {
                float x = 0, y = 0, w = 64, h = 24, r = 0.2F, g = 0.25F, b = 0.35F;
                int32_t z = 0;
                read_f32(d, blob.data(), "x", &x);
                read_f32(d, blob.data(), "y", &y);
                read_f32(d, blob.data(), "width", &w);
                read_f32(d, blob.data(), "height", &h);
                read_f32(d, blob.data(), "r", &r);
                read_f32(d, blob.data(), "g", &g);
                read_f32(d, blob.data(), "b", &b);
                if (const mq_field_desc* fz = mq_reflect_field(d, "z")) z = *(const int32_t*)(blob.data() + fz->offset);
                (void)z;
                // 矩形控件以"中心 + 半径"图元表达（渲染后端当前只有 sprite 图元）
                mq_renderer_item it{};
                it.x = e.world_x + x + w * 0.5F; it.y = (float)eng.H - (e.world_y + y) - h * 0.5F;
                it.size = w * 0.5F > h * 0.5F ? w * 0.5F : h * 0.5F;
                it.r = r; it.g = g; it.b = b; it.a = 1.0F;
                it.texture = -1; it.layer = 1; it.z = z;
                out.push_back(it);
                continue;
            }
            if (d->role & (MQ_ROLE_UI_LABEL | MQ_ROLE_UI_TEXT)) {
                eng.extract_text(eng, idx, e, d, out, e.opacity);   // 真实字形渲染（docs/13/17）
                continue;
            }
            if (!(d->role & MQ_ROLE_SPRITE)) continue;
            /* 节点属性（active/angle/scale/opacity）此前被**整个忽略** ——
             * 实测：node_set_angle 不旋转、node_set_active(0) 不隐藏、node_set_scale /
             * node_set_opacity 对 sprite 无效。渲染项 ABI 本来就有这些字段
             *（rotation/a/half_w/half_h），只是提取时没填。 */
            if (!node_active_effective(eng, idx)) continue;   // 隐藏（含整棵子树）
            float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
            accumulate_node_attrs(eng, idx, node_angle, node_scale, node_opacity);
            float radius = 8, r = 1, g = 1, b = 1, sw = 0, sh = 0;
            read_f32(d, blob.data(), "radius", &radius);
            read_f32(d, blob.data(), "width", &sw);
            read_f32(d, blob.data(), "height", &sh);
            read_f32(d, blob.data(), "r", &r);
            read_f32(d, blob.data(), "g", &g);
            read_f32(d, blob.data(), "b", &b);
            /* 只读提取（docs/13 §8 / docs/15 §1）：纹理与材质已在装配期预解析并缓存，
             * 此处仅查表，**不得**触发加载或改写任何引擎状态。 */
            std::string texref;
            if (const std::string* tp = read_str(d, blob.data(), "texture")) texref = *tp;
            if (const std::string* mp = read_str(d, blob.data(), "material"))
                if (!mp->empty()) {
                    auto mi = eng.material_cache.find(*mp);
                    if (mi != eng.material_cache.end()) {
                        r = mi->second.base_color.x; g = mi->second.base_color.y; b = mi->second.base_color.z;
                        if (!mi->second.texture.empty()) texref = mi->second.texture;
                    }
                }
            int tex = -1;
            if (!texref.empty()) {
                auto ti = eng.tex_cache.find(texref);
                tex = ti == eng.tex_cache.end() ? -1 : ti->second;
            }
            mq_renderer_item it{};
            it.x = e.world_x; it.y = (float)eng.H - e.world_y;
            /* width/height > 0 → **矩形**图元（瓦片/方块/非方形贴图）；否则 radius 的圆。
             * 节点缩放对两者都生效（半径/半宽半高同时乘）。 */
            const bool rect = sw > 0.0F && sh > 0.0F;
            it.size = radius * node_scale;
            it.half_w = rect ? sw * 0.5F * node_scale : 0.0F;
            it.half_h = rect ? sh * 0.5F * node_scale : 0.0F;
            /* 角度：实体存度，渲染项要弧度，且 y 轴已翻到屏幕空间 → 取负保持视觉方向一致
             *（世界 y 向上、屏幕 y 向下；不取负的话"抬头"会变成"低头"）。 */
            it.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
            it.r = r; it.g = g; it.b = b;
            it.a = node_opacity;
            it.texture = tex;
            it.layer = 0;                                        // 世界层
            it.z = (int)idx;                                     // 稳定序（按实体 id，禁指针序）
            out.push_back(it);
        }
    }
}

/* ---- UI 命中测试（docs/19 §4）：z-order + 树深 稳定排序，命中最上层 ---- */
struct HitBest { int entity = -1; int z = INT32_MIN; int depth = -1; float lx = 0, ly = 0; };

void collect_hits(Engine& eng, int idx, int depth, float px, float py, HitBest& best) {
    Entity* e = eng.scene->at(idx);
    if (!e) return;
    /* 有效 z = 自身 z + 祖先 z（层级叠加，稳定） */
    int zsum = 0;
    /* ui.Control 提供盒模型（anchor/offset/size），交互性由 role 决定 */
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
        if (d->role & MQ_ROLE_UI_CONTROL) {
            float w = 0, h = 0;
            if (const mq_field_desc* f = mq_reflect_field(d, "offset_max")) {
                const float* v = (const float*)(blob.data() + f->offset);
                w = v[0]; h = v[1];
            }
            if (const mq_field_desc* f = mq_reflect_field(d, "offset_min")) {
                const float* v = (const float*)(blob.data() + f->offset);
                w -= v[0]; h -= v[1];
            }
            if (const mq_field_desc* fz = mq_reflect_field(d, "z_index"))
                zsum += *(const int32_t*)(blob.data() + fz->offset);
            bool interactive = false;
            for (auto& [t2, b2] : e->comps) {
                const mq_type_desc* d2 = mq_reflect_type_by_id(eng.reflect, t2);
                if (d2 && (d2->role & (MQ_ROLE_UI_BUTTON | MQ_ROLE_UI_SLIDER | MQ_ROLE_UI_CHECK)))
                    interactive = true;
            }
            if (interactive && w > 0 && h > 0) {
                /* 节点位置 = 控件中心（与精灵居中绘制一致）；输入已是世界坐标 → 直接比较 */
                const float cx = e->world_x, cy = e->world_y;
                const float bx = cx - w * 0.5F, by = cy - h * 0.5F;
                if (px >= bx && px <= bx + w && py >= by && py <= by + h) {
                    if (zsum > best.z || (zsum == best.z && depth > best.depth)) {
                        best.entity = idx; best.z = zsum; best.depth = depth;
                        best.lx = px - bx; best.ly = py - by;
                    }
                }
            } else if (e->active) {
                zsum += 0;                                            // 非交互：仅参与 z 叠加
            }
            continue;
        }
        bool ui = (d->role & (MQ_ROLE_UI_BUTTON | MQ_ROLE_UI_LABEL)) != 0;
        if (!ui) continue;
        float x = 0, y = 0, w = 0, h = 0;
        int32_t z = 0;
        read_f32(d, blob.data(), "x", &x);
        read_f32(d, blob.data(), "y", &y);
        read_f32(d, blob.data(), "width", &w);
        read_f32(d, blob.data(), "height", &h);
        if (const mq_field_desc* fz = mq_reflect_field(d, "z")) z = *(const int32_t*)(blob.data() + fz->offset);
        zsum += z;
        if (!(d->role & MQ_ROLE_UI_BUTTON)) continue;        // 仅可交互控件参与命中
        /* 本地 (x,y) = 世界坐标下的矩形左下角 → 矩形 = [x, x+w] × [y, y+h]（y 向上） */
        const float bw = w > 0 ? w : 64, bh = h > 0 ? h : 24;
        const float bx = e->world_x + x, by = e->world_y + y;
        if (px >= bx && px <= bx + bw && py >= by && py <= by + bh) {
            const int score = zsum;
            /* 稳定：z 大者优先；同 z 取树深大者（后绘制者在上） */
            if (score > best.z || (score == best.z && depth > best.depth)) {
                best.entity = idx; best.z = score; best.depth = depth;
                best.lx = px - bx; best.ly = py - by;
            }
        }
    }
    for (auto& [k, child] : eng.scene->ents)
        if (child.parent == idx) collect_hits(eng, k, depth + 1, px, py, best);
}

/* 命中查询（事件与控制面/脚本共用一套）→ 行为一致（docs/19 §4）。 */
int ui_hit_entity(Engine& eng, float px, float py, mq_input_target* out) {
    if (out) { out->entity = -1; out->handle = (uint32_t)-1; out->depth = 0; out->z = 0; }
    if (!eng.scene || !eng.scene->count()) return -1;
    HitBest best;
    for (auto& [k, e] : eng.scene->ents)
        if (e.parent < 0) collect_hits(eng, k, 0, px, py, best);
    if (best.entity < 0) return -1;
    if (out) {
        out->entity = best.entity; out->handle = (uint32_t)best.entity;
        out->depth = best.depth; out->z = best.z;
        out->local_x = best.lx; out->local_y = best.ly;
    }
    return best.entity;
}

int runtime_hit_test(void* user, const mq_input_event* ev, mq_input_target* out) {
    /* 输入事件坐标在进入引擎前已统一为**世界坐标**（设备事件在平台边界映射、
     * 注入/模拟事件按世界坐标给出）→ 这里直接按世界矩形命中（不再二次映射）。 */
    auto* eng = (Engine*)user;
    out->entity = -1; out->handle = (uint32_t)-1;
    if (!eng->scene || !eng->scene->count()) return 0;
    return ui_hit_entity(*eng, ev->x, ev->y, out) >= 0 ? 1 : 0;
}

int runtime_parent(void* user, int entity) {
    Entity* e = ((Engine*)user)->scene->at(entity);
    return e ? e->parent : -1;
}

/* 事件 → 脚本回调：映射与参数个数全部由 DSL 声明、codegen 生成（docs/14 §4、docs/19 §8）。 */
/* 事件 kind → 处理器名（生成的事件表是单一真相）→ 总线广播。
 * 多脚本/多语言下这是**唯一**的事件入口：按声明顺序投给定义了该处理器的每个 VM。 */
int dispatch_event(Engine& eng, int kind, double a, double b, double c, const char* text) {
    if (!eng.bus) return 0;
    uint32_t n = 0;
    const mq_event_desc* tab = mq_event_table(&n);
    if (!tab || kind < 0 || (uint32_t)kind >= n) return 0;
    return mq_script_bus_call3(eng.bus, tab[kind].name, a, b, c, text);
}

void call_script_event(Engine& eng, const mq_input_event* ev) {
    const int kind = mq_event_for_input(ev->type);
    if (kind < 0) return;
    const char* text = ev->type == MQ_INPUT_TEXT ? ev->text : nullptr;
    /* 载荷按事件类型选择（生成的回调签名见 callbacks 段）：
     *   指针：on_pointer_*(target, x, y)
     *   **按键：on_key_down/up(target, key)** —— key 走第 2 个参数
     *   文本：on_text(target, text)
     * 此前按键误把指针 x 当作 key 传入（key 恒为 0）→ 键盘驱动的游戏无法工作。 */
    const bool keyish = ev->type == MQ_INPUT_KEY_DOWN || ev->type == MQ_INPUT_KEY_UP;
    dispatch_event(eng, kind, (double)ev->target,
                      keyish ? (double)ev->key : (double)ev->x,
                      keyish ? 0.0 : (double)ev->y, text);
}

/* 窗口像素 → **世界坐标**：等比缩放 + 居中留边（SDL_RenderSetLogicalSize 同规则）+ **y 轴翻转**。
 * 窗口/SDL 事件是 y 向下；引擎世界与脚本是 y 向上（docs/12 §坐标、docs/19 §4）——
 * 漏掉翻转会导致点击位置上下镜像（曾经的真实缺陷）。 */
void window_to_world(const Engine& eng, float* x, float* y) {
    /* 窗口设备像素 → 设计坐标（由画布策略决定缩放/留边；多视图时除以视图数落到单视图） */
    float dx = 0.0F, dy = 0.0F;
    if (mq_canvas_window_to_design(&eng.canvas, *x, *y, &dx, &dy) != 0) return;
    const int views = eng.nviews > 0 ? eng.nviews : 1;
    *x = dx / (float)views;
    *y = (float)eng.H - dy;                         // 设计 y 向下 → 世界 y 向上
}

/* 设备事件：位置映射 + 方向量翻转（指针位移 dy 与滚轮 scroll_y 同属 y 轴方向） */
void device_event_to_world(const Engine& eng, mq_platform_pointer_event* p) {
    window_to_world(eng, &p->x, &p->y);
    p->dy = -p->dy;
    p->scroll_y = -p->scroll_y;
}

int runtime_input_handler(void* user, const mq_input_event* ev, mq_input_target* target) {
    auto* eng = (Engine*)user;
    /* 引擎内部一律**世界坐标**：设备事件在平台边界转换（见 step_frame），
     * 注入/模拟事件（mq_input_inject）按世界坐标给出 —— 命中测试、组件语义、脚本回调一致。 */
    eng->ev = ev;
    if (ev->type == MQ_INPUT_POINTER_MOVE || ev->type == MQ_INPUT_POINTER_DOWN ||
        ev->type == MQ_INPUT_POINTER_UP || ev->type == MQ_INPUT_CLICK) {
        eng->mouse_x = ev->x; eng->mouse_y = ev->y;
    }
    if (ev->type == MQ_INPUT_POINTER_MOVE && target->entity != eng->hover) {
        eng->hover = target->entity;                    // 悬停态（脚本经 ui_hover 读）
    }
    if (target->entity >= 0) {
        /* 控件状态写回（按钮按下/悬停；docs/19 §8：组件订阅语义事件） */
        Entity* e = eng->scene->at(target->entity);
        if (e && ev->type == MQ_INPUT_POINTER_DOWN)
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
                if (const mq_field_desc* fp = mq_reflect_field(d, "pressed"))
                    *(bool*)(blob.data() + fp->offset) = true;
            }
        if (e && ev->type == MQ_INPUT_POINTER_UP)
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
                if (const mq_field_desc* fp = mq_reflect_field(d, "pressed"))
                    *(bool*)(blob.data() + fp->offset) = false;
            }
        /* Slider：按下进入拖拽，拖动中按水平比例改值（滑块语义） */
        if (e) {
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_SLIDER)) continue;
                if (ev->type == MQ_INPUT_POINTER_DOWN) {
                    write_num(d, blob.data(), "dragging", 1.0);
                    slider_apply_pointer(*eng, target->entity, d, blob.data(), ev->x);
                } else if (ev->type == MQ_INPUT_POINTER_UP) {
                    write_num(d, blob.data(), "dragging", 0.0);
                    slider_apply_pointer(*eng, target->entity, d, blob.data(), ev->x);
                } else if (ev->type == MQ_INPUT_POINTER_MOVE) {
                    if (read_num(d, blob.data(), "dragging", 0.0F) != 0.0F)
                        slider_apply_pointer(*eng, target->entity, d, blob.data(), ev->x);
                }
            }
            /* CheckBox：点击切换 + toggled 事件（开关语义） */
            if (ev->type == MQ_INPUT_CLICK) {
                for (auto& [tid, blob] : e->comps) {
                    const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
                    if (!d || !(d->role & MQ_ROLE_UI_CHECK)) continue;
                    if (read_num(d, blob.data(), "disabled", 0.0F) != 0.0F) continue;
                    const bool now = read_num(d, blob.data(), "checked", 0.0F) == 0.0F;
                    write_num(d, blob.data(), "checked", now ? 1.0 : 0.0);
                    host::node_emit_number(eng, target->entity, "toggled", now ? 1.0F : 0.0F);
                }
            }
        }
    }
    call_script_event(*eng, ev);          // 分发表由生成代码提供
    if (eng->trace_hits)
        std::print("[input] type={} phase={} target={} pos=({:.1f},{:.1f})\n",
                   (int)ev->type, (int)ev->phase, target->entity, ev->x, ev->y);
    return 0;   // 不消费：脚本/组件各自决定
}

/* cfg：反射读 scn.Scene 标量字段。 */
double cfg_field(Engine* eng, const char* path) {
    const mq_type_desc* t = eng->scene_type;
    void* obj = &eng->scene->desc;
    const char* seg = path;
    const mq_field_desc* f = nullptr; void* parent = obj;
    while (*seg) {
        const char* dot = seg; while (*dot && *dot != '.') ++dot;
        std::string name(seg, (size_t)(dot - seg));
        f = mq_reflect_field(t, name.c_str());
        if (!f) return 0.0;
        if (*dot == '\0') { parent = obj; break; }
        if (f->kind != MQ_TK_STRUCT) return 0.0;
        obj = (uint8_t*)obj + f->offset;
        t = mq_reflect_type_by_id(eng->reflect, f->type_id);
        if (!t) return 0.0;
        seg = dot + 1;
    }
    double v = 0.0; mq_reflect_get_f64(eng->reflect, parent, f, &v); return v;
}

std::vector<uint8_t>* comp_by_role(Engine& eng, int id, uint32_t role) {
    Entity* e = eng.scene->at(id);
    if (!e) return nullptr;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (d && (d->role & role)) return &blob;
    }
    return nullptr;
}

/* 直接按类型名找组件（getComponent("Type") 语义；支持短名与 fq 名）。 */
std::vector<uint8_t>* comp_by_name(Engine& eng, int id, const char* type_name,
                                   const mq_type_desc** out_desc) {
    Entity* e = eng.scene->at(id);
    if (!e || !type_name) return nullptr;
    const mq_type_desc* want = mq_reflect_type_by_name(eng.reflect, type_name);
    if (!want) {                                   // 允许短名（"Sprite" → "scn.Sprite"）
        char fq[128];
        std::snprintf(fq, sizeof fq, "scn.%s", type_name);
        want = mq_reflect_type_by_name(eng.reflect, fq);
        if (!want) { std::snprintf(fq, sizeof fq, "ui.%s", type_name);
                     want = mq_reflect_type_by_name(eng.reflect, fq); }
        if (!want) { std::snprintf(fq, sizeof fq, "physics.%s", type_name);
                     want = mq_reflect_type_by_name(eng.reflect, fq); }
    }
    if (!want) return nullptr;
    auto it = e->comps.find(want->type_id);
    if (it == e->comps.end()) return nullptr;
    if (out_desc) *out_desc = want;
    return &it->second;
}

float read_num(const mq_type_desc* d, const void* base, const char* field, float def) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (!f) return def;
    switch (f->kind) {
        case MQ_TK_F32:  return *(const float*)((const uint8_t*)base + f->offset);
        case MQ_TK_F64:  return (float)*(const double*)((const uint8_t*)base + f->offset);
        case MQ_TK_I8:   return (float)*(const int8_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U8:   return (float)*(const uint8_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_I16:  return (float)*(const int16_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U16:  return (float)*(const uint16_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_I32:  return (float)*(const int32_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U32:  return (float)*(const uint32_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_I64:  return (float)*(const int64_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_U64:  return (float)*(const uint64_t*)((const uint8_t*)base + f->offset);
        case MQ_TK_BOOL: return *(const bool*)((const uint8_t*)base + f->offset) ? 1.0F : 0.0F;
        case MQ_TK_ENUM: {
            /* 枚举：按底层整型宽度读（BodyType::Dynamic 等，docs/06） */
            const uint8_t* p = (const uint8_t*)base + f->offset;
            if (f->count == 1) return (float)*(const int8_t*)p;
            if (f->count == 2) return (float)*(const int16_t*)p;
            return (float)*(const int32_t*)p;
        }
        default: return def;
    }
}

bool write_num(const mq_type_desc* d, void* base, const char* field, double v) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (!f) return false;
    uint8_t* p = (uint8_t*)base + f->offset;
    switch (f->kind) {
        case MQ_TK_ENUM: {
            if (f->count == 1) { *(int8_t*)p = (int8_t)v; return true; }
            if (f->count == 2) { *(int16_t*)p = (int16_t)v; return true; }
            *(int32_t*)p = (int32_t)v; return true;
        }
        case MQ_TK_F32:  *(float*)p = (float)v; return true;
        case MQ_TK_F64:  *(double*)p = v; return true;
        case MQ_TK_I8:   *(int8_t*)p = (int8_t)v; return true;
        case MQ_TK_U8:   *(uint8_t*)p = (uint8_t)v; return true;
        case MQ_TK_I16:  *(int16_t*)p = (int16_t)v; return true;
        case MQ_TK_U16:  *(uint16_t*)p = (uint16_t)v; return true;
        case MQ_TK_I32:  *(int32_t*)p = (int32_t)v; return true;
        case MQ_TK_U32:  *(uint32_t*)p = (uint32_t)v; return true;
        case MQ_TK_I64:  *(int64_t*)p = (int64_t)v; return true;
        case MQ_TK_U64:  *(uint64_t*)p = (uint64_t)v; return true;
        case MQ_TK_BOOL: *(bool*)p = v != 0.0; return true;
        default: return false;
    }
}

/* Slider：把指针 x 映射为值（Slider 的 progress 语义）。 */
void slider_apply_pointer(Engine& eng, int id, const mq_type_desc* d, void* base, float px) {
    Entity* e = eng.scene->at(id);
    if (!e) return;
    float w = 100.0F;
    if (auto* ctrl = comp_by_role(eng, id, MQ_ROLE_UI_CONTROL)) {
        const mq_type_desc* cd = mq_reflect_type_by_name(eng.reflect, "ui.Control");
        if (cd) w = read_num(cd, ctrl->data(), "offset_max", 100.0F);
    }
    if (w <= 0.0F) w = 1.0F;
    float t = (px - (e->world_x - w * 0.5F)) / w;
    t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
    if (read_num(d, base, "invert", 0.0F) != 0.0F) t = 1.0F - t;
    const float lo = read_num(d, base, "min_value", 0.0F);
    const float hi = read_num(d, base, "max_value", 1.0F);
    float v = lo + (hi - lo) * t;
    const float step = read_num(d, base, "step", 0.0F);
    if (step > 0.0F) v = lo + std::round((v - lo) / step) * step;
    write_num(d, base, "value", v);
    write_num(d, base, "ratio", t);
    host::node_emit_number(&eng, id, "changed", v);
}


/* 文本：装配期选字体
 *   MAQUE_FONT → **项目字体（scenes/fonts/ 下首个 .ttf，仓库自带含中文）** → 常见系统字体
 *   → 内置 5x7 位图字体。
 * AGENTS.md 一直这么写，但实现里缺了"项目字体"这一档：只查 MAQUE_FONT 与 Linux 系统路径，
 * 于是 Windows（没有 /usr/share/fonts）**恒回退内置 5x7**，中文与 CJK 全丢，
 * 且依赖真实字宽的排版用例（text_e2e 的折行行数）必然失败。
 * 图集纹理按字号惰性创建并缓存（确定性：同字号同图集，docs/10）。 */
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
void Engine::extract_text(Engine& eng, int idx, Entity& e, const mq_type_desc* d,
                          std::vector<mq_renderer_item>& out, float inherit_alpha) {
    std::string text, font_path_comp;
    float size = 16.0F, r = 1, g = 1, b = 1, a = 1, max_w = 0, line_spacing = 0;
    int align = 0, wrap = 0;
    if (const std::string* tp = read_str(d, e.comps[d->type_id].data(), "text")) text = *tp;
    if (const std::string* fp = read_str(d, e.comps[d->type_id].data(), "font")) font_path_comp = *fp;
    read_f32(d, e.comps[d->type_id].data(), "font_size", &size);
    read_f32(d, e.comps[d->type_id].data(), "max_width", &max_w);
    read_f32(d, e.comps[d->type_id].data(), "line_spacing", &line_spacing);
    /* 颜色：scn.Label 用 r/g/b/a；ui.* 用 color（Vec4） */
    if (const mq_field_desc* fc = mq_reflect_field(d, "color")) {
        const float* v = (const float*)(e.comps[d->type_id].data() + fc->offset);
        r = v[0]; g = v[1]; b = v[2]; a = v[3];
    } else {
        read_f32(d, e.comps[d->type_id].data(), "r", &r);
        read_f32(d, e.comps[d->type_id].data(), "g", &g);
        read_f32(d, e.comps[d->type_id].data(), "b", &b);
        read_f32(d, e.comps[d->type_id].data(), "a", &a);
    }
    align = (int)read_num(d, e.comps[d->type_id].data(), "align", 0.0F);
    if (!mq_reflect_field(d, "align")) align = (int)read_num(d, e.comps[d->type_id].data(), "align_h", 0.0F);
    wrap = (int)read_num(d, e.comps[d->type_id].data(), "wrap", 0.0F);
    if (!mq_reflect_field(d, "wrap")) wrap = (int)read_num(d, e.comps[d->type_id].data(), "wrap_mode", 0.0F);
    /* parse_tags=false：把标记当纯文本（转义 '<'） */
    if (const mq_field_desc* fpt = mq_reflect_field(d, "parse_tags")) {
        if (!*(const bool*)(e.comps[d->type_id].data() + fpt->offset)) {
            std::string esc;
            for (char c : text) { if (c == '<') esc += "&lt;"; else esc.push_back(c); }
            text = esc;
        }
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
    st.device_scale = eng.ss;   /* 字形按**渲染目标像素**栅格化（目标 = 设计 × ss，见 Engine::ss） */
    st.pixel_snap = 1;                                  /* pixel-perfect：字形对齐整数像素 */
    st.line_spacing = line_spacing;
    st.wrap = (uint32_t)(wrap < 0 ? 0 : wrap);
    st.align = (uint32_t)(align < 0 ? 0 : align);
    st.max_width = max_w;
    st.r = r; st.g = g; st.b = b; st.a = a * inherit_alpha;

    /* 先测量（写回 transient 字段，供脚本/控制面读取），再排版 */
    /* 字体：标签的 font 字段优先（相对路径按 CWD/场景目录解析），否则默认字体 */
    mq_font* label_font = eng.text_font_for(font_path_comp);
    mq_text_metrics m{};
    const int mrc = mq_text_measure(label_font, &st, text.c_str(), &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text] id={} type={} len={} size={:.0f} wrap={} rc={} lines={}\n",
                   idx, d->name, text.size(), st.font_size, st.wrap, mrc, m.line_count);
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
    /* 度量写回：换算回**设计像素**（与节点坐标同一坐标系；设备像素只用于栅格化） */
    const float inv = st.device_scale > 0.0F ? 1.0F / st.device_scale : 1.0F;
    if (const mq_field_desc* fw = mq_reflect_field(d, "text_w"))
        *(float*)(e.comps[d->type_id].data() + fw->offset) = m.width * inv;
    if (const mq_field_desc* fh = mq_reflect_field(d, "text_h"))
        *(float*)(e.comps[d->type_id].data() + fh->offset) = m.height * inv;
    if (const mq_field_desc* flc = mq_reflect_field(d, "line_count"))
        *(int32_t*)(e.comps[d->type_id].data() + flc->offset) = (int32_t)m.line_count;
    if (const mq_field_desc* fts = mq_reflect_field(d, "text_size")) {
        float* v = (float*)(e.comps[d->type_id].data() + fts->offset);
        v[0] = m.width * inv; v[1] = m.height * inv;
    }
    /* render.RichText 用 content_size 承载实测尺寸 */
    if (const mq_field_desc* fcs = mq_reflect_field(d, "content_size")) {
        float* v = (float*)(e.comps[d->type_id].data() + fcs->offset);
        v[0] = m.width * inv; v[1] = m.height * inv;
    }

    /* 锚点：scn.Label 的 x/y 为左上角偏移；ui.* 以节点位置为左上角 */
    float ox = 0, oy = 0;
    read_f32(d, e.comps[d->type_id].data(), "x", &ox);
    read_f32(d, e.comps[d->type_id].data(), "y", &oy);
    /* 阶段 1：只收集（纹理与 UV 留到 text_flush，按**最终**图集计算） */
    for (uint32_t i = 0; i < n; ++i) {
        const mq_text_glyph& gl = glyphs[i];
        if (gl.w <= 0.0F || gl.h <= 0.0F) continue;
        /* 位图尺寸/位置都是**设备像素**（按设备字号栅格化）→ 换算回设计像素再落位，
         * 否则窗口放大 N 倍时文字会随之变成 N 倍大。 */
        const float half_w = gl.w * 0.5F * inv, half_h = gl.h * 0.5F * inv;
        PendingQuad q{};
        q.font = label_font; q.size = gl.device_size;
        q.ax = gl.ax; q.ay = gl.ay;
        q.tw = (int)gl.w; q.th = (int)gl.h;
        q.cp = gl.codepoint;
        /* 锚点 = 位图左上角 (x0,y0)；排版 y 向下、屏幕 y 向下 → 基准翻转后相加 */
        q.x = e.world_x + ox + gl.x0 * inv + half_w;
        q.y = (float)eng.H - (e.world_y + oy) + gl.y0 * inv + half_h;
        q.half_w = half_w; q.half_h = half_h;
        q.r = gl.r; q.g = gl.g; q.b = gl.b; q.a = gl.a;
        q.z = 100 + (int)i;
        eng.pending_quads.push_back(q);
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
        it.snap = 1;                                            /* 左上角吸附整数像素 */
        it.r = q.r; it.g = q.g; it.b = q.b; it.a = q.a;
        it.texture = tex;
        it.uv0 = (float)q.ax / (float)aw;
        it.uv1 = (float)q.ay / (float)ah;
        it.uv2 = (float)(q.ax + q.tw) / (float)aw;
        it.uv3 = (float)(q.ay + q.th) / (float)ah;
        it.layer = 1; it.z = q.z;
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
static bool read_sim_scenario(mq_vfs* vfs, const char* path, std::string& out) {
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

/* ---- 键值存储（docs/25）----
 * 模式与根：MAQUE_STORAGE（spec）> 默认 file://<场景目录>/.maque（远程场景/只读目录由模块退到 XDG 数据目录）；
 * 命名空间：MAQUE_UID / MAQUE_SESSION（缺省 "default" / "main"）→ 键空间 <uid>/<session>/<key>。
 * 切场景时按新场景目录重建（旧实例销毁）。 */
void Engine::storage_setup() {
    if (storage) { mq_storage_destroy(storage); storage = nullptr; }
    const char* spec_env = std::getenv("MAQUE_STORAGE");
    const char* uid = std::getenv("MAQUE_UID");
    const char* session = std::getenv("MAQUE_SESSION");

    const bool remote = scn_full_path.rfind("http://", 0) == 0 || scn_full_path.rfind("https://", 0) == 0;
    std::string spec;
    if (spec_env && *spec_env) spec = spec_env;
    else if (!remote) {
        /* 默认：**场景目录**旁的 .maque（跑 `scenes/sokoban/sokoban.scn` → scenes/sokoban/.maque；
         * 场景就在 CWD 时 → <CWD>/.maque）。远程场景没有可写目录 → 交给模块的 XDG 兜底。 */
        std::error_code ec;
        const auto abs = std::filesystem::absolute(std::filesystem::path(scn_dir.empty() ? "." : scn_dir) / ".maque", ec);
        spec = "file://" + (ec ? (std::filesystem::path(scn_dir.empty() ? "." : scn_dir) / ".maque").string() : abs.string());
    } else {
        spec = "file://";                        // 远程场景：模块用 XDG 数据目录兜底（可持久）
    }

    storage = mq_storage_create(spec.c_str(), uid, session);
    if (!storage) {
        std::print(stderr, "runtime: storage 创建失败（spec='{}'）→ 回退 mem://（不落盘）\n", spec);
        storage = mq_storage_create("mem://", uid, session);
    }
    if (storage) {
        std::print("runtime: storage mode='{}' root='{}' ns='{}'\n",
                   mq_storage_mode(storage), mq_storage_root(storage), mq_storage_ns(storage));
    }
}

/* ---- 画布与分辨率策略（docs/17）----
 * 设计分辨率 = 场景 width/height（或 ui.Canvas.design_*）；渲染目标即设计分辨率；
 * 画布策略决定"设计 → 绘制区（设备像素）"的缩放与留边，供 呈现 / 输入映射 / 文字栅格化 三方共用。
 * 没有 ui.Canvas 实体时按 ShowAll + 居中兜底（与既有行为一致）。 */
void Engine::canvas_setup() {
    canvas_entity = -1;
    if (!scene) return;
    for (auto& [idx, e] : scene->ents) {
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
            canvas_entity = idx;                       /* 升序首个画布 */
            canvas_active = true;
            std::print("runtime: canvas on entity {} (design {}x{})\n", idx,
                       read_num(d, blob.data(), "design_width", 0.0F),
                       read_num(d, blob.data(), "design_height", 0.0F));
            break;
        }
        if (canvas_entity >= 0) break;
    }
}

void Engine::canvas_resolve() {
    mq_canvas_desc desc{};
    desc.struct_size = (uint32_t)sizeof desc;
    /* 设计分辨率：画布组件优先，其次场景尺寸（向后兼容） */
    desc.design_w = (float)W;
    desc.design_h = (float)H;
    desc.fit = MQ_CANVAS_FIT_SHOW_ALL;
    desc.align = MQ_CANVAS_ALIGN_CENTER;
    desc.dpi_scale = 1.0F;
    desc.safe_left = canvas_safe[0]; desc.safe_top = canvas_safe[1];
    desc.safe_right = canvas_safe[2]; desc.safe_bottom = canvas_safe[3];
    if (canvas_entity >= 0) {
        Entity* e = scene->at(canvas_entity);
        if (e) {
            for (auto& [tid, blob] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
                const float dw = read_num(d, blob.data(), "design_width", 0.0F);
                const float dh = read_num(d, blob.data(), "design_height", 0.0F);
                if (dw > 0.0F && dh > 0.0F) { desc.design_w = dw; desc.design_h = dh; }
                desc.fit = (int32_t)read_num(d, blob.data(), "fit_mode", (float)MQ_CANVAS_FIT_SHOW_ALL);
                desc.align = (int32_t)read_num(d, blob.data(), "align", (float)MQ_CANVAS_ALIGN_CENTER);
                desc.min_scale = read_num(d, blob.data(), "min_scale", 0.0F);
                desc.max_scale = read_num(d, blob.data(), "max_scale", 0.0F);
                const bool dpi = read_num(d, blob.data(), "dpi_scale", 1.0F) != 0.0F;
                desc.dpi_scale = (dpi && platform && window) ? mq_platform_content_scale(platform) : 1.0F;
                /* 写回解析结果（transient）：脚本/控制面可读 */
                break;
            }
        }
    } else if (platform && window) {
        desc.dpi_scale = mq_platform_content_scale(platform);
    }
    /* 绘制区 = 窗口**设备像素**（HiDPI 下大于逻辑尺寸）；
     * 无窗口（headless/CI）→ 取渲染目标的**单视图**尺寸（= 设计分辨率，缩放 1） */
    const int views = nviews > 0 ? nviews : 1;
    desc.window_w = (float)(TW / views);
    desc.window_h = (float)H;
    if (platform && window) {
        int32_t dw = 0, dh = 0;
        mq_platform_drawable_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) { desc.window_w = (float)dw; desc.window_h = (float)dh; }
    }
    const int rc = mq_canvas_resolve(&desc, &canvas);
    if (rc != 0 && std::getenv("MAQUE_TRACE_CANVAS"))
        std::print(stderr, "runtime: canvas resolve fallback (rc={})\n", rc);
    /* 回写 transient 字段 */
    if (canvas_entity >= 0) {
        Entity* e = scene->at(canvas_entity);
        if (e) {
            std::vector<uint8_t>* blob = nullptr;
            const mq_type_desc* d = nullptr;
            for (auto& [tid, b] : e->comps) {
                const mq_type_desc* t = mq_reflect_type_by_id(reflect, tid);
                if (t && (t->role & MQ_ROLE_UI_CANVAS)) { blob = &b; d = t; break; }
            }
            if (blob && d) {
                write_num(d, blob->data(), "scale", canvas.scale);
                write_num(d, blob->data(), "scale_x", canvas.scale_x);
                write_num(d, blob->data(), "scale_y", canvas.scale_y);
                write_num(d, blob->data(), "offset_x", canvas.offset_x);
                write_num(d, blob->data(), "offset_y", canvas.offset_y);
                write_num(d, blob->data(), "viewport_w", canvas.viewport_w);
                write_num(d, blob->data(), "viewport_h", canvas.viewport_h);
                write_num(d, blob->data(), "window_w", canvas.window_w);
                write_num(d, blob->data(), "window_h", canvas.window_h);
                write_num(d, blob->data(), "device_scale", canvas.device_scale);
            }
        }
    }
    if (std::getenv("MAQUE_TRACE_CANVAS"))
        std::print("[canvas] design={}x{} win={}x{} fit={} scale=({:.3f},{:.3f}) off=({:.1f},{:.1f}) dpi={:.2f}\n",
                   desc.design_w, desc.design_h, desc.window_w, desc.window_h, desc.fit,
                   canvas.scale_x, canvas.scale_y, canvas.offset_x, canvas.offset_y, canvas.device_scale);
}

/* ---- 输入模拟器（docs/19）----
 * MAQUE_SIM=<file.sim.json> 或控制面 Input.sim_load 载入场景；每帧把**到期**事件注入输入流
 * （与设备事件同一条路径：mq_input_inject → 输入模块 → 命中测试 → 脚本回调），
 * 因此同样被录制/回放覆盖（docs/10 D3：输入即数据）。 */
void Engine::sim_setup() {
    if (!sim) sim = mq_sim_create();
    const char* path = std::getenv("MAQUE_SIM");
    const char* clicks = std::getenv("MAQUE_SIM_CLICK");
    std::string json;
    if (path && *path) {
        if (!read_sim_scenario(vfs, path, json))
            std::print(stderr, "runtime: sim scenario not found: {}\n", path);
        sim_path = path;
    } else if (clicks && *clicks) {
        /* 简写：MAQUE_SIM_CLICK="frame:x:y[;frame:x:y]" → 内联场景 */
        json = "{\"name\":\"inline MAQUE_SIM_CLICK\",\"space\":\"window\",\"events\":[";
        const char* q = clicks; bool first = true;
        while (*q) {
            long f = 0; int cx = 0, cy = 0;
            if (std::sscanf(q, "%ld:%d:%d", &f, &cx, &cy) == 3) {
                if (!first) json += ",";
                json += "{\"frame\":" + std::to_string(f) + ",\"type\":\"click\",\"x\":" +
                        std::to_string(cx) + ",\"y\":" + std::to_string(cy) + "}";
                first = false;
            }
            const char* semi = std::strchr(q, ';');
            if (!semi) break;
            q = semi + 1;
        }
        json += "]}";
        sim_path = "(inline MAQUE_SIM_CLICK)";
    }
    if (json.empty()) return;
    char err[256] = { 0 };
    const int rc = mq_sim_load_json(sim, json.c_str(), json.size(), err, sizeof err);
    if (rc != 0) {
        std::print(stderr, "runtime: sim load failed ({}): {}\n", rc, err);
        return;
    }
    sim_active = true;
    std::print("runtime: sim '{}' loaded: {} event(s), last frame {}, loop={}\n",
               sim_path, mq_sim_count(sim), mq_sim_last_frame(sim), mq_sim_loop(sim));
}

void Engine::sim_feed(uint64_t frame) {
    if (!sim_active || !sim || !input) return;
    if (mq_input_replaying(input)) return;          // 回放只吃记录（设备/模拟输入都被忽略）
    mq_sim_event batch[16];
    uint32_t n = 0;
    uint32_t guard = 0;
    do {
        n = 0;
        if (mq_sim_due(sim, frame, batch, 16, &n) != 0 && n == 0) break;
        for (uint32_t i = 0; i < n; ++i) {
            const mq_sim_event& se = batch[i];
            mq_input_event ev{};
            ev.pointer_id = 0;
            ev.pointer_type = 0;                     // 鼠标
            ev.button = se.button;
            /* 键码统一到引擎规范键空间（MQ_KEY_* + 大写 ASCII）。
             * 为什么必须做：真实设备事件在平台边界经 key_from_sdl 归一化，而 .sim.json 里
             * 人手写的是 SDL 风格码（空格 32、'w' 119）。不归一化则"同一串按键走设备路径与
             * 走注入路径含义不同" —— 脚本被迫写两套码的防御分支，回放/热更/CI 都不可靠。 */
            ev.key = mq_sim_normalize_key(se.key);
            ev.x = se.x; ev.y = se.y;
            ev.dx = se.dx; ev.dy = se.dy;
            ev.scroll_x = se.dx; ev.scroll_y = se.dy;
            if (se.kind == MQ_SIM_TEXT) {
                ev.type = MQ_INPUT_TEXT;
                /* 输入事件的 text 是定长内联缓冲（16B，见 abi/input.h）→ 截断拷贝 */
                std::snprintf(ev.text, sizeof ev.text, "%s", se.text ? se.text : "");
            } else {
                ev.type = se.kind == MQ_SIM_POINTER_MOVE ? MQ_INPUT_POINTER_MOVE
                         : se.kind == MQ_SIM_POINTER_DOWN ? MQ_INPUT_POINTER_DOWN
                         : se.kind == MQ_SIM_POINTER_UP   ? MQ_INPUT_POINTER_UP
                         : se.kind == MQ_SIM_POINTER_WHEEL ? MQ_INPUT_POINTER_WHEEL
                         : se.kind == MQ_SIM_KEY_DOWN     ? MQ_INPUT_KEY_DOWN
                         : MQ_INPUT_KEY_UP;
            }
            /* 坐标空间：window（窗口像素）→ 世界坐标（含 y 翻转与方向量翻转）；world 直接用 */
            if (se.space == MQ_SIM_SPACE_WINDOW) {
                window_to_world(*this, &ev.x, &ev.y);
                ev.dy = -ev.dy; ev.scroll_y = -ev.scroll_y;
            }
            if (trace_hits)
                std::print("[sim] frame={} kind={} -> input type={} pos=({:.1f},{:.1f})\n",
                           frame, se.kind, (int)ev.type, ev.x, ev.y);
            mq_input_inject(input, &ev);
        }
        ++guard;
    } while (n == 16 && guard < 16);
}

/* ---- Spine 2D（docs/27）：装配期建实例、帧内步进、提取期出网格图元 ----
 *
 * 分层与数据流（每一层只做一件事）：
 *   VFS 读 .skel/.atlas ──► mq_spine_data_create（骨架 + 图集，页纹理走回调）
 *   纹理回调：页名 → VFS 路径 → image 解码 → rapi->create_texture → 回填非零句柄
 *   每帧：mq_spine_instance_step(dt*speed) → mq_spine_instance_draw(CPU 顶点/UV/颜色/索引/批次)
 *         → 顶点做"骨架空间 → 渲染目标像素空间"映射（y 向上翻成 y 向下，见 spine_extract 注释）
 *   提取：每个批次 = 一条 mq_renderer_mesh（同一图集页 + 同一混合模式）
 *
 * ⚠️ 实测事实（决定了下面的变换怎么写，别再凭印象改）：
 *   spine 的绘制顶点在**骨架子空间**里 y **向上**（spineboy 站立姿势包围盒 y ∈ [-8, 654]，
 *   脚在 y≈0、头在正 y；spine 编辑器与大多数 2D 运行时一致）。所以映射到引擎的
 *   "屏幕像素、y 向下"必须**翻一次 y**（H - world_y - local_y），而不是"直接用"。 */

/* 页纹理回调：spine 把页名交给我们，我们按 (atlas 目录 + 页名) 去 VFS 读 → 解码 → 上传。
 * 句柄约定：0 = 未回填（spine 侧语义），而 gfx 的纹理 id 从 0 开始 → 句柄 = id + 1。 */
static int spine_texture_cb(void* user, const char* page_name, const void* /*file_data*/,
                            size_t /*size*/, uint64_t* out_texture) {
    auto* cx = static_cast<SpineTexCtx*>(user);
    if (!cx || !cx->eng || !page_name || !*page_name || !out_texture) return MQ_SPINE_ERR_ARG;
    *out_texture = 0;
    const std::string path = cx->dir + page_name;
    const int tex = cx->eng->load_texture(path);
    if (tex < 0) {
        std::print(stderr, "runtime: spine atlas page read failed '{}'\n", path);
        return MQ_SPINE_ERR_PAGE;
    }
    *out_texture = (uint64_t)tex + 1u;      // 非零（0 = 未回填）
    return MQ_SPINE_OK;
}

/* 路径归一：组件里的资源引用可以是 "/art/x.skel" 或 "art/x.skel" → VFS 一律要前导 '/'。 */
static std::string vfs_path_of(const std::string& ref) {
    return (!ref.empty() && ref[0] == '/') ? ref : ("/" + ref);
}
static std::string dir_of(const std::string& path) {          // "/art/x.atlas" → "/art/"
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string("/") : path.substr(0, slash + 1);
}
static std::string stem_of(const std::string& path) {         // "/art/x.skel" → "/art/x"
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

Engine::Spine* Engine::spine_of(int entity) {
    for (auto& sp : spines) if (sp.entity == entity) return &sp;
    return nullptr;
}

/* 事件回调（spine2d → 引擎）：把 spine 的动画事件**翻译成引擎的脚本事件**。
 *
 * 为什么走这条路（而不是让脚本轮询 ABI）：docs/14 的事件总线是引擎唯一的脚本回调路径，
 * 按**处理器名**路由 → JS/Lua 语言无关、投递顺序确定、错误隔离都在总线里做好了。
 * 同一份事实还留了 pull 面（spine_event_count/…），push 与 pull 是同一次记录（有单测对拍）。
 *
 * 命名约定（与官方 demo 的 5 类事件一一对应，多一个用户事件）：
 *   start→"spine_start" interrupt→"spine_interrupt" end→"spine_end"
 *   complete→"spine_complete" dispose→"spine_dispose" event→"spine_event"
 * 处理器值 = 轨下标；用户事件的细节（名字/int/float/string）用 spine_event_* 取。
 *
 * ⚠️ 单线程、帧内同步调用（spine 的 AnimationState 就是这样），无线程、无异步 → 确定性不变（docs/10）。 */
static void spine_event_cb(void* user, mq_spine_instance* /*inst*/, const mq_spine_event* ev) {
    auto* sp = static_cast<Engine::Spine*>(user);
    if (!sp || !sp->eng || !ev) return;
    Engine& eng = *sp->eng;
    const char* name = nullptr;
    switch (ev->type) {
    case MQ_SPINE_EVENT_START:     name = "spine_start"; break;
    case MQ_SPINE_EVENT_INTERRUPT: name = "spine_interrupt"; break;
    case MQ_SPINE_EVENT_END:       name = "spine_end"; break;
    case MQ_SPINE_EVENT_COMPLETE:  name = "spine_complete"; break;
    case MQ_SPINE_EVENT_DISPOSE:   name = "spine_dispose"; break;
    case MQ_SPINE_EVENT_EVENT:     name = "spine_event"; break;
    default: return;
    }
    /* 值语义：用户事件带 int 值（官方 demo 的 "%d event: %s, %d, %f, %s" 里那个 int），
     * 其余事件带 track —— 都只是"标量提示"，精确字段由 pull 面给（docs/14 载荷只有标量）。 */
    const float value = (ev->type == MQ_SPINE_EVENT_EVENT) ? (float)ev->int_value : (float)ev->track;
    /* 重入护栏：脚本处理器里再调 spine_play 会同步触发新事件 → 再投递 → 若处理器又 play，
     * 就会无限递归到爆栈。正常链式播放只需要 1~2 层，故限深 8；超限只跳过**推送**
     *（事件本身仍进 pull 记录，数据不丢），并明确报一次。 */
    if (eng.spine_event_depth >= 8) {
        static bool warned = false;
        if (!warned) { warned = true;
            std::print(stderr, "runtime: spine 事件推送重入超过 8 层 → 后续只记录不推送（脚本处理器疑似自递归）\n"); }
        return;
    }
    ++eng.spine_event_depth;
    host::node_emit_number(&eng, (float)sp->entity, name, value);
    --eng.spine_event_depth;
}

void Engine::spine_release() {
    for (auto& sp : spines) {
        if (sp.inst) { mq_spine_instance_destroy(sp.inst); sp.inst = nullptr; }
        if (sp.data) { mq_spine_data_destroy(sp.data); sp.data = nullptr; }   // 页纹理归引擎（tex_cache），此处不释放
        /* 运行时合成的皮肤：本引擎是分配者 → 由本引擎释放（ABI 的"分配者释放"约定） */
        for (mq_spine_skin* sk : sp.own_skins) mq_spine_skin_destroy(sk);
        sp.own_skins.clear();
        sp.composed = nullptr;
    }
    spines.clear();
}

/* 帧边界：清空所有实例的事件记录。
 * 位置很关键 —— 必须在**脚本 update 之后、spine_step 之前**：
 *   · spine_step 在本帧脚本之后跑 → 它产生的事件留到下一帧脚本，才 pull 得到；
 *   · 脚本自己调 spine_play 产生的 start 也在本帧脚本之后才被清 → 下一帧同样 pull 得到。
 * 若放在脚本 update **之前**清（踩过）：上一帧 step 的事件会在脚本读之前被抹掉，
 * spine_event_count 永远是 0（push 面照常工作，但 pull 面瞎了）。 */
void Engine::spine_clear_events() {
    for (auto& sp : spines) {
        if (!sp.inst) continue;
        sp.events_last = mq_spine_instance_event_count(sp.inst);
        mq_spine_instance_clear_events(sp.inst);
    }
}

void Engine::spine_setup() {
    if (!scene || !vfs || !rapi) return;
    std::vector<int> ids;
    for (auto& [idx, e] : scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());                         // 稳定序（声明序 = id 升序）
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* d = nullptr;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* dd = mq_reflect_type_by_id(reflect, tid);
            if (dd && (dd->role & MQ_ROLE_SPINE)) { d = dd; break; }
        }
        if (!d) continue;
        auto& blob = e->comps[d->type_id];
        const std::string* skelref = read_str(d, blob.data(), "skeleton");
        if (!skelref || skelref->empty()) {
            std::print(stderr, "runtime: spine entity {} missing 'skeleton' ref\n", id);
            continue;
        }
        Spine sp;
        sp.entity = id;
        sp.comp = d->type_id;
        sp.skel_path = vfs_path_of(*skelref);
        /* 图集路径：① 组件的 atlas 字段（显式）；② material 字段（spine 约定：指向 .atlas）；
         * ③ 与骨架同名的 .atlas（约定，最省事） */
        if (const std::string* ap = read_str(d, blob.data(), "atlas"); ap && !ap->empty())
            sp.atlas_path = vfs_path_of(*ap);
        else if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty())
            sp.atlas_path = vfs_path_of(*mp);
        else
            sp.atlas_path = stem_of(sp.skel_path) + ".atlas";
        sp.tex_dir = dir_of(sp.atlas_path);

        /* 骨架：从内存进（.skel = 二进制，.json = 文本；docs/27 的实测约定） */
        std::string ext;
        { const size_t dot = sp.skel_path.rfind('.');
          if (dot != std::string::npos) ext = sp.skel_path.substr(dot); }
        const int is_binary = (ext == ".json") ? 0 : 1;
        void* sd = nullptr; size_t sn = 0;
        if (mq_vfs_read(vfs, sp.skel_path.c_str(), &sd, &sn) != MQ_VFS_OK || !sd || sn == 0) {
            std::print(stderr, "runtime: spine skeleton read failed '{}'\n", sp.skel_path);
            continue;
        }
        std::vector<char> skel((char*)sd, (char*)sd + sn);
        mq_vfs_free(sd);
        void* ad = nullptr; size_t an = 0;
        if (mq_vfs_read(vfs, sp.atlas_path.c_str(), &ad, &an) != MQ_VFS_OK || !ad || an == 0) {
            std::print(stderr, "runtime: spine atlas read failed '{}'"
                               " (set the component's material field to the .atlas path)\n", sp.atlas_path);
            continue;
        }
        std::string atlas_text((char*)ad, an);
        mq_vfs_free(ad);
        atlas_text.push_back('\0');                            // spine 内部按 C 字符串解析

        sp.tex_ctx.eng = this;
        sp.tex_ctx.dir = sp.tex_dir;
        sp.data = mq_spine_data_create(skel.data(), sn, is_binary, atlas_text.c_str(), "",
                                       spine_texture_cb, &sp.tex_ctx);
        if (!sp.data) {
            std::print(stderr, "runtime: spine parse failed '{}' (binary={})\n", sp.skel_path, is_binary);
            continue;
        }
        sp.inst = mq_spine_instance_create(sp.data);
        if (!sp.inst) { mq_spine_data_destroy(sp.data); sp.data = nullptr; continue; }

        const int n_anim = mq_spine_data_animation_count(sp.data);
        const int n_skin = mq_spine_data_skin_count(sp.data);
        const int n_page = mq_spine_data_page_count(sp.data);
        for (int i = 0; i < n_anim; ++i) {
            const char* nm = mq_spine_data_animation_name(sp.data, i);
            sp.anim_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_skin; ++i) {
            const char* nm = mq_spine_data_skin_name(sp.data, i);
            sp.skin_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_page; ++i) {
            mq_spine_page pg{};
            if (mq_spine_data_page(sp.data, i, &pg) != MQ_SPINE_OK) continue;
            Spine::Page p;
            p.name = pg.name ? pg.name : "";
            p.width = pg.width; p.height = pg.height; p.handle = pg.texture; p.pma = pg.pma;
            sp.pages.push_back(std::move(p));
        }
        /* 初值：皮肤 / 动画 / 循环（组件字段是稳定路径，见 schema/world/anim.yaml） */
        int start_anim = (int)read_num(d, blob.data(), "animation", 0.0F);
        if (start_anim < 0 || start_anim >= n_anim) start_anim = 0;
        const int skin_idx = (int)read_num(d, blob.data(), "skin", 0.0F);
        if (skin_idx > 0 && skin_idx < n_skin)
            mq_spine_instance_set_skin(sp.inst, sp.skin_names[(size_t)skin_idx].c_str());
        /* 混合/时间缩放/槽区间：组件字段 → 实例（脚本可随时覆盖；见 host.yaml 的 spine_* 段） */
        mq_spine_instance_set_default_mix(sp.inst, read_num(d, blob.data(), "default_mix", 0.0F));
        mq_spine_instance_set_time_scale(sp.inst, read_num(d, blob.data(), "time_scale", 1.0F));
        const int sf = (int)read_num(d, blob.data(), "slots_from", -1.0F);
        const int st = (int)read_num(d, blob.data(), "slots_to", -1.0F);
        if (sf >= 0) mq_spine_instance_set_slots_range(sp.inst, sf, st);
        const bool autoplay = read_num(d, blob.data(), "autoplay", 1.0F) != 0.0F;
        if (autoplay && n_anim > 0) {
            const bool loop = read_num(d, blob.data(), "play_mode", 1.0F) == 1.0F;   // PlayMode::Loop = 1
            if (mq_spine_instance_play(sp.inst, sp.anim_names[(size_t)start_anim].c_str(), loop ? 1 : 0, 0) == MQ_SPINE_OK)
                sp.last_anim = start_anim;
        }
        /* 绘制缓冲：按"顶点数 ≈ 附件数 × 段数"的规模预留（骨架 4k 顶点足够；不足时下一帧扩容重试） */
        sp.verts.resize(8192); sp.uvs.resize(8192); sp.cols.resize(16384); sp.darks.resize(16384);
        sp.tverts.resize(8192);
        sp.idxs.resize(32768); sp.batches.resize(512);
        sp.z = (int)read_num(d, blob.data(), "z_index", 0.0F) * 100000 + id;
        /* instance 字段（transient）：给自省/脚本一个"实例存在"的证据（下标 + 1，0 = 无） */
        if (const mq_field_desc* f = mq_reflect_field(d, "instance"))
            *(uint64_t*)(blob.data() + f->offset) = (uint64_t)(spines.size() + 1);
        std::print("runtime: spine entity {} '{}' skel='{}' atlas='{}' anims={} skins={} pages={}\n",
                   id, e->name, sp.skel_path, sp.atlas_path, n_anim, n_skin, n_page);
        if (!sp.pages.empty()) {
            std::print("runtime: spine pages:");
            for (const auto& p : sp.pages)
                std::print(" [{} {}x{} tex={} pma={}]", p.name, p.width, p.height, (long long)p.handle, p.pma);
            std::print("\n");
        }
        spines.push_back(std::move(sp));
    }
    spines.shrink_to_fit();          // 之后不再增删 → tex_ctx / eng / 事件回调 user 的地址稳定
    /* 事件回调：**必须在向量定型之后**装（user = &spines[i]，push_back 会让旧地址失效）。
     * 装完即生效：之后任何 play/addAnimation/step 触发的动画事件都会同步投给脚本总线。 */
    for (auto& sp : spines) {
        sp.eng = this;
        mq_spine_instance_set_event_cb(sp.inst, spine_event_cb, &sp);
    }
    std::print("runtime: spine instances={}\n", spines.size());
}

void Engine::spine_step(float dt) {
    if (!scene) return;
    for (auto& sp : spines) {
        if (!sp.inst || !sp.data) continue;
        Entity* e = scene->at(sp.entity);
        const mq_type_desc* d = mq_reflect_type_by_id(reflect, sp.comp);
        if (!e || !d) continue;
        auto it = e->comps.find(sp.comp);
        if (it == e->comps.end() || it->second.empty()) continue;
        std::vector<uint8_t>& blob = it->second;

        /* 组件字段 → 语义（脚本改这些字段即生效；无需额外 API 也能驱动动画） */
        const int n_anim = (int)sp.anim_names.size();
        int want = (int)read_num(d, blob.data(), "animation", 0.0F);
        if (want < 0 || want >= n_anim) want = sp.last_anim;
        const bool loop = read_num(d, blob.data(), "play_mode", 1.0F) == 1.0F;
        if (want != sp.last_anim && want >= 0) {                 // 组件里换了动画 → 真的去播
            /* 但若该轨**已经在播**这个动画（例如 add_animation 排队到点自己切过去了），
             * 只同步 last_anim，不要重启（重启会白白打断播放头/触发一次 start+interrupt）。 */
            const char* cur = mq_spine_instance_track_animation(sp.inst, 0);
            if (cur && sp.anim_names[(size_t)want] == cur) {
                sp.last_anim = want;
            } else if (mq_spine_instance_play(sp.inst, sp.anim_names[(size_t)want].c_str(), loop ? 1 : 0, 0) == MQ_SPINE_OK) {
                sp.last_anim = want;
            }
        }
        const float speed = read_num(d, blob.data(), "speed", 1.0F);
        /* 物理时间轴：SpinePhysTimestep::NoPhysics(2) → 只应用已有物理姿势（Physics_Pose），
         * 其余（Inherit/Fixed）→ 正常更新（Physics_Update）。忽略这个字段会让"冻结物理"失效。 */
        const int phys_mode = (int)read_num(d, blob.data(), "phys_timestep", 0.0F);
        const int physics = (phys_mode == 2) ? MQ_SPINE_PHYSICS_POSE : MQ_SPINE_PHYSICS_UPDATE;
        mq_spine_instance_step_ex(sp.inst, dt * speed, physics);  // 固定步长（确定性，docs/10 D1）

        /* 变换：模块负责 缩放/翻转（骨架单位 → 屏幕单位），旋转/平移留给提取期（见 spine_extract） */
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, sp.entity, angle, scale, opacity);
        const int flip = (int)read_num(d, blob.data(), "flip", 0.0F);       // FlipMode：1=水平 2=垂直 3=两者
        mq_spine_instance_set_transform(sp.inst, 0.0F, 0.0F, scale,
                                        (flip & 1) ? 1 : 0, (flip & 2) ? 1 : 0);
        float cr = 1, cg = 1, cb = 1, ca = 1;
        if (const mq_field_desc* f = mq_reflect_field(d, "color"); f && f->kind == MQ_TK_STRUCT) {
            const mq_type_desc* ct = mq_reflect_type_by_id(reflect, f->type_id);
            const uint8_t* cp = blob.data() + f->offset;
            if (ct) {
                read_f32(ct, cp, "r", &cr); read_f32(ct, cp, "g", &cg);
                read_f32(ct, cp, "b", &cb); read_f32(ct, cp, "a", &ca);
            }
        }
        mq_spine_instance_set_color(sp.inst, cr, cg, cb, ca * opacity);

        /* 绘制数据（CPU 侧；只读导出，不改模拟状态） */
        mq_spine_draw out{};
        const int rc = mq_spine_instance_draw(sp.inst, sp.verts.data(), sp.uvs.data(), sp.cols.data(),
                                              sp.darks.data(), sp.idxs.data(), sp.batches.data(),
                                              (int)(sp.verts.size() / 2), (int)sp.idxs.size(),
                                              (int)sp.batches.size(), &out);
        if (rc != MQ_SPINE_OK) { sp.draw = mq_spine_draw{}; continue; }
        if (out.vertex_count >= (int)(sp.verts.size() / 2) || out.index_count >= (int)sp.idxs.size()) {
            /* 缓冲满 = 几何被截断（会少画附件）→ 翻倍后下一帧重试，并明确报一次 */
            sp.verts.resize(sp.verts.size() * 2); sp.uvs.resize(sp.uvs.size() * 2);
            sp.cols.resize(sp.cols.size() * 2); sp.darks.resize(sp.darks.size() * 2);
            sp.tverts.resize(sp.tverts.size() * 2);
            sp.idxs.resize(sp.idxs.size() * 2); sp.batches.resize(sp.batches.size() * 2);
            static bool warned = false;
            if (!warned) { warned = true;
                std::print(stderr, "runtime: spine draw buffers full -> growing"
                                   " (this frame's geometry may be truncated)\n"); }
        }
        sp.draw = out;
        sp.z = (int)read_num(d, blob.data(), "z_index", 0.0F) * 100000 + sp.entity;

        /* 骨架空间 → 渲染目标像素空间：
         *   · spine 顶点 y **向上**（实测；见本节顶部注释），引擎渲染目标 y 向下 → 翻一次；
         *   · 累计节点角度绕实体原点旋转，再平移到实体世界位置；
         *   · world 是 y 向上的世界坐标，屏幕 y = H - world_y。 */
        const float rad = angle * 3.14159265358979323846F / 180.0F;
        const float cs = std::cos(rad), sn = std::sin(rad);
        const float ox = e->world_x, oy = (float)H - e->world_y;
        const int vc = out.vertex_count;
        if ((int)(sp.tverts.size() / 2) < vc) sp.tverts.resize((size_t)vc * 2);
        for (int i = 0; i < vc; ++i) {
            const float lx = sp.verts[(size_t)i * 2], ly = sp.verts[(size_t)i * 2 + 1];
            const float rx = lx * cs - ly * sn;
            const float ry = lx * sn + ly * cs;
            sp.tverts[(size_t)i * 2] = ox + rx;
            sp.tverts[(size_t)i * 2 + 1] = oy - ry;
        }
        /* 诊断（MAQUE_TRACE_SPINE=1）：绘制数据规模 / 变换前后包围盒 / UV 统计 —— 排查"看不见"的第一手证据
         * （实测靠它定位过两个坑：mesh.vertex_count 忘填 → 渲染器静默不画；UV 取错 accessor → 全采样到空白） */
        if (std::getenv("MAQUE_TRACE_SPINE") && (frame % 20 == 0)) {
            float minx = 0, maxx = 0, miny = 0, maxy = 0;
            int uv_zero = 0; float u0 = 2, u1 = -1, v0 = 2, v1 = -1;
            for (int i = 0; i < vc; ++i) {
                const float x = sp.tverts[(size_t)i * 2], y = sp.tverts[(size_t)i * 2 + 1];
                if (i == 0) { minx = maxx = x; miny = maxy = y; }
                else { minx = std::min(minx, x); maxx = std::max(maxx, x);
                       miny = std::min(miny, y); maxy = std::max(maxy, y); }
                const float uu = sp.uvs[(size_t)i * 2], vv = sp.uvs[(size_t)i * 2 + 1];
                if (uu == 0.0F && vv == 0.0F) ++uv_zero;
                u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                v0 = std::min(v0, vv); v1 = std::max(v1, vv);
            }
            std::print("[spine] frame={} entity={} verts={} idx={} batches={} local_bbox=({:.1f},{:.1f},{:.1f},{:.1f})"
                       " screen_bbox=({:.1f},{:.1f})-({:.1f},{:.1f}) scale={:.2f} angle={:.1f} origin=({:.1f},{:.1f})"
                       " pages={} uv_zero={}/{} uv_span=u[{:.4f}..{:.4f}]v[{:.4f}..{:.4f}] events={}\n",
                       (long long)frame, sp.entity, out.vertex_count, out.index_count, out.batch_count,
                       out.bounds_x, out.bounds_y, out.bounds_w, out.bounds_h,
                       minx, miny, maxx, maxy, scale, angle, ox, oy, sp.pages.size(),
                       uv_zero, vc, u0, u1, v0, v1, sp.events_last);
        }
        /* transient 回写：自省/脚本能读到"正在播什么、播到哪"（不进快照/校验和） */
        if (const mq_field_desc* f = mq_reflect_field(d, "state"); f && f->kind == MQ_TK_STRUCT) {
            const mq_type_desc* st = mq_reflect_type_by_id(reflect, f->type_id);
            uint8_t* base = blob.data() + f->offset;
            if (st) {
                if (const mq_field_desc* ft = mq_reflect_field(st, "time"))
                    *(float*)(base + ft->offset) = mq_spine_instance_time(sp.inst, 0);
                if (const mq_field_desc* ft = mq_reflect_field(st, "track"))
                    *(int32_t*)(base + ft->offset) = 0;
                if (const mq_field_desc* ft = mq_reflect_field(st, "loop"))
                    *(bool*)(base + ft->offset) = loop;
            }
        }
    }
}

void Engine::spine_extract(std::vector<mq_renderer_mesh>& out) {
    for (auto& sp : spines) {
        if (!sp.inst || sp.draw.vertex_count <= 0 || sp.draw.index_count <= 0) continue;
        out.reserve(out.size() + (size_t)sp.draw.batch_count);        for (int b = 0; b < sp.draw.batch_count; ++b) {
            const mq_spine_batch& sb = sp.batches[(size_t)b];
            mq_renderer_mesh m{};
            m.vertices = sp.tverts.data();
            m.uvs = sp.uvs.data();
            m.colors = sp.cols.data();
            m.dark_colors = sp.darks.data();     // 两色着色（TintBlack）暗色通道；长度同 colors
            m.indices = sp.idxs.data();
            m.vertex_count = (uint32_t)sp.draw.vertex_count;     // 顶点数（少了它 = 渲染器不画，且是静默的）
            m.first_index = sb.first_index;
            m.index_count = sb.index_count;
            m.base_vertex = 0;
            m.blend = sb.blend;
            m.layer = 0;
            m.z = sp.z;
            m.texture = -1;
            if (sb.page >= 0 && sb.page < (int)sp.pages.size()) {
                const Spine::Page& p = sp.pages[(size_t)sb.page];
                m.pma = p.pma;
                /* 句柄 = gfx 纹理 id + 1（0 表示未回填）→ 还原成引擎纹理 id 交给渲染器 */
                if (p.handle > 0) {
                    const int tex = (int)(p.handle - 1);
                    if (rapi && rapi->has_texture && rapi->has_texture(rself, tex)) m.texture = tex;
                }
            }
            out.push_back(m);
        }
    }
}

/* 批量截图落地：模板 → 文件名 → 写图；并判断"是不是最后一张"（是则退出）。
 * 模板里的 `{}` 换成帧号；没有 `{}` 就自动插到扩展名前（`out.png` → `out_0042.png`）。
 * 格式仍按扩展名（.png → PNG，其余 → PPM）。 */
void Engine::write_seq_shot(const std::vector<uint8_t>& px, int tw, int th) {
    std::string path = shot_seq;
    char num[32];
    std::snprintf(num, sizeof num, "%d", (int)frame);
    const size_t brace = path.find("{}");
    if (brace != std::string::npos) {
        path.replace(brace, 2, num);
    } else {
        const size_t dot = path.find_last_of('.');
        char pad[32];
        std::snprintf(pad, sizeof pad, "_%04d", (int)frame);
        if (dot == std::string::npos) path += pad;
        else path.insert(dot, pad);
    }
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    const int rc = png ? mq_image_write_png(path.c_str(), (uint32_t)tw, (uint32_t)th,
                                            px.data(), (uint32_t)tw * 4u)
                       : (write_ppm(path, tw, th, px) ? 0 : 1);
    if (rc == 0) std::print("runtime: shot[{}] '{}' {}x{} frame={}\n",
                            shots_taken + 1, path, tw, th, (int)frame);
    else std::print(stderr, "runtime: shot 写入失败 '{}'\n", path);
    ++shots_taken;
    /* 收尾条件：显式帧号列表 → 拍完最大帧即退；every 模式 → 拍够 count（count=0 → 由
     * MAQUE_MAX_FRAMES 控制，不自己退）。 */
    if (!shot_list.empty()) {
        const int last = *std::max_element(shot_list.begin(), shot_list.end());
        if ((int)frame >= last) quit = true;
    } else if (shot_count > 0 && shots_taken >= shot_count) {
        quit = true;
    }
}

/* ---- ImGui 调试面板（docs/27 §ImGui）------------------------------------------------
 *   ① **不进确定性**：面板是视图层 → 不写任何实体/组件、不进快照/校验和（docs/10）。
 *   ② **画在所有引擎图层之上**：网格 layer = MQ_IMGUI_LAYER（引擎 UI 是 1），
 *      在**引擎渲染完（含 ui pass）、present 之前**追加 → 截图里也有面板（可自证）。
 *   ③ **不点穿**：ImGui 想要鼠标/键盘时，清空本帧设备事件，世界输入即被阻断。
 *
 * 未启用时（默认）**一次都不碰** ImGui 上下文 —— tank/flappy 等程序零开销。
 */
void Engine::imgui_setup() {
    if (imgui_inited) return;
    /* 字体：复用文本模块已解析的路径（MAQUE_FONT → scenes/fonts/*.ttf → 系统）。
     * **必须给 TTF**：ImGui 默认字体只有 ASCII，中文会渲染成 '?'（实测）。
     * 字号默认 18px（调试面板要能看清），可用 MAQUE_IMGUI_FONT_SIZE 覆盖。 */
    text_setup();                                // 确保 font_path 已解析（幂等）
    /* 默认设计字号 26px：窗口默认 800x650 看 1600x1300 场景时是 0.5 倍，
     * 屏幕上约 13px（可读）；字号按屏幕像素栅格化，所以清晰。 */
    float fs = 26.0F;
    if (const char* e = std::getenv("MAQUE_IMGUI_FONT_SIZE")) {
        const float v = (float)std::atof(e);
        if (v > 0.0F) fs = v;
    }
    /* 字体栅格化倍率：ImGui 显示空间 = 目标/设计空间（见 imgui_frame_begin），
     * 所以字号就是**设计像素**，不需要再乘窗口缩放；窗口比设计小时最终画面整体缩小，
     * 字体仍按设计尺寸栅格化才清晰（缩放交给 GPU）。 */
    /* 字体栅格化倍率 = 窗口/目标比：面板几何在**设计空间**（目标像素），
     * 但最终要缩放到窗口显示。字按「设计字号」栅格化的话，经过一次降采样会糊；
     * 按「字号 × 窗口/目标」栅格化，采样时的缩放恰好抵消最后的降采样 → **1:1 落到屏幕像素**，不糊。
     * 无窗口（headless/截图）时目标就是最终像素 → 1.0。 */
    float ps = 1.0F;
    if (window && TW > 0 && win_w > 0) ps = (float)win_w / (float)TW;
    if (canvas.device_scale > 0.0F) ps *= canvas.device_scale;
    if (ps <= 0.0F) ps = 1.0F;
    /* 官方后端优先：先拿到 SDL_Renderer（提前创建），再声明 backend 模式 → 字体图集交给官方后端。 */
    void* pre_ren = (window && platform) ? mq_platform_ensure_renderer(platform) : nullptr;
    if (pre_ren) mq_imgui_set_backend_mode(1);
    if (mq_imgui_init_with_font(font_path.empty() ? nullptr : font_path.c_str(), fs, ps) != 0) {
        std::print(stderr, "runtime: imgui init failed\n");
        imgui_on = false;
        return;
    }
    if (pre_ren && mq_imgui_backend_init(pre_ren) == 0) {
        imgui_backend = true;
        std::print("runtime: imgui uses official SDL_Renderer backend\n");
    }
    imgui_inited = true;
    imgui_tex = -1;
    imgui_tex_gen = 0;
    std::print("runtime: imgui enabled (layer={}, font={} size={:.0f} scale={:.2f})\n",
               MQ_IMGUI_LAYER, font_path.empty() ? "(default)" : font_path.c_str(), fs, ps);
}

/* 开关面板：关掉时释放上下文 + 字体纹理（不占 GPU/输入）；再开会重新初始化。 */
void Engine::imgui_enable(bool on) {
    if (on == imgui_on && (on ? imgui_inited : !imgui_inited)) return;   // 幂等
    if (!on) {
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
        std::print("runtime: imgui panel OFF\n");
        return;
    }
    imgui_on = true;
    imgui_setup();
    std::print("runtime: imgui panel ON\n");
}

/* 把本帧设备事件转发给 ImGui（**窗口像素坐标**；调用点在转成世界坐标**之前**）。
 * ImGui 的显示空间就是窗口像素 → 不需要换算（画布缩放只影响世界层）。 */
void Engine::imgui_input_feed(const mq_platform_pointer_event* ptrs, uint32_t np,
                              const mq_platform_key_event* keys, uint32_t nk) {
    if (!imgui_on || !imgui_inited) return;
    float wheel = 0.0F;
    for (uint32_t i = 0; i < np; ++i) {
        const mq_platform_pointer_event& p = ptrs[i];
        imgui_ptr_x = p.x;
        imgui_ptr_y = p.y;
        if (p.type == MQ_PLATFORM_POINTER_DOWN && p.button == 0) imgui_ptr_down = true;
        else if (p.type == MQ_PLATFORM_POINTER_UP && p.button == 0) imgui_ptr_down = false;
        else if (p.type == MQ_PLATFORM_POINTER_WHEEL) wheel += p.scroll_y != 0.0F ? p.scroll_y : p.dy;
    }
    mq_imgui_input_pointer(ptr_target_x(), ptr_target_y(), imgui_ptr_down ? 1 : 0, wheel);
    for (uint32_t i = 0; i < nk; ++i) {
        const mq_platform_key_event& k = keys[i];
        mq_imgui_input_key((int)k.key, k.down ? 1 : 0);
        if (k.down && k.text[0]) mq_imgui_input_chars(k.text);
    }
}

void Engine::imgui_frame_begin(float dt) {
    /* 首次调用时按 MAQUE_IMGUI 决定初始开关（默认**关**：不写脚本的游戏不受影响）。 */
    if (!imgui_env_checked) {
        imgui_env_checked = true;
        /* 默认：**真窗口且不是截图/离屏**时自动开启（所有游戏都有 stats 面板）。
         * MAQUE_IMGUI=0 关掉；=1 强制开（含 headless/截图）；F2 随时开关。 */
        /* 默认关闭（需要显式开）：MAQUE_IMGUI=1 或运行时按 F2。
         * 默认开启会改变所有窗口程序的画面与输入命中（stats 面板覆盖左上角），
         * 现有 e2e（imgui_toggle_e2e 期望初始为关、canvas_e2e 依赖像素/命中）
         * 需要一并调整后才能改默认值 —— 见提交说明的待办。 */
        /* 默认：**真窗口 + 非截图 + 未关呈现** → 开启（所有游戏都有 stats 面板；
         * stats 是 NoInputs 覆盖层，不挡点击）。MAQUE_IMGUI=0 关；=1 强制开（含 headless）。 */
        const char* e = std::getenv("MAQUE_IMGUI");
        const char* pres = std::getenv("MAQUE_PRESENT");
        const bool present_off = pres && (std::strcmp(pres, "off") == 0 || std::strcmp(pres, "0") == 0);
        const bool force_off = e && *e && std::strcmp(e, "0") == 0;
        const bool force_on = e && *e && std::strcmp(e, "0") != 0;
        /* MAQUE_CAPTURE（窗口表面回读做像素断言）时也不自动开：覆盖层会出现在捕获里。 */
        const bool capturing = std::getenv("MAQUE_CAPTURE") != nullptr;
        if (force_on || (!force_off && window && shot_path.empty() && !present_off && !capturing)) {
            imgui_on = true;
            std::print("runtime: imgui on{} (F2 toggles)\n", force_on ? " (MAQUE_IMGUI)" : " (default)");
        }
    }
    if (!imgui_on) return;
    /* 加速呈现 + 有窗口：SDL_Renderer 要到首次 present 才创建 → 等它出现再初始化，
     * 且**先声明 backend 模式**（图集交给官方后端动态烘焙，避免静态 Build 断言）。 */
    const char* pres_env2 = std::getenv("MAQUE_PRESENT");
    const bool want_backend = window && platform && pres_env2 &&
                              std::strcmp(pres_env2, "accelerated") == 0;
    if (want_backend && !imgui_backend) {
        void* sdl_ren = mq_platform_sdl_renderer(platform);
        if (sdl_ren) {
            if (!imgui_inited) { mq_imgui_set_backend_mode(1); imgui_setup(); }
            if (imgui_inited && mq_imgui_backend_init(sdl_ren) == 0) {
                imgui_backend = true;
                std::print("runtime: imgui uses official SDL_Renderer backend\n");
            }
        }
        if (!imgui_backend) return;      // renderer 还没创建好：本帧不画面板
    }
    if (!imgui_inited) imgui_setup();
    if (!imgui_inited) return;
    /* 官方后端延迟切换：SDL_Renderer 要到首次 present 才创建，setup 时还是 null。
     * 每帧尝试一次，成功即永久切到官方后端（面板直接画窗口、不经过网格导出链）。 */
    if (!imgui_backend && window && platform) {
        void* sdl_ren = mq_platform_sdl_renderer(platform);
        if (sdl_ren && mq_imgui_backend_init(sdl_ren) == 0) {
            imgui_backend = true;
            std::print("runtime: imgui uses official SDL_Renderer backend\n");
        }
    }
    const int target_w = TW;
    const int target_h = (int)(H * ss + 0.5F);
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

    /* 映射参数：**ImGui 的显示空间直接 = 目标/设计空间**（scale=1、offset=0）。
     *
     * 为什么不让显示空间 = 窗口像素：脚本是按 `screen_w()`（设计分辨率）定位面板的
     *（如 all.scn 1600x1300 把面板放在 x=1284）。若显示空间是窗口像素（默认 800x650），
     * 面板会落在显示空间外 → ImGui 把它裁成**宽度 0**（实测 clip=(800,32)-(800,477)，
     * 面板整块消失）。设计空间 = 目标空间时，两种模式（有窗口/headless）行为一致。
     *
     * 代价：输入坐标要把"窗口像素"反算到设计坐标（见 imgui_input_feed）。 */
    const float show_w = (float)target_w;
    const float show_h = (float)target_h;
    const float scale = 1.0F, off_x = 0.0F, off_y = 0.0F;
    const float dpi = 1.0F;
    mq_imgui_begin_frame(show_w, show_h, (float)target_w, (float)target_h,
                        scale, off_x, off_y, dt, dpi);
    imgui_frame_active = true;

    /* ---- 默认 stats 面板：**所有游戏都有**，左上角，无需脚本参与 ----
     * 数据是上一帧的（开帧时就显示）。
     * · **不设尺寸**：让窗口按内容自动收缩 —— 之前固定 268x300，用户拖动后内容被截断
     *   （窗口尺寸写死、内容行数/字号变化就溢出）；
     * · 内容压缩成 4 行，避免"窗体偏大"。位置仍用 FirstUseEver（可拖动、不弹回）。 */
    if (mq_imgui_ready()) {
        /* 位置/尺寸只在**首帧**设一次（之后拖动/缩放不被复位）。尺寸按 4 行内容给够。 */
        if (!stat_window_sized) {
            mq_imgui_set_pos(8.0F, 8.0F);
            mq_imgui_set_size(400.0F, 260.0F);
            stat_window_sized = true;
        }
        if (mq_imgui_begin_overlay("stats")) {
            char line[160];
            std::snprintf(line, sizeof line, "FPS %.0f  (%.1f ms)",
                          stat_fps, stat_fps > 0 ? 1000.0F / stat_fps : 0.0F);
            mq_imgui_text(line);
            std::snprintf(line, sizeof line, "logic %.1f ms  render %.1f ms", stat_logic_ms, stat_render_ms);
            mq_imgui_text(line);
            std::snprintf(line, sizeof line, "draws %d  tris %d", stat_calls, stat_tris);
            mq_imgui_text(line);
            std::snprintf(line, sizeof line, "frame %llu  target %dx%d",
                          (unsigned long long)frame, target_w, target_h);
            mq_imgui_text(line);
        }
        mq_imgui_end();
    }

    /* 注：③ "不点穿"（清空本帧设备事件）放在调用点 —— iframe 是 step_frame 的局部量。 */
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

/* UI 组件语义（role 驱动；引擎不认具体类型名，docs/17）：
 *   · ui_layout：H/V/GRID 自动布局（Layout）
 *   · ui_progress：按 value 调整填充子节点宽度（ProgressBar）
 *   · ui_safe_area：按内缩重排目标节点（SafeArea/Widget）
 *   · ui_scroll：内容偏移（ScrollView 的 scrollOffset） */
void Engine::ui_step() {
    if (!scene) return;
    std::vector<int> ids;
    for (auto& [k, e] : scene->ents) ids.push_back(k);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d) continue;

            if (d->role & MQ_ROLE_UI_LAYOUT) {
                const float mode = read_num(d, blob.data(), "mode", 0.0F);
                if (std::getenv("MAQUE_TRACE_UI")) std::print("[ui] layout entity={} mode={}\n", id, (int)mode);
                const float spacing = read_num(d, blob.data(), "spacing", 0.0F);
                const int columns = (int)read_num(d, blob.data(), "columns", 1.0F);
                float pad_l = 0, pad_t = 0;
                if (const mq_field_desc* f = mq_reflect_field(d, "padding")) {
                    const float* v = (const float*)(blob.data() + f->offset);
                    pad_l = v[0]; pad_t = v[1];
                }
                std::vector<int> kids;
                for (auto& [k, ke] : scene->ents) if (ke.parent == id) kids.push_back(k);
                std::sort(kids.begin(), kids.end());          // 稳定：创建序
                float cursor = 0.0F;
                int col = 0;
                for (int k : kids) {
                    Entity* ke = scene->at(k);
                    if (!ke) continue;
                    float kw = 32.0F, kh = 32.0F;
                    if (auto* ctrl = comp_by_role(*this, k, MQ_ROLE_UI_CONTROL)) {
                        const mq_type_desc* cd = mq_reflect_type_by_name(reflect, "ui.Control");
                        if (const mq_field_desc* f = mq_reflect_field(cd, "offset_max")) {
                            const float* v = (const float*)(ctrl->data() + f->offset);
                            kw = v[0]; kh = v[1];        // Vec2：offset_max 即内容尺寸（contentSize）
                        }
                    }
                    /* mode: 0=Free 1=Horizontal 2=Vertical 3=Grid */
                    /* 位置经 entity_set_pos 写回 Transform2D 组件（否则被 flatten_world 覆盖） */
                    if ((int)mode == 1) {
                        host::entity_set_pos(this, (float)k, e->world_x + pad_l + cursor + kw * 0.5F,
                                             e->world_y + pad_t + kh * 0.5F);
                        cursor += kw + spacing;
                    } else if ((int)mode == 2) {
                        host::entity_set_pos(this, (float)k, e->world_x + pad_l + kw * 0.5F,
                                             e->world_y + pad_t + cursor + kh * 0.5F);
                        cursor += kh + spacing;
                    } else if ((int)mode == 3) {
                        const int c = columns > 0 ? columns : 1;
                        const int row = col / c, column = col % c;
                        host::entity_set_pos(this, (float)k,
                                             e->world_x + pad_l + (float)column * (kw + spacing) + kw * 0.5F,
                                             e->world_y + pad_t + (float)row * (kh + spacing) + kh * 0.5F);
                        ++col;
                    }
                    (void)ke;
                }
            }

            if (d->role & MQ_ROLE_UI_PROGRESS) {
                float v = read_num(d, blob.data(), "value", 0.0F);
                v = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
                write_num(d, blob.data(), "phase", (double)v);
                /* 填充子节点：第一个带 sprite/control 的子节点，宽度 = 父宽 × value */
                std::vector<int> kids;
                for (auto& [k, ke] : scene->ents) if (ke.parent == id) kids.push_back(k);
                std::sort(kids.begin(), kids.end());
                for (int k : kids) {
                    if (auto* ctrl = comp_by_role(*this, k, MQ_ROLE_UI_CONTROL)) {
                        const mq_type_desc* cd = mq_reflect_type_by_name(reflect, "ui.Control");
                        float full = host::node_width(this, (float)id);
                        if (full <= 0.0F) full = read_num(cd, ctrl->data(), "offset_max", 100.0F);
                        if (const mq_field_desc* f = mq_reflect_field(cd, "offset_max"))
                            *(float*)(ctrl->data() + f->offset) = full * v;
                        break;
                    }
                }
            }

            if (d->role & MQ_ROLE_UI_SAFEAREA) {
                const float il = read_num(d, blob.data(), "inset_left", 0.0F);
                const float it = read_num(d, blob.data(), "inset_top", 0.0F);
                const float ir = read_num(d, blob.data(), "inset_right", 0.0F);
                const float ib = read_num(d, blob.data(), "inset_bottom", 0.0F);
                if (const mq_field_desc* f = mq_reflect_field(d, "resolved_inset")) {
                    float* v = (float*)(blob.data() + f->offset);
                    v[0] = il; v[1] = it; v[2] = ir; v[3] = ib;
                }
                /* 目标（或首个 Control 子节点）按内缩重排 */
                int target = -1;
                for (auto& [k, ke] : scene->ents) if (ke.parent == id) { target = k; break; }
                if (target >= 0) {
                    host::entity_set_pos(this, (float)target, e->world_x + il, e->world_y + it);
                    if (auto* ctrl = comp_by_role(*this, target, MQ_ROLE_UI_CONTROL)) {
                        const mq_type_desc* cd = mq_reflect_type_by_name(reflect, "ui.Control");
                        if (const mq_field_desc* f = mq_reflect_field(cd, "offset_max")) {
                            float* v = (float*)(ctrl->data() + f->offset);
                            v[0] = (float)W - il - ir;
                            v[1] = (float)H - it - ib;
                        }
                    }
                }
            }

            if (d->role & MQ_ROLE_UI_SCROLL) {
                const float sv = read_num(d, blob.data(), "scroll_v", 0.0F);
                if (const mq_field_desc* f = mq_reflect_field(d, "scroll_offset")) {
                    float* v = (float*)(blob.data() + f->offset);
                    v[1] = sv;
                }
                for (auto& [k, ke] : scene->ents)
                    if (ke.parent == id) {
                        const float kx = ke.world_x;
                        host::entity_set_pos(this, (float)k, kx, e->world_y - sv);
                        break;
                    }
            }
        }
    }
}


/* Tween（tween().to()）：在固定步长内把字段从 from 插值到 to；
 * 缓动 0=linear 1=ease_in 2=ease_out 3=ease_in_out；完成派发 "tween_complete"。 */
void Engine::step_tweens(float dt) {
    if (tweens.empty()) return;
    std::vector<size_t> done;
    for (size_t i = 0; i < tweens.size(); ++i) {
        Tween& tw = tweens[i];
        tw.elapsed += dt;
        float t = tw.duration > 0.0F ? tw.elapsed / tw.duration : 1.0F;
        if (t > 1.0F) t = 1.0F;
        float e = t;
        switch (tw.easing) {
            case 1: e = t * t; break;                       // ease_in
            case 2: e = 1.0F - (1.0F - t) * (1.0F - t); break;   // ease_out
            case 3: e = t < 0.5F ? 2.0F * t * t : 1.0F - 2.0F * (1.0F - t) * (1.0F - t); break;
            default: break;
        }
        const float v = tw.from + (tw.to - tw.from) * e;
        const mq_type_desc* td = nullptr;
        const mq_field_desc* fd = nullptr;
        void* base = nullptr;
        if (resolve_field_path(*this, tw.entity, tw.path, &td, &fd, &base) && fd)
            write_num(td, base, fd->name, (double)v);
        if (t >= 1.0F) {
            host::node_emit_number(this, (float)tw.entity, "tween_complete", tw.to);
            done.push_back(i);
        }
    }
    for (auto it = done.rbegin(); it != done.rend(); ++it) tweens.erase(tweens.begin() + (long)*it);
}

/* 定时器（定时器）：按固定步长累计，到点调用脚本函数（按名字）。 */
void Engine::step_schedules(float dt) {
    if (schedules.empty() || !bus) return;
    std::vector<size_t> drop;
    for (size_t i = 0; i < schedules.size(); ++i) {
        Scheduled& sc = schedules[i];
        sc.remaining -= dt;
        if (sc.remaining > 0.0F) continue;
        mq_script_bus_call(bus, sc.handler.c_str(), (double)dt, 0.0, nullptr);
        if (sc.repeat) sc.remaining += sc.interval > 0.0F ? sc.interval : (1.0F / 60.0F);
        else drop.push_back(i);
    }
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) schedules.erase(schedules.begin() + (long)*it);
}

/* node.destroy()：延迟到帧边界统一销毁（销毁在当前帧末生效）。 */
void Engine::flush_pending_destroy() {
    if (pending_destroy.empty()) return;
    if (std::getenv("MAQUE_TRACE_UI")) std::print("[ui] destroy {} node(s)\n", pending_destroy.size());
    std::vector<int> ids = pending_destroy;
    pending_destroy.clear();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) if (scene->at(id)) scene->remove(id);
}

/* 物理组件（RigidBody2D/Collider2D 等价）：装配期建体，帧内步进并回写变换。
 * 只在场景里存在 physics_body role 时启用（不影响脚本显式驱动 physics_step 的场景）。 */
void Engine::physics_components_setup() {
    if (!scene || !physics) return;
    std::vector<int> ids;
    for (auto& [k, e] : scene->ents) ids.push_back(k);
    std::sort(ids.begin(), ids.end());
    physics_role_bodies.clear();
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        auto* body_blob = comp_by_role(*this, id, MQ_ROLE_PHYSICS_BODY);
        if (!body_blob) continue;
        const mq_type_desc* bd = nullptr;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (d && (d->role & MQ_ROLE_PHYSICS_BODY)) { bd = d; break; }
        }
        if (!bd) continue;
        /* BodyType: Static=0 / Kinematic=1 / Dynamic=2（Kinematic 暂按 static 处理并记录） */
        const double body_type = read_num(bd, body_blob->data(), "body_type", 0.0F);
        const double is_static = body_type == 0.0 ? 1.0 : 0.0;
        double radius = 0.0, hw = 0.0, hh = 0.0;
        double shape_type = 0.0, friction = 0.5, restitution = 0.0;
        if (auto* col = comp_by_role(*this, id, MQ_ROLE_PHYSICS_COLLIDER)) {
            const mq_type_desc* cd = nullptr;
            for (auto& [tid, b] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (d && (d->role & MQ_ROLE_PHYSICS_COLLIDER)) { cd = d; break; }
            }
            if (cd) {
                /* 字段名必须与 schema 的 `physics.Collider`（world/physics.yaml）**逐字段一致**。
                 * 踩过的大坑：这里曾读 `half_w`/`half_h` —— 那两个字段在 Collider 里**根本不存在**，
                 * 于是永远拿到默认值、几何全错；同时完全没读 `shape_type`（形状靠 radius>0 猜）。
                 * 结果是"碰撞表面看着存在、球却直接穿过地板"：bug 藏在默认值里，很难看出来。
                 *   shape_type: ShapeType（Box=0 Circle=1 Capsule=2 …）→ 求解器只支持 Ball/Cuboid
                 *   half_extents: math.Vec3（**嵌套 struct**）→ 2D 取其 x/y，z 忽略 */
                shape_type = read_num(cd, col->data(), "shape_type", 0.0F);
                radius = read_num(cd, col->data(), "radius", 0.5F);
                friction = read_num(cd, col->data(), "friction", 0.5F);
                restitution = read_num(cd, col->data(), "restitution", 0.0F);
                if (const mq_field_desc* f = mq_reflect_field(cd, "half_extents")) {
                    const mq_type_desc* vt = mq_reflect_type_by_id(reflect, f->type_id);
                    const uint8_t* p = col->data() + f->offset;
                    if (const mq_field_desc* fx = vt ? mq_reflect_field(vt, "x") : nullptr)
                        hw = *(const float*)(p + fx->offset);
                    if (const mq_field_desc* fy = vt ? mq_reflect_field(vt, "y") : nullptr)
                        hh = *(const float*)(p + fy->offset);
                }
                /* Capsule(2)/Cylinder(3) 的轴向半高在 half_height；2D 下沿用为半高 */
                if (shape_type == 2.0 || shape_type == 3.0)
                    hh = read_num(cd, col->data(), "half_height", 0.5F);
            }
        }
        /* 形状按**声明的 shape_type** 决定，而不是靠"radius 是否 > 0"猜。
         * Box → Cuboid；Circle（以及 2D 下退化的 Capsule）→ Ball；
         * Convex/TriMesh/HeightField 求解器不支持：明确告警并退化为 Box。 */
        bool is_circle = (shape_type == 1.0 || shape_type == 2.0);
        if (shape_type >= 4.0) {
            std::print(stderr, "runtime: entity {} '{}' 的 Collider.shape_type={} 在 2D 求解器下不支持；"
                               "已按 Box 处理\n", id, e->name, (int)shape_type);
            is_circle = false;
        }
        if (is_circle) {
            hw = hh = 0.0;
            if (radius <= 0.0) {
                std::print(stderr, "runtime: entity {} '{}' 的 Collider 是 Circle 但 radius<=0；已按 8 兜底\n",
                           id, e->name);
                radius = 8.0;
            }
        } else {
            radius = 0.0;
            /* 尺寸非正 = 无效碰撞体：**不静默兜底**（静默兜底正是上面那个 bug 藏住的原因），
             * 明确告警并按圆兜底（旧行为），让作者一眼看到"忘了写尺寸"。 */
            if (hw <= 0.0 || hh <= 0.0) {
                std::print(stderr, "runtime: entity {} '{}' 的 Collider 是 Box 但 half_extents 非正"
                                   "（{:.2f},{:.2f}）；已按半径 8 的圆兜底 —— 请显式声明尺寸\n",
                           id, e->name, hw, hh);
                hw = hh = 0.0; radius = 8.0; is_circle = true;
            }
        }
        mq_physics_body_desc desc{};
        desc.struct_size = (uint32_t)sizeof desc;
        desc.type = is_static != 0.0 ? MQ_PHYSICS_BODY_STATIC : MQ_PHYSICS_BODY_DYNAMIC;
        desc.shape = radius > 0.0 ? MQ_PHYSICS_SHAPE_BALL : MQ_PHYSICS_SHAPE_CUBOID;
        desc.x = e->world_x; desc.y = e->world_y;
        desc.radius = (float)radius;
        desc.half_w = (float)hw; desc.half_h = (float)hh;
        /* 摩擦/弹性来自组件声明（此前写死 0.5/0.2，声明被静默忽略 —— 表现是
         * "把 Collider.restitution 从 0.0 改成 0.95，逐帧位置完全一致"，回弹比恒为 0.5）。
         * 注意：ABI 的 mq_physics_body_desc 没有 layer/mask 字段，按层过滤暂不可用。 */
        desc.mass = 1.0F;
        desc.restitution = (float)restitution;
        desc.friction = (float)friction;
        mq_physics_body out_body{};
        if (mq_physics_add_body(physics, &desc, &out_body) == MQ_PHYSICS_OK)
            physics_role_bodies.push_back({ id, out_body });
    }
}

void Engine::physics_components_step(float) {
    static bool reported = false;
    if (!reported && !physics_role_bodies.empty()) {
        std::print("runtime: {} physics body(ies) from components\n", physics_role_bodies.size());
        reported = true;
    }
    contacts_dirty = true;                          // 新一轮 step 后接触对需重建（见 refresh_contacts）
    if (!physics || physics_role_bodies.empty()) return;
    mq_physics_step(physics);                       // 固定步长在世界配置里（docs/10 D1）
    /* 诊断（MAQUE_TRACE_PHYSICS=1）：逐帧打印实体位置 + 本帧接触对（排查"为什么判定不中"） */
    if (std::getenv("MAQUE_TRACE_PHYSICS")) {
        static int trace_n = 0;
        if (trace_n < 40) {
            std::print(stderr, "[phys] f{}", trace_n);
            for (auto& [id, body] : physics_role_bodies) {
                float x = 0, y = 0, vx = 0, vy = 0;
                mq_physics_get_transform(physics, body, &x, &y);
                mq_physics_get_velocity(physics, body, &vx, &vy);
                std::print(stderr, " e{}@({:.1f},{:.1f})v({:.0f},{:.0f})", id, x, y, vx, vy);
            }
            const uint32_t nc = mq_physics_contact_count(physics);
            std::print(stderr, " contacts={}", nc);
            for (uint32_t i = 0; i < nc; ++i) {
                mq_physics_contact c{};
                if (mq_physics_contact_at(physics, i, &c) == 0)
                    std::print(stderr, " [a={} b={} n=({:.1f},{:.1f}) d={:.2f}]", c.a_index, c.b_index, c.nx, c.ny, c.depth);
            }
            std::print(stderr, "\n");
            ++trace_n;
        }
    }
    for (auto& [id, body] : physics_role_bodies) {
        Entity* e = scene->at(id);
        if (!e) continue;
        float x = e->world_x, y = e->world_y;
        if (mq_physics_get_transform(physics, body, &x, &y) == MQ_PHYSICS_OK) {
            e->world_x = x; e->world_y = y;
            host::entity_set_pos(this, (float)id, x, y);   // 写回 Transform2D（否则被 flatten_world 覆盖）
        }
    }
}

/* 把求解器**本帧**的接触对映射回实体 id（docs/15 §3、docs/19）。
 *
 * 设计取舍：
 *   · 引擎只提供**查询**，不提供"碰撞回调"——玩法（撞管死亡/得分）属于脚本层，
 *     引擎不硬编码任何组件名或事件语义（与"引擎即浏览器、游戏即资源"一致）；
 *   · 结果保持求解器顺序（a_index 升序）→ 确定性（docs/10 D4）；
 *   · 不认识的 body.index（例如 spawn_ball 的临时球）映射为 -1，脚本据此跳过；
 *   · 撞世界边界是**平面接触**（b_index = MQ_PHYSICS_PLANE_INDEX）→ 该对映射为 (-1,-1)，
 *     脚本用 physics_contact_pair() 区分"实体对实体"与"撞边界"；
 *   · 惰性重建：一次 step 后首次查询建表，之后复用；下一帧 step 置脏。 */
void Engine::refresh_contacts() {
    if (!contacts_dirty) return;
    contacts_dirty = false;
    contact_entities.clear();
    if (!physics) return;
    const uint32_t n = mq_physics_contact_count(physics);
    contact_entities.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        mq_physics_contact c{};
        if (mq_physics_contact_at(physics, i, &c) != 0) { contact_entities.emplace_back(-1, -1); continue; }
        int ea = -1, eb = -1;
        for (const auto& [id, body] : physics_role_bodies) {
            if (body.index == c.a_index) ea = id;
            if (body.index == c.b_index) eb = id;
        }
        contact_entities.emplace_back(ea, eb);
    }
}

} // namespace

namespace host {

float window_w(void* ctx) { return (float)((Engine*)ctx)->canvas.window_w; }
float window_h(void* ctx) { return (float)((Engine*)ctx)->canvas.window_h; }
float canvas_scale(void* ctx) { return ((Engine*)ctx)->canvas.scale; }
float canvas_scale_x(void* ctx) { return ((Engine*)ctx)->canvas.scale_x; }
float canvas_scale_y(void* ctx) { return ((Engine*)ctx)->canvas.scale_y; }
float canvas_offset_x(void* ctx) { return ((Engine*)ctx)->canvas.offset_x; }
float canvas_offset_y(void* ctx) { return ((Engine*)ctx)->canvas.offset_y; }
float canvas_device_scale(void* ctx) { return ((Engine*)ctx)->canvas.device_scale; }
float canvas_safe_area(void* ctx, float edge) {
    auto* eng = (Engine*)ctx;
    const int e = (int)edge;
    if (e < 0 || e > 3) return 0.0F;
    return eng->canvas_safe[e];
}
float screen_w(void* ctx) { return (float)((Engine*)ctx)->W; }
float screen_h(void* ctx) { return (float)((Engine*)ctx)->H; }
float rng(void* ctx) { return (float)((Engine*)ctx)->next_rng() / 4294967296.0f; }
void  quit(void* ctx) {
    auto* eng = (Engine*)ctx;
    /* MAQUE_KEEP_OPEN=1：演示/人工查看时忽略脚本的 quit()（关闭窗口仍退出） */
    if (keep_open) { std::print("runtime: script requested quit (ignored: MAQUE_KEEP_OPEN)\n"); return; }
    eng->quit = true;
}
void  clear(void* ctx, float r, float g, float b) {
    auto* e = (Engine*)ctx; e->bg[0] = r; e->bg[1] = g; e->bg[2] = b;
}
void  log(void*, const char* msg) { std::print("[game] {}\n", msg ? msg : ""); }
float cfg(void* ctx, const char* path) { return (float)cfg_field((Engine*)ctx, path); }
float load_texture(void* ctx, const char* path) {
    auto* e = (Engine*)ctx;
    std::string p = (path && path[0] == '/') ? path : ("/" + std::string(path ? path : ""));
    return (float)e->load_texture(p);
}
float entity_spawn(void* ctx) { return (float)((Engine*)ctx)->scene->spawn(); }
void  entity_set_pos(void* ctx, float id, float x, float y) {
    auto* e = (Engine*)ctx;
    if (Entity* ent = e->scene->at((int)id)) {
        /* 无 Transform2D 组件的节点（如纯 UI 节点）：位置存在实体自身（父空间局部坐标） */
        bool has_tf = false;
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            if (d && (d->role & MQ_ROLE_TRANSFORM2D)) { has_tf = true; break; }
        }
        if (!has_tf) { ent->world_x = x; ent->world_y = y; return; }
        // 写回 transform2d 组件（按 role；兼容扁平 x/y 与 math.Transform2D.local.position）
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            if (!d || !(d->role & MQ_ROLE_TRANSFORM2D)) continue;
            const mq_field_desc* fx = mq_reflect_field(d, "x");
            const mq_field_desc* fy = mq_reflect_field(d, "y");
            if (fx && fy) {
                *(float*)(blob.data() + fx->offset) = x;
                *(float*)(blob.data() + fy->offset) = y;
                continue;
            }
            if (const mq_field_desc* fl = mq_reflect_field(d, "local")) {
                const mq_type_desc* lt = mq_reflect_type_by_id(e->reflect, fl->type_id);
                if (!lt) continue;
                void* lp = blob.data() + fl->offset;
                if (const mq_field_desc* fp = mq_reflect_field(lt, "position")) {
                    const mq_type_desc* pt = mq_reflect_type_by_id(e->reflect, fp->type_id);
                    if (!pt) continue;
                    void* pos = (uint8_t*)lp + fp->offset;
                    if (const mq_field_desc* px = mq_reflect_field(pt, "x")) *(float*)((uint8_t*)pos + px->offset) = x;
                    if (const mq_field_desc* py = mq_reflect_field(pt, "y")) *(float*)((uint8_t*)pos + py->offset) = y;
                }
            }
        }
        ent->world_x = x; ent->world_y = y;
    }
}
float entity_x(void* ctx, float id) { Entity* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->world_x : 0.0f; }
float entity_y(void* ctx, float id) { Entity* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->world_y : 0.0f; }
float entity_count(void* ctx) { return (float)((Engine*)ctx)->scene->count(); }
void  entity_destroy(void* ctx, float id) { ((Engine*)ctx)->scene->remove((int)id); }

/* 实体查询（脚本按名字找节点，docs/12/19） */
const char* entity_name(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->name.c_str() : "";
}
float entity_find(void* ctx, const char* name) {
    if (!name) return -1.0F;
    auto* eng = (Engine*)ctx;
    int best = -1;
    for (auto& [k, e] : eng->scene->ents)          // 稳定：取最小 id（确定性，docs/10 D4）
        if (e.name == name && (best < 0 || k < best)) best = k;
    return (float)best;
}
/* 运行期配置：读宿主环境变量（不存在 → ""）。返回 std::string 的 c_str，绑定侧立即拷贝。 */
const char* env(void*, const char* name) {
    static std::string scratch;
    scratch.clear();
    if (name && *name) {
        if (const char* v = std::getenv(name)) scratch = v;
    }
    return scratch.c_str();
}

/* 物理步进（docs/15 §3）：无参数——固定步长由求解器配置决定（禁止脚本改变 dt）。
 * 求解器内部按 body.index 稳定排序、固定迭代 → 确定性；结果只读回写到组件。 */
void bounce_step(void*, float) {
}

void physics_step(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng->physics) return;
    mq_physics_step(eng->physics);
    std::sort(eng->balls.begin(), eng->balls.end(),
              [](const Engine::Ball& a, const Engine::Ball& b) { return a.id < b.id; });
    for (auto& b : eng->balls) {                       // 稳定序回写（渲染只读）
        float x = 0, y = 0;
        if (mq_physics_get_transform(eng->physics, b.body, &x, &y) != MQ_PHYSICS_OK) continue;
        b.vx = 0; b.vy = 0;
        mq_physics_get_velocity(eng->physics, b.body, &b.vx, &b.vy);
        host::entity_set_pos(eng, (float)b.id, x, y);
    }
}

/* ---- 碰撞查询（docs/15 §3、docs/19）------------------------------------------------
 * 把求解器**本帧**算出的接触对交给脚本判玩法（撞管死亡 / 穿越得分 / 机关触发）。
 * 刻意只做"查询"不做"回调"：引擎不硬编码碰撞事件语义，玩法仍在脚本层。
 * 索引越界一律安全返回（false / -1 / 0），不崩——脚本是远程/热更内容，必须容错。 */
float physics_contact_count(void* ctx) {
    auto* eng = (Engine*)ctx;
    eng->refresh_contacts();
    return (float)eng->contact_entities.size();
}
bool physics_contact_pair(void* ctx, float i) {
    auto* eng = (Engine*)ctx;
    eng->refresh_contacts();
    const int k = (int)i;
    if (k < 0 || (size_t)k >= eng->contact_entities.size()) return false;
    const auto& [a, b] = eng->contact_entities[(size_t)k];
    return a >= 0 && b >= 0;                        // 撞世界边界（平面接触）→ false
}
float physics_contact_a(void* ctx, float i) {
    auto* eng = (Engine*)ctx;
    eng->refresh_contacts();
    const int k = (int)i;
    return (k >= 0 && (size_t)k < eng->contact_entities.size()) ? (float)eng->contact_entities[(size_t)k].first : -1.0F;
}
float physics_contact_b(void* ctx, float i) {
    auto* eng = (Engine*)ctx;
    eng->refresh_contacts();
    const int k = (int)i;
    return (k >= 0 && (size_t)k < eng->contact_entities.size()) ? (float)eng->contact_entities[(size_t)k].second : -1.0F;
}
/* 接触法线（单位向量；a → b 方向，见 abi/physics.h）。平面接触同样有法线。 */
static bool contact_normal_at(Engine* eng, float i, float* nx, float* ny) {
    if (!eng->physics) return false;
    mq_physics_contact c{};
    if (mq_physics_contact_at(eng->physics, (uint32_t)i, &c) != 0) return false;
    *nx = c.nx; *ny = c.ny;
    return true;
}
float physics_contact_normal_x(void* ctx, float i) {
    float nx = 0, ny = 0;
    return contact_normal_at((Engine*)ctx, i, &nx, &ny) ? nx : 0.0F;
}
float physics_contact_normal_y(void* ctx, float i) {
    float nx = 0, ny = 0;
    return contact_normal_at((Engine*)ctx, i, &nx, &ny) ? ny : 0.0F;
}

float spawn_ball(void* ctx, float x, float y, float vx, float vy, float radius) {
    auto* eng = (Engine*)ctx;
    float id = host::entity_spawn(eng);
    if (id < 0) return -1.0F;
    attach_component(*eng, (int)id, "scn.Transform2D");
    attach_component(*eng, (int)id, "scn.Sprite");
    Entity* be = eng->scene->at((int)id);
    if (be) {
        for (auto& [tid, blob] : be->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
            if (!d || !(d->role & MQ_ROLE_SPRITE)) continue;
            if (const mq_field_desc* fr = mq_reflect_field(d, "radius")) *(float*)(blob.data() + fr->offset) = radius > 0 ? radius : 8.0F;
            if (const mq_field_desc* fg = mq_reflect_field(d, "r")) *(float*)(blob.data() + fg->offset) = 0.4F + eng->next_rng() / 4294967296.0F * 0.6F;
            if (const mq_field_desc* fg = mq_reflect_field(d, "g")) *(float*)(blob.data() + fg->offset) = 0.5F;
            if (const mq_field_desc* fb = mq_reflect_field(d, "b")) *(float*)(blob.data() + fb->offset) = 1.0F;
        }
    }
    host::entity_set_pos(eng, id, x, y);
    Engine::Ball ball{ (int)id, vx, vy, radius > 0 ? radius : 8.0F, mq_physics_body{} };
    if (eng->physics) {
        mq_physics_body_desc bd{};
        bd.struct_size = (uint32_t)sizeof bd;
        bd.type = MQ_PHYSICS_BODY_DYNAMIC;
        bd.shape = MQ_PHYSICS_SHAPE_BALL;
        bd.x = x; bd.y = y; bd.vx = vx; bd.vy = vy;
        bd.radius = ball.radius;
        bd.mass = 1.0F;
        bd.restitution = eng->scene->desc.restitution;
        bd.friction = 0.2F;
        if (mq_physics_add_body(eng->physics, &bd, &ball.body) != MQ_PHYSICS_OK)
            ball.body = mq_physics_body{};
    }
    eng->balls.push_back(ball);
    return id;
}

/* 写回 Label 组件的 text（按 role；字符串字段，docs/19 §8）。 */
void set_label(void* ctx, float id, const char* text) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e || !text) return;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_LABEL)) continue;
        if (const mq_field_desc* f = mq_reflect_field(d, "text"))
            *(std::string*)(blob.data() + f->offset) = text;
    }
}


/* ============================================================================
 * 节点/组件 API（对照 参考实现：Node / Component / Director）
 * 全部经反射按 role 处理，不硬编码组件类型名（docs/06/17/18）。
 * ==========================================================================*/

/* 按 role 找组件 blob（找不到返回 nullptr）。 */


/* ---------------- 宿主 API 实现（声明来自生成的 maque_api.h） ---------------- */
/* ---- 语义辅助（全局：host 函数与引擎内部共用）---- */

/* ---- 层级 ---- */
float node_set_parent(void* ctx, float id, float parent) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    const int p = (int)parent;
    if (p == (int)id) return 0.0F;                       // 禁止自环
    for (int a = p; a >= 0; ) {                          // 禁止成环（沿祖先链上溯）
        if (a == (int)id) return 0.0F;
        Entity* pe = eng->scene->at(a);
        a = pe ? pe->parent : -1;
    }
    e->parent = p;
    return 1.0F;
}

float node_parent(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (float)e->parent : -1.0F;
}

float node_child_count(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    int n = 0;
    for (auto& [k, e] : eng->scene->ents) if (e.parent == (int)id) ++n;
    return (float)n;
}

float node_child_at(void* ctx, float id, float index) {
    auto* eng = (Engine*)ctx;
    std::vector<int> kids;
    for (auto& [k, e] : eng->scene->ents) if (e.parent == (int)id) kids.push_back(k);
    std::sort(kids.begin(), kids.end());                 // 稳定：按实体 id（创建序）
    const int i = (int)index;
    return (i >= 0 && i < (int)kids.size()) ? (float)kids[(size_t)i] : -1.0F;
}

float node_find_child(void* ctx, float id, const char* name) {
    auto* eng = (Engine*)ctx;
    std::vector<int> kids;
    for (auto& [k, e] : eng->scene->ents) if (e.parent == (int)id) kids.push_back(k);
    std::sort(kids.begin(), kids.end());
    for (int k : kids) {
        Entity* e = eng->scene->at(k);
        if (e && e->name == (name ? name : "")) return (float)k;
    }
    return -1.0F;
}

void node_set_name(void* ctx, float id, const char* name) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    if (e && name) e->name = name;
}

/* ---- 节点属性 ---- */
void node_set_active(void* ctx, float id, float on) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->active = on != 0.0F;
}
float node_active(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (e->active ? 1.0F : 0.0F) : 0.0F;
}
void node_set_scale(void* ctx, float id, float v) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->scale = v;
}
float node_scale(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->scale : 1.0F;
}
void node_set_angle(void* ctx, float id, float deg) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->angle = deg;
}
float node_angle(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->angle : 0.0F;
}
void node_set_opacity(void* ctx, float id, float a) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->opacity = a < 0.0F ? 0.0F : (a > 1.0F ? 1.0F : a);
}
float node_opacity(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? e->opacity : 1.0F;
}
void node_set_z_index(void* ctx, float id, float z) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    if (e) e->z_index = (int)z;
}
float node_z_index(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (float)e->z_index : 0.0F;
}

/* 位置（node.position / setPosition）：写回 Transform2D 组件（非仅缓存字段）。 */
void node_set_position(void* ctx, float id, float x, float y) { entity_set_pos(ctx, id, x, y); }
float node_position_x(void* ctx, float id) { return entity_x(ctx, id); }
float node_position_y(void* ctx, float id) { return entity_y(ctx, id); }

/* 内容尺寸/锚点：由 ui.Control 承载（UITransform）。 */
float node_width(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    if (!blob || !d) return 0.0F;
    auto* v = (const float*)nullptr;
    (void)v;
    const uint8_t* base = blob->data();
    const mq_field_desc* f = mq_reflect_field(d, "offset_max");
    if (!f) return 0.0F;
    return *(const float*)(base + f->offset);            // Vec2.x
}
float node_height(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    if (!blob || !d) return 0.0F;
    const uint8_t* base = blob->data();
    const mq_field_desc* f = mq_reflect_field(d, "offset_max");
    if (!f) return 0.0F;
    return *(const float*)(base + f->offset + sizeof(float));   // Vec2.y
}
void node_set_content_size(void* ctx, float id, float w, float h) {
    auto* eng = (Engine*)ctx;
    if (!comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL))
        attach_component(*eng, (int)id, "ui.Control");        // 控件盒模型恒在（按需补挂）
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    if (!blob || !d) return;
    uint8_t* base = blob->data();
    if (const mq_field_desc* f = mq_reflect_field(d, "offset_max")) {
        *(float*)(base + f->offset) = w;
        *(float*)(base + f->offset + sizeof(float)) = h;
    }
}
void node_set_anchor(void* ctx, float id, float ax, float ay) {
    auto* eng = (Engine*)ctx;
    auto* blob = comp_by_role(*eng, (int)id, MQ_ROLE_UI_CONTROL);
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, "ui.Control");
    if (!blob || !d) return;
    uint8_t* base = blob->data();
    for (const char* fn : { "anchor_min", "anchor_max", "pivot" }) {
        if (const mq_field_desc* f = mq_reflect_field(d, fn)) {
            *(float*)(base + f->offset) = ax;
            *(float*)(base + f->offset + sizeof(float)) = ay;
        }
    }
}

/* ---- 组件 ---- */
float node_has_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    return comp_by_name(*eng, (int)id, type, nullptr) ? 1.0F : 0.0F;
}

float node_add_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    const char* tn = type ? type : "";
    const mq_type_desc* d = mq_reflect_type_by_name(eng->reflect, tn);
    if (!d) {
        char fq[128];
        for (const char* ns : { "scn", "ui", "physics", "anim", "render", "input" }) {
            std::snprintf(fq, sizeof fq, "%s.%s", ns, tn);
            d = mq_reflect_type_by_name(eng->reflect, fq);
            if (d) break;
        }
    }
    if (!d || !eng->scene->at((int)id)) return 0.0F;
    attach_component(*eng, (int)id, d->name);
    return 1.0F;
}

float node_remove_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, type, &d);
    if (!e || !blob || !d) return 0.0F;
    destroy_blob(*eng, d->type_id, *blob);
    e->comps.erase(d->type_id);
    return 1.0F;
}

float node_component_count(void* ctx, float id) {
    Entity* e = ((Engine*)ctx)->scene->at((int)id);
    return e ? (float)e->comps.size() : 0.0F;
}

const char* node_component_at(void* ctx, float id, float index) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e) return "";
    std::vector<uint32_t> tids;
    for (auto& [t, b] : e->comps) tids.push_back(t);
    std::sort(tids.begin(), tids.end());                 // 稳定顺序（type_id 升序）
    const int i = (int)index;
    if (i < 0 || i >= (int)tids.size()) return "";
    const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tids[(size_t)i]);
    return d ? d->name : "";
}

float node_child_with_component(void* ctx, float id, const char* type) {
    auto* eng = (Engine*)ctx;
    std::vector<int> kids;
    for (auto& [k, e] : eng->scene->ents) if (e.parent == (int)id) kids.push_back(k);
    std::sort(kids.begin(), kids.end());
    for (int k : kids) if (comp_by_name(*eng, k, type, nullptr)) return (float)k;
    return -1.0F;
}

/* ---- 组件字段（反射读写）---- */
float node_get_number(void* ctx, float id, const char* comp, const char* field) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    return read_num(d, blob->data(), field ? field : "", 0.0F);
}

float node_set_number(void* ctx, float id, const char* comp, const char* field, float value) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    return write_num(d, blob->data(), field ? field : "", (double)value) ? 1.0F : 0.0F;
}

float node_get_bool(void* ctx, float id, const char* comp, const char* field) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, field ? field : "");
    if (!f || f->kind != MQ_TK_BOOL) return 0.0F;
    return *(const bool*)(blob->data() + f->offset) ? 1.0F : 0.0F;
}

float node_set_bool(void* ctx, float id, const char* comp, const char* field, float value) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, field ? field : "");
    if (!f || f->kind != MQ_TK_BOOL) return 0.0F;
    *(bool*)(blob->data() + f->offset) = value != 0.0F;
    return 1.0F;
}

const char* node_get_string(void* ctx, float id, const char* comp, const char* field) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return "";
    if (const std::string* sp = read_str(d, blob->data(), field ? field : "")) return sp->c_str();
    return "";
}

float node_set_string(void* ctx, float id, const char* comp, const char* field, const char* value) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = comp_by_name(*eng, (int)id, comp, &d);
    if (!blob || !d) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, field ? field : "");
    if (!f || f->kind != MQ_TK_STRING) return 0.0F;
    *(std::string*)(blob->data() + f->offset) = value ? value : "";
    /* 改了资源引用就**当场解析并缓存**。
     * 为什么必须在这里做：渲染提取（Scene::extract）是**只读**的（docs/13 §8 / docs/15 §1：
     * 不得触发加载），它只查 material_cache / tex_cache。装配期会预解析场景自带实体的资源，
     * 但**脚本运行时生成的**实体没有那次机会 —— 于是贴图缺失、图元退化成纯色方块
     *（实测：动态生成的水管全是 sprite 默认蓝色，而不是贴图的绿色）。
     * 所以"赋值即解析"是这条路径上唯一能补齐的时机。 */
    if (d->role & MQ_ROLE_SPRITE) {
        const std::string ref = value ? value : "";
        if (!ref.empty()) {
            if (f == mq_reflect_field(d, "material")) {
                scn::SpriteMaterial m{};
                if (eng->load_material(ref, m)) {
                    eng->material_cache[ref] = m;
                    if (!m.texture.empty()) eng->load_texture_ref(m.texture);
                }
            } else if (f == mq_reflect_field(d, "texture")) {
                eng->load_texture_ref(ref);
            }
        }
    }
    return 1.0F;
}

/* ---- 事件（node.on/off/emit；emit 沿父链冒泡）---- */
float node_on(void* ctx, float id, const char* event, const char* handler) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene->at((int)id) || !event || !handler) return 0.0F;
    eng->node_events[(int)id].push_back({ event, handler });
    return 1.0F;
}

float node_off(void* ctx, float id, const char* event) {
    auto* eng = (Engine*)ctx;
    auto it = eng->node_events.find((int)id);
    if (it == eng->node_events.end()) return 0.0F;
    const std::string ev = event ? event : "";
    auto& v = it->second;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Engine::NodeHandler& h) { return h.event == ev; }), v.end());
    return (float)(before - v.size());
}

float node_emit(void* ctx, float id, const char* event) { return node_emit_number(ctx, id, event, 0.0F); }

float node_emit_number(void* ctx, float id, const char* event, float value) {
    auto* eng = (Engine*)ctx;
    if (!eng->bus || !event) return 0.0F;
    const std::string ev = event;
    int hits = 0;
    /* 目标 → 祖先（冒泡）；每层按注册顺序派发（确定性） */
    for (int cur = (int)id; cur >= 0; ) {
        auto it = eng->node_events.find(cur);
        if (it != eng->node_events.end()) {
            for (const Engine::NodeHandler& h : it->second) {
                if (h.event != ev) continue;
                mq_script_bus_call(eng->bus, h.handler.c_str(), (double)cur, (double)value, nullptr);
                ++hits;
            }
        }
        Entity* e = eng->scene->at(cur);
        cur = e ? e->parent : -1;
    }
    return (float)hits;
}

/* ---- UI 控件值（按 role 统一读写）---- */
float ui_set_value(void* ctx, float id, float v) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    if (auto* b = comp_by_name(*eng, (int)id, "ProgressBar", &d)) {
        write_num(d, b->data(), "value", v < 0 ? 0 : (v > 1 ? 1 : v));
        return 1.0F;
    }
    if (auto* b = comp_by_name(*eng, (int)id, "Slider", &d)) {
        const float lo = read_num(d, b->data(), "min_value", 0.0F);
        const float hi = read_num(d, b->data(), "max_value", 1.0F);
        const float c = v < lo ? lo : (v > hi ? hi : v);
        write_num(d, b->data(), "value", c);
        write_num(d, b->data(), "ratio", hi > lo ? (c - lo) / (hi - lo) : 0.0F);
        node_emit_number(eng, id, "changed", c);
        return 1.0F;
    }
    return 0.0F;
}

float ui_get_value(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    if (auto* b = comp_by_name(*eng, (int)id, "ProgressBar", &d)) return read_num(d, b->data(), "value", 0.0F);
    if (auto* b = comp_by_name(*eng, (int)id, "Slider", &d)) return read_num(d, b->data(), "value", 0.0F);
    return 0.0F;
}

float ui_set_checked(void* ctx, float id, float on) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*eng, (int)id, "CheckBox", &d);
    if (!b) return 0.0F;
    write_num(d, b->data(), "checked", on != 0.0F ? 1.0 : 0.0);
    node_emit_number(eng, id, "toggled", on != 0.0F ? 1.0F : 0.0F);
    return 1.0F;
}

float ui_get_checked(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*eng, (int)id, "CheckBox", &d);
    if (!b) return 0.0F;
    return read_num(d, b->data(), "checked", 0.0F);
}



/* ---- 文本（Label/TextLabel/RichText 通用：按 role 定位组件）---- */
std::vector<uint8_t>* text_comp(Engine& eng, int id, const mq_type_desc** out_d) {
    std::vector<uint8_t>* blob = comp_by_role(eng, id, MQ_ROLE_UI_LABEL);
    if (!blob) blob = comp_by_role(eng, id, MQ_ROLE_UI_TEXT);
    if (blob && out_d) {
        Entity* e = eng.scene->at(id);
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (d && (d->role & (MQ_ROLE_UI_LABEL | MQ_ROLE_UI_TEXT)) && &b == blob) { *out_d = d; break; }
        }
    }
    return blob;
}

float ui_set_text(void* ctx, float id, const char* text) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d || !text) return 0.0F;
    const mq_field_desc* f = mq_reflect_field(d, "text");
    if (!f || f->kind != MQ_TK_STRING) return 0.0F;
    *(std::string*)(blob->data() + f->offset) = text;
    return 1.0F;
}

const char* ui_get_text(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return "";
    if (const std::string* s = read_str(d, blob->data(), "text")) return s->c_str();
    return "";
}

/* 实测排版尺寸：兼容 text_w/text_h（scn.Label）、text_size（ui.TextLabel）、content_size（RichText） */
float ui_text_width(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return 0.0F;
    for (const char* fn : { "text_w", "text_size", "content_size" })
        if (const mq_field_desc* f = mq_reflect_field(d, fn))
            return *(const float*)(blob->data() + f->offset);
    return 0.0F;
}

float ui_text_height(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return 0.0F;
    /* text_h 是**标量** float；text_size / content_size 是 Vec2（y 在第二个分量）。
     * 此前一律按 +sizeof(float) 读 → 标量路径读到下一个字段（line_count 的 int 位型）。 */
    if (const mq_field_desc* f = mq_reflect_field(d, "text_h"))
        return *(const float*)(blob->data() + f->offset);
    for (const char* fn : { "text_size", "content_size" })
        if (const mq_field_desc* f = mq_reflect_field(d, fn))
            return *((const float*)(blob->data() + f->offset) + 1);
    return 0.0F;
}

float ui_text_lines(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const mq_type_desc* d = nullptr;
    auto* blob = text_comp(*eng, (int)id, &d);
    if (!blob || !d) return 0.0F;
    if (const mq_field_desc* f = mq_reflect_field(d, "line_count"))
        return (float)*(const int32_t*)(blob->data() + f->offset);
    return 0.0F;
}

/* ---- Tween ---- */
float tween_start(void* ctx, float id, const char* field, float to, float duration, float easing) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene->at((int)id) || !field) return 0.0F;
    const mq_type_desc* td = nullptr;
    const mq_field_desc* fd = nullptr;
    void* base = nullptr;
    if (!resolve_field_path(*eng, (int)id, field, &td, &fd, &base) || !fd) return 0.0F;
    const float from = read_num(td, base, fd->name, 0.0F);
    eng->tweens.push_back({ (int)id, field, from, to, 0.0F, duration > 0.0F ? duration : (1.0F / 60.0F),
                            (int)easing });
    return 1.0F;
}

float tween_stop(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    const size_t before = eng->tweens.size();
    eng->tweens.erase(std::remove_if(eng->tweens.begin(), eng->tweens.end(),
                                     [&](const Engine::Tween& t) { return t.entity == (int)id; }),
                      eng->tweens.end());
    return (float)(before - eng->tweens.size());
}

float tween_count(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    int n = 0;
    for (const auto& t : eng->tweens) if (t.entity == (int)id) ++n;
    return (float)n;
}

float tween_progress(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    for (const auto& t : eng->tweens)
        if (t.entity == (int)id) return t.duration > 0.0F ? (t.elapsed / t.duration) : 1.0F;
    return 1.0F;
}

/* ---- schedule / scheduleOnce ---- */
float schedule_once(void* ctx, const char* handler, float delay) {
    auto* eng = (Engine*)ctx;
    if (!handler || !*handler) return 0.0F;
    eng->schedules.push_back({ handler, 0.0F, delay > 0.0F ? delay : 0.0F, false });
    return 1.0F;
}

float schedule(void* ctx, const char* handler, float interval) {
    auto* eng = (Engine*)ctx;
    if (!handler || !*handler || interval <= 0.0F) return 0.0F;
    eng->schedules.push_back({ handler, interval, interval, true });
    return 1.0F;
}

float unschedule(void* ctx, const char* handler) {
    auto* eng = (Engine*)ctx;
    if (!handler) return 0.0F;
    const std::string h = handler;
    const size_t before = eng->schedules.size();
    eng->schedules.erase(std::remove_if(eng->schedules.begin(), eng->schedules.end(),
                                        [&](const Engine::Scheduled& s) { return s.handler == h; }),
                         eng->schedules.end());
    return (float)(before - eng->schedules.size());
}

float schedule_count(void* ctx) { return (float)((Engine*)ctx)->schedules.size(); }

/* ---- Spine 2D（docs/27）：按实体 id 寻址 ----
 * 语义：**问引擎当前状态**（组件字段 + 实例是唯一真相），脚本不自己记一份；
 * 越界 / 实体没有 SpineSkeleton 组件 → 0 / "" / -1，且不崩（脚本侧无需防御）。 */
static Engine::Spine* spine_at(Engine* eng, float id) {
    return eng ? eng->spine_of((int)id) : nullptr;
}

float spine_animation_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)sp->anim_names.size() : 0.0F;
}
const char* spine_animation_name(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return "";
    const int i = (int)index;
    if (i < 0 || i >= (int)sp->anim_names.size()) return "";
    return sp->anim_names[(size_t)i].c_str();
}
float spine_animation_index(void* ctx, float id, const char* name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !name) return -1.0F;
    for (size_t i = 0; i < sp->anim_names.size(); ++i)
        if (sp->anim_names[i] == name) return (float)i;
    return -1.0F;
}
float spine_play(void* ctx, float id, const char* name, float loop) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->inst || !sp->data || !name || !*name) return -1.0F;
    const int idx = mq_spine_data_animation_index(sp->data, name);
    if (idx < 0) return -1.0F;                                  // 名字不存在（不静默播错动画）
    if (mq_spine_instance_play(sp->inst, name, loop != 0.0F ? 1 : 0, 0) != MQ_SPINE_OK) return -1.0F;
    sp->last_anim = idx;
    /* 组件字段同步 = 唯一真相：否则下一帧 spine_step 会按组件里的旧 animation 又切回去 */
    node_set_number(ctx, id, "anim.SpineSkeleton", "animation", (float)idx);
    node_set_number(ctx, id, "anim.SpineSkeleton", "play_mode", loop != 0.0F ? 1.0F : 0.0F);
    return 0.0F;
}
const char* spine_current_animation(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->inst) return "";
    const char* nm = mq_spine_instance_animation_name(sp->inst, 0);
    return nm ? nm : "";
}
float spine_set_skin(void* ctx, float id, const char* name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->inst || !name || !*name) return -1.0F;
    if (mq_spine_instance_set_skin(sp->inst, name) != MQ_SPINE_OK) return -1.0F;
    for (size_t i = 0; i < sp->skin_names.size(); ++i)
        if (sp->skin_names[i] == name) {
            node_set_number(ctx, id, "anim.SpineSkeleton", "skin", (float)i);
            break;
        }
    return 0.0F;
}
float spine_skin_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)sp->skin_names.size() : 0.0F;
}
const char* spine_skin_name(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return "";
    const int i = (int)index;
    if (i < 0 || i >= (int)sp->skin_names.size()) return "";
    return sp->skin_names[(size_t)i].c_str();
}
float spine_time(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return (sp && sp->inst) ? mq_spine_instance_time(sp->inst, 0) : 0.0F;
}
float spine_set_time(void* ctx, float id, float t) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->inst) return -1.0F;
    if (mq_spine_instance_set_time(sp->inst, 0, t) != MQ_SPINE_OK) return -1.0F;
    /* 坑（docs/27 §4）：set_time 不重算姿势 → 立刻 step(0) 让本帧就按新时间求值 */
    mq_spine_instance_step(sp->inst, 0.0F);
    return 0.0F;
}
float spine_pages(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)sp->pages.size() : 0.0F;
}
float spine_page_texture(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    const int i = (int)index;
    if (i < 0 || i >= (int)sp->pages.size()) return -1.0F;
    const uint64_t h = sp->pages[(size_t)i].handle;
    return h > 0 ? (float)(h - 1u) : -1.0F;          // 句柄 = gfx 纹理 id + 1
}

/* ---- Spine 续：多轨 / 混合 / 事件 / 皮肤合成 / 骨骼 / 槽 / 物理 / 序列帧 ----------------
 * 语义（一致）：成功 0、失败 -1；查询类越界/无实例 → 0 / "" / -1，不崩（脚本无需防御）。
 * 事件投递=两条路：① pull（spine_event_*）② push（按名冒泡到 node_on 注册的处理函数，
 * 见 runtime.cpp 顶部 spine_event_cb 的命名约定）。两者是同一次记录（单测对拍过）。 */
float spine_set_animation(void* ctx, float id, float track, const char* name, float loop) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->inst || !sp->data || !name || !*name || track < 0.0F) return -1.0F;
    const int idx = mq_spine_data_animation_index(sp->data, name);
    if (idx < 0) return -1.0F;
    if (mq_spine_instance_set_animation(sp->inst, (int)track, name, loop != 0.0F ? 1 : 0) != MQ_SPINE_OK)
        return -1.0F;
    /* track 0 与组件字段同步（组件=唯一真相；否则下一帧 spine_step 会按旧值又切回去） */
    if ((int)track == 0) {
        sp->last_anim = idx;
        node_set_number(ctx, id, "anim.SpineSkeleton", "animation", (float)idx);
        node_set_number(ctx, id, "anim.SpineSkeleton", "play_mode", loop != 0.0F ? 1.0F : 0.0F);
    }
    return 0.0F;
}
float spine_add_animation(void* ctx, float id, float track, const char* name, float loop, float delay) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->inst || !sp->data || !name || !*name || track < 0.0F) return -1.0F;
    if (mq_spine_data_animation_index(sp->data, name) < 0) return -1.0F;
    return mq_spine_instance_add_animation(sp->inst, (int)track, name, loop != 0.0F ? 1 : 0, delay) == MQ_SPINE_OK
               ? 0.0F : -1.0F;
}
float spine_track_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)mq_spine_instance_track_count(sp->inst) : 0.0F;
}
const char* spine_track_animation(void* ctx, float id, float track) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || track < 0.0F) return "";
    const char* nm = mq_spine_instance_track_animation(sp->inst, (int)track);
    return nm ? nm : "";
}
float spine_queue_count(void* ctx, float id, float track) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || track < 0.0F) return 0.0F;
    return (float)mq_spine_instance_queue_count(sp->inst, (int)track);
}
float spine_clear_track(void* ctx, float id, float track) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || track < 0.0F) return -1.0F;
    mq_spine_instance_clear_track(sp->inst, (int)track);
    return 0.0F;
}
float spine_track_time(void* ctx, float id, float track) {
    auto* sp = spine_at((Engine*)ctx, id);
    return (sp && track >= 0.0F) ? mq_spine_instance_time(sp->inst, (int)track) : 0.0F;
}
float spine_set_track_time(void* ctx, float id, float track, float t) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || track < 0.0F) return -1.0F;
    if (mq_spine_instance_set_time(sp->inst, (int)track, t) != MQ_SPINE_OK) return -1.0F;
    /* 坑（docs/27 §4）：set_time 不重算姿势 → 立刻 step(0) 让本帧就按新时间求值 */
    mq_spine_instance_step(sp->inst, 0.0F);
    return 0.0F;
}
float spine_track_time_scale(void* ctx, float id, float track) {
    auto* sp = spine_at((Engine*)ctx, id);
    return (sp && track >= 0.0F) ? mq_spine_instance_track_time_scale(sp->inst, (int)track) : 0.0F;
}
float spine_set_track_time_scale(void* ctx, float id, float track, float scale) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || track < 0.0F) return -1.0F;
    return mq_spine_instance_set_track_time_scale(sp->inst, (int)track, scale) == MQ_SPINE_OK ? 0.0F : -1.0F;
}
float spine_time_scale(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? mq_spine_instance_time_scale(sp->inst) : 0.0F;
}
float spine_set_time_scale(void* ctx, float id, float scale) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    mq_spine_instance_set_time_scale(sp->inst, scale);
    node_set_number(ctx, id, "anim.SpineSkeleton", "time_scale", scale);   // 组件=唯一真相
    return 0.0F;
}
float spine_default_mix(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? mq_spine_instance_default_mix(sp->inst) : 0.0F;
}
float spine_set_default_mix(void* ctx, float id, float duration) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    mq_spine_instance_set_default_mix(sp->inst, duration);
    node_set_number(ctx, id, "anim.SpineSkeleton", "default_mix", duration);
    return 0.0F;
}
float spine_set_mix(void* ctx, float id, const char* from, const char* to, float duration) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !from || !to) return -1.0F;
    return mq_spine_instance_set_mix(sp->inst, from, to, duration) == MQ_SPINE_OK ? 0.0F : -1.0F;
}

/* 事件 pull：一次取一个字段（脚本只有标量，不做结构体） */
static const mq_spine_event* spine_event_at(Engine* eng, float id, float index) {
    Engine::Spine* sp = spine_at(eng, id);
    if (!sp || index < 0.0F) return nullptr;
    if (index >= (float)mq_spine_instance_event_count(sp->inst)) return nullptr;
    /* 事件记录在实例内部（预分配数组）→ 直接下标取；用 ABI 复制到线程局部静态槽返回指针。 */
    static thread_local mq_spine_event slot;
    if (mq_spine_instance_event(sp->inst, (int)index, &slot) != MQ_SPINE_OK) return nullptr;
    return &slot;
}
float spine_event_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)mq_spine_instance_event_count(sp->inst) : 0.0F;
}
float spine_event_type(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return ev ? (float)ev->type : -1.0F;
}
float spine_event_track(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return ev ? (float)ev->track : -1.0F;
}
const char* spine_event_animation(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return (ev && ev->animation) ? ev->animation : "";
}
const char* spine_event_name(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return (ev && ev->name) ? ev->name : "";
}
float spine_event_int(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return ev ? (float)ev->int_value : 0.0F;
}
float spine_event_float(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return ev ? ev->float_value : 0.0F;
}
const char* spine_event_string(void* ctx, float id, float index) {
    const mq_spine_event* ev = spine_event_at((Engine*)ctx, id, index);
    return (ev && ev->string_value) ? ev->string_value : "";
}
float spine_clear_events(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    mq_spine_instance_clear_events(sp->inst);
    return 0.0F;
}

/* 骨骼（官方 IK demo：把 crosshair 设到指针位置 → IK 链跟随） */
float spine_bone_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)mq_spine_instance_bone_count(sp->inst) : 0.0F;
}
const char* spine_bone_name(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return "";
    const char* nm = mq_spine_instance_bone_name(sp->inst, (int)index);
    return nm ? nm : "";
}
float spine_bone_find(void* ctx, float id, const char* name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !name || !*name) return -1.0F;
    return (float)mq_spine_instance_bone_find(sp->inst, name);
}
float spine_bone_value(void* ctx, float id, float index, float what) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return 0.0F;
    float x = 0.0F, y = 0.0F;
    const int i = (int)index, w = (int)what;
    /* what: 0=局部x 1=局部y 2=世界x 3=世界y（世界坐标 = 骨架子空间，y 向上） */
    const int rc = (w < 2) ? mq_spine_instance_bone_position_at(sp->inst, i, &x, &y)
                           : mq_spine_instance_bone_world_position(sp->inst, i, &x, &y);
    if (rc != MQ_SPINE_OK) return 0.0F;
    return (w == 0 || w == 2) ? x : y;
}
float spine_bone_set_position(void* ctx, float id, float index, float x, float y) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return -1.0F;
    return mq_spine_instance_bone_set_position(sp->inst, (int)index, x, y) == MQ_SPINE_OK ? 0.0F : -1.0F;
}
float spine_bone_set_world_position(void* ctx, float id, float index, float x, float y) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return -1.0F;
    return mq_spine_instance_bone_set_world_position(sp->inst, (int)index, x, y) == MQ_SPINE_OK ? 0.0F : -1.0F;
}
float spine_update_world_transform(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    /* 只重算世界变换（不 apply 动画）→ 设完骨骼位置后让 IK 链**当帧**跟随。
     * 用 Physics_Update 与主循环一致（物理时间轴不变）。 */
    return mq_spine_instance_update_world_transform(sp->inst, MQ_SPINE_PHYSICS_UPDATE) == MQ_SPINE_OK ? 0.0F : -1.0F;
}

/* 槽（官方 Separator demo：同一骨架拆前后两段渲染） */
float spine_slot_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)mq_spine_instance_slot_count(sp->inst) : 0.0F;
}
const char* spine_slot_name(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return "";
    const char* nm = mq_spine_instance_slot_name(sp->inst, (int)index);
    return nm ? nm : "";
}
float spine_slot_index(void* ctx, float id, const char* name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !name || !*name) return -1.0F;
    return (float)mq_spine_instance_slot_index(sp->inst, name);
}
const char* spine_slot_attachment(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return "";
    const char* nm = mq_spine_instance_slot_attachment(sp->inst, (int)index);
    return nm ? nm : "";
}
float spine_set_slots_range(void* ctx, float id, float from, float to) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    if (mq_spine_instance_set_slots_range(sp->inst, (int)from, (int)to) != MQ_SPINE_OK) return -1.0F;
    node_set_number(ctx, id, "anim.SpineSkeleton", "slots_from", from);
    node_set_number(ctx, id, "anim.SpineSkeleton", "slots_to", to);
    return 0.0F;
}
float spine_slots_from(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    int f = -1, t = -1;
    mq_spine_instance_slots_range(sp->inst, &f, &t);
    return (float)f;
}
float spine_slots_to(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    int f = -1, t = -1;
    mq_spine_instance_slots_range(sp->inst, &f, &t);
    return (float)t;
}
float spine_set_attachment(void* ctx, float id, const char* slot_name, const char* attachment_name,
                           const char* skin_name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !slot_name || !*slot_name) return -1.0F;
    return mq_spine_instance_set_attachment(sp->inst, slot_name, attachment_name, skin_name) == MQ_SPINE_OK
               ? 0.0F : -1.0F;
}
float spine_skin_find(void* ctx, float id, const char* name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !sp->data || !name || !*name) return -1.0F;
    return (float)mq_spine_data_skin_find(sp->data, name);
}
/* 合成皮肤句柄：脚本侧只有标量 → 用 sp->own_skins 的下标当句柄（>=0；-1 失败）。
 * 生命周期归引擎（spine_release 统一销毁）→ 脚本不用管释放。 */
static Engine::Spine* spine_skin_owner(Engine* eng, float id, float handle, mq_spine_skin** out) {
    Engine::Spine* sp = spine_at(eng, id);
    if (!sp) return nullptr;
    if (handle < 0.0F) {
        *out = sp->composed ? sp->composed : (sp->own_skins.empty() ? nullptr : sp->own_skins.back());
        return *out ? sp : nullptr;
    }
    const int i = (int)handle;
    if (i >= (int)sp->own_skins.size()) return nullptr;
    *out = sp->own_skins[(size_t)i];
    return *out ? sp : nullptr;
}
float spine_skin_create(void* ctx, float id, const char* name) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || !name || !*name) return -1.0F;
    mq_spine_skin* sk = mq_spine_skin_create(name);
    if (!sk) return -1.0F;
    sp->own_skins.push_back(sk);
    return (float)(sp->own_skins.size() - 1);
}
float spine_skin_add(void* ctx, float id, float handle, const char* source_skin) {
    auto* eng = (Engine*)ctx;
    mq_spine_skin* dst = nullptr;
    Engine::Spine* sp = spine_skin_owner(eng, id, handle, &dst);
    if (!sp || !dst || !sp->data || !source_skin || !*source_skin) return -1.0F;
    const int idx = mq_spine_data_skin_find(sp->data, source_skin);
    if (idx < 0) {
        std::print(stderr, "runtime: spine_skin_add: 骨架里没有皮肤 '{}'\n", source_skin);
        return -1.0F;
    }
    mq_spine_skin* src = mq_spine_data_skin_at(sp->data, idx);
    return mq_spine_skin_add(dst, src) == MQ_SPINE_OK ? 0.0F : -1.0F;
}
float spine_set_skin_obj(void* ctx, float id, float handle) {
    auto* eng = (Engine*)ctx;
    mq_spine_skin* sk = nullptr;
    Engine::Spine* sp = spine_skin_owner(eng, id, handle, &sk);
    if (!sp || !sk) return -1.0F;
    if (mq_spine_instance_set_skin_obj(sp->inst, sk) != MQ_SPINE_OK) return -1.0F;
    sp->composed = sk;
    return 0.0F;
}
/* 皮肤合成（官方 MixAndMatch）：names = 逗号分隔的皮肤名（骨架内声明序的皮肤）。
 * 内部分三步：建皮肤 → 逐个 addSkin → 设为当前；句柄由引擎持有（切场景/退出时释放）。 */
float spine_skin_compose(void* ctx, float id, const char* names) {
    auto* eng = (Engine*)ctx;
    auto* sp = spine_at(eng, id);
    if (!sp || !sp->data || !names || !*names) return -1.0F;
    mq_spine_skin* combo = mq_spine_skin_create(names);     // 名字用整串（唯一、可诊断）
    if (!combo) return -1.0F;
    int added = 0;
    std::string seg;
    auto flush = [&]() -> bool {
        if (seg.empty()) return true;
        /* 允许前后空白，写场景时好读 */
        size_t b = seg.find_first_not_of(" \t");
        size_t e = seg.find_last_not_of(" \t");
        if (b == std::string::npos) { seg.clear(); return true; }
        const std::string nm = seg.substr(b, e - b + 1);
        seg.clear();
        const int idx = mq_spine_data_skin_find(sp->data, nm.c_str());
        if (idx < 0) {
            std::print(stderr, "runtime: spine_skin_compose: 皮肤 '{}' 不存在（骨架里没有）\n", nm);
            return false;
        }
        mq_spine_skin* src = mq_spine_data_skin_at(sp->data, idx);
        if (!src || mq_spine_skin_add(combo, src) != MQ_SPINE_OK) return false;
        ++added;
        return true;
    };
    bool ok = true;
    for (const char* p = names;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!flush()) { ok = false; break; }
            if (*p == '\0') break;
        } else {
            seg.push_back(*p);
        }
    }
    if (!ok || added == 0) { mq_spine_skin_destroy(combo); return -1.0F; }
    const int rc = mq_spine_instance_set_skin_obj(sp->inst, combo);
    if (rc != MQ_SPINE_OK) { mq_spine_skin_destroy(combo); return -1.0F; }
    sp->own_skins.push_back(combo);
    sp->composed = combo;
    std::print("runtime: spine entity {} 合成皮肤 '{}'（{} 个来源，附件 {}）\n",
               sp->entity, names, added, mq_spine_skin_attachment_count(combo));
    return 0.0F;
}
float spine_skin_attachment_count(void* ctx, float id, float handle) {
    mq_spine_skin* sk = nullptr;
    if (!spine_skin_owner((Engine*)ctx, id, handle, &sk)) return -1.0F;   // 没合成过 → -1（区别于 0 个附件）
    return (float)mq_spine_skin_attachment_count(sk);
}
float spine_set_two_color(void* ctx, float id, float on) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    /* 官方 setTwoColorTint(on)：在官方运行时里 = "给渲染器准备逐顶点暗色通道"。
     * 本引擎的网格着色**恒定**走两色着色路径（暗色通道始终产出、顶点 stride 固定含它），
     * 素材没有 dark 时填 (0,0,0,1) → 与关闭逐字节等价（见 mesh shader 与 gfx_twocolor 测试）。
     * 所以这里没有可切换的东西：如实把开关记进组件字段（可自省/可回放），不假装改变输出。 */
    node_set_number(ctx, id, "anim.SpineSkeleton", "two_color", on != 0.0F ? 1.0F : 0.0F);
    return 0.0F;
}
float spine_ik_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)mq_spine_instance_ik_count(sp->inst) : 0.0F;
}
const char* spine_ik_name(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return "";
    const char* nm = mq_spine_instance_ik_name(sp->inst, (int)index);
    return nm ? nm : "";
}
float spine_set_ik_mix(void* ctx, float id, float index, float mix) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return -1.0F;
    return mq_spine_instance_ik_set_mix(sp->inst, (int)index, mix) == MQ_SPINE_OK ? 0.0F : -1.0F;
}
float spine_physics_count(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    return sp ? (float)mq_spine_instance_physics_count(sp->inst) : 0.0F;
}
const char* spine_physics_name(void* ctx, float id, float index) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || index < 0.0F) return "";
    const char* nm = mq_spine_instance_physics_name(sp->inst, (int)index);
    return nm ? nm : "";
}
float spine_physics_translate(void* ctx, float id, float x, float y) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    mq_spine_instance_physics_translate(sp->inst, x, y);
    return 0.0F;
}
float spine_physics_reset(void* ctx, float id) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp) return -1.0F;
    mq_spine_instance_physics_reset(sp->inst);
    return 0.0F;
}
/* 序列帧（官方 Sequence demo：dragon-ess 的 flying） */
static bool spine_seq_info(Engine* eng, float id, float slot, mq_spine_sequence_info* out) {
    Engine::Spine* sp = spine_at(eng, id);
    if (!sp || slot < 0.0F) return false;
    return mq_spine_attachment_sequence_info(sp->inst, (int)slot, out) == MQ_SPINE_OK;
}
float spine_sequence_frames(void* ctx, float id, float slot) {
    mq_spine_sequence_info si{};
    return spine_seq_info((Engine*)ctx, id, slot, &si) ? (float)si.frames : 0.0F;
}
float spine_sequence_index(void* ctx, float id, float slot) {
    mq_spine_sequence_info si{};
    return spine_seq_info((Engine*)ctx, id, slot, &si) ? (float)si.index : -1.0F;
}
const char* spine_sequence_path(void* ctx, float id, float slot) {
    static thread_local std::string buf;                    // ABI 的 path 活到下次调用；这里再拷一份
    mq_spine_sequence_info si{};
    if (!spine_seq_info((Engine*)ctx, id, slot, &si) || !si.path) return "";
    buf = si.path;
    return buf.c_str();
}
float spine_sequence_value(void* ctx, float id, float slot, float what) {
    mq_spine_sequence_info si{};
    if (!spine_seq_info((Engine*)ctx, id, slot, &si)) return 0.0F;
    switch ((int)what) {
    case 0: return si.u;
    case 1: return si.v;
    case 2: return si.u2;
    case 3: return si.v2;
    case 4: return (float)si.width;
    case 5: return (float)si.height;
    case 6: return (float)si.setup_index;
    default: return 0.0F;
    }
}
float spine_set_sequence_index(void* ctx, float id, float slot, float frame) {
    auto* sp = spine_at((Engine*)ctx, id);
    if (!sp || slot < 0.0F) return -1.0F;
    return mq_spine_instance_set_sequence_index(sp->inst, (int)slot, (int)frame) == MQ_SPINE_OK ? 0.0F : -1.0F;
}

/* ---- destroy / find(path) ---- */
float node_destroy(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene->at((int)id)) return 0.0F;
    eng->pending_destroy.push_back((int)id);
    return 1.0F;
}

/* isValid(node)：实体仍存在且未处于待销毁队列。 */
/* isValid(node)：节点在**帧末真正销毁前**仍然有效（destroy 只是排队）。 */
float node_valid(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    return eng->scene->at((int)id) ? 1.0F : 0.0F;
}

float node_find_path(void* ctx, float root, const char* path) {
    auto* eng = (Engine*)ctx;
    if (!path || !*path) return -1.0F;
    int cur = (int)root;
    std::string seg;
    for (const char* p = path;; ++p) {
        if (*p == '/' || *p == '\0') {
            if (!seg.empty() && seg != ".") {
                int next = -1;
                std::vector<int> kids;
                for (auto& [k, e] : eng->scene->ents) if (e.parent == cur) kids.push_back(k);
                std::sort(kids.begin(), kids.end());
                for (int k : kids) {
                    Entity* e = eng->scene->at(k);
                    if (e && e->name == seg) { next = k; break; }
                }
                cur = next;
                if (cur < 0) return -1.0F;
            }
            seg.clear();
            if (*p == '\0') break;
        } else {
            seg.push_back(*p);
        }
    }
    return (float)cur;
}

/* ---- Director ---- */
void director_pause(void* ctx) { ((Engine*)ctx)->director_paused = true; }
void director_resume(void* ctx) { ((Engine*)ctx)->director_paused = false; }
float director_paused(void* ctx) { return ((Engine*)ctx)->director_paused ? 1.0F : 0.0F; }
float director_frame(void* ctx) { return (float)((Engine*)ctx)->frame; }

float director_load_scene(void* ctx, const char* path) {
    auto* eng = (Engine*)ctx;
    if (!path || !*path) return 0.0F;
    eng->pending_scene = path;                     // 帧边界生效（docs/10：帧内不加载资源）
    return 1.0F;
}


} // namespace host


namespace host {

/* 导航（确定性 2D 网格）：建网格/设障碍/指定目标/查询路径点数 */
void nav_setup(void* ctx, float cols, float rows, float cell) {
    auto* eng = (Engine*)ctx;
    mq_nav_config cfg{};
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.width = cols > 0 ? (uint32_t)cols : 32u;
    cfg.height = rows > 0 ? (uint32_t)rows : 32u;
    cfg.cell_size = cell > 0 ? cell : 16.0F;
    cfg.allow_diagonal = 1;
    if (eng->nav) mq_nav_destroy(eng->nav);
    eng->nav = mq_nav_create(&cfg);
    eng->agent_paths.clear();
}

void nav_block(void* ctx, float cx, float cy, float w, float h) {
    auto* eng = (Engine*)ctx;
    if (!eng->nav) return;
    mq_nav_set_rect(eng->nav, (uint32_t)cx, (uint32_t)cy,
                    w > 0 ? (uint32_t)w : 1u, h > 0 ? (uint32_t)h : 1u, 1);
}

float nav_goto(void* ctx, float id, float x, float y) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_NAV_AGENT)) continue;
        if (const mq_field_desc* fx = mq_reflect_field(d, "tx")) *(float*)(blob.data() + fx->offset) = x;
        if (const mq_field_desc* fy = mq_reflect_field(d, "ty")) *(float*)(blob.data() + fy->offset) = y;
        if (const mq_field_desc* fh = mq_reflect_field(d, "has_target")) *(bool*)(blob.data() + fh->offset) = true;
        eng->agent_paths[(int)id].path.clear();          // 目标变化 → 下一帧重算
        eng->agent_paths[(int)id].index = 0;
        return 1.0F;
    }
    return 0.0F;
}

float nav_path_len(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    auto it = eng->agent_paths.find((int)id);
    return it == eng->agent_paths.end() ? 0.0F : (float)(it->second.path.size() / 2);
}

void nav_tick(void* ctx) { ((Engine*)ctx)->nav_step(); }

void ball_clear(void* ctx) {
    auto* eng = (Engine*)ctx;
    for (auto& b : eng->balls) {
        if (eng->physics && b.body.index) mq_physics_remove_body(eng->physics, b.body);
        host::entity_destroy(eng, (float)b.id);
    }
    eng->balls.clear();
}

float ui_hit_at(void* ctx, float x, float y) {
    auto* eng = (Engine*)ctx;
    return (float)ui_hit_entity(*eng, x, y, nullptr);
}

float entity_valid(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    return eng->scene->at((int)id) ? 1.0F : 0.0F;
}

float entity_z(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
        if (const mq_field_desc* fz = mq_reflect_field(d, "z"))
            return (float)*(const int32_t*)(blob.data() + fz->offset);
    }
    return 0.0F;
}
void entity_set_z(void* ctx, float id, float z) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e) return;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
        if (const mq_field_desc* fz = mq_reflect_field(d, "z")) *(int32_t*)(blob.data() + fz->offset) = (int32_t)z;
    }
}

/* 输入状态（事件发生时；docs/19 §9：不读设备，回放一致） */
float input_x(void* ctx) { return ((Engine*)ctx)->mouse_x; }
float input_y(void* ctx) { return ((Engine*)ctx)->mouse_y; }
float input_target(void* ctx) {
    auto* e = (Engine*)ctx;
    return e->ev ? (float)e->ev->target : -1.0F;
}
float input_down(void* ctx) {
    auto* e = (Engine*)ctx;
    return e->input && mq_input_pointer_down(e->input, 0) ? 1.0F : 0.0F;
}
float input_key(void* ctx, float code) {
    auto* e = (Engine*)ctx;
    return e->input && mq_input_key_down(e->input, (uint32_t)code) ? 1.0F : 0.0F;
}
float input_focus(void* ctx) { return (float)mq_input_focus(((Engine*)ctx)->input); }
void  input_set_focus(void* ctx, float id) { mq_input_set_focus(((Engine*)ctx)->input, (int)id); }

/* UI 命中查询（复用与控制面同一套命中测试 → 行为一致） */
float ui_hit(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng->input) return -1.0F;
    mq_input_event probe{};
    probe.type = MQ_INPUT_POINTER_MOVE;
    probe.x = eng->mouse_x; probe.y = eng->mouse_y;
    mq_input_target t{};
    t.entity = -1;
    runtime_hit_test(eng, &probe, &t);
    return (float)t.entity;
}
float ui_hover(void* ctx) { return (float)((Engine*)ctx)->hover; }
float ui_pressed(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    Entity* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_UI_BUTTON)) continue;
        if (const mq_field_desc* fp = mq_reflect_field(d, "pressed"))
            return *(const bool*)(blob.data() + fp->offset) ? 1.0F : 0.0F;
    }
    return 0.0F;
}

} // namespace host

/* ---- ImGui 脚本 API ---- */
/* ImGui 脚本 API 实现（host 命名空间）。
 *
 * 分两类：
 *   ① **Engine 相关**（启用开关/帧计数/输入隔离）→ 读 Engine 成员；
 *   ② **纯控件** → 一行转发到 `mq_imgui_*` C ABI（模块边界干净：runtime 不 include imgui.h）。
 *
 * 所有函数都过同一道闸：未启用/未初始化时返回 0 或 ""，**脚本侧无需防御**
 *（面板是可选开发工具，关掉后脚本里的 imgui_* 调用变成无害空操作）。
 */
namespace host {

/* 统一前置：面板未启用 → 不调 ImGui（避免 ImGui 状态机被半初始化）。
 * 返回 null 表示"不要把调用透给模块"。 */
static Engine* imgui_ctx(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->imgui_on) return nullptr;
    if (!eng->imgui_inited) eng->imgui_setup();
    return eng->imgui_inited ? eng : nullptr;
}

float imgui_ready(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_inited) ? 1.0F : 0.0F;
}

float imgui_enabled(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on) ? 1.0F : 0.0F;
}

float imgui_set_enabled(void* ctx, float on) {
    auto* eng = (Engine*)ctx;
    if (!eng) return 0.0F;
    eng->imgui_enable(on != 0.0F);          // 与引擎级 F2 热键同一条实现
    return eng->imgui_on ? 1.0F : 0.0F;
}

float imgui_begin(void* ctx, const char* title) {
    Engine* eng = imgui_ctx(ctx);
    return eng ? (float)mq_imgui_begin(title) : 0.0F;
}

void imgui_end(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end(); }

float imgui_begin_closed(void* ctx, const char* title, float x, float y, float w, float h) {
    Engine* eng = imgui_ctx(ctx);
    return eng ? (float)mq_imgui_begin_at(title, x, y, w, h) : 0.0F;
}

void imgui_set_pos(void* ctx, float x, float y) { if (imgui_ctx(ctx)) mq_imgui_set_pos(x, y); }
void imgui_set_size(void* ctx, float w, float h) { if (imgui_ctx(ctx)) mq_imgui_set_size(w, h); }
void imgui_same_line(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_same_line(); }
void imgui_separator(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_separator(); }
void imgui_spacing(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_spacing(); }
void imgui_new_line(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_new_line(); }
void imgui_begin_disabled(void* ctx, float disabled) { if (imgui_ctx(ctx)) mq_imgui_begin_disabled(disabled != 0.0F); }
void imgui_end_disabled(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_disabled(); }
float imgui_begin_child(void* ctx, const char* id, float w, float h, float border) {
    Engine* eng = imgui_ctx(ctx);
    return eng ? (float)mq_imgui_begin_child(id, w, h, border != 0.0F) : 0.0F;
}
void imgui_end_child(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_child(); }
float imgui_begin_menu_bar(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_menu_bar() : 0.0F; }
void imgui_end_menu_bar(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_menu_bar(); }
float imgui_begin_menu(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_menu(label) : 0.0F;
}
void imgui_end_menu(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_menu(); }
float imgui_menu_item(void* ctx, const char* label, float selected, float enabled) {
    Engine* e = imgui_ctx(ctx);
    return e ? (float)mq_imgui_menu_item(label, selected != 0.0F, enabled != 0.0F) : 0.0F;
}
float imgui_begin_tab_bar(void* ctx, const char* id) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_tab_bar(id) : 0.0F;
}
void imgui_end_tab_bar(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_tab_bar(); }
float imgui_begin_tab_item(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_tab_item(label) : 0.0F;
}
void imgui_end_tab_item(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_tab_item(); }
float imgui_begin_table(void* ctx, const char* id, float cols) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_begin_table(id, (int)cols) : 0.0F;
}
void imgui_end_table(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_end_table(); }
void imgui_table_next_row(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_table_next_row(); }
void imgui_table_next_column(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_table_next_column(); }
void imgui_table_headers_row(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_table_headers_row(); }
void imgui_text(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_text(text); }
void imgui_text_wrapped(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_text_wrapped(text); }
void imgui_text_disabled(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_text_disabled(text); }
void imgui_text_colored(void* ctx, const char* text, float r, float g, float b, float a) {
    if (imgui_ctx(ctx)) mq_imgui_text_colored(text, r, g, b, a);
}
void imgui_label_text(void* ctx, const char* label, const char* value) {
    if (imgui_ctx(ctx)) mq_imgui_label_text(label, value);
}
void imgui_bullet_text(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_bullet_text(text); }
float imgui_button(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_button(label) : 0.0F;
}
float imgui_small_button(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_small_button(label) : 0.0F;
}
float imgui_invisible_button(void* ctx, const char* id, float w, float h) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_invisible_button(id, w, h) : 0.0F;
}
float imgui_arrow_button(void* ctx, const char* id, float dir) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_arrow_button(id, (int)dir) : 0.0F;
}
float imgui_checkbox(void* ctx, const char* label, float value) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_checkbox(label, value) : value;
}
float imgui_radio_button(void* ctx, const char* label, float active) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_radio_button(label, active != 0.0F) : 0.0F;
}
float imgui_slider_float(void* ctx, const char* label, float value, float min, float max) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_slider_float(label, value, min, max) : value;
}
float imgui_slider_int(void* ctx, const char* label, float value, float min, float max) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_slider_int(label, value, min, max) : value;
}
float imgui_drag_float(void* ctx, const char* label, float value, float speed) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_drag_float(label, value, speed) : value;
}
float imgui_drag_int(void* ctx, const char* label, float value, float speed) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_drag_int(label, value, speed) : value;
}
float imgui_input_float(void* ctx, const char* label, float value) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_input_float(label, value) : value;
}
float imgui_input_int(void* ctx, const char* label, float value) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_input_int(label, value) : value;
}
const char* imgui_input_text(void* ctx, const char* label, const char* text) {
    Engine* e = imgui_ctx(ctx);
    return e ? mq_imgui_input_text(label, text) : (text ? text : "");
}
float imgui_combo(void* ctx, const char* label, float current, const char* items) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_combo(label, current, items) : current;
}
float imgui_list_box(void* ctx, const char* label, float current, const char* items) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_list_box(label, current, items) : current;
}
float imgui_selectable(void* ctx, const char* label, float selected) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_selectable(label, selected != 0.0F) : 0.0F;
}
void imgui_progress_bar(void* ctx, float fraction, float w, float h, const char* overlay) {
    if (imgui_ctx(ctx)) mq_imgui_progress_bar(fraction, w, h, overlay);
}
void imgui_plot_value(void* ctx, const char* id, float value) {
    if (imgui_ctx(ctx)) mq_imgui_plot_value(id, value);
}
float imgui_color_edit(void* ctx, const char* label, float packed) {
    Engine* e = imgui_ctx(ctx); return e ? mq_imgui_color_edit(label, packed) : packed;
}
float imgui_tree_node(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_tree_node(label) : 0.0F;
}
void imgui_tree_pop(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_tree_pop(); }
float imgui_collapsing_header(void* ctx, const char* label) {
    Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_collapsing_header(label) : 0.0F;
}
void imgui_set_next_item_width(void* ctx, float w) { if (imgui_ctx(ctx)) mq_imgui_set_next_item_width(w); }
void imgui_push_item_width(void* ctx, float w) { if (imgui_ctx(ctx)) mq_imgui_push_item_width(w); }
void imgui_pop_item_width(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_pop_item_width(); }
void imgui_push_id(void* ctx, const char* id) { if (imgui_ctx(ctx)) mq_imgui_push_id(id); }
void imgui_pop_id(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_pop_id(); }
void imgui_tooltip(void* ctx, const char* text) { if (imgui_ctx(ctx)) mq_imgui_tooltip(text); }
float imgui_is_item_hovered(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_is_item_hovered() : 0.0F; }
float imgui_is_item_active(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_is_item_active() : 0.0F; }
float imgui_is_window_hovered(void* ctx) { Engine* e = imgui_ctx(ctx); return e ? (float)mq_imgui_is_window_hovered() : 0.0F; }
void imgui_set_item_default(void* ctx) { if (imgui_ctx(ctx)) mq_imgui_set_item_default(); }

float imgui_show_demo(void* ctx) {
    Engine* eng = imgui_ctx(ctx);
    if (!eng) return 0.0F;
    int open = eng->imgui_demo ? 1 : 0;
    mq_imgui_show_demo(&open);
    eng->imgui_demo = open != 0;
    return eng->imgui_demo ? 1.0F : 0.0F;
}

float imgui_want_mouse(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on && eng->imgui_inited) ? (float)mq_imgui_want_mouse() : 0.0F;
}
float imgui_want_keyboard(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on && eng->imgui_inited) ? (float)mq_imgui_want_keyboard() : 0.0F;
}
float imgui_hovered(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->imgui_on && eng->imgui_inited) ? (float)mq_imgui_hovered() : 0.0F;
}
float imgui_frame(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng ? (float)eng->imgui_frames : 0.0F;
}
float imgui_last_cmd_count(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng ? (float)eng->imgui_last_cmds : 0.0F;
}

} // namespace host


/* ---- 控制平面方法（引擎能力，docs/11） ---- */
extern "C" int mq_control_Perf_get(void* u, const mq_json*, char* out, size_t cap) {
    std::snprintf(out, cap, "%s", mq_obs_dump_json(((Engine*)u)->obs));
    return 0;
}
extern "C" int mq_control_Scene_info(void* u, const mq_json*, char* out, size_t cap) {
    auto& d = ((Engine*)u)->scene->desc;
    std::snprintf(out, cap, "{\"title\":\"%s\",\"script\":\"%s\",\"width\":%d,\"height\":%d,\"views\":%d}",
                  d.title.c_str(), d.script.c_str(), d.width, d.height, d.views);
    return 0;
}
extern "C" int mq_control_World_checksum(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap,
                  "{\"trail\":\"%llu\",\"last\":\"%llu\",\"frame\":%llu,"
                  "\"scene\":\"%llu\",\"physics\":\"%llu\",\"particles\":%zu}",
                  (unsigned long long)e->world_checksum(), (unsigned long long)e->last_checksum,
                  (unsigned long long)e->frame,
                  (unsigned long long)(e->scene ? e->scene->checksum(*e) : 0),
                  (unsigned long long)(e->physics ? mq_physics_checksum(e->physics) : 0),
                  e->particles.size());

    return 0;
}

/* Types.describe：反射自省（docs/06/11 A1）——类型/字段/属性导出给 AI/工具。 */
/* ---- base64 编码（控制面是行分隔 JSON → 二进制走 base64 分块；零第三方依赖）---- */
std::string b64_encode(const uint8_t* data, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)data[i] << 16 |
                           (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) |
                           (i + 2 < n ? (uint32_t)data[i + 2] : 0);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += i + 1 < n ? T[(v >> 6) & 63] : '=';
        out += i + 2 < n ? T[v & 63] : '=';
    }
    return out;
}

/* Font.info：所有已栅格化的 (字体, 字号) 图集（inspector 字体面板的数据源）*/
extern "C" int mq_control_Font_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    size_t n = 0;
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "[");
    bool first_font = true;
    for (size_t fi = 0; fi < eng->fonts.size(); ++fi) {
        mq_font* f = eng->fonts[fi].font;
        if (!f) continue;
        /* 字号集合：从图集纹理表里取（= 实际排版用过的字号） */
        bool first = true;
        for (const auto& fa : eng->font_atlases) {
            if (fa.font != f) continue;
            uint32_t aw = 0, ah = 0;
            mq_font_atlas_size(f, fa.size, &aw, &ah);
            const uint32_t glyphs = mq_font_glyph_count(f, fa.size);
            if (first) {
                n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0,
                                           "%s{\"path\":\"%s\",\"primary\":%s,\"atlases\":[",
                                           first_font ? "" : ",", eng->fonts[fi].path.c_str(),
                                           fi == 0 ? "true" : "false");
                first_font = false;
            }
            n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0,
                                       "%s{\"index\":%zu,\"size\":%.1f,\"w\":%u,\"h\":%u,"
                                       "\"glyphs\":%u,\"generation\":%u,\"texture\":%d}",
                                       first ? "" : ",", fi, (double)fa.size, aw, ah, glyphs,
                                       mq_font_atlas_generation(f, fa.size), fa.texture);
            first = false;
        }
        if (!first) n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]}");
    }
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]");
    return 0;
}

/* Font.glyphs：字形列表（码点 + 图集矩形）——面板据此画格线与悬浮信息 */
extern "C" int mq_control_Font_glyphs(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int idx = (int)mq_json_num(mq_json_get(params, "index"), 0);
    const float size = (float)mq_json_num(mq_json_get(params, "size"), 0);
    if (idx < 0 || (size_t)idx >= eng->fonts.size() || !eng->fonts[(size_t)idx].font) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    mq_font* f = eng->fonts[(size_t)idx].font;
    const uint32_t cnt = mq_font_glyph_count(f, size);
    size_t n = 0;
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "{\"ok\":true,\"glyphs\":[");
    for (uint32_t i = 0; i < cnt && n + 128 < cap; ++i) {
        mq_text_glyph g{};
        if (mq_font_glyph_at(f, size, i, &g) != MQ_TEXT_OK) continue;
        n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0,
                                   "%s{\"cp\":%u,\"ax\":%d,\"ay\":%d,\"w\":%d,\"h\":%d}",
                                   i ? "," : "", g.codepoint, g.ax, g.ay, (int)g.w, (int)g.h);
    }
    n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]}");
    return 0;
}

/* Font.atlas：图集像素（PPM P6 → base64 分块；offset/max 控制单次大小）*/
extern "C" int mq_control_Font_atlas(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int idx = (int)mq_json_num(mq_json_get(params, "index"), 0);
    const float size = (float)mq_json_num(mq_json_get(params, "size"), 0);
    const size_t offset = (size_t)mq_json_num(mq_json_get(params, "offset"), 0);
    size_t max_bytes = (size_t)mq_json_num(mq_json_get(params, "max"), 24 * 1024);
    if (max_bytes == 0 || max_bytes > (1u << 20)) max_bytes = 24 * 1024;   /* 允许单次传整图（≤1MB） */
    if (idx < 0 || (size_t)idx >= eng->fonts.size() || !eng->fonts[(size_t)idx].font) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    mq_font* f = eng->fonts[(size_t)idx].font;
    uint32_t aw = 0, ah = 0;
    mq_font_atlas_size(f, size, &aw, &ah);
    std::vector<uint8_t> rgba((size_t)aw * ah * 4);
    if (mq_font_atlas(f, size, rgba.data(), rgba.size(), &aw, &ah) != MQ_TEXT_OK) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    /* PPM P6（RGB；白字 + alpha 直接当灰度）——头长按**实际**计算（曾硬编码 15 → 越界崩溃） */
    char header[48];
    const int hlen = std::snprintf(header, sizeof header, "P6\n%u %u\n255\n", aw, ah);
    const size_t total = (size_t)hlen + (size_t)aw * ah * 3;
    auto byte_at = [&](size_t i) -> uint8_t {
        if (i < (size_t)hlen) return (uint8_t)header[i];
        const size_t p = i - (size_t)hlen;
        const size_t px = p / 3, ch = p % 3;
        const uint8_t a = rgba[px * 4 + 3];
        if (ch == 0) return a;                    /* 白字 + alpha → 灰度 */
        if (ch == 1) return a;
        return a;
    };
    const size_t take = std::min(max_bytes, offset < total ? total - offset : 0);
    std::string raw;
    raw.reserve(take);
    for (size_t i = 0; i < take; ++i) raw += (char)byte_at(offset + i);
    const std::string b64 = b64_encode((const uint8_t*)raw.data(), raw.size());
    std::snprintf(out, cap, "{\"ok\":true,\"w\":%u,\"h\":%u,\"total\":%zu,\"offset\":%zu,"
                            "\"size\":%zu,\"data\":\"%s\"}",
                  aw, ah, total, offset, take, b64.c_str());
    return 0;
}

/* Capture.screen：最近一帧渲染结果（PPM → base64 分块）——实时预览面板 */
extern "C" int mq_control_Capture_screen(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const size_t offset = (size_t)mq_json_num(mq_json_get(params, "offset"), 0);
    size_t max_bytes = (size_t)mq_json_num(mq_json_get(params, "max"), 24 * 1024);
    if (max_bytes == 0 || max_bytes > (1u << 20)) max_bytes = 24 * 1024;
    if (eng->frame_pixels.empty() || eng->frame_w <= 0 || eng->frame_h <= 0) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no frame\"}");
        return 0;
    }
    /* 缩略图：max_dim > 0 时盒式降采样（预览没必要传整帧；960x720 全帧约 2MB → 480x360 约 0.5MB） */
    const int src_w = eng->frame_w, src_h = eng->frame_h;
    const int max_dim = (int)mq_json_num(mq_json_get(params, "max_dim"), 0);
    int w = src_w, h = src_h;
    std::vector<uint8_t> small;
    const uint8_t* src = eng->frame_pixels.data();
    int src_stride = src_w * 4;
    if (max_dim > 0 && (src_w > max_dim || src_h > max_dim)) {
        const int f = (src_w > src_h ? (src_w + max_dim - 1) / max_dim : (src_h + max_dim - 1) / max_dim);
        w = src_w / f > 0 ? src_w / f : 1;
        h = src_h / f > 0 ? src_h / f : 1;
        small.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (int dy = 0; dy < f; ++dy)
                    for (int dx = 0; dx < f; ++dx) {
                        const int sx = x * f + dx, sy = y * f + dy;
                        if (sx >= src_w || sy >= src_h) continue;
                        const uint8_t* p = src + (size_t)sy * src_stride + (size_t)sx * 4;
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    }
                const uint32_t n = (uint32_t)(f * f);
                for (int c = 0; c < 4; ++c) small[((size_t)y * w + x) * 4 + c] = (uint8_t)(acc[c] / n);
            }
        src = small.data();
        src_stride = w * 4;
    }
    char header[48];
    const int hlen = std::snprintf(header, sizeof header, "P6\n%d %d\n255\n", w, h);
    const size_t total = (size_t)hlen + (size_t)w * h * 3;
    auto byte_at = [&](size_t i) -> uint8_t {
        if (i < (size_t)hlen) return (uint8_t)header[i];
        const size_t p = i - (size_t)hlen;
        const size_t px = p / 3, ch = p % 3;
        return src[px * 4 + ch];
    };
    const size_t take = std::min(max_bytes, offset < total ? total - offset : 0);
    std::string raw;
    raw.reserve(take);
    for (size_t i = 0; i < take; ++i) raw += (char)byte_at(offset + i);
    const std::string b64 = b64_encode((const uint8_t*)raw.data(), raw.size());
    std::snprintf(out, cap, "{\"ok\":true,\"w\":%d,\"h\":%d,\"total\":%zu,\"offset\":%zu,"
                            "\"size\":%zu,\"data\":\"%s\"}",
                  w, h, total, offset, take, b64.c_str());
    return 0;
}

/* Scene.emit：宿主/工具 → 脚本事件（与脚本内 node_emit_number 同一路径）。
 * 多语言下按**处理器名**路由：哪个 VM 注册了该处理器就由谁处理（跨语言/宿主→脚本共用一条路）。 */
extern "C" int mq_control_Scene_emit(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const char* event = mq_json_str(mq_json_get(params, "event"), nullptr);
    const float value = (float)mq_json_num(mq_json_get(params, "value"), 0);
    if (id < 0 || !event || !*event) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"bad args\"}"); return 0; }
    const float hits = host::node_emit_number(eng, (float)id, event, value);
    std::snprintf(out, cap, "{\"ok\":true,\"event\":\"%s\",\"id\":%d,\"value\":%.4g,\"handlers\":%.0f}",
                  event, id, (double)value, (double)hits);
    return 0;
}

/* Control.info：端点/权限/计数（inspector 状态栏；plans/17 §3） */
extern "C" int mq_control_Control_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const char* url = eng->ctl_server ? mq_control_server_url(eng->ctl_server) : "";
    (void)std::snprintf(out, cap,
        "{\"endpoint\":\"%s\",\"read_only\":%s,\"clients\":%u,\"rejected\":%u,"
        "\"methods\":%u,\"frame\":%llu}",
        url, eng->inspect_read_only ? "true" : "false",
        mq_control_server_clients(eng->ctl_server), mq_control_server_rejected(eng->ctl_server),
        mq_control_method_count(eng->control), (unsigned long long)eng->frame);
    return 0;                                   /* 约定：内容写入 out，返回 0（见其它 handler） */
}

extern "C" int mq_control_Types_describe(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "[";
    const uint32_t n = mq_reflect_count(e->reflect);
    char item[512];
    for (uint32_t i = 0; i < n; ++i) {
        const mq_type_desc* d = mq_reflect_type_at(e->reflect, i);
        if (!d) continue;
        std::snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"id\":%u,\"fields\":%u,\"role\":%u}",
                      i ? "," : "", d->name, d->type_id, d->field_count, d->role);
        o += item;
    }
    o += "]";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Scene.entities：实体/组件 dump（AI 可读；docs/12）。 */
extern "C" int mq_control_Scene_entities(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "[";
    bool first = true;
    char item[256];
    for (auto& [idx, ent] : e->scene->ents) {
        std::snprintf(item, sizeof item, "%s{\"id\":%d,\"name\":\"%s\",\"parent\":%d,\"x\":%.4f,\"y\":%.4f,\"components\":%zu}",
                      first ? "" : ",", idx, ent.name.c_str(), ent.parent, ent.world_x, ent.world_y, ent.comps.size());
        first = false;
        o += item;
    }
    o += "]";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Scene.get / Scene.set：按实体 + 字段路径读写组件字段（反射驱动；编辑器/AI 的编辑面）。
 * params: { id, path, value? }。path 形式 "Type.field" 或裸 "field"（docs/11 §3/§4）。 */
extern "C" int mq_control_Scene_get(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    /* path == "" → **dump 整个实体**（反射驱动：组件 + 全部标量字段）——inspector 属性面板用 */
    if (path && !*path) {
        Entity* ent = e->scene ? e->scene->at(id) : nullptr;
        if (!ent) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no entity\"}"); return 0; }
        size_t n = 0;
        n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"components\":[",
                                   id, ent->name.c_str());
        bool first_c = true;
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            if (!d) continue;
            n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "%s{\"type\":\"%s\",\"fields\":{",
                                       first_c ? "" : ",", d->name);
            first_c = false;
            bool first_f = true;
            for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                const mq_field_desc* f = &d->fields[fi];
                void* fp = blob.data() + f->offset;
                char val[160] = { 0 };
                switch (f->kind) {
                    case MQ_TK_F32: std::snprintf(val, sizeof val, "%.4g", *(float*)fp); break;
                    case MQ_TK_I32: std::snprintf(val, sizeof val, "%d", *(int32_t*)fp); break;
                    case MQ_TK_U32: std::snprintf(val, sizeof val, "%u", *(uint32_t*)fp); break;
                    case MQ_TK_I64: std::snprintf(val, sizeof val, "%lld", (long long)*(int64_t*)fp); break;
                    case MQ_TK_U64: std::snprintf(val, sizeof val, "%llu", (unsigned long long)*(uint64_t*)fp); break;
                    case MQ_TK_BOOL: std::snprintf(val, sizeof val, "%s", *(bool*)fp ? "true" : "false"); break;
                    case MQ_TK_STRING: std::snprintf(val, sizeof val, "\"%s\"", ((std::string*)fp)->c_str()); break;
                    default: std::snprintf(val, sizeof val, "null"); break;   /* 复合类型：面板暂不展开 */
                }
                n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "%s\"%s\":%s",
                                           first_f ? "" : ",", f->name, val);
                first_f = false;
            }
            n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "}}");
        }
        n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]}");
        return 0;
    }
    const mq_type_desc* tt = nullptr; const mq_field_desc* tf = nullptr; void* base = nullptr;
    if (id < 0 || !path || !resolve_field_path(*e, id, path, &tt, &tf, &base)) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    void* fp = (uint8_t*)base + tf->offset;
    if (tf->kind == MQ_TK_F32)        std::snprintf(out, cap, "{\"ok\":true,\"value\":%.6f}", (double)*(float*)fp);
    else if (tf->kind == MQ_TK_STRING) std::snprintf(out, cap, "{\"ok\":true,\"value\":\"%s\"}", ((std::string*)fp)->c_str());
    else if (tf->kind == MQ_TK_STRUCT) {
        const mq_type_desc* vt = mq_reflect_type_by_id(e->reflect, tf->type_id);
        std::string vals = "{\"ok\":true,\"value\":{";
        if (vt) for (uint32_t i = 0; i < vt->field_count; ++i) {
            const mq_field_desc* vf = &vt->fields[i];
            if (vf->kind != MQ_TK_F32) continue;
            vals += std::string(i ? ",\"" : "\"") + vf->name + "\":" + std::to_string(*(float*)((uint8_t*)fp + vf->offset));
        }
        vals += "}}";
        std::snprintf(out, cap, "%s", vals.c_str());
    } else {
        int64_t iv = 0;
        mq_reflect_get_i64(e->reflect, base, tf, &iv);
        std::snprintf(out, cap, "{\"ok\":true,\"value\":%lld}", (long long)iv);
    }
    return 0;
}

extern "C" int mq_control_Scene_set(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    const mq_json* vj = mq_json_get(params, "value");
    const mq_type_desc* tt = nullptr; const mq_field_desc* tf = nullptr; void* base = nullptr;
    if (id < 0 || !path || !vj || !resolve_field_path(*e, id, path, &tt, &tf, &base)) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"not found\"}");
        return 0;
    }
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true}"); return 0; }
    void* fp = (uint8_t*)base + tf->offset;
    if (tf->kind == MQ_TK_STRING) {
        const char* s = mq_json_str(vj, "");
        *(std::string*)fp = s ? s : "";
    } else {
        mq_reflect_set_f64(e->reflect, base, tf, mq_json_num(vj, 0.0));
    }
    std::snprintf(out, cap, "{\"ok\":true,\"id\":%d,\"path\":\"%s\"}", id, path);
    return 0;
}

/* Scene.spawn / Scene.destroy：实体增删（编辑器/AI 编辑；返回句柄与 undo 信息，docs/11 §4）。 */
extern "C" int mq_control_Scene_spawn(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    const char* name = mq_json_str(mq_json_get(params, "name"), "");
    const double parent = mq_json_num(mq_json_get(params, "parent"), -1);
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true,\"would_spawn\":\"%s\"}", name); return 0; }
    const int id = e->scene->spawn();
    if (id < 0) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"spawn failed\"}"); return 0; }
    if (Entity* ent = e->scene->at(id)) {
        ent->name = name;
        ent->parent = (int)parent;
        if (const char* tn = mq_json_str(mq_json_get(params, "component"), nullptr)) attach_component(*e, id, tn);
    }
    std::snprintf(out, cap, "{\"ok\":true,\"id\":%d,\"undo\":{\"destroy\":%d}}", id, id);
    return 0;
}

extern "C" int mq_control_Scene_destroy(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    Entity* ent = e->scene->at(id);
    if (!ent) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no such entity\"}"); return 0; }
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true,\"name\":\"%s\"}", ent->name.c_str()); return 0; }
    e->scene->remove(id);
    std::snprintf(out, cap, "{\"ok\":true,\"destroyed\":%d,\"entities\":%d}", id, e->scene->count());
    return 0;
}

/* Scene.expand_prefabs：把 PrefabInstance 展开（detached=true）→ 场景自包含（docs/12 §5）。
 * 旧名 Scene.bake 保留为兼容别名（语义完全相同；"bake" 一词不再用于世界展平）。 */
extern "C" int mq_control_Scene_expand_prefabs(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    uint32_t expanded = 0;
    for (auto& [idx, ent] : e->scene->ents) {
        for (auto& [tid, blob] : ent.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, tid);
            if (!d) continue;
            const std::string* src = read_str(d, blob.data(), "source");
            if (!src || src->empty()) continue;
            const mq_field_desc* fd = mq_reflect_field(d, "detached");
            if (!fd) continue;
            if (*(const bool*)(blob.data() + fd->offset)) continue;
            if (!dry) *(bool*)(blob.data() + fd->offset) = true;
            ++expanded;
        }
    }
    std::snprintf(out, cap, "{\"ok\":true,\"expanded\":%u,\"dry_run\":%s}", expanded, dry ? "true" : "false");
    return 0;
}

/* 脚本面（schema/io/storage.yaml 的 api_list，ctx:true → 运行时实现；docs/14）
 * storage.get/set/remove/exists/keys/info —— JS 与 Lua 同一套绑定（DSL 生成，docs/21）。 */
namespace storage {

/* 返回值放进 Engine 的复用缓冲：生成的绑定会**立即**复制（ret_str → snprintf）→ 单线程安全 */
const char* get(void* ctx, const char* key) {
    auto* e = (Engine*)ctx;
    e->script_ret.clear();
    if (!e->storage || !key) return "";
    void* d = nullptr; size_t n = 0;
    if (mq_storage_get(e->storage, key, &d, &n) != MQ_STORAGE_OK) return "";
    e->script_ret.assign((const char*)d, n);
    mq_storage_free(d);
    return e->script_ret.c_str();
}

float set(void* ctx, const char* key, const char* value) {
    auto* e = (Engine*)ctx;
    if (!e->storage || !key || !value) return 0.0F;
    return mq_storage_put(e->storage, key, value, std::strlen(value)) == MQ_STORAGE_OK ? 1.0F : 0.0F;
}

float remove(void* ctx, const char* key) {
    auto* e = (Engine*)ctx;
    if (!e->storage || !key) return 0.0F;
    return mq_storage_remove(e->storage, key) == MQ_STORAGE_OK ? 1.0F : 0.0F;
}

float exists(void* ctx, const char* key) {
    auto* e = (Engine*)ctx;
    return (e->storage && key && mq_storage_exists(e->storage, key)) ? 1.0F : 0.0F;
}

const char* keys(void* ctx) {
    auto* e = (Engine*)ctx;
    e->script_ret.assign(65536, '\0');
    if (!e->storage || mq_storage_keys(e->storage, e->script_ret.data(), e->script_ret.size()) != MQ_STORAGE_OK) {
        e->script_ret = "[]";
        return e->script_ret.c_str();
    }
    e->script_ret.resize(std::strlen(e->script_ret.c_str()));
    return e->script_ret.c_str();
}

const char* info(void* ctx) {
    auto* e = (Engine*)ctx;
    std::string json = "{\"mode\":\"";
    json += e->storage ? mq_storage_mode(e->storage) : "";
    json += "\",\"root\":";
    { std::string q; write_json_string_pub(q, e->storage ? mq_storage_root(e->storage) : ""); json += q; }
    json += ",\"ns\":";
    { std::string q; write_json_string_pub(q, e->storage ? mq_storage_ns(e->storage) : ""); json += q; }
    json += "}";
    e->script_ret = std::move(json);
    return e->script_ret.c_str();
}

} // namespace storage

/* 控制面（docs/11 §3、docs/25）：只读 info/list/get，变更 put/remove（只读会话拒绝）。 */
extern "C" int mq_control_Storage_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->storage) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no storage\"}"); return 0; }
    std::string root;
    write_json_string_pub(root, mq_storage_root(e->storage));
    std::string ns;
    write_json_string_pub(ns, mq_storage_ns(e->storage));
    std::snprintf(out, cap, "{\"ok\":true,\"mode\":\"%s\",\"root\":%s,\"ns\":%s}",
                  mq_storage_mode(e->storage), root.c_str(), ns.c_str());
    return 0;
}

extern "C" int mq_control_Storage_list(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->storage) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no storage\"}"); return 0; }
    std::vector<char> buf(65536, '\0');
    const int rc = mq_storage_keys(e->storage, buf.data(), buf.size());
    if (rc != MQ_STORAGE_OK) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"keys failed (%d)\"}", rc); return 0; }
    std::snprintf(out, cap, "{\"ok\":true,\"keys\":%s}", buf.data());
    return 0;
}

extern "C" int mq_control_Storage_get(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* key = mq_json_str(mq_json_get(params, "key"), nullptr);
    if (!e->storage || !key) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing key\"}"); return 0; }
    void* d = nullptr; size_t n = 0;
    const int rc = mq_storage_get(e->storage, key, &d, &n);
    if (rc != MQ_STORAGE_OK) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"notfound\"}"); return 0; }
    std::string value;
    write_json_string_pub(value, std::string((char*)d, n));
    mq_storage_free(d);
    std::snprintf(out, cap, "{\"ok\":true,\"value\":%s}", value.c_str());
    return 0;
}

extern "C" int mq_control_Storage_put(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* key = mq_json_str(mq_json_get(params, "key"), nullptr);
    const char* value = mq_json_str(mq_json_get(params, "value"), nullptr);
    if (!e->storage || !key || !value) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing key/value\"}"); return 0; }
    const int rc = mq_storage_put(e->storage, key, value, std::strlen(value));
    std::snprintf(out, cap, "{\"ok\":%s,\"error\":\"%s\"}", rc == MQ_STORAGE_OK ? "true" : "false",
                  rc == MQ_STORAGE_OK ? "" : "put failed");
    return 0;
}

extern "C" int mq_control_Storage_remove(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* key = mq_json_str(mq_json_get(params, "key"), nullptr);
    if (!e->storage || !key) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing key\"}"); return 0; }
    const int rc = mq_storage_remove(e->storage, key);
    std::snprintf(out, cap, "{\"ok\":%s,\"error\":\"%s\"}", rc == MQ_STORAGE_OK ? "true" : "false",
                  rc == MQ_STORAGE_OK ? "" : (rc == MQ_STORAGE_ERR_NOTFOUND ? "notfound" : "remove failed"));
    return 0;
}

/* Scene.bake：Scene.expand_prefabs 的兼容别名（已弃用；docs/12 §5 改名） */
extern "C" int mq_control_Scene_bake(void* u, const mq_json* params, char* out, size_t cap) {
    return mq_control_Scene_expand_prefabs(u, params, out, cap);
}

/* Scene.save：把当前世界写回 .scn（编辑器落盘；docs/11 §4）。
 * 组件字段由反射序列化（与 load_json 往返一致）→ 落盘文件可直接再次加载。 */
extern "C" int mq_control_Scene_save(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!path) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path\"}"); return 0; }

    std::string json = "{\n  \"title\": ";
    write_json_string_pub(json, e->scene->desc.title);
    json += ",\n  \"script\": ";
    write_json_string_pub(json, e->scene->desc.script);
    json += ",\n  \"width\": " + std::to_string(e->scene->desc.width);
    json += ",\n  \"height\": " + std::to_string(e->scene->desc.height);
    json += ",\n  \"seed\": " + std::to_string(e->scene->desc.seed);
    json += ",\n  \"gravity_y\": " + std::to_string(e->scene->desc.gravity_y);
    json += ",\n  \"restitution\": " + std::to_string(e->scene->desc.restitution);
    json += ",\n  \"entities\": [";

    std::vector<int> ids;
    for (auto& [idx, ent] : e->scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());
    std::function<void(int, int)> emit = [&](int id, int indent) {
        Entity* ent = e->scene->at(id);
        if (!ent) return;
        const std::string pad((size_t)indent * 2, ' ');
        json += "\n" + pad + "{ \"name\": ";
        write_json_string_pub(json, ent->name);
        json += ", \"components\": [";
        std::vector<uint32_t> tids;
        for (auto& [t, b] : ent->comps) tids.push_back(t);
        std::sort(tids.begin(), tids.end());
        std::vector<char> buf(16384);
        bool f2 = true;
        for (uint32_t t : tids) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, t);
            if (!d) continue;
            size_t need = mq_persist_write_json(e->reflect, d, ent->comps[t].data(), buf.data(), buf.size());
            if (need + 1 > buf.size()) { buf.resize(need + 1); need = mq_persist_write_json(e->reflect, d, ent->comps[t].data(), buf.data(), buf.size()); }
            if (need == 0 || need + 1 > buf.size()) continue;
            if (!f2) json += ", ";
            f2 = false;
            json += "{ \"type\": ";
            write_json_string_pub(json, d->name);
            json += ", ";
            json += std::string(buf.data() + 1, need - 2);          // 去掉外层 { }
            json += " }";
        }
        json += "]";
        std::vector<int> kids;
        for (auto& [k, c] : e->scene->ents) if (c.parent == id) kids.push_back(k);
        std::sort(kids.begin(), kids.end());
        if (!kids.empty()) {
            json += ", \"children\": [";
            for (size_t i = 0; i < kids.size(); ++i) { if (i) json += ","; emit(kids[i], indent + 1); }
            json += "\n" + pad + "]";
        }
        json += " }";
    };
    bool first = true;
    for (int id : ids) {
        Entity* ent = e->scene->at(id);
        if (!ent || (ent->parent >= 0 && e->scene->at(ent->parent))) continue;   // 仅从根开始
        if (!first) json += ",";
        first = false;
        emit(id, 2);
    }
    json += "\n  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"cannot write\"}"); return 0; }
    f << json;
    std::snprintf(out, cap, "{\"ok\":true,\"path\":\"%s\",\"entities\":%d,\"bytes\":%zu}",
                  path, e->scene->count(), json.size());
    return 0;
}

/* World.snapshot：抓当前世界快照（返回字节数与校验和；AI undo/回滚点）。 */
extern "C" int mq_control_World_snapshot(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->snap_manual) e->snap_manual = mq_snapshot_create();     // 独立于周期池（不会被 60 帧环覆盖）
    if (!e->world_snapshot(e->snap_manual)) { std::snprintf(out, cap, "{\"ok\":false}"); return 0; }
    size_t n = 0; mq_snapshot_bytes(e->snap_manual, &n);
    std::snprintf(out, cap, "{\"ok\":true,\"bytes\":%zu,\"checksum\":\"%llu\",\"frame\":%llu}",
                  n, (unsigned long long)mq_snapshot_checksum(e->snap_manual),
                  (unsigned long long)e->frame);
    return 0;
}

/* World.restore：回到最近快照（A5 快迭代/undo，docs/10 §4）。 */
extern "C" int mq_control_World_restore(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    mq_snapshot* src = e->snap_manual ? e->snap_manual : e->snap_last;
    if (!src) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no snapshot\"}"); return 0; }
    e->world_restore(src);
    std::snprintf(out, cap, "{\"ok\":true,\"entities\":%d}", e->scene->count());
    return 0;
}

extern "C" int mq_control_World_trail(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "{\"trail\":\"" + std::to_string(e->world_trail()) + "\",\"frames\":[";
    char item[64];
    for (size_t i = 0; i < e->trail.size(); ++i) {
        std::snprintf(item, sizeof item, "%s[%llu,\"%llu\"]", i ? "," : "",
                      (unsigned long long)e->trail[i].first, (unsigned long long)e->trail[i].second);
        o += item;
    }
    o += "]}";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Input.inject：AI/CI 注入输入（docs/11/19 §9）——与设备事件同一事件流。 */
extern "C" int mq_control_Input_inject(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    mq_input_event ev{};
    ev.type = (uint32_t)mq_json_num(mq_json_get(params, "type"), (double)MQ_INPUT_POINTER_DOWN);
    ev.pointer_id = (uint32_t)mq_json_num(mq_json_get(params, "pointer_id"), 0);
    ev.pointer_type = (uint32_t)mq_json_num(mq_json_get(params, "pointer_type"), 0);
    ev.button = (uint32_t)mq_json_num(mq_json_get(params, "button"), 0);
    ev.key = (uint32_t)mq_json_num(mq_json_get(params, "key"), 0);
    ev.x = (float)mq_json_num(mq_json_get(params, "x"), 0);
    ev.y = (float)mq_json_num(mq_json_get(params, "y"), 0);
    mq_input_inject(e->input, &ev);
    std::snprintf(out, cap, "{\"ok\":true,\"injected\":\"%s\"}", mq_json_str(mq_json_get(params, "type"), "?"));
    return 0;
}


/* ---- 输入模拟器控制面（docs/19）：场景加载 / 状态 / 事件列表 / 停止 ----
 * 场景来源：path（VFS 路径）或 json（内联字符串）；载入后可 reset 重放、loop 循环。 */
extern "C" int mq_control_Input_sim_load(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string json;
    std::string src = "inline";
    if (const char* p = mq_json_str(mq_json_get(params, "path"), nullptr); p && *p) {
        if (!read_sim_scenario(e->vfs, p, json)) {
            std::snprintf(out, cap, "{\"ok\":false,\"error\":\"scenario not found: %s\"}", p);
            return 0;
        }
        src = p;
    } else if (const char* j = mq_json_str(mq_json_get(params, "json"), nullptr); j && *j) {
        json = j;
    } else {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"need path or json\"}");
        return 0;
    }
    if (!e->sim) e->sim = mq_sim_create();
    char err[256] = { 0 };
    const int rc = mq_sim_load_json(e->sim, json.c_str(), json.size(), err, sizeof err);
    if (rc != 0) {
        std::snprintf(out, cap, "{\"ok\":false,\"rc\":%d,\"error\":\"%s\"}", rc, err);
        return 0;
    }
    if (mq_json_num(mq_json_get(params, "loop"), 0.0) != 0.0) mq_sim_set_loop(e->sim, 1);
    if (mq_json_num(mq_json_get(params, "reset"), 1.0) != 0.0) mq_sim_reset(e->sim);
    e->sim_active = true;
    e->sim_path = src;
    std::snprintf(out, cap,
                  "{\"ok\":true,\"source\":\"%s\",\"events\":%u,\"last_frame\":%llu,\"loop\":%s}",
                  src.c_str(), mq_sim_count(e->sim), (unsigned long long)mq_sim_last_frame(e->sim),
                  mq_json_num(mq_json_get(params, "loop"), 0.0) != 0.0 ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Input_sim_status(void* u, const mq_json* params, char* out, size_t cap) {
    (void)params;
    auto* e = (Engine*)u;
    if (!e->sim || !mq_sim_loaded(e->sim)) {
        std::snprintf(out, cap, "{\"active\":false,\"loaded\":false}");
        return 0;
    }
    std::snprintf(out, cap,
                  "{\"active\":%s,\"loaded\":true,\"source\":\"%s\",\"cursor\":%u,\"total\":%u,"
                  "\"finished\":%s,\"last_frame\":%llu,\"frame\":%llu}",
                  e->sim_active ? "true" : "false", e->sim_path.c_str(), mq_sim_cursor(e->sim),
                  mq_sim_count(e->sim), mq_sim_finished(e->sim) ? "true" : "false",
                  (unsigned long long)mq_sim_last_frame(e->sim), (unsigned long long)e->frame);
    return 0;
}

extern "C" int mq_control_Input_sim_events(void* u, const mq_json* params, char* out, size_t cap) {
    (void)params;
    auto* e = (Engine*)u;
    if (!e->sim || !mq_sim_loaded(e->sim)) {
        std::snprintf(out, cap, "{\"events\":[]}");
        return 0;
    }
    std::string s = "{\"events\":[";
    const uint32_t n = mq_sim_count(e->sim);
    for (uint32_t i = 0; i < n; ++i) {
        mq_sim_event ev{};
        if (mq_sim_event_at(e->sim, i, &ev) != 0) continue;
        if (i) s += ",";
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "{\"frame\":%llu,\"kind\":%u,\"space\":%u,\"x\":%.1f,\"y\":%.1f,"
                      "\"dx\":%.1f,\"dy\":%.1f,\"button\":%u,\"key\":%u,\"text\":\"%s\"}",
                      (unsigned long long)ev.frame, ev.kind, ev.space, ev.x, ev.y, ev.dx, ev.dy,
                      ev.button, ev.key, ev.text ? ev.text : "");
        s += buf;
    }
    s += "]}";
    std::snprintf(out, cap, "%s", s.c_str());
    return 0;
}

extern "C" int mq_control_Input_sim_stop(void* u, const mq_json* params, char* out, size_t cap) {
    (void)params;
    auto* e = (Engine*)u;
    const bool was = e->sim_active;
    e->sim_active = false;
    std::snprintf(out, cap, "{\"ok\":true,\"was_active\":%s}", was ? "true" : "false");
    return 0;
}

/* Control.mcp：MCP 语义（initialize/tools/list/tools/call）经控制平面进出。
 * 请求体放 params.request（字符串）；也可直接传 params.method/params.name。 */
/* Reload.script / Reload.scene：手动触发帧边界重载；dry_run=1 只报告不执行（docs/11 §4）。 */
extern "C" int mq_control_Reload_script(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    if (dry) {
        void* d = nullptr; size_t n = 0;
        uint64_t h = 0;
        if (!e->script_path.empty() && mq_vfs_read(e->vfs, ("/" + e->script_path).c_str(), &d, &n) == MQ_VFS_OK) {
            h = mq_vfs_hash(d, n); mq_vfs_free(d);
        }
        std::snprintf(out, cap, "{\"would_reload\":%s,\"script\":\"%s\"}",
                      h != e->script_hash ? "true" : "false", e->script_path.c_str());
        return 0;
    }
    std::string err;
    const bool ok = e->reload_script(&err);
    std::snprintf(out, cap, "{\"reloaded\":%s,\"reloads\":%u,\"error\":\"%s\"}",
                  ok ? "true" : "false", e->reloads, err.c_str());
    return 0;
}
extern "C" int mq_control_Reload_scene(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    if (dry) { std::snprintf(out, cap, "{\"would_reload\":true,\"scene\":\"%s\"}", e->scn_vfs_path.c_str()); return 0; }
    const bool ok = e->reload_scene();
    std::snprintf(out, cap, "{\"reloaded\":%s,\"reloads\":%u,\"entities\":%d}",
                  ok ? "true" : "false", e->reloads, e->scene ? e->scene->count() : 0);
    return 0;
}
extern "C" int mq_control_Reload_files(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "{\"watching\":" + std::to_string(e->watcher ? mq_watcher_count(e->watcher) : 0) + ",\"files\":[";
    if (e->watcher)
        for (uint32_t i = 0; i < mq_watcher_count(e->watcher); ++i) {
            const char* p = mq_watcher_path_at(e->watcher, i);
            o += std::string(i ? ",\"" : "\"") + (p ? p : "") + "\"";
        }
    o += "],\"reloads\":" + std::to_string(e->reloads) + "}";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Control.mcp：MCP 语义（initialize/tools/list/tools/call）。
 * 两种入参形态：
 *   ① params.request = "<完整 MCP 请求 JSON 文本>"（原样走 MCP 包装）
 *   ② params.method  = "tools/call" + params.name/arguments（直接转发到同名方法，
 *      避免手工拼 JSON —— 结构化入参优先，docs/11 §4）。 */
extern "C" int mq_control_Control_mcp(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (const char* raw = mq_json_str(mq_json_get(params, "request"), nullptr)) {
        std::snprintf(out, cap, "%s", mq_control_dispatch_mcp(e->control, raw));
        return 0;
    }
    const char* method = mq_json_str(mq_json_get(params, "method"), nullptr);
    if (!method) { std::snprintf(out, cap, "{\"error\":\"missing request/method\"}"); return 0; }
    if (std::strcmp(method, "tools/call") == 0) {
        const char* name = mq_json_str(mq_json_get(params, "name"), nullptr);
        const mq_json* args = mq_json_get(params, "arguments");
        if (!name) {
            std::snprintf(out, cap, "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"missing tool name\"}]}");
            return 0;
        }
        /* 直接调用已注册的 JSON-RPC 方法（与 MCP tools/call 相同的语义） */
        std::string inner = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + std::string(name) + "\"";
        if (args) {
            std::string argtxt = "{";
            struct Ctx { std::string* out; bool first; };
            Ctx cx{ &argtxt, true };
            mq_json_object_each(args, [](void* ud, const char* k, const char* v) {
                auto* c = (Ctx*)ud;
                if (!c->first) *c->out += ",";
                c->first = false;
                const bool numeric = v && *v && ((v[0] >= '0' && v[0] <= '9') ||
                                                 ((v[0] == '-' || v[0] == '+') && v[1] >= '0' && v[1] <= '9'));
                *c->out += std::string("\"") + k + "\":" +
                           (numeric ? std::string(v) : ("\"" + std::string(v ? v : "") + "\""));
                return 0;
            }, &cx);
            argtxt += "}";
            inner += ",\"params\":" + argtxt;
        }
        inner += "}";
        const char* resp = mq_control_dispatch(e->control, inner.c_str());
        std::string text = resp ? resp : "null";
        std::string escaped;
        for (char c : text) { if (c == '"' || c == '\\') escaped += '\\'; escaped += c; }
        std::snprintf(out, cap, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", escaped.c_str());
        return 0;
    }
    /* initialize / tools/list / 其它：经 MCP 包装 */
    std::string req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + std::string(method) + "\"";
    if (const mq_json* p = mq_json_get(params, "params")) {
        if (const char* nm = mq_json_str(mq_json_get(p, "name"), nullptr)) {
            req += std::string(",\"params\":{\"name\":\"") + nm + "\"}";
        }
    }
    req += "}";
    /* MCP 包装返回 {"jsonrpc":..,"result":X}；此处抽出 X（外层由控制平面再包一次） */
    const char* resp = mq_control_dispatch_mcp(e->control, req.c_str());
    const char* res = resp ? std::strstr(resp, "\"result\":") : nullptr;
    std::snprintf(out, cap, "%s", res ? res + 9 : (resp ? resp : "{}"));
    if (res) {   // 去掉末尾多余的 '}'（外层控制平面会补）
        size_t n = std::strlen(out);
        if (n > 0 && out[n - 1] == '}') out[n - 1] = '\0';
    }
    return 0;
}

/* Replay.checkpoint：列出检查点（帧号/校验和）——分歧定位的锚点。 */
extern "C" int mq_control_Replay_checkpoints(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "{\"interval\":" + std::to_string(e->cp_interval) + ",\"active\":";
    o += e->cp_active ? "true" : "false";
    o += ",\"checkpoints\":[";
    bool first = true;
    for (auto* s : e->checkpoints) {
        if (!s || mq_snapshot_frame(s) == 0) continue;
        if (!first) o += ",";
        first = false;
        o += "{\"frame\":" + std::to_string(mq_snapshot_frame(s)) +
             ",\"checksum\":\"" + std::to_string(mq_snapshot_checksum(s)) + "\"}";
    }
    o += "]}";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Replay.seek：跳到指定帧号的检查点（回滚 + 重建世界）。 */
extern "C" int mq_control_Replay_seek(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const uint64_t want = (uint64_t)mq_json_num(mq_json_get(params, "frame"), 0);
    mq_snapshot* best = nullptr;
    for (auto* s : e->checkpoints) {
        if (!s) continue;
        const uint64_t f = mq_snapshot_frame(s);
        if (f && f <= want && (!best || f > mq_snapshot_frame(best))) best = s;
    }
    if (!best) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no checkpoint at or before frame %llu\",\"interval\":%u}",
                      (unsigned long long)want, e->cp_interval);
        return 0;
    }
    e->world_restore(best);
    std::snprintf(out, cap, "{\"ok\":true,\"frame\":%llu,\"checksum\":\"%llu\"}",
                  (unsigned long long)mq_snapshot_frame(best), (unsigned long long)e->world_checksum());
    return 0;
}

/* Runtime.frame：当前模拟帧号与状态摘要（脚本/CI 读模拟时间；回滚后自动一致）。 */
extern "C" int mq_control_Director_pause(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    e->director_paused = true;
    std::snprintf(out, cap, "{\"ok\":true,\"paused\":true,\"frame\":%llu}", (unsigned long long)e->frame);
    return 0;
}

extern "C" int mq_control_Director_resume(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    e->director_paused = false;
    std::snprintf(out, cap, "{\"ok\":true,\"paused\":false,\"frame\":%llu}", (unsigned long long)e->frame);
    return 0;
}

extern "C" int mq_control_Director_status(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap, "{\"paused\":%s,\"frame\":%llu,\"script_frames\":%llu}",
                  e->director_paused ? "true" : "false",
                  (unsigned long long)e->frame, (unsigned long long)e->script_frames);
    return 0;
}

extern "C" int mq_control_Runtime_frame(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap, "{\"frame\":%llu,\"entities\":%d,\"particles\":%zu,\"quit\":%s}",
                  (unsigned long long)e->frame, e->scene ? e->scene->count() : 0,
                  e->particles.size(), e->quit ? "true" : "false");
    return 0;
}

/* Runtime.step：确定性推进 n 帧（AI/CI 逐帧驱动，docs/11 §5）。 */
extern "C" int mq_control_Runtime_step(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const double n = mq_json_num(mq_json_get(params, "frames"), 1);
    const uint64_t steps = n > 0 ? (uint64_t)n : 1;
    e->run_frames(steps);
    std::snprintf(out, cap, "{\"frame\":%llu,\"checksum\":\"%llu\",\"entities\":%d,\"quit\":%s}",
                  (unsigned long long)e->frame, (unsigned long long)e->last_checksum,
                  e->scene ? e->scene->count() : 0, e->quit ? "true" : "false");
    return 0;
}

/* Replay.log：导出输入日志（录制/回放是数据，docs/10 §3）。 */
extern "C" int mq_control_Replay_log(void* u, const mq_json*, char* out, size_t cap) {
    const char* json = mq_input_log_json(((Engine*)u)->input);
    std::snprintf(out, cap, "%s", json ? json : "{}");
    return 0;
}

/* 场景来源 → VFS 根挂载（docs/04 §4/§5）：
 *   · 本地路径 → 挂 file://<场景目录> 到 "/"
 *   · http(s)://…  → 挂 URL 目录（基址 = 场景 URL 的父目录）到 "/"（远程游戏即资源）
 * 输出场景在 VFS 里的路径（"/<文件名>"）与来源目录（本地路径 / URL 基址）。
 * 返回 false：路径非法或挂载失败（未链接 libcurl 时 URL 挂载返回 UNSUPPORTED）。 */
bool mount_scene_source(mq_vfs* vfs, const std::string& scn_path, std::string& vfs_path, std::string& dir_out) {
    if (!vfs || scn_path.empty()) return false;
    const bool remote = scn_path.rfind("http://", 0) == 0 || scn_path.rfind("https://", 0) == 0;
    std::string dir, file;
    if (remote) {
        const size_t slash = scn_path.find_last_of('/');
        if (slash == std::string::npos || slash + 1 >= scn_path.size()) return false;
        dir = scn_path.substr(0, slash + 1);                  // 含结尾 '/' —— URL 基址
        file = scn_path.substr(slash + 1);
    } else {
        const std::filesystem::path scnp(scn_path);
        dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();
        file = scnp.filename().string();
    }
    const std::string source = remote ? dir : ("file://" + dir);
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

    const char* plugin = renderer_override ? renderer_override : std::getenv("MAQUE_RENDERER");
#ifdef MQ_RENDERER_DEFAULT
    if (!plugin) plugin = MQ_RENDERER_DEFAULT;
#endif
    /* 平台默认插件文件名：Windows 是 maque_gfx.dll，类 UNIX 是 libmaque_gfx.so。
     * （写死 .so 会让 Windows 上永远「cannot load renderer plugin」。） */
#if defined(_WIN32)
    if (!plugin) plugin = "maque_gfx.dll";
#else
    if (!plugin) plugin = "libmaque_gfx.so";
#endif
    host = mq_host_create();
    mq_loaded_module* rmod = nullptr;
    if (mq_host_load(host, plugin, &rmod) != 0 ||
        !(rapi = (const mq_renderer_v1*)mq_host_query(host, MQ_RENDERER_API_NAME, 1))) {
        std::print("runtime: cannot load renderer plugin '{}'\n", plugin);
        return false;
    }
    const char* backend = nullptr;
    rself = rapi->create(&backend);
    if (!rself) { std::print("runtime: renderer create failed\n"); return false; }
    std::print("runtime: renderer plugin '{}' backend='{}'\n", plugin, backend ? backend : "?");

    obs = mq_obs_create();                       // 场景装配期即可能产生事件（脚本错误等）
    if (!load_scene_into(scn_path)) return false;
    /* 窗口标题：MAQUE_TITLE 覆盖场景标题（默认用场景 title） */
    std::string win_title = scene->desc.title;
    if (const char* ts = std::getenv("MAQUE_TITLE"); ts && *ts) win_title = ts;
    platform = mq_platform_create(win_title.c_str(), win_w, win_h);
    window = mq_platform_has_window(platform) != 0;
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
    namespace fs = std::filesystem;
    /* 场景来源挂载（本地目录 / http(s) URL 目录）——后挂载者优先 → 新场景来源生效 */
    std::string scn_vfs;
    if (!mount_scene_source(vfs, scn_path, scn_vfs, scn_dir)) return false;
    apply_env_mounts(vfs);        // MAQUE_MOUNT 覆盖目录保持最高优先级（幂等，不重复追加）

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, scn_vfs.c_str(), &d, &n) == MQ_VFS_OK) {
          text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) return false;

    /* ---- 世界重置（切场景：旧世界全部释放；脚本状态不跨场景保留，等价  onDestroy）---- */
    if (bus) { mq_script_bus_destroy(bus); bus = nullptr; vm = nullptr; }   // 总线拥有 VM
    spine_release();                     // Spine 实例随世界一起释放（派生数据，重建即可）
    agent_paths.clear();
    node_events.clear();
    physics_role_bodies.clear();
    particles.clear();
    director_paused = false;
    scene_store = Scene();
    scene = &scene_store;
    if (!scene->deserialize(*this, text)) return false;
    scene->instantiate(*this);
    scene->flatten_world(*this);

    W = scene->desc.width > 0 ? scene->desc.width : 320;
    H = scene->desc.height > 0 ? scene->desc.height : 240;
    nviews = scene->desc.views > 0 ? scene->desc.views : 1;
    /* 启动期覆盖（宿主/CI 用；docs/05、docs/22）：
     *   MAQUE_WINDOW=WxH  窗口与渲染目标尺寸（世界坐标不变，仅改可视区）
     *   MAQUE_VIEWS=n     视图数（1 = 单视图；>1 = 并排多视图/立体） */
    /* **默认窗口 800x650**（用户要求）：世界/渲染目标仍是场景尺寸（W×H），
     * 窗口内等比缩放 + 居中留边显示 —— 场景比窗口大就缩小，小就放大，都不裁剪。
     * 之前的默认是"窗口 = 场景尺寸"，大场景（如 spine 的 all.scn 1600x1300）
     * 在 1080p 屏上会超出显示器，必须改成固定小窗口。
     *
     * MAQUE_WINDOW=WxH 覆盖；`0x0` 表示**不建窗口**（CI/无显示环境用），
     * 所以解析成功时原样接受（含 0），不要被"必须 >0"的校验吃掉语义。 */
    win_w = 800; win_h = 650;
    if (const char* ws = std::getenv("MAQUE_WINDOW")) {
        int ow = 0, oh = 0;
        if (std::sscanf(ws, "%dx%d", &ow, &oh) == 2) {
            win_w = ow; win_h = oh;                 // 只改窗口；世界/目标不变 → 窗口内等比缩放显示
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
    if (const char* vs = std::getenv("MAQUE_VIEWS")) {
        const int v = std::atoi(vs);
        if (v >= 1 && v <= 4) nviews = v;
    }
    /* 渲染分辨率倍率：0 → 自动（窗口/设计，上限 4）；1 → 设计分辨率；N → 固定 N 倍 */
    float rs = 0.0F;
    if (const char* e = std::getenv("MAQUE_RENDER_SCALE")) rs = (float)std::atof(e);
    if (rs <= 0.0F && scene && canvas_entity >= 0) {                 /* 画布组件的 render_scale */
        if (Entity* ce = scene->at(canvas_entity)) {
            for (auto& [tid, blob] : ce->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
                read_f32(d, blob.data(), "render_scale", &rs);
                break;
            }
        }
    }
    /* 渲染倍率（超采样）：
     *   · 场景 `render_scale` 显式给了就用它；
     *   · 否则**跟随真实窗口像素**（窗口/场景比）—— 窗口比设计分辨率大时字才清晰；
     *   · **没有窗口时保持 1.0**（= 设计分辨率）。判据必须是"真的有窗口"，
     *     不能只看 `win_w > 0`：`win_w` 现在是**默认 800x650**（不再是场景尺寸），
     *     离屏/headless 下若仍按它算，渲染目标会被放大成 800x… → 截图尺寸、
     *     文本折行行数全变（实测 shot_e2e / text_e2e 因此失败）。
     *   · 嵌入宿主（`mq_host_create`）**根本没有 platform** → 必须先判空，
     *     否则 `mq_platform_has_window(nullptr)` 直接崩（实测 host_tick/host_multi_instance 0xC0000409）。 */
    /* 自动超采样：只在"**确实会有窗口**且不是一次性任务"时跟随窗口像素。
     * 判据不能用 `platform`（此刻还没创建）也不能只看 `win_w`（默认已是 800x650），
     * 所以看**意图**：headless / 截图 / 不要呈现 → 按设计分辨率 1:1；
     * 否则（真窗口）→ 渲染目标 = 窗口像素，呈现时 1:1 不缩放 → 清晰。 */
    const char* present_env = std::getenv("MAQUE_PRESENT");
    const bool window_intent = !std::getenv("MAQUE_HEADLESS") && !std::getenv("MAQUE_SHOT") &&
                               !(present_env && (std::strcmp(present_env, "off") == 0 || std::strcmp(present_env, "0") == 0));
    const bool has_real_window = window_intent && win_w > 0;
    if (rs <= 0.0F && has_real_window && win_w > 0) rs = (float)win_w / (float)(W > 0 ? W : 1);
    if (rs <= 0.0F) rs = 1.0F;
    /* 渲染倍率允许 **< 1**（下限 0.25）：窗口比场景小的时候（如默认窗口 800x650 看
     * 1600x1300 的 all.scn），按 窗口/场景 倍率渲染 → 渲染目标正好等于窗口像素，
     * 呈现时 **1:1 不缩放** → 清晰。
     * 之前下限是 1.0：目标恒为设计分辨率，再被缩小一半显示 → 整帧糊（用户实测反馈）。
     * ⚠️ 无窗口/headless 时 rs 恒为 1.0（上面已处理），截图尺寸与文本折行不受影响。 */
    ss = rs > 4.0F ? 4.0F : (rs < 0.25F ? 0.25F : rs);
    const int SW = (int)(W * ss + 0.5F), SH = (int)(H * ss + 0.5F);
    TW = SW * nviews;
    views.assign((size_t)nviews, mq_renderer_view{});
    for (int i = 0; i < nviews; ++i) {
        mq_renderer_view& v = views[(size_t)i];
        v = mq_renderer_view{};
        v.vx = (float)(i * SW); v.vy = 0;
        v.vw = (float)SW; v.vh = (float)SH;
        /* pan 必须留在 0，**不要**再放 i*SW。
         * shader 里 `pan` 与 `view` 是两个独立的像素平移项：
         *     pc = … + vec2f(u.size.x*0.5, u.size.y*0.5) + vec2f(u.pan, u.eye) + u.view
         * 而 `view` 已经是该视图在渲染目标里的像素原点（v.vx）。
         * 两个都设成 i*SW 会把偏移**叠加两次**：
         *   · view 0：vx=0 → 看不出问题（但内容仍被视图边界裁掉，表现为"只画一半"）
         *   · view 1：偏移 320+320=640 → 整幅被推到画布右侧之外 → **完全不绘制**
         * `pan` 的用途是"整体平移画面"（如跟随相机），不是视图定位；视图定位只由 vx/vy 负责。 */
        v.pan = 0.0F;
        v.eye = (i - (nviews - 1) * 0.5F) * scene->desc.eye_sep;
        v.cx = W * 0.5F; v.cy = H * 0.5F;
        v.zoom = ss;                          /* 世界 → 目标：按倍率放大（设计分辨率 × ss） */
    }
    for (auto& [idx, e] : scene->ents) {                   // 场景声明的 Camera 覆盖视图相机
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_CAMERA)) continue;
            bool active = true;
            if (const mq_field_desc* fa = mq_reflect_field(d, "active")) active = *(const bool*)(blob.data() + fa->offset);
            if (!active) continue;
            float cx = W * 0.5F, cy = H * 0.5F, zoom = 1.0F, rot = 0.0F;
            read_f32(d, blob.data(), "x", &cx);
            read_f32(d, blob.data(), "y", &cy);
            read_f32(d, blob.data(), "zoom", &zoom);
            read_f32(d, blob.data(), "rotation", &rot);
            for (auto& v : views) { v.cx = cx; v.cy = cy; v.zoom = (zoom > 0 ? zoom : 1.0F) * ss; v.rot = rot; }
        }
    }
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

    script_path = scene->desc.script;
    /* 多脚本（多语言）：scripts 非空则按**声明顺序**逐个装载（顺序 = 事件投递顺序） */
    std::vector<std::string> load_list;
    if (!scene->desc.scripts.empty()) load_list = scene->desc.scripts;
    else if (!script_path.empty()) load_list.push_back(script_path);
    script_paths = load_list;
    scn_vfs_path = scn_vfs;
    scn_full_path = scn_path;
    /* scn_dir = 来源目录（本地路径或 URL 基址），由 mount_scene_source 写入 */
    /* ---- 脚本 VM：按脚本语言选后端（docs/14「统一后端 ABI」：js / lua）----
     * 语言由文件扩展名决定（mq_script_lang_for_path），未编入的后端会明确报错而不是静默降级。 */
    if (!bus) bus = mq_script_bus_create();
    for (const std::string& path : load_list) {
        const char* want = mq_script_lang_for_path(path.c_str());
        mq_script_vm* one = mq_script_vm_create_lang(want);
        if (!one) {
            std::print(stderr, "runtime: script backend '{}' unavailable for '{}' (backends: {})\n",
                       want, path, [] { char b[64] = { 0 }; mq_script_backends(b, sizeof b); return std::string(b); }());
            return false;
        }
        std::print("runtime: script backend '{}' for '{}'\n", mq_script_lang(one), path);
        mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
        mq_bind_generated(one, this);
        std::string src;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, ("/" + path).c_str(), &d, &n) == MQ_VFS_OK) { src.assign((char*)d, n); mq_vfs_free(d); } }
        if (src.empty()) { std::print("runtime: cannot read script '{}'\n", path); return false; }
        if (load_list.size() == 1) script_hash = mq_vfs_hash(src.data(), src.size());
        script_hashes.push_back(mq_vfs_hash(src.data(), src.size()));
        if (mq_script_eval(one, src.c_str(), path.c_str()) != 0) { std::print("runtime: script error\n"); return false; }
        mq_script_bus_add(bus, one, path.c_str());
    }
    vm = mq_script_bus_vm_at(bus, 0);          /* 便捷指针：只读/自省用途 */
    if (load_list.size() > 1)
        std::print("runtime: multi-script bus with {} VM(s) (dispatch order = declaration order)\n",
                   mq_script_bus_count(bus));
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
            std::print("runtime: hot reload watching {} file(s)\n", mq_watcher_count(watcher));
        }
    }
    preload_scene_assets();
    spine_setup();                              // Spine：骨架/图集/页纹理装配（装配期做 IO，不在模拟帧内）
    text_setup();                               // 字体（MAQUE_FONT → 系统字体 → 内置 5x7）
    sim_setup();                                // 输入模拟器（MAQUE_SIM / MAQUE_SIM_CLICK）
    canvas_setup();                             // 画布（分辨率策略，docs/17）
    storage_setup();                            // 键值存储（docs/25：默认 <场景目录>/.maque）
    if (const char* sa = std::getenv("MAQUE_SAFE_AREA")) {   // "l,t,r,b"（设备像素；测试/刘海屏）
        float l = 0, t = 0, r = 0, b = 0;
        if (std::sscanf(sa, "%f,%f,%f,%f", &l, &t, &r, &b) == 4) {
            canvas_safe[0] = l; canvas_safe[1] = t; canvas_safe[2] = r; canvas_safe[3] = b;
            if (platform) mq_platform_set_safe_area(platform, l, t, r, b);
        }
    }
    physics_components_setup();
    const uint32_t err0 = bus ? mq_script_bus_error_count(bus) : 0;
    dispatch_event(*this, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr);
    if (bus && mq_script_bus_error_count(bus) != err0) {
        char ebuf[512] = { 0 };
        mq_script_bus_last_error(bus, ebuf, (int)sizeof ebuf);
        write_crash_context(ebuf[0] ? ebuf : "on_start failed");
    }
    return true;
}

void Engine::teardown() {
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
    if (bus) { mq_script_bus_destroy(bus); bus = nullptr; vm = nullptr; }   // 总线拥有 VM
    if (obs) { mq_obs_destroy(obs); obs = nullptr; }
    if (reflect) {
        const mq_type_desc* ct = mq_reflect_type_by_name(reflect, "asset.AnimationClip");
        if (ct) for (auto& c : clips) destroy_blob(*this, ct->type_id, c.blob);
        clips.clear();
    } else {
        clips.clear();
    }
    agent_paths.clear();
    for (auto* s : checkpoints) mq_snapshot_destroy(s);
    checkpoints.clear();
    mq_snapshot_destroy(snap_manual); snap_manual = nullptr;
    mq_snapshot_pool_destroy(snaps); snaps = nullptr;
    mq_watcher_destroy(watcher); watcher = nullptr;
    if (reflect) { mq_reflect_destroy(reflect); reflect = nullptr; }
    if (sim) { mq_sim_destroy(sim); sim = nullptr; }
    if (font) { mq_font_destroy(font); font = nullptr; }
    font_atlases.clear();
    mq_nav_destroy(nav); nav = nullptr;
    mq_physics_destroy(physics); physics = nullptr;
    spine_release();                     // Spine 实例/骨架数据（纹理归 tex_cache → 随渲染器销毁）
    mq_input_destroy(input); input = nullptr;
    mq_platform_destroy(platform); platform = nullptr;
    if (rapi && rself) { rapi->destroy(rself); rself = nullptr; }
    mq_host_destroy(host); host = nullptr;
    mq_vfs_destroy(vfs); vfs = nullptr;
    if (storage) { mq_storage_destroy(storage); storage = nullptr; }
}

extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;
    auto* eng = new Engine();
    if (!eng->setup(scn_path, nullptr)) { eng->teardown(); delete eng; return 2; }

    /* 控制端点：inspector 用（MAQUE_INSPECT=7777 / tcp:7777 / uds:/tmp/x.sock） */
    if (const char* spec = std::getenv("MAQUE_INSPECT"); spec && *spec && std::strcmp(spec, "off") != 0) {
        const bool rw = std::getenv("MAQUE_INSPECT_RW") != nullptr;
        eng->inspect_read_only = !rw;
        eng->ctl_server = mq_control_server_create(eng->control, spec, eng->inspect_read_only ? 1 : 0);
        if (eng->ctl_server)
            std::print("runtime: control endpoint {} ({})\n", mq_control_server_url(eng->ctl_server),
                       eng->inspect_read_only ? "read-only" : "read-write");
        else
            std::print(stderr, "runtime: control endpoint 创建失败: {}\n", spec);
    }

    if (const char* req = std::getenv("MAQUE_CONTROL")) {
        const bool replayed = eng->prepare_replay();
        if (!replayed && std::getenv("MAQUE_RECORD")) mq_input_record_begin(eng->input);
        auto serve = [&](const char* r) { std::print("{}\n", mq_control_dispatch(eng->control, r)); };
        if (std::strcmp(req, "-") == 0) {
            char line[8192];
            while (std::fgets(line, sizeof line, stdin)) { line[std::strcspn(line, "\n")] = 0; if (*line) serve(line); }
        } else serve(req);
        eng->finalize_run();
        if (replayed) {
            const char* rt = mq_input_log_get_meta(eng->input, "trail");
            if (rt) {
                char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)eng->world_trail());
                if (rt == std::string(tb)) std::print("runtime: replay matches recorded trail ({})\n", tb);
                else std::print("runtime: DESYNC — replay trail {} != recorded {}\n", tb, rt);
            }
        }
        eng->teardown(); delete eng;
        return 0;
    }

    std::string replay_trail;
    if (eng->prepare_replay()) {
        if (const char* mt = mq_input_log_get_meta(eng->input, "trail")) replay_trail = mt;
    }
    if (std::getenv("MAQUE_RECORD")) mq_input_record_begin(eng->input);

    eng->main_loop(frames_dir ? frames_dir : "", std::filesystem::path(scn_path).filename().string());
    const bool quit = eng->quit || eng->frames_capped;
    eng->teardown();
    delete eng;
    return quit ? 0 : 1;
}

/* ---------------- 嵌入宿主接口（P3，docs/05）：主循环归宿主 ---------------- */

struct mq_host_scene {
    Engine* eng = nullptr;
    std::string response;
};

extern "C" mq_host_scene* mq_host_scene_create(const char* scn_path, const mq_host_desc* desc) {
    if (!scn_path) return nullptr;
    auto* hs = new mq_host_scene();
    hs->eng = new Engine();
    const char* renderer = desc ? desc->renderer : nullptr;
    if (!hs->eng->setup(scn_path, renderer)) {
        hs->eng->teardown();
        delete hs->eng;
        delete hs;
        return nullptr;
    }
    return hs;
}

extern "C" void mq_host_scene_destroy(mq_host_scene* s) {
    if (!s) return;
    if (s->eng) { s->eng->teardown(); delete s->eng; }
    delete s;
}

extern "C" int mq_host_tick(mq_host_scene* s, uint8_t* out, size_t out_size) {
    if (!s || !s->eng) return 1;
    Engine& e = *s->eng;
    e.step_frame();                                   // 帧序与 mq_run_scene 完全一致
    // 渲染该帧到宿主缓冲（step_frame 内部已渲染；这里补一次输出到调用方缓冲）
    std::vector<mq_renderer_item> draws;
    std::vector<mq_renderer_mesh> meshes;
    if (e.scene) { e.scene->extract(e, draws); e.spine_extract(meshes); }
    if (!out || out_size < (size_t)e.TW * e.H * 4) return 2;
    float bg[4] = { e.bg[0], e.bg[1], e.bg[2], e.bg[3] };
    const int rc = (!meshes.empty() && e.rapi->render_mesh_views)
        ? e.rapi->render_mesh_views(e.rself, e.TW, e.H, e.views.data(), (uint32_t)e.views.size(),
                                    draws.data(), (uint32_t)draws.size(),
                                    meshes.data(), (uint32_t)meshes.size(), bg, out, (uint32_t)out_size)
        : e.rapi->render_views(e.rself, e.TW, e.H, e.views.data(), (uint32_t)e.views.size(),
                               draws.data(), (uint32_t)draws.size(), bg, out, (uint32_t)out_size);
    return rc == 0 ? 0 : 3;
}

extern "C" void mq_host_size(mq_host_scene* s, int* w, int* h) {
    if (!s || !s->eng) return;
    if (w) *w = s->eng->TW;
    if (h) *h = s->eng->H;
}

extern "C" void mq_host_push_pointer(mq_host_scene* s, int type, int button, float x, float y) {
    if (!s || !s->eng || !s->eng->input) return;
    mq_input_event ev{};
    ev.type = (uint32_t)type;
    ev.button = (uint32_t)button;
    ev.x = x; ev.y = y;
    ev.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
    ev.pressure = type == MQ_INPUT_POINTER_DOWN ? 1.0F : 0.0F;
    mq_input_inject(s->eng->input, &ev);
}

extern "C" void mq_host_push_key(mq_host_scene* s, int key, int down, const char* text) {
    if (!s || !s->eng || !s->eng->input) return;
    mq_input_event ev{};
    ev.type = text ? MQ_INPUT_TEXT : (down ? MQ_INPUT_KEY_DOWN : MQ_INPUT_KEY_UP);
    ev.key = (uint32_t)key;
    if (text) { std::strncpy(ev.text, text, sizeof(ev.text) - 1); }
    mq_input_inject(s->eng->input, &ev);
}

extern "C" uint64_t mq_host_frame(mq_host_scene* s) { return s && s->eng ? s->eng->frame : 0; }
extern "C" uint64_t mq_host_checksum(mq_host_scene* s) { return s && s->eng ? s->eng->world_checksum() : 0; }
extern "C" int mq_host_should_quit(mq_host_scene* s) { return s && s->eng && s->eng->quit ? 1 : 0; }

extern "C" const char* mq_host_control(mq_host_scene* s, const char* request) {
    if (!s || !s->eng || !s->eng->control) return nullptr;
    s->response = mq_control_dispatch(s->eng->control, request ? request : "{}");
    return s->response.c_str();
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
