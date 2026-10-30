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
#include "abi/audio.h"                // 音频模块 C ABI（docs/30）
#include "text/mq_text.h"
#include "sim/mq_sim.h"
#include "canvas/mq_canvas.h"
#include "anim/mq_spine2d.h"          // Spine 2D 官方运行时（docs/27）：骨架/实例/绘制数据
#include "ui/mq_imgui.h"              // 引擎侧 ImGui 调试面板：帧管理/绘制数据导出
#include "ui/mq_imgui_widgets.h"      // ImGui 控件 C ABI（runtime 不直接依赖 ImGui 头）
#include "core/tracy/mq_tracy.h"      // Tracy 性能分析封装（docs/29；MAQUE_TRACY=OFF 时全部 no-op）
#include "gltf_model.h"                // 运行时 glTF/GLB 解析（plans/23/24，CPU 侧；无 GPU 依赖）

#include <fstream>
#include "core/control/mq_control.h"
#include "core/api/mq_api.h"
#include "maque_api.h"                 // 生成：host:: 声明（api_list）
#include "maque_ifaces.h"
#include "maque_control.h"
#include "maque_events.h"                 // 生成：mq_renderer_v1（插件接口 ABI）
#include "core/module/mq_module.h"          // mq_host（dlopen 加载插件）
#ifdef MQ_RENDERER_STATIC_NULL
#include "render/null/mq_renderer_null.h"
#endif
#ifdef MQ_RENDERER_STATIC_GFX
extern "C" void* mq_module_query(const char* name, uint32_t min_version);
#endif
#include "maque_types.h"

#include <print>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <chrono>
#if __has_include(<zlib.h>)
#include <zlib.h>
#define MQ_RUNTIME_HAVE_ZLIB 1
#endif
#include <thread>
#include <utility>
#include <functional>
#include <csignal>
#include <algorithm>
#include "core/compat/mq_posix_io.h"   // 崩溃路径的低层 fd I/O（POSIX open/write/pwrite ↔ Windows _open/_write）

namespace {

#define MQ_RENDERER_HAS(api, member) \
    ((api) && (api)->header.struct_size >= offsetof(mq_renderer_v1, member) + sizeof((api)->member) && \
     (api)->member)

struct Entity {
    std::string name;
    int parent = -1;
    /* 世界坐标：**每帧由 flatten_world() 从节点树展平写到这里的派生缓存**（不是位置真相！
     * 位置真相 = Transform 组件里的局部坐标；改位置请经 node/entity API 写组件，
     * 直接改 world_x/y 会在下一帧被覆盖）。 */
    float world_x = 0, world_y = 0;
    /* 3D 世界矩阵（列主序，派生缓存）：每帧由 flatten_world3d() 从节点树（+ glTF 片段派生姿态）
     * 重算；不是位置真相（真相在 scene.Transform3D.local / .clip.json 采样里）。
     * 平级场景 world_m == m4_from_trs(local)，旧场景零漂移（plans/22 §6.3）。 */
    float world_m[16] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                          0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
    /* 节点属性（active/scale/angle/opacity）：active/scale/angle/opacity 影响
     * 自身与子树；z_index 参与 UI/绘制稳定排序。全部进快照与校验和（docs/10）。 */
    bool  active = true;
    float scale = 1.0F;
    float angle = 0.0F;                                    // 角度（度）
    float opacity = 1.0F;                                  // 0..1
    int   z_index = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> comps;   // type_id → blob
};

/* ---- glTF 片段 / 蒙皮 / morph 运行时状态（plans/22 §6）----
 * 全部是**派生缓存**：真相仍是场景组件（Transform3D.local / AnimationPlayer 字段 / 资源文件），
 * 不进快照、不进校验和；切场景由 clear_asset_caches() 统一清空。 */
struct AnimPose {
    bool has_pos = false, has_rot = false, has_scale = false;
    math::Vec3 pos{};
    math::Vec3 scale{ 1.0F, 1.0F, 1.0F };
    float rot[4] = { 0.0F, 0.0F, 0.0F, 1.0F };       /* 四元数 (x,y,z,w) */
};
/* .clip.json 轨道：path 0=translation 1=rotation 2=scale 3=weights */
struct GClipTrack {
    std::string node;
    int path = 0;
    bool step = false;                                /* interpolation == "STEP" */
    uint32_t comps = 0;                               /* 每关键帧浮点个数（3/4/权重数） */
    std::vector<float> times;
    std::vector<float> values;                        /* 扁平：keys × comps */
};
struct GClip {
    bool failed = false;                              /* 解析失败（缓存起来避免每帧重试） */
    std::string name;
    float duration = 0.0F;
    std::vector<GClipTrack> tracks;
};
/* .skel.json：joint 名 + inverse_bind（列主序 mat4） */
struct Skeleton {
    bool failed = false;
    std::string name;
    std::vector<std::string> joint_names;
    std::vector<float> inverse_bind;                  /* joint_count × 16 */
};
/* CPU morph：基础顶点（已按 GPU stride 打包）+ 各 target 的 pos/normal 增量 */
struct MorphTarget {
    std::vector<float> positions;                     /* 每顶点 3 */
    std::vector<float> normals;                       /* 每顶点 3（可空） */
};
struct MorphMesh {
    int32_t mesh = -1;
    uint32_t vertex_count = 0;
    uint32_t stride = 0;
    int32_t layout = 0;
    std::vector<uint8_t> base;                        /* 打包后的基础顶点（morph 重算起点） */
    std::vector<MorphTarget> targets;
    std::vector<float> default_weights;
    std::vector<float> applied_weights;               /* 上次已上传的权重（不变就不重算） */
    int owner_entity = -1;                            /* 共享 mesh：首实体驱动（其余忽略 + 告警一次） */
    bool warned_shared = false;
    bool warned_no_update = false;
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
/* VFS 相对路径拼接（定义见场景实例装配段；watcher 等前置代码也要用）。 */
static std::string vfs_join(const std::string& base, const std::string& rel);
std::vector<uint8_t>* comp_by_name(Engine& eng, int id, const char* type_name, const mq_type_desc** out_desc);
void accumulate_node_attrs(Engine& eng, int idx, float& angle_deg, float& scale, float& opacity);
bool node_active_effective(Engine& eng, int idx);
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
    /* 每帧从**节点树 + glTF 片段派生姿态**展平出 3D 世界矩阵（写进 Entity::world_m，列主序）。 */
    void flatten_world3d(Engine& eng);
    uint64_t checksum(Engine& eng);     // 世界状态校验和（确定性；反射驱动）
    void encode(Engine& eng, SnapshotWriter& w);   // 世界 → 字节（二进制 codec）
    size_t decode(Engine& eng, const uint8_t* in, size_t n);
    void extract(Engine& eng, std::vector<mq_renderer_item>& out);
};

/* ---- 场景实例（docs/32）：`.scn` = 节点树 + 行为（scripts）+ 场景级配置。
 * 每次加载/实例化 = 一个 SceneInstance：独立 VM 组（bus）、独立生命周期与事件作用域。
 * 实例树：根场景 = 0 号；`scn.SceneInstance` 组件在加载期把嵌套 .scn 实例化为子实例。
 * 所有结构变化都发生在帧边界（加载期或动态请求队列），保证确定性。 */
struct SceneInstanceRuntime {
    int id = -1;
    int parent = -1;                 // 父实例（根 = -1）
    int attach_entity = -1;          // 父世界里的挂载壳实体（根 = -1）
    std::string source;              // VFS 路径（"/dir/x.scn"；根 = scn_vfs_path）
    std::string base;                // 资源解析基准（相对 VFS 根；根场景 = ""）
    std::string params;              // 实例参数（JSON 文本；docs/32 §16.2）
    std::vector<int> roots;          // 本实例的顶层实体（parent == attach_entity 且 owner==id）
    std::vector<int> children;       // 子实例（创建/声明顺序）
    mq_script_bus* bus = nullptr;    // 本实例的 VM 组（总线拥有 VM）
    std::vector<std::string> scripts;     // 相对 base 的脚本路径（声明序）
    std::vector<uint64_t> script_hashes;  // 内容哈希（热重载）
    bool paused = false;
    bool active = true;
    bool detached = false;           // 已烘焙：内容属于父场景（保存时写出子树，加载时不再实例化）
    bool owns_shell = false;         // 动态实例自动建的挂载壳（卸载时一并删除）
    int state = 0;                   // 0=pending 1=active 2=destroyed 3=error 4=pending-unload
    std::string error;
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
    /* ---- Tracy 性能分析（docs/29）----
     * 客户端**手动启动**：F3（并顺手拉起 profiler GUI）或启动期 MAQUE_TRACY=1（等外部连）。
     * 未启动时所有埋点宏 no-op → 普通运行/CI 零开销。 */
    bool                tracy_env_checked = false;          // 是否已按 MAQUE_TRACY 决定过初始启动
    bool                tracy_f3_held = false;              // F3 沿检测（按住只触发一次）
    bool                tracy_gui_launched = false;         // profiler GUI 是否已拉起（避免重复弹窗）
    void tracy_tick();                                      // 帧首：env 启动 + 1Hz CPU/内存采样
    void tracy_start(const char* why);                      // 启动 client + 配置基础数据曲线
    void tracy_launch_gui();                                // 拉起 tracy-profiler（F3）
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
    /* 节点事件表（entity → [(event, handler, owner_instance)]；注册序即派发序，确定性）。
     * owner_instance：注册该处理器的脚本所属实例（0=根，-2=常驻覆盖层）；派发只调它自己的 bus。 */
    struct NodeHandler { std::string event; std::string handler; int instance = 0; };
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
    /* ---- 窗口尺寸 / 渲染目标（FBO）重建 ----
     * 窗口可 resize / 最大化：SDL SIZE_CHANGED → sync_window_size() → auto 倍率时
     * 重建 ss/TW/views（= 新 FBO），直呈路径同步 resize_surface；显式倍率时目标固定。 */
    void resolve_render_scale();          // 读 MAQUE_RENDER_SCALE / ui.Canvas.render_scale + window_intent
    void rebuild_render_target();         // 解析 ss + TW + 每视图 view（装配期与窗口 resize 共用）
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
    float imgui_ptr_x = 0, imgui_ptr_y = 0;    // 最近指针位置（窗口像素）
    bool  imgui_ptr_down = false;              // 左键是否按住
    /* 指针的"窗口像素 → ImGui 显示空间"换算：
     *   · 官方后端：显示空间 = 绘制区**设备像素**；SDL 鼠标是**逻辑像素** →
     *     乘 content_scale（非 HiDPI 时 1:1）；
     *   · 网格链（无窗口/present off）：显示空间 = 目标像素 → 按画布映射反算。
     * 无窗口时 scale=1/offset=0，等价于直接用。 */
    float ptr_target_x() const {
        if (imgui_backend) return imgui_ptr_x * imgui_input_scale;
        if (!window) return imgui_ptr_x;
        const float s = canvas.scale > 0.0F ? canvas.scale : 1.0F;
        return (imgui_ptr_x - canvas.offset_x) / s;
    }
    float ptr_target_y() const {
        if (imgui_backend) return imgui_ptr_y * imgui_input_scale;
        if (!window) return imgui_ptr_y;
        const float s = canvas.scale > 0.0F ? canvas.scale : 1.0F;
        return (imgui_ptr_y - canvas.offset_y) / s;
    }
    bool  imgui_env_checked = false;           // 是否已按 MAQUE_IMGUI 决定过初始开关
    bool  imgui_frame_active = false;          // 本帧已 begin（保证 end 一定配对）
    bool  imgui_f2_held = false;               // F2 是否按住（热键沿检测：按住只切一次）
    /* ImGui 面板的**固定 UI 度量**：`imgui_resolve_metrics()` 一次算定
     *（屏幕逻辑分辨率 + DPI + 渲染倍率），**不随游戏窗口/渲染目标变化** ——
     * 同一块屏上 stats 字号/面板大小恒定；换显示器/换缩放才变。 */
    float imgui_ui_scale = 1.0F;               // 屏幕分辨率派生的 UI 缩放
    float imgui_pixel_scale = 1.0F;            // 实际像素倍率（字号 × 样式尺寸）
    float imgui_content_scale = 1.0F;          // 窗口 绘制区/逻辑区 比（HiDPI）
    float imgui_input_scale = 1.0F;            // 输入坐标 → ImGui 显示空间 的倍率
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

    /* ---- 场景实例（docs/32）----
     * instances[id] = 实例；id 单调、永不复用（陈旧句柄可判定）；根场景 = 0。
     * entity_instance：实体 → 所属实例（运行期归属，不序列化；save/snapshot 用它过滤）。
     * active_instance / in_overlay：脚本上下文（派发期间设置、嵌套调用保存恢复）。 */
    std::vector<std::unique_ptr<SceneInstanceRuntime>> instances;
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
    int   create_instance(int parent, int attach, const std::string& vfs_source);
    bool  entity_in_instance_subtree(int instance, int entity) const;
    void  reset_instances();
    bool  build_instance(SceneInstanceRuntime& inst, int depth);
    bool  load_instance_scripts(SceneInstanceRuntime& inst, std::vector<std::string> rels, bool is_root);
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
    void  destroy_instances();
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
    int W = 0, H = 0, nviews = 1, TW = 0;
    /* 渲染分辨率倍率（超采样，docs/17 §渲染分辨率）：渲染目标 = 设计分辨率 × ss。
     * ss = 0 → 自动跟随窗口像素（上限 4）；1 → 设计分辨率（像素风）；N → 固定 N 倍。
     * 文字按目标像素栅格化（device_scale = ss）→ ss 越接近"窗口/设计"，文字越平滑（无锯齿）。 */
    float ss = 1.0F;
    /* 显式渲染倍率请求（MAQUE_RENDER_SCALE 或 ui.Canvas.render_scale）；<=0 = 自动跟随窗口。
     * 窗口 resize 时：自动模式重建渲染目标（FBO）；显式模式目标固定、由 present 缩放呈现。 */
    float render_scale_req = 0.0F;
    /* 启动期"确实会有真实窗口"的意图（headless / --shot / present=off → false）。
     * 装配期 platform 还没创建，只能靠意图判断"是否按窗口像素算倍率"。 */
    bool  window_intent = false;
    int   win_last_w = 0, win_last_h = 0;         // 上次采样的窗口逻辑尺寸（0 = 尚未采样）
    int win_w = 0, win_h = 0;                     // 窗口逻辑尺寸（MAQUE_WINDOW 可覆盖；世界等比缩放显示）
    std::vector<mq_renderer_view> views;
    /* 相机节点 RenderTexture：GPU 句柄仍在 renderer 的公共纹理命名空间；uri 进入 tex_cache，
     * 因而 Sprite/网格/PBR 不需要第二套纹理类型。状态是派生资源，不进快照/校验和。 */
    struct RenderTextureState {
        int entity = -1;
        int32_t handle = -1;
        int32_t width = 0, height = 0;
        int32_t filter = 1;
        int32_t update_mode = 0;
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
    /* 场景资源全屏 shader pass（docs/13）：shader = 场景目录里的 .wgsl（**不是** builtin shader）。
     * 每个实体最多一个 render.ShaderPass；顺序 = 实体 id 序（确定性）。 */
    struct LoadedShaderPass { int entity = -1; int32_t handle = -1; };
    bool has_shader_pass = false;
    std::vector<LoadedShaderPass> loaded_shader_passes;
    /* 3D OrbitControls（鼠标拖拽环绕 + 滚轮缩放；在场景相机之上覆盖；MAQUE_ORBIT=yaw,pitch,dist 供 CI） */
    bool  orbit_enabled = false;
    bool  orbit_dragging = false;
    bool  orbit_inited = false;
    float orbit_yaw = 0.0F, orbit_pitch = 0.3F, orbit_dist = 1.0F;
    float orbit_last_x = 0.0F, orbit_last_y = 0.0F;

    /* ---- 引擎选项（F4 ImGui 面板；docs/28）----
     * 运行期可热调的纯视图选项：
     *   · rendered / solid / wireframe 渲染模式，以及游戏 3D、2D/UI 图层开关
     *   · MSAA（0=跟随场景/MAQUE_MSAA，1=关，4=4x）
     *   · 默认太阳光（场景无光源时补的方向光：开关 / 强度 / 传播方向）
     *   · 主相机 near/far 覆盖
     * 只在 extract_3d / 成帧时生效，**不写回场景组件/不进快照/校验和**（视图层调试覆盖，docs/10）。
     * 同一组值也可用 `MAQUE_ENGINE_OPTIONS` 给启动初值（见 engine_options_parse_env）。 */
    bool  opt_panel = false;                   // F4 面板开关
    bool  opt_f4_held = false;                 // F4 沿检测（按住只切换一次）
    bool  opt_opened_imgui = false;            // F4 自己打开的 ImGui（收起面板时恢复"关"→ 不残留 stats）
    int   opt_msaa = 0;                        // 0=跟随场景(MAQUE_MSAA 次之) 1=关 4=4x
    int   opt_render_mode = 0;                 // 0=rendered 1=solid 2=wireframe
    bool  opt_3d_enabled = true;               // 游戏 3D 层（不影响 F4 面板）
    bool  opt_2d_enabled = true;               // 游戏 2D/UI 层（不影响 F4 面板）
    bool  opt_sun_enabled = true;              // 场景无光源时是否补默认太阳光
    float opt_sun_intensity = 1.1F;
    float opt_sun_dir[3] = { 0.35F, -0.85F, -0.4F };   // 光的**传播方向**（shader 内 normalize）
    bool  opt_cam_override = false;            // 覆盖主相机 near/far
    float opt_cam_near = 0.1F, opt_cam_far = 100.0F;   // 未覆盖时跟随场景实际值（面板默认值随之同步）
    /* 上一帧实际生效值（面板只读回显） */
    int   opt_last_msaa = 0;
    bool  opt_last_sun = false;
    float opt_last_near = 0.1F, opt_last_far = 100.0F;
    bool  opt_env_parsed = false;
    void engine_options_parse_env();           // 启动初值 MAQUE_ENGINE_OPTIONS（只解析一次）
    void engine_options_panel(float show_w, float show_h);   // F4 面板内容（本帧 ImGui 已 begin）

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
    /* 场景资源全屏 shader pass（docs/13）：加载 .wgsl → create_shader_pass；每帧推进 time/提取。 */
    void setup_shader_passes();
    void destroy_shader_passes();
    void shader_pass_step(float dt);
    void extract_shader_passes(std::vector<mq_renderer_shader_pass>& out);
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
    void extract_text(Engine& eng, int idx, Entity& e, const mq_type_desc* d,
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
/* ---- 贴图路径 sampler 后缀（plans/20：glTF sampler 的 wrap/filter 落到 VFS 路径上）----
 * 路径形如 `textures/x.png#wrap=clamp,mirror&filter=nearest`：
 *   · '#' 之前是真正的 VFS 路径（读文件/解码用它）；
 *   · wrap=repeat|clamp|mirror → create_texture2 的 wrap_u/wrap_v（0/1/2）；可写一个（u=v）或 "u,v"；
 *   · filter=nearest|linear → create_texture2 的 filter（0/1）；
 *   · 无 '#'：文件贴图默认 repeat+linear（glTF sampler 缺省）；builtin 贴图由调用方默认 clamp。
 * 缓存 key 用**完整字符串**：同一张图配不同 sampler 是两个独立的 GPU 纹理对象。 */
struct TexPathParams {
    std::string vfs;                 /* 去掉 sampler 后缀的 VFS 路径 */
    int wrap_u = 0, wrap_v = 0;      /* 0=repeat 1=clamp 2=mirror（文件贴图默认 repeat） */
    int filter = 1;                  /* 0=nearest 1=linear */
    bool wrap_set = false, filter_set = false;
};

static int wrap_mode_code(const std::string& s) {
    if (s == "repeat") return 0;
    if (s == "clamp") return 1;
    if (s == "mirror") return 2;
    return -1;                       /* 未知值 → 忽略该项（用默认） */
}

static TexPathParams parse_tex_path(const std::string& ref) {
    TexPathParams p;
    p.vfs = ref;
    const size_t hash = ref.find('#');
    if (hash == std::string::npos) return p;
    p.vfs.assign(ref, 0, hash);
    const std::string query = ref.substr(hash + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string key = kv.substr(0, eq);
            const std::string val = kv.substr(eq + 1);
            if (key == "wrap") {
                const size_t comma = val.find(',');
                const int u = wrap_mode_code(comma == std::string::npos ? val : val.substr(0, comma));
                const int v = wrap_mode_code(comma == std::string::npos ? val : val.substr(comma + 1));
                if (u >= 0) { p.wrap_u = u; p.wrap_v = (v >= 0 ? v : u); p.wrap_set = true; }
            } else if (key == "filter") {
                if (val == "nearest") { p.filter = 0; p.filter_set = true; }
                else if (val == "linear") { p.filter = 1; p.filter_set = true; }
            }
        }
        pos = amp + 1;
    }
    return p;
}

std::string Engine::scoped_texture_ref(int entity, const std::string& ref) const {
    static constexpr const char* kRt = "rendertexture://";
    if (ref.rfind(kRt, 0) != 0) return ref;
    const std::string tail = ref.substr(std::strlen(kRt));
    if (tail.find('/') != std::string::npos) return ref;       /* 已是规范 URI */
    auto it = entity_instance.find(entity);
    const int owner = it == entity_instance.end() ? 0 : it->second;
    return std::string(kRt) + std::to_string(owner) + "/" + tail;
}

int Engine::load_texture(const std::string& path) {
    auto it = tex_cache.find(path);
    if (it != tex_cache.end()) return it->second;
    /* 旧 create_texture 没有 sampler 参数：这里只把 '#' 后缀剥掉再读文件（sampler 由 2D 全局采样器决定）；
     * PBR 贴图走 load_texture_srgb/mips → create_texture2 才真正生效。 */
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
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

/* plans/19：CPU box-filter mip 链（RGBA8，各层连续存放）。
 * 无 mip 时高频贴图（布纹/车漆细节）在远处会强烈闪烁/摩尔纹——PBR 正确性的重要一环。
 * srgb=true（M4g albedo/emissive）：RGB 必须**先解码到线性再平均、再编码回 sRGB**——
 * 直接在 sRGB 编码字节上平均会让明暗混合显著偏暗（黑白棋盘 2×2 的线性均值 0.5 →
 * 编码域平均只剩 ~0.214，线性亮度 -57%）。alpha 始终是线性数据，直接平均。
 * 线性数据贴图（MR/AO/normal）走 srgb=false，与旧实现逐字节一致。 */
static void srgb_mip_tables(float* decode, float* encode_thr) {
    for (int i = 0; i < 256; ++i) {
        const double c = (double)i / 255.0;
        decode[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    /* encode_thr[b] = 线性域中「编码值恰为 (b+0.5)/255」的门槛：值 >= 门槛即进位到 b+1，
     * 二分即得 round(255*srgb(linear))，避免逐像素 pow。 */
    for (int b = 0; b < 255; ++b) {
        const double s = ((double)b + 0.5) / 255.0;
        encode_thr[b] = (float)(s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4));
    }
}

static uint8_t srgb_encode_from_linear(float linear, const float* encode_thr) {
    if (linear <= 0.0F) return 0;
    if (linear >= 1.0F) return 255;
    int lo = 0, hi = 255;                       /* 找第一个 > linear 的门槛 → 输出 lo */
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (linear < encode_thr[mid]) hi = mid; else lo = mid + 1;
    }
    return (uint8_t)lo;
}

static std::vector<uint8_t> build_mip_chain(const uint8_t* src, int w0, int h0, int& levels,
                                            bool srgb) {
    levels = 0;
    std::vector<uint8_t> out;
    if (!src || w0 <= 0 || h0 <= 0) return out;
    out.assign(src, src + (size_t)w0 * (size_t)h0 * 4u);
    levels = 1;
    float decode[256];
    float encode_thr[255];
    if (srgb) srgb_mip_tables(decode, encode_thr);
    int w = w0, h = h0;
    size_t prev_off = 0;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const size_t dst_off = out.size();
        out.resize(dst_off + (size_t)nw * (size_t)nh * 4u);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                int acc[4] = { 0, 0, 0, 0 };
                float lin[3] = { 0.0F, 0.0F, 0.0F };
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(w - 1, x * 2 + dx);
                        const int sy = std::min(h - 1, y * 2 + dy);
                        const uint8_t* p = &out[prev_off + ((size_t)sy * (size_t)w + (size_t)sx) * 4u];
                        if (srgb) {
                            for (int c = 0; c < 3; ++c) lin[c] += decode[p[c]];
                            acc[3] += p[3];
                        } else {
                            for (int c = 0; c < 4; ++c) acc[c] += p[c];
                        }
                    }
                }
                uint8_t* q = &out[dst_off + ((size_t)y * (size_t)nw + (size_t)x) * 4u];
                if (srgb) {
                    for (int c = 0; c < 3; ++c) q[c] = srgb_encode_from_linear(lin[c] * 0.25F, encode_thr);
                    q[3] = (uint8_t)((acc[3] + 2) / 4);
                } else {
                    for (int c = 0; c < 4; ++c) q[c] = (uint8_t)((acc[c] + 2) / 4);
                }
            }
        }
        prev_off = dst_off;
        w = nw;
        h = nh;
        ++levels;
    }
    return out;
}

/* M4g：PBR albedo 贴图必须按 sRGB 采样（文件存 sRGB、shader 需要线性）——
 * create_texture2 的 RGBA8_SRGB；MR/AO/normal 是线性数据，仍走 load_texture_mips（RGBA8Unorm）。 */

/* HDR env 解码 helper 的提前声明（定义在 load_texture_ref 之前）。 */
static bool parse_radiance_hdr(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba);
/* OpenEXR scanline（NONE/RLE/ZIP/ZIPS；half/float 通道）→ 线性 RGBA float。 */
static bool parse_exr_env(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba);
static void make_studio_env_base(std::vector<float>& out, int& w, int& h);
/* PMREM：GGX 粗糙度预过滤（specular mip 链）+ 余弦卷积（diffuse irradiance）。 */
static void build_pmrem(const std::vector<float>& base, int w, int h,
                        std::vector<uint8_t>& spec_chain, int& levels,
                        std::vector<uint8_t>& irr, int& irr_w, int& irr_h);
static void build_pmrem_cube(const std::vector<std::vector<float>>& base, int w,
                             std::vector<uint8_t>& spec_chain, int& levels,
                             std::vector<uint8_t>& irr, int& irr_w);

/* envmap 加载（PMREM）：.hdr（RGBE）/ EXR / PNG/JPEG / builtin studio → RGBA16F 预过滤 mip 链 + irradiance。
 * 全部在装配期 CPU 预计算（确定性；与 GPU/后端无关），失败时 spec/irr 均为 -1 → shader 回退 analytic。 */
void Engine::load_env_textures(const std::string& path, int32_t& spec_id, int32_t& irr_id) {
    spec_id = irr_id = -1;
    auto cached = env_tex_cache.find(path);
    if (cached != env_tex_cache.end()) { spec_id = cached->second.first; irr_id = cached->second.second; return; }
    const TexPathParams tp = parse_tex_path(path);
    int w = 0, h = 0;
    std::vector<float> base;
    if (tp.vfs == "builtin:/textures/studio_env") {
        make_studio_env_base(base, w, h);
    } else if (!load_env_image_linear(path, w, h, base)) {
        env_tex_cache[path] = { -1, -1 };
        return;
    }
    if (w <= 0 || h <= 0 || base.empty()) {
        env_tex_cache[path] = { -1, -1 };
        return;
    }
    std::vector<uint8_t> spec_chain, irr;
    int levels = 1, irr_w = 0, irr_h = 0;
    const auto t0 = std::chrono::steady_clock::now();
    build_pmrem(base, w, h, spec_chain, levels, irr, irr_w, irr_h);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rapi && rself && rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = w; td.height = h;
        td.pixel_format = 2;                       /* 2 = RGBA16F（线性 HDR / 预过滤结果） */
        td.kind = 0; td.mip_levels = levels;
        td.filter = 1;                             /* linear */
        td.wrap_u = 0; td.wrap_v = 1;              /* 经度 repeat、纬度 clamp */
        td.data = spec_chain.data(); td.data_size = (uint64_t)spec_chain.size();
        if (rapi->create_texture2(rself, &td, &spec_id) != 0) spec_id = -1;
        mq_renderer_texture_desc id_ = {};
        id_.width = irr_w; id_.height = irr_h;
        id_.pixel_format = 2; id_.kind = 0; id_.mip_levels = 1;
        id_.filter = 1; id_.wrap_u = 0; id_.wrap_v = 1;
        id_.data = irr.data(); id_.data_size = (uint64_t)irr.size();
        if (rapi->create_texture2(rself, &id_, &irr_id) != 0) irr_id = -1;
    }
    if (spec_id < 0) std::print(stderr, "runtime: env_texture '{}' PMREM 上传失败\n", path);
    else
        std::print("runtime: env PMREM '{}' {}x{} → {} mips (GGX) + irradiance {}x{} ({:.1f} ms)\n",
                   path, w, h, levels, irr_w, irr_h, ms);
    env_tex_cache[path] = { spec_id, irr_id };
}

/* env 图片解码（.hdr RGBE / EXR / PNG/JPEG）→ 线性 RGBA float；供 equirect 与 6 面 cube 复用。 */
bool Engine::load_env_image_linear(const std::string& path, int& w, int& h, std::vector<float>& base) {
    w = 0; h = 0; base.clear();
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print(stderr, "runtime: env_texture not found '{}'\n", path);
        return false;
    }
    const bool is_hdr = (n >= 4 && data && ((const uint8_t*)data)[0] == '#' &&
                         ((const uint8_t*)data)[1] == '?') ||
                        (tp.vfs.size() >= 4 &&
                         (tp.vfs.compare(tp.vfs.size() - 4, 4, ".hdr") == 0 ||
                          tp.vfs.compare(tp.vfs.size() - 4, 4, ".HDR") == 0));
    const bool is_exr = n >= 4 && data && ((const uint8_t*)data)[0] == 0x76 &&
                        ((const uint8_t*)data)[1] == 0x2f && ((const uint8_t*)data)[2] == 0x31 &&
                        ((const uint8_t*)data)[3] == 0x01;
    if (is_hdr) {
        const bool ok = parse_radiance_hdr((const uint8_t*)data, n, w, h, base);
        mq_vfs_free(data);
        if (!ok) {
            std::print(stderr, "runtime: env_texture '{}' HDR(RGBE) 解析失败，回退 analytic\n", path);
            return false;
        }
        if (const char* info = std::getenv("MAQUE_RENDER3D_INFO"); info && info[0] == '1') {
            const size_t c = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            std::print(stderr, "runtime: env HDR {}x{} p0=({:.4f},{:.4f},{:.4f}) "
                               "pc=({:.4f},{:.4f},{:.4f})\n",
                       w, h, base[0], base[1], base[2], base[c], base[c + 1], base[c + 2]);
        }
        return true;
    }
    if (is_exr) {
        const bool ok = parse_exr_env((const uint8_t*)data, n, w, h, base);
        mq_vfs_free(data);
        if (!ok) {
            std::print(stderr, "runtime: env_texture '{}' EXR 解析失败（仅支持 scanline NONE/RLE/ZIP/ZIPS，"
                               "half/float 通道），回退 analytic\n", path);
            return false;
        }
        if (const char* info = std::getenv("MAQUE_RENDER3D_INFO"); info && info[0] == '1') {
            float mx = 0.0F;
            for (float v : base) mx = std::max(mx, v);
            const size_t c = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            std::print(stderr, "runtime: env EXR {}x{} p0=({:.4f},{:.4f},{:.4f}) "
                               "pc=({:.4f},{:.4f},{:.4f}) max={:.3f}\n",
                       w, h, base[0], base[1], base[2], base[c], base[c + 1], base[c + 2], mx);
        }
        return true;
    }
    uint8_t* rgba = nullptr; uint32_t iw = 0, ih = 0;
    char err[128] = { 0 };
    const int rc = mq_image_decode(data, n, &rgba, &iw, &ih, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) {
        std::print(stderr, "runtime: env_texture '{}' 解码失败: {}\n", path, err);
        return false;
    }
    /* PNG/JPEG 存的是 sRGB 编码值：先解码到线性域再做 PMREM（否则高光/平均亮度错误）。 */
    w = (int)iw; h = (int)ih;
    base.assign((size_t)w * h * 4, 1.0f);
    for (size_t i = 0, np = (size_t)w * h; i < np; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float sv = (float)rgba[i * 4 + (size_t)c] / 255.0f;
            base[i * 4 + (size_t)c] = (sv <= 0.04045f) ? sv / 12.92f
                                                       : std::pow((sv + 0.055f) / 1.055f, 2.4f);
        }
        base[i * 4 + 3] = 1.0f;
    }
    mq_image_free(rgba);
    return true;
}

/* 立方体 env 加载（第 26 轮）：基名 + 6 面后缀 → GGX 预过滤 cube（mip 链）+ irradiance cube。
 * 失败时 spec/irr 均为 -1 → shader 回退 equirect/analytic（不崩）。 */
void Engine::load_env_cubemap(const std::string& prefix, int32_t& spec_id, int32_t& irr_id) {
    spec_id = irr_id = -1;
    auto cached = env_cube_cache.find(prefix);
    if (cached != env_cube_cache.end()) { spec_id = cached->second.first; irr_id = cached->second.second; return; }
    static const char* kSuffix[6] = { "_px", "_nx", "_py", "_ny", "_pz", "_nz" };
    static const char* kExts[5] = { ".hdr", ".exr", ".png", ".jpg", ".jpeg" };
    std::vector<std::vector<float>> faces(6);
    int w = 0;
    bool ok = false;
    for (const char* ext : kExts) {
        int fw = 0, fh = 0;
        bool all = true;
        for (int f = 0; f < 6; ++f) {
            const std::string p = prefix + kSuffix[f] + std::string(ext);
            int iw = 0, ih = 0;
            std::vector<float> img;
            if (!load_env_image_linear(p, iw, ih, img)) { all = false; break; }
            if (f == 0) { fw = iw; fh = ih; }
            else if (iw != fw || ih != fh) { all = false; break; }
            faces[(size_t)f] = std::move(img);
        }
        if (all && fw > 0 && fw == fh) { w = fw; ok = true; break; }
    }
    if (!ok) {
        std::print(stderr, "runtime: env_cubemap '{}' 6 面加载失败（<基名>_px/_nx/_py/_ny/_pz/_nz + "
                           ".hdr/.exr/.png/.jpg），回退 equirect/analytic\n", prefix);
        env_cube_cache[prefix] = { -1, -1 };
        return;
    }
    std::vector<uint8_t> spec_chain, irr;
    int levels = 1, irr_w = 0;
    const auto t0 = std::chrono::steady_clock::now();
    build_pmrem_cube(faces, w, spec_chain, levels, irr, irr_w);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rapi && rself && rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = w; td.height = w;
        td.pixel_format = 2;                        /* RGBA16F */
        td.kind = 1;                                /* cube（6 面 × mip 链） */
        td.mip_levels = levels;
        td.filter = 1; td.wrap_u = 1; td.wrap_v = 1;
        td.data = spec_chain.data(); td.data_size = (uint64_t)spec_chain.size();
        if (rapi->create_texture2(rself, &td, &spec_id) != 0) spec_id = -1;
        mq_renderer_texture_desc id_{};
        id_.width = irr_w; id_.height = irr_w;
        id_.pixel_format = 2; id_.kind = 1; id_.mip_levels = 1;
        id_.filter = 1; id_.wrap_u = 1; id_.wrap_v = 1;
        id_.data = irr.data(); id_.data_size = (uint64_t)irr.size();
        if (rapi->create_texture2(rself, &id_, &irr_id) != 0) irr_id = -1;
    }
    if (spec_id < 0) std::print(stderr, "runtime: env_cubemap '{}' PMREM 上传失败\n", prefix);
    else
        std::print("runtime: env CUBEMAP '{}' {}x{} ×6 → {} mips (GGX) + irradiance {}x{}×6 ({:.1f} ms)\n",
                   prefix, w, w, levels, irr_w, irr_w, ms);
    env_cube_cache[prefix] = { spec_id, irr_id };
}

int Engine::load_texture_srgb(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    /* builtin: 是程序化纹理（checker 等），沿用既有 UNorm 路径，不做 sRGB 解码 */
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "srgb:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print("runtime: srgb texture not found '{}'\n", path);
        return -1;
    }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: srgb decode failed '{}': {}\n", path, err); return -1; }
    int32_t id = -1;
    if (rapi && rself && rapi->create_texture2 && w > 0 && h > 0) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, /*srgb=*/true);
        mq_renderer_texture_desc td{};
        td.width = (int32_t)w; td.height = (int32_t)h;
        td.pixel_format = 1;                  /* 1 = RGBA8_SRGB */
        td.kind = 0; td.mip_levels = levels;
        td.filter = tp.filter;                /* glTF sampler 后缀（缺省 linear） */
        td.wrap_u = tp.wrap_u; td.wrap_v = tp.wrap_v;   /* 缺省 repeat */
        td.data = mips.data(); td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    mq_image_free(rgba);
    if (id < 0) std::print("runtime: srgb texture upload failed '{}'\n", path);
    tex_cache[key] = id;
    return id;
}

/* plans/19：线性数据贴图（MR/AO/normal/clearcoat normal）+ CPU mip 链；采样 RGBA8Unorm。 */
int Engine::load_texture_mips(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "mip:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print("runtime: texture not found '{}'\n", path);
        return -1;
    }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int32_t id = -1;
    if (rapi && rself && rapi->create_texture2 && w > 0 && h > 0) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, /*srgb=*/false);
        mq_renderer_texture_desc td{};
        td.width = (int32_t)w; td.height = (int32_t)h;
        td.pixel_format = 0;                  /* 0 = RGBA8（线性数据） */
        td.kind = 0; td.mip_levels = levels;
        td.filter = tp.filter;                /* glTF sampler 后缀（缺省 linear） */
        td.wrap_u = tp.wrap_u; td.wrap_v = tp.wrap_v;   /* 缺省 repeat */
        td.data = mips.data(); td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    mq_image_free(rgba);
    if (id < 0) id = load_texture(path);      /* create_texture2 不可用时退回无 mip 路径（内部会剥后缀） */
    tex_cache[key] = id;
    return id;
}

/* builtin:/ → 引擎内建（此处程序化生成）；否则 assets（VFS）。
 * ref 可带 sampler 后缀（`...` 见 parse_tex_path）；缓存 key 用完整字符串。 */

/* 内建 equirect studio 环境（512x256，**线性浮点**）：天空-地面渐变 + 4 个软光箱
 *（方向与 analytic studio IBL 一致）；作为 PMREM 的源 radiance，与 .hdr 走同一条预过滤路径。 */
static void make_studio_env_base(std::vector<float>& out, int& w, int& h) {
    w = 512; h = 256;
    constexpr float kPi = 3.14159265f;
    struct Box { float dx, dy, dz; float r, g, b; float amp; float sharp; };
    static const Box boxes[4] = {
        { -0.55f, 0.78f, 0.55f, 1.00f, 0.97f, 0.92f, 4.2f, 18.0f },  /* key softbox */
        { -0.30f, 0.84f, 0.62f, 1.00f, 0.98f, 0.95f, 2.0f, 22.0f },  /* fill */
        {  0.80f, 0.30f, 0.45f, 0.85f, 0.92f, 1.00f, 1.3f, 10.0f },  /* cool rim */
        {  0.05f, 0.25f, -0.95f,0.92f, 0.95f, 1.00f, 0.8f, 12.0f }, /* back */
    };
    auto norm = [](float x, float y, float z, float& ox, float& oy, float& oz) {
        const float l = std::sqrt(x * x + y * y + z * z) + 1e-8f;
        ox = x / l; oy = y / l; oz = z / l;
    };
    out.assign((size_t)w * h * 4, 1.0f);
    for (int y = 0; y < h; ++y) {
        const float lat = kPi * (0.5f - (float(y) + 0.5f) / (float)h);
        const float cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < w; ++x) {
            const float lon = kPi * 2.0f * ((float(x) + 0.5f) / (float)w) - kPi;
            const float dx = cl * std::cos(lon), dy = sl, dz = cl * std::sin(lon);
            /* 半球渐变：地面暖灰、天空冷灰、地平线略亮 */
            const float t = std::clamp(dy * 0.5f + 0.5f, 0.0f, 1.0f);
            float r = 0.16f + (0.48f - 0.16f) * t;
            float g = 0.15f + (0.52f - 0.15f) * t;
            float b = 0.14f + (0.60f - 0.14f) * t;
            const float horizon = std::exp(-std::fabs(dy) * 8.0f);
            r += horizon * 0.10f; g += horizon * 0.10f; b += horizon * 0.11f;
            /* 软光箱（方向余弦的锐利幂） */
            for (const Box& bx : boxes) {
                float nx, ny, nz; norm(bx.dx, bx.dy, bx.dz, nx, ny, nz);
                const float c = std::max(0.0f, dx * nx + dy * ny + dz * nz);
                const float spot = std::pow(c, bx.sharp) * bx.amp;
                r += bx.r * spot; g += bx.g * spot; b += bx.b * spot;
            }
            float* px = &out[((size_t)y * w + x) * 4];
            px[0] = r; px[1] = g; px[2] = b; px[3] = 1.0f;
        }
    }
}


/* ---- envmap：Radiance .hdr（RGBE）解码 + 线性 mip（PMREM 式预过滤近似） ---- */
static uint16_t f32_to_f16(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = (int32_t)((x >> 23) & 0xffu) - 127;
    uint32_t mantissa = x & 0x7fffffu;
    if (exponent == 128) return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent > 15) return (uint16_t)(sign | 0x7c00u);
    if (exponent < -14) {
        if (exponent < -24) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(-exponent - 14);
        return (uint16_t)(sign | (mantissa >> (13 + shift)));
    }
    return (uint16_t)(sign | ((uint32_t)(exponent + 15) << 10) | (mantissa >> 13));
}

static void append_half_rgba(std::vector<uint8_t>& out, float r, float g, float b) {
    const uint16_t h[4] = { f32_to_f16(r), f32_to_f16(g), f32_to_f16(b), f32_to_f16(1.0f) };
    const uint8_t* p = (const uint8_t*)h;
    out.insert(out.end(), p, p + sizeof h);
}

/* 解析 Radiance RGBE（new RLE 扫描线；老格式按平铺 RGBE 兼容）。 */
static bool parse_radiance_hdr(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba) {
    size_t pos = 0;
    auto get_line = [&](std::string& line) {
        line.clear();
        while (pos < n && d[pos] != '\n') line.push_back((char)d[pos++]);
        if (pos < n) ++pos;
    };
    std::string line;
    get_line(line);
    if (line.rfind("#?", 0) != 0) return false;
    bool rgbe = false;
    for (;;) {
        get_line(line);
        if (line.empty() || pos >= n) break;
        if (line.rfind("FORMAT=", 0) == 0) {
            if (line.find("32-bit_rle_rgbe") != std::string::npos) rgbe = true;
            else return false;
        }
    }
    if (!rgbe) return false;
    get_line(line);                       /* 分辨率行：-Y <h> +X <w>（也支持 +Y 等简单变体） */
    int h = 0, w = 0;
    char s1 = 0, s2 = 0;
    if (std::sscanf(line.c_str(), "%c%c %d %c%c %d", &s1, &s2, &h, &s1, &s2, &w) != 6) return false;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 64u * 1024u * 1024u) return false;
    W = w; H = h;
    rgba.assign((size_t)w * h * 4, 0.0f);
    std::vector<uint8_t> scan((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        if (pos + 4 > n) return false;
        const uint8_t b0 = d[pos], b1 = d[pos + 1], b2 = d[pos + 2], b3 = d[pos + 3];
        bool new_rle = (b0 == 2 && b1 == 2 && (((int)b2 << 8) | b3) == w);
        if (new_rle) {
            pos += 4;
            for (int ch = 0; ch < 4; ++ch) {
                int x = 0;
                while (x < w) {
                    if (pos >= n) return false;
                    const int cnt = d[pos++];
                    if (cnt > 128) {
                        const int run = cnt - 128;
                        if (pos >= n) return false;
                        const uint8_t v = d[pos++];
                        for (int i = 0; i < run && x < w; ++i) scan[(size_t)(x++) * 4 + ch] = v;
                    } else {
                        for (int i = 0; i < cnt && x < w; ++i) {
                            if (pos >= n) return false;
                            scan[(size_t)(x++) * 4 + ch] = d[pos++];
                        }
                    }
                }
            }
        } else {
            /* 老格式/平铺 RGBE：直接读 w*4 字节 */
            if (pos + (size_t)w * 4 > n) return false;
            std::memcpy(scan.data(), d + pos, (size_t)w * 4);
            pos += (size_t)w * 4;
        }
        for (int x = 0; x < w; ++x) {
            const uint8_t* p4 = &scan[(size_t)x * 4];
            float* out = &rgba[((size_t)y * w + x) * 4];
            if (p4[3] == 0) { out[0] = out[1] = out[2] = 0.0f; }
            else {
                const float f = std::ldexp(1.0f, (int)p4[3] - 136);
                out[0] = p4[0] * f; out[1] = p4[1] * f; out[2] = p4[2] * f;
            }
            out[3] = 1.0f;
        }
    }
    return true;
}

/* ---- OpenEXR scanline 解码（envmap 用）----
 * 支持：单 part scanline、compression NONE/RLE/ZIP(16 行)/ZIPS(1 行)、通道类型 HALF/FLOAT/UINT、
 * 通道名 R/G/B(/A) 或 Y（灰度）。PIZ/DWAA/B44/深数据/平铺图不支持（返回 false → 调用方回退 analytic）。
 * 输出线性 RGBA float（EXR 本身是线性域，无需 sRGB 转换）。 */
struct ExrChannel { std::string name; int type = 2; int bytes = 4; };
static float exr_half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;                       /* ±0 */
        else {                                           /* subnormal */
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);         /* inf/NaN */
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f = 0.0F;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
/* OpenEXR 的 ZIP/RLE 压缩在 deflate/RLE 之前做了「predictor + 字节交错」两步变换（ImfZip/ImfRle），
 * 解压后必须逆变换：先按字节累加还原（b[i] = b[i-1] + b[i] - 128），再把数据从中间劈成两半交错。
 * 少这一步会得到量级离谱的浮点垃圾（EXR 刚接入时踩过）。 */
static void exr_unpredict_reorder(std::vector<uint8_t>& buf) {
    const size_t n = buf.size();
    if (n < 2) return;
    for (size_t i = 1; i < n; ++i)
        buf[i] = (uint8_t)((int)buf[i - 1] + (int)buf[i] - 128);
    std::vector<uint8_t> out(n);
    const size_t half = (n + 1) / 2;
    size_t p1 = 0, p2 = half;
    for (size_t i = 0; i < n; i += 2) {
        out[i] = buf[p1++];
        if (i + 1 < n) out[i + 1] = buf[p2++];
    }
    buf.swap(out);
}
static bool exr_rle_decode(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
    out.clear(); out.reserve(expected);
    size_t i = 0;
    while (out.size() < expected && i < n) {
        const int cnt = (int)(int8_t)src[i++];
        if (cnt < 0) {
            const size_t lit = (size_t)(-cnt);
            if (i + lit > n) return false;
            out.insert(out.end(), src + i, src + i + lit);
            i += lit;
        } else {
            if (i >= n) return false;
            out.insert(out.end(), (size_t)cnt + 1, src[i++]);
        }
    }
    return out.size() == expected;
}
static bool exr_unzip(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
#ifdef MQ_RUNTIME_HAVE_ZLIB
    out.resize(expected);
    uLongf dst = (uLongf)expected;
    if (uncompress(out.data(), &dst, src, (uLong)n) != Z_OK) return false;
    out.resize((size_t)dst);
    return dst == (uLongf)expected;
#else
    (void)src; (void)n; (void)expected; (void)out;
    return false;
#endif
}
static bool parse_exr_env(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba) {
    if (!d || n < 8) return false;
    if (d[0] != 0x76 || d[1] != 0x2f || d[2] != 0x31 || d[3] != 0x01) return false;
    uint32_t version = 0;
    std::memcpy(&version, d + 4, 4);
    if ((version & 0xFFu) != 2u) return false;            /* version 2 */
    if (version & 0xFFFFFE00u) return false;              /* tiled/long-names/deep/multipart 不支持 */
    size_t pos = 8;
    std::vector<ExrChannel> channels;
    int compression = -1;
    int line_order = 0;
    int xmin = 0, ymin = 0, xmax = -1, ymax = -1;
    auto need = [&](size_t k) { return pos + k <= n; };
    while (pos < n) {
        if (d[pos] == 0) { ++pos; break; }                /* header 结束 */
        const size_t name_beg = pos;
        while (pos < n && d[pos] != 0) ++pos;
        if (pos >= n) return false;
        std::string name((const char*)d + name_beg, pos - name_beg);
        ++pos;
        const size_t type_beg = pos;
        while (pos < n && d[pos] != 0) ++pos;
        if (pos >= n) return false;
        std::string type((const char*)d + type_beg, pos - type_beg);
        ++pos;
        if (!need(4)) return false;
        uint32_t size = 0;
        std::memcpy(&size, d + pos, 4); pos += 4;
        if (!need(size)) return false;
        const uint8_t* v = d + pos;
        if (name == "channels" && type == "chlist") {
            size_t q = 0;
            while (q < size && v[q] != 0) {
                const size_t nb = q;
                while (q < size && v[q] != 0) ++q;
                if (q >= size) return false;
                ExrChannel ch;
                ch.name.assign((const char*)v + nb, q - nb);
                ++q;
                if (q + 16 > size) return false;           /* type4 + pLinear/reserved4 + x/ySampling8 */
                int ptype = 0;
                std::memcpy(&ptype, v + q, 4); q += 4;
                q += 4;                                    /* pLinear + 3 reserved */
                int32_t xs = 0, ys = 0;
                std::memcpy(&xs, v + q, 4); q += 4;
                std::memcpy(&ys, v + q, 4); q += 4;
                if (xs != 1 || ys != 1) return false;      /* 子采样通道不支持 */
                if (ptype < 0 || ptype > 2) return false;
                ch.type = ptype;
                ch.bytes = (ptype == 1) ? 2 : 4;
                channels.push_back(std::move(ch));
            }
        } else if (name == "compression") {
            if (size < 1) return false;
            compression = v[0];
        } else if (name == "lineOrder") {
            if (size < 1) return false;
            line_order = v[0];
        } else if (name == "dataWindow") {
            if (size < 16) return false;
            std::memcpy(&xmin, v, 4); std::memcpy(&ymin, v + 4, 4);
            std::memcpy(&xmax, v + 8, 4); std::memcpy(&ymax, v + 12, 4);
        }
        pos += size;
    }
    if (channels.empty()) return false;
    if (compression < 0) compression = 0;
    if (line_order != 0) return false;                    /* 只支持 increasing Y（HDRI 常态） */
    const int w = xmax - xmin + 1, h = ymax - ymin + 1;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 64u * 1024u * 1024u) return false;
    int lines_per_block = 1;
    if (compression == 3) lines_per_block = 16;           /* ZIP；ZIPS(2)/RLE(1)/NONE(1) 均为 1 行 */
    else if (compression > 3) return false;
    const int blocks = (h + lines_per_block - 1) / lines_per_block;
    if (!need((size_t)blocks * 8)) return false;
    std::vector<uint64_t> offsets((size_t)blocks);
    std::memcpy(offsets.data(), d + pos, (size_t)blocks * 8);
    size_t bytes_per_pixel = 0;
    for (const auto& ch : channels) bytes_per_pixel += (size_t)ch.bytes;
    int ri = -1, gi = -1, bi = -1, ai = -1, yi = -1;
    for (int i = 0; i < (int)channels.size(); ++i) {
        if (channels[(size_t)i].name == "R") ri = i;
        else if (channels[(size_t)i].name == "G") gi = i;
        else if (channels[(size_t)i].name == "B") bi = i;
        else if (channels[(size_t)i].name == "A") ai = i;
        else if (channels[(size_t)i].name == "Y") yi = i;
    }
    const bool rgb = ri >= 0 && gi >= 0 && bi >= 0;
    if (!rgb && yi < 0) return false;
    auto channel_offset = [&](int c) {
        size_t off = 0;
        for (int i = 0; i < c; ++i) off += (size_t)channels[(size_t)i].bytes;
        return off * (size_t)w;      /* 同一扫描线内按通道整行排列（不是逐像素交错） */
    };
    auto sample = [](const uint8_t* p, int type) -> float {
        if (type == 1) { uint16_t hv = 0; std::memcpy(&hv, p, 2); return exr_half_to_float(hv); }
        if (type == 2) { float f = 0.0F; std::memcpy(&f, p, 4); return f; }
        uint32_t u = 0; std::memcpy(&u, p, 4); return (float)u;
    };
    W = w; H = h;
    rgba.assign((size_t)w * h * 4, 0.0f);
    for (size_t i = 0; i < (size_t)w * h; ++i) rgba[i * 4 + 3] = 1.0f;
    std::vector<uint8_t> raw, dec;
    for (int b = 0; b < blocks; ++b) {
        const uint64_t off = offsets[(size_t)b];
        if (off > (uint64_t)n || (uint64_t)n - off < 8) return false;
        int32_t by = 0, dsize = 0;
        std::memcpy(&by, d + (size_t)off, 4); std::memcpy(&dsize, d + (size_t)off + 4, 4);
        if (dsize < 0 || (uint64_t)dsize > (uint64_t)n - off - 8) return false;
        const int nrows = std::min(lines_per_block, h - (by - ymin));
        if (nrows <= 0 || by < ymin || by > ymax) return false;
        const size_t expected = (size_t)nrows * w * bytes_per_pixel;
        const uint8_t* src = d + off + 8;
        const uint8_t* rows = nullptr;
        if (compression == 0) {
            if ((size_t)dsize != expected) return false;
            rows = src;
        } else if (compression == 1) {
            if (!exr_rle_decode(src, (size_t)dsize, expected, dec)) return false;
            exr_unpredict_reorder(dec);
            rows = dec.data();
        } else {
            if (!exr_unzip(src, (size_t)dsize, expected, dec)) return false;
            exr_unpredict_reorder(dec);
            rows = dec.data();
        }
        for (int r = 0; r < nrows; ++r) {
            const uint8_t* row = rows + (size_t)r * w * bytes_per_pixel;
            for (int x = 0; x < w; ++x) {
                float* out = &rgba[((size_t)(by - ymin + r) * w + x) * 4];
                if (rgb) {
                    out[0] = sample(row + channel_offset(ri) + (size_t)x * channels[(size_t)ri].bytes,
                                    channels[(size_t)ri].type);
                    out[1] = sample(row + channel_offset(gi) + (size_t)x * channels[(size_t)gi].bytes,
                                    channels[(size_t)gi].type);
                    out[2] = sample(row + channel_offset(bi) + (size_t)x * channels[(size_t)bi].bytes,
                                    channels[(size_t)bi].type);
                } else {
                    const float y = sample(row + channel_offset(yi) + (size_t)x * channels[(size_t)yi].bytes,
                                           channels[(size_t)yi].type);
                    out[0] = out[1] = out[2] = y;
                }
                if (ai >= 0)
                    out[3] = sample(row + channel_offset(ai) + (size_t)x * channels[(size_t)ai].bytes,
                                    channels[(size_t)ai].type);
            }
        }
    }
    return true;
}

/* ---- 真正 PMREM（IBL 预过滤，CPU、确定性）----
 * three.js / UE 的 split-sum IBL：镜面 = GGX importance-sample 预过滤的粗糙度 mip 链
 *（V=N=R 假设下按 NdotL 加权归一），漫反射 = 余弦加权半球卷积的 irradiance 贴图。
 * 之前实现是 2×2 box mip 近似：粗糙度增大时只是"更糊"，不是 GGX 能量分布，
 * 且漫反射仍用 analytic —— 这两点在此替换。EXR 已支持；cubemap kind=1 仍列后续。 */
static constexpr float kEnvPi = 3.14159265358979f;

static void env_dir_from_uv(float u, float v, float out[3]) {
    const float phi = (u - 0.5f) * 2.0f * kEnvPi;
    const float theta = v * kEnvPi;
    const float st = std::sin(theta);
    out[0] = st * std::cos(phi);
    out[1] = std::cos(theta);
    out[2] = st * std::sin(phi);
}
static void env_uv_from_dir(const float d[3], float& u, float& v) {
    u = std::atan2(d[2], d[0]) / (2.0f * kEnvPi) + 0.5f;
    v = std::acos(std::clamp(d[1], -1.0f, 1.0f)) / kEnvPi;
}
static void env_normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-20f;
    v[0] /= l; v[1] /= l; v[2] /= l;
}
static float env_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
/* 以 n 为 z 轴的切线基（避免极点退化）。 */
static void env_basis(const float n[3], float t[3], float b[3]) {
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (std::fabs(n[1]) > 0.999f) { up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f; }
    t[0] = up[1] * n[2] - up[2] * n[1];
    t[1] = up[2] * n[0] - up[0] * n[2];
    t[2] = up[0] * n[1] - up[1] * n[0];
    env_normalize(t);
    b[0] = n[1] * t[2] - n[2] * t[1];
    b[1] = n[2] * t[0] - n[0] * t[2];
    b[2] = n[0] * t[1] - n[1] * t[0];
}
/* equirect 双线性采样：经度 wrap、纬度 clamp（与 GPU 侧 sampler 语义一致）。 */
static void env_sample_bilinear(const std::vector<float>& img, int w, int h,
                                const float d[3], float out[3]) {
    float u = 0.0f, v = 0.0f;
    env_uv_from_dir(d, u, v);
    const float fx = u * (float)w - 0.5f;
    const float fy = v * (float)h - 0.5f;
    const int x0 = (int)std::floor(fx);
    const int y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto at = [&](int x, int y) -> const float* {
        x %= w; if (x < 0) x += w;
        y = std::clamp(y, 0, h - 1);
        return &img[((size_t)y * (size_t)w + (size_t)x) * 4];
    };
    const float* p00 = at(x0, y0);
    const float* p10 = at(x0 + 1, y0);
    const float* p01 = at(x0, y0 + 1);
    const float* p11 = at(x0 + 1, y0 + 1);
    for (int c = 0; c < 3; ++c) {
        const float a = p00[c] + (p10[c] - p00[c]) * tx;
        const float b = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = a + (b - a) * ty;
    }
}
/* Hammersley 低差异序列（radical inverse base 2）。 */
static float radical_inverse_vdc(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}
/* GGX/Trowbridge-Reitz 重要性采样：返回围绕 n 的半程向量 H。 */
static void env_importance_sample_ggx(float xi1, float xi2, const float n[3], float rough, float h[3]) {
    const float a = rough * rough;
    const float a2 = a * a;
    const float phi = 2.0f * kEnvPi * xi1;
    const float cos_theta = std::sqrt((1.0f - xi2) / (1.0f + (a2 - 1.0f) * xi2));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    float t[3], b[3];
    env_basis(n, t, b);
    const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
    for (int i = 0; i < 3; ++i) h[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
}
/* ---- cubemap env 辅助（第 26 轮）----
 * 面序：0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z；面内 u 向右、v 向下（图片行 0 = 顶部），
 * 与 tools/equirect_to_cube.py 的 face_dir 逐式一致；env_face_uv_from_dir 是其逆映射。 */
static void env_face_dir(int face, float u, float v, float d[3]) {
    const float s = 2.0f * u - 1.0f, t = 2.0f * v - 1.0f;
    switch (face) {
    case 0: d[0] = 1.0f;  d[1] = -t;    d[2] = -s;    break;   /* +X */
    case 1: d[0] = -1.0f; d[1] = -t;    d[2] = s;     break;   /* -X */
    case 2: d[0] = s;     d[1] = 1.0f;  d[2] = t;     break;   /* +Y */
    case 3: d[0] = s;     d[1] = -1.0f; d[2] = -t;    break;   /* -Y */
    case 4: d[0] = s;     d[1] = -t;    d[2] = 1.0f;  break;   /* +Z */
    default: d[0] = -s;   d[1] = -t;    d[2] = -1.0f; break;   /* -Z */
    }
}
static void env_face_uv_from_dir(const float d[3], int& face, float& u, float& v) {
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    const float ma = std::max(ax, std::max(ay, az));
    if (ma <= 1e-8f) { face = 0; u = 0.5f; v = 0.5f; return; }
    if (ax >= ay && ax >= az) {
        if (d[0] >= 0.0f) {
            face = 0; u = 0.5f * (1.0f - d[2] / ma); v = 0.5f * (1.0f - d[1] / ma);
        } else {
            face = 1; u = 0.5f * (1.0f - d[2] / ma); v = 0.5f * (1.0f + d[1] / ma);
        }
    } else if (ay >= az) {
        if (d[1] >= 0.0f) {
            face = 2; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f + d[2] / ma);
        } else {
            face = 3; u = 0.5f * (1.0f - d[0] / ma); v = 0.5f * (1.0f + d[2] / ma);
        }
    } else {
        if (d[2] >= 0.0f) {
            face = 4; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f - d[1] / ma);
        } else {
            face = 5; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f + d[1] / ma);
        }
    }
}
static void env_sample_cube(const std::vector<std::vector<float>>& faces, int w,
                            const float dir[3], float out[3]) {
    float n[3] = { dir[0], dir[1], dir[2] };
    env_normalize(n);
    int face = 0;
    float u = 0.5f, v = 0.5f;
    env_face_uv_from_dir(n, face, u, v);
    const std::vector<float>& img = faces[(size_t)face];
    const float x = u * (float)w - 0.5f, y = v * (float)w - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - (float)x0, fy = y - (float)y0;
    auto px = [&](int xx, int yy, float c[3]) {
        xx = std::clamp(xx, 0, w - 1);
        yy = std::clamp(yy, 0, w - 1);
        const float* p = &img[((size_t)yy * (size_t)w + (size_t)xx) * 4];
        c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
    };
    float c00[3], c10[3], c01[3], c11[3];
    px(x0, y0, c00); px(x0 + 1, y0, c10); px(x0, y0 + 1, c01); px(x0 + 1, y0 + 1, c11);
    for (int i = 0; i < 3; ++i)
        out[i] = (c00[i] * (1.0f - fx) + c10[i] * fx) * (1.0f - fy) +
                 (c01[i] * (1.0f - fx) + c11[i] * fx) * fy;
}
/* cube PMREM：mip0 = 原始 6 面（镜面），mip L = GGX 预过滤（rough = L/(levels-1)），
 * 另外生成 16×16×6 的余弦卷积 irradiance cube。输出布局 = “每个 mip 依次 6 面”（gfx create_texture2 约定）。 */
static void build_pmrem_cube(const std::vector<std::vector<float>>& base, int w,
                             std::vector<uint8_t>& spec_chain, int& levels,
                             std::vector<uint8_t>& irr, int& irr_w) {
    levels = 1;
    for (int sz = w; sz > 1; sz >>= 1) ++levels;
    spec_chain.clear();
    for (int f = 0; f < 6; ++f) {
        for (size_t i = 0, np = (size_t)w * (size_t)w; i < np; ++i) {
            append_half_rgba(spec_chain, base[(size_t)f][i * 4], base[(size_t)f][i * 4 + 1],
                             base[(size_t)f][i * 4 + 2]);
        }
    }
    for (int lvl = 1; lvl < levels; ++lvl) {
        const int ow = std::max(1, w >> lvl);
        const float rough = (levels > 1) ? (float)lvl / (float)(levels - 1) : 0.0f;
        const int samples = std::clamp(32 + (int)(rough * 96.0f), 32, 128);
        for (int f = 0; f < 6; ++f) {
            for (int y = 0; y < ow; ++y) {
                for (int x = 0; x < ow; ++x) {
                    float n[3];
                    env_face_dir(f, ((float)x + 0.5f) / (float)ow, ((float)y + 0.5f) / (float)ow, n);
                    env_normalize(n);
                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    float wsum = 0.0f;
                    for (int s = 0; s < samples; ++s) {
                        const float xi1 = ((float)s + 0.5f) / (float)samples;
                        const float xi2 = radical_inverse_vdc((uint32_t)s);
                        float hv[3], ldir[3];
                        env_importance_sample_ggx(xi1, xi2, n, std::max(rough, 1e-3f), hv);
                        const float ndh = env_dot(n, hv);
                        for (int i = 0; i < 3; ++i) ldir[i] = 2.0f * ndh * hv[i] - n[i];
                        env_normalize(ldir);
                        const float ndl = env_dot(n, ldir);
                        if (ndl <= 0.0f) continue;
                        float c[3];
                        env_sample_cube(base, w, ldir, c);
                        for (int i = 0; i < 3; ++i) acc[i] += c[i] * ndl;
                        wsum += ndl;
                    }
                    if (wsum > 0.0f) { for (int i = 0; i < 3; ++i) acc[i] /= wsum; }
                    append_half_rgba(spec_chain, acc[0], acc[1], acc[2]);
                }
            }
        }
    }
    irr_w = 16;
    const int irr_samples = 128;
    irr.clear();
    for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < irr_w; ++y) {
            for (int x = 0; x < irr_w; ++x) {
                float n[3];
                env_face_dir(f, ((float)x + 0.5f) / (float)irr_w, ((float)y + 0.5f) / (float)irr_w, n);
                env_normalize(n);
                float t[3], b[3];
                env_basis(n, t, b);
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                for (int s = 0; s < irr_samples; ++s) {
                    const float xi1 = radical_inverse_vdc((uint32_t)s);
                    const float xi2 = ((float)s + 0.5f) / (float)irr_samples;
                    const float phi = 2.0f * kEnvPi * xi1;
                    const float cos_theta = std::sqrt(std::max(0.0f, 1.0f - xi2));
                    const float sin_theta = std::sqrt(xi2);
                    const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
                    float ldir[3];
                    for (int i = 0; i < 3; ++i) ldir[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
                    env_normalize(ldir);
                    float c[3];
                    env_sample_cube(base, w, ldir, c);
                    for (int i = 0; i < 3; ++i) acc[i] += c[i];
                }
                for (int i = 0; i < 3; ++i) acc[i] /= (float)irr_samples;
                append_half_rgba(irr, acc[0], acc[1], acc[2]);
            }
        }
    }
}

static void build_pmrem(const std::vector<float>& base, int w, int h,
                        std::vector<uint8_t>& spec_chain, int& levels,
                        std::vector<uint8_t>& irr, int& irr_w, int& irr_h) {
    levels = 1;
    for (int s = std::max(w, h); s > 1; s >>= 1) ++levels;
    spec_chain.clear();
    /* mip0 = 原始 radiance（roughness 0 = 镜面，shader 采样它必须保持锐利） */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float* p = &base[((size_t)y * (size_t)w + (size_t)x) * 4];
            append_half_rgba(spec_chain, p[0], p[1], p[2]);
        }
    }
    for (int lvl = 1; lvl < levels; ++lvl) {
        const int ow = std::max(1, w >> lvl), oh = std::max(1, h >> lvl);
        const float rough = (levels > 1) ? (float)lvl / (float)(levels - 1) : 0.0f;
        const int samples = std::clamp(32 + (int)(rough * 96.0f), 32, 128);
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                float n[3];
                env_dir_from_uv(((float)x + 0.5f) / (float)ow, ((float)y + 0.5f) / (float)oh, n);
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                float wsum = 0.0f;
                for (int s = 0; s < samples; ++s) {
                    const float xi1 = ((float)s + 0.5f) / (float)samples;
                    const float xi2 = radical_inverse_vdc((uint32_t)s);
                    float hv[3], ldir[3];
                    env_importance_sample_ggx(xi1, xi2, n, std::max(rough, 1e-3f), hv);
                    const float ndh = env_dot(n, hv);
                    /* V=N=R 假设：L = reflect(-V, H) = 2*(V·H)*H - V */
                    for (int i = 0; i < 3; ++i) ldir[i] = 2.0f * ndh * hv[i] - n[i];
                    env_normalize(ldir);
                    const float ndl = env_dot(n, ldir);
                    if (ndl <= 0.0f) continue;
                    float c[3];
                    env_sample_bilinear(base, w, h, ldir, c);
                    for (int i = 0; i < 3; ++i) acc[i] += c[i] * ndl;
                    wsum += ndl;
                }
                if (wsum > 0.0f) { for (int i = 0; i < 3; ++i) acc[i] /= wsum; }
                append_half_rgba(spec_chain, acc[0], acc[1], acc[2]);
            }
        }
    }
    /* 漫反射 irradiance：余弦加权半球采样 → E/π（= 与 Lambert albedo/π 相乘后的等效辐亮度）。
     * 低分辨率即可（irradiance 在方向域很平滑）：64×32，256 样本。 */
    irr_w = 64; irr_h = 32;
    const int irr_samples = 256;
    irr.clear();
    for (int y = 0; y < irr_h; ++y) {
        for (int x = 0; x < irr_w; ++x) {
            float n[3];
            env_dir_from_uv(((float)x + 0.5f) / (float)irr_w, ((float)y + 0.5f) / (float)irr_h, n);
            float t[3], b[3];
            env_basis(n, t, b);
            float acc[3] = { 0.0f, 0.0f, 0.0f };
            for (int s = 0; s < irr_samples; ++s) {
                const float xi1 = radical_inverse_vdc((uint32_t)s);
                const float xi2 = ((float)s + 0.5f) / (float)irr_samples;
                const float phi = 2.0f * kEnvPi * xi1;
                const float cos_theta = std::sqrt(std::max(0.0f, 1.0f - xi2));
                const float sin_theta = std::sqrt(xi2);
                const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
                float ldir[3];
                for (int i = 0; i < 3; ++i) ldir[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
                env_normalize(ldir);
                float c[3];
                env_sample_bilinear(base, w, h, ldir, c);
                for (int i = 0; i < 3; ++i) acc[i] += c[i];
            }
            for (int i = 0; i < 3; ++i) acc[i] /= (float)irr_samples;
            append_half_rgba(irr, acc[0], acc[1], acc[2]);
        }
    }
}

int Engine::load_texture_ref(const std::string& ref) {
    const TexPathParams tp = parse_tex_path(ref);
    if (tp.vfs.rfind("builtin:", 0) == 0) {
        auto it = tex_cache.find(ref);
        if (it != tex_cache.end()) return it->second;
        int w = 1, h = 1;
        std::vector<uint8_t> px;
        int builtin_mips = 1;
        int builtin_format = 0;             /* 0=RGBA8Unorm 1=RGBA8UnormSrgb */
        int builtin_wrap_u = -1, builtin_wrap_v = -1;
        if (tp.vfs == "builtin:/textures/checker") {
            w = h = 16; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                bool alt = (((x / 4) + (y / 4)) & 1) != 0;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = alt ? 255 : 230; c[1] = alt ? 255 : 140; c[2] = alt ? 255 : 60; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/toon_ramp") {
            /* Toon 渐变：256×1 四档硬阶（与 toon_steps=4 对应；线性 clamp 采样） */
            w = 256; h = 1; px.resize((size_t)w * 4);
            static const uint8_t steps[4] = { 64, 132, 200, 255 };
            for (int x = 0; x < w; ++x) {
                const uint8_t v = steps[std::min(3, x * 4 / w)];
                uint8_t* c = &px[(size_t)x * 4]; c[0] = v; c[1] = v; c[2] = v; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/matcap_studio") {
            /* Matcap：256×256 冷色相影棚球（左上高光、右下暗），线性采样/clamp；用作 view-space 法线查表 */
            w = h = 256; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const float nx = (float(x) + 0.5F) / w * 2.0F - 1.0F;
                const float ny = (float(y) + 0.5F) / h * 2.0F - 1.0F;
                const float r2 = nx * nx + ny * ny;
                const float nz = std::sqrt(std::max(0.0F, 1.0F - r2));
                /* 视空间光：左上前方（与 view 法线约定一致；纹理 v 向下 → ny 取反） */
                const float lx = -0.42F, ly = -0.50F, lz = 0.76F;
                const float ndl = std::max(0.0F, nx * lx + ny * ly + nz * lz);
                const float spec = std::pow(std::max(0.0F, nx * (-0.20F) + ny * (-0.26F) + nz * 0.94F), 28.0F);
                const float diff = 0.12F + 0.88F * ndl;
                const float edge = std::clamp(1.0F - r2, 0.0F, 1.0F);   /* 球外渐暗 */
                const float r = (0.20F * diff + spec * 0.95F) * edge;
                const float g = (0.34F * diff + spec * 0.98F) * edge;
                const float b = (0.62F * diff + spec * 0.95F) * edge;
                const float a = 1.0F;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = (uint8_t)std::clamp((int)(r * 255.0F + 0.5F), 0, 255);
                c[1] = (uint8_t)std::clamp((int)(g * 255.0F + 0.5F), 0, 255);
                c[2] = (uint8_t)std::clamp((int)(b * 255.0F + 0.5F), 0, 255);
                c[3] = (uint8_t)(a * 255.0F);
            }
        } else { px = { 255, 255, 255, 255 }; }
        int id = -1;
        if (rapi && rself && rapi->create_texture2) {
            /* builtin 查表纹理默认 clamp（matcap 球/渐变在边界外不能 wrap）；路径后缀可覆盖。 */
            const int wrap_u = tp.wrap_set ? tp.wrap_u : (builtin_wrap_u >= 0 ? builtin_wrap_u : 1);
            const int wrap_v = tp.wrap_set ? tp.wrap_v : (builtin_wrap_v >= 0 ? builtin_wrap_v : 1);
            const int filter = tp.filter_set ? tp.filter : 1;
            mq_renderer_texture_desc td{};
            td.width = w; td.height = h; td.pixel_format = builtin_format; td.kind = 0;
            td.mip_levels = builtin_mips;
            td.filter = filter; td.wrap_u = wrap_u; td.wrap_v = wrap_v;
            td.data = px.data(); td.data_size = px.size();
            if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
        }
        if (id < 0) id = rapi->create_texture(rself, px.data(), w, h);
        tex_cache[ref] = id;
        return id;
    }
    return load_texture(ref);   // assets（load_texture 内部剥 sampler 后缀）
}

/* M4c：加载离线导入的 .mesh.json（tools/gltf_import.py 产出）→ 渲染器网格句柄。
 * 只读资产文件；按路径缓存（失败也缓存 -1，避免每帧重试/刷日志）。
 * plans/22 §6.1：vertex_layout 2/3 = 48/56B 基础 + joints u16x4 + weights f32x4（stride 72/80）；
 * morph 网格额外保存**打包后的基础顶点** + targets/default_weights，创建后立即应用默认权重。 */
int32_t Engine::load_mesh(const std::string& path) {
    auto it = mesh_cache.find(path);
    if (it != mesh_cache.end()) return it->second;
    if (!gltf_env_read) read_gltf_env();
    const auto t0 = std::chrono::steady_clock::now();
    int32_t id = -1;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != 0) {
        std::print(stderr, "runtime: mesh not found '{}'\n", path);
    } else {
        mq_json* dom = mq_json_parse((const char*)data, n);
        mq_vfs_free(data);
        if (!dom) {
            std::print(stderr, "runtime: mesh json 解析失败 '{}'\n", path);
        } else {
            std::vector<float> verts;
            std::vector<uint32_t> idx;
            /* 顶点布局（plans/22 §5.1）：
             *   0 = 旧 48B：pos3|normal3|tangent4|uv0 2（12 float/顶点）
             *   1 = 新 56B：pos3|normal3|color4|uv0 2|uv1 2（14 float/顶点）
             *   2 = 48B + joints u16x4 + weights f32x4（stride 72；JSON 行宽仍 12）
             *   3 = 56B + joints u16x4 + weights f32x4（stride 80；JSON 行宽仍 14）
             * 蒙皮数据在独立 joints/weights 数组 → 运行时按 GPU stride 打包。 */
            int32_t vertex_layout = 0;
            if (const mq_json* jlay = mq_json_get(dom, "vertex_layout"))
                vertex_layout = (int32_t)mq_json_num(jlay, 0.0);
            const bool base56 = vertex_layout == 1 || vertex_layout == 3;
            const uint32_t row_floats = base56 ? 14u : 12u;
            const uint32_t base_stride = base56 ? 56u : 48u;
            const bool skinned = vertex_layout >= 2 && vertex_layout <= 3;
            const uint32_t stride = base_stride + (skinned ? 24u : 0u);
            const mq_json* jv = mq_json_get(dom, "vertices");
            const uint32_t vn = mq_json_len(jv);
            verts.reserve((size_t)vn * row_floats);
            const mq_json* ji = mq_json_get(dom, "indices");
            const uint32_t in = mq_json_len(ji);
            idx.reserve(in);
            bool ok = vn > 0 && in > 0 && vertex_layout >= 0 && vertex_layout <= 3;
            if (!ok) std::print(stderr, "runtime: mesh '{}' 无效（vertices={} indices={} vertex_layout={}）\n",
                                path, vn, in, vertex_layout);
            for (uint32_t i = 0; ok && i < vn; ++i) {
                const mq_json* row = mq_json_at(jv, i);
                if (mq_json_len(row) != row_floats) { ok = false; break; }
                for (uint32_t k = 0; k < row_floats; ++k)
                    verts.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
            }
            for (uint32_t i = 0; ok && i < in; ++i)
                idx.push_back((uint32_t)mq_json_num(mq_json_at(ji, i), 0.0));
            /* 蒙皮属性打包（joints u16x4 + weights f32x4 追加在基础顶点之后） */
            std::vector<uint8_t> packed;
            const void* vptr = verts.data();
            uint64_t vbytes = (uint64_t)verts.size() * sizeof(float);
            if (ok && skinned) {
                const mq_json* jj = mq_json_get(dom, "joints");
                const mq_json* jw = mq_json_get(dom, "weights");
                ok = mq_json_len(jj) == vn && mq_json_len(jw) == vn;
                if (!ok) std::print(stderr, "runtime: mesh '{}' 蒙皮数据缺失/长度不符（joints={} weights={} vertices={}）\n",
                                    path, mq_json_len(jj), mq_json_len(jw), vn);
                if (ok) {
                    packed.resize((size_t)vn * stride);
                    for (uint32_t i = 0; i < vn; ++i) {
                        uint8_t* dst = packed.data() + (size_t)i * stride;
                        std::memcpy(dst, verts.data() + (size_t)i * row_floats, base_stride);
                        const mq_json* jr = mq_json_at(jj, i);
                        const mq_json* wr = mq_json_at(jw, i);
                        if (mq_json_len(jr) != 4 || mq_json_len(wr) != 4) { ok = false; break; }
                        uint16_t* jdst = (uint16_t*)(dst + base_stride);
                        for (uint32_t k = 0; k < 4; ++k) {
                            int v = (int)mq_json_num(mq_json_at(jr, k), 0.0);
                            if (v < 0) v = 0;
                            if (v > 65535) v = 65535;
                            jdst[k] = (uint16_t)v;
                        }
                        float* wdst = (float*)(dst + base_stride + 8);
                        for (uint32_t k = 0; k < 4; ++k)
                            wdst[k] = (float)mq_json_num(mq_json_at(wr, k), 0.0);
                    }
                    if (ok) { vptr = packed.data(); vbytes = packed.size(); }
                }
            }
            /* morph（可选）：default_weights + targets（逐顶点 pos/normal 增量；targets ≤ 8） */
            MorphMesh morph;
            if (ok) {
                if (const mq_json* jm = mq_json_get(dom, "morph")) {
                    const mq_json* jdw = mq_json_get(jm, "default_weights");
                    const uint32_t ndw = mq_json_len(jdw);
                    morph.default_weights.reserve(ndw);
                    for (uint32_t k = 0; k < ndw; ++k)
                        morph.default_weights.push_back((float)mq_json_num(mq_json_at(jdw, k), 0.0));
                    const mq_json* jts = mq_json_get(jm, "targets");
                    const uint32_t nts = mq_json_len(jts);
                    if (nts > 8)
                        std::print(stderr, "runtime: mesh '{}' morph targets={} 超出上限 8，后续忽略\n",
                                   path, nts);
                    for (uint32_t t = 0; t < nts && t < 8 && ok; ++t) {
                        const mq_json* jt = mq_json_at(jts, t);
                        MorphTarget tgt;
                        const mq_json* jp = mq_json_get(jt, "positions");
                        const mq_json* jn = mq_json_get(jt, "normals");
                        if (mq_json_len(jp) != vn) { ok = false; break; }
                        tgt.positions.reserve((size_t)vn * 3);
                        for (uint32_t i = 0; i < vn && ok; ++i) {
                            const mq_json* row = mq_json_at(jp, i);
                            if (mq_json_len(row) != 3) { ok = false; break; }
                            for (uint32_t k = 0; k < 3; ++k)
                                tgt.positions.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                        }
                        if (ok && mq_json_len(jn) == vn) {
                            tgt.normals.reserve((size_t)vn * 3);
                            for (uint32_t i = 0; i < vn && ok; ++i) {
                                const mq_json* row = mq_json_at(jn, i);
                                if (mq_json_len(row) != 3) { ok = false; break; }
                                for (uint32_t k = 0; k < 3; ++k)
                                    tgt.normals.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                            }
                        }
                        if (ok) morph.targets.push_back(std::move(tgt));
                    }
                    if (morph.default_weights.size() < morph.targets.size())
                        morph.default_weights.resize(morph.targets.size(), 0.0F);
                }
            }
            if (ok && rapi && rself && rapi->create_mesh) {
                mq_renderer_mesh_gpu md{};
                md.vertices = vptr;
                md.vertex_bytes = vbytes;
                md.vertex_stride = stride;
                md.vertex_layout = vertex_layout;
                md.indices = idx.data();
                md.index_count = (uint32_t)idx.size();
                const mq_json* bmin = mq_json_get(dom, "bounds_min");
                const mq_json* bmax = mq_json_get(dom, "bounds_max");
                for (uint32_t k = 0; k < 3 && k < mq_json_len(bmin); ++k)
                    md.bounds_min[k] = (float)mq_json_num(mq_json_at(bmin, k), 0.0);
                for (uint32_t k = 0; k < 3 && k < mq_json_len(bmax); ++k)
                    md.bounds_max[k] = (float)mq_json_num(mq_json_at(bmax, k), 0.0);
                if (rapi->create_mesh(rself, &md, &id) != 0) id = -1;
                if (id >= 0) mesh_stats[id] = { vn, (uint32_t)idx.size() };
            }
            /* morph：保存打包后的基础顶点（CPU 重算起点），创建后立即应用默认权重 */
            if (id >= 0 && !morph.targets.empty()) {
                morph.mesh = id;
                morph.vertex_count = vn;
                morph.stride = stride;
                morph.layout = vertex_layout;
                morph.base = std::move(packed);
                if (morph.base.empty()) {
                    morph.base.resize((size_t)vn * stride);
                    std::memcpy(morph.base.data(), verts.data(), (size_t)vn * base_stride);
                }
                morph.applied_weights = morph.default_weights;
                const std::string mp_key = path;
                morph_meshes[mp_key] = std::move(morph);
                bool any_w = false;
                for (float w : morph_meshes[mp_key].default_weights) if (w != 0.0F) { any_w = true; break; }
                if (any_w) (void)apply_morph(mp_key, morph_meshes[mp_key].default_weights);
                if (render3d_info)
                    std::print("runtime: morph mesh '{}' targets={} vertices={} stride={}\n",
                               path, (int)morph_meshes[mp_key].targets.size(),
                               (long long)morph_meshes[mp_key].vertex_count,
                               (long long)morph_meshes[mp_key].stride);
            }
            if (id < 0) std::print(stderr, "runtime: mesh 载入失败 '{}'\n", path);
            else if (render3d_info) {
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::print("runtime: mesh '{}' vertices={} indices={} ({:.1f} ms)\n",
                           path, vn, in, ms);
            }
            mesh_layout[path] = (id >= 0) ? vertex_layout : -1;
            mq_json_free(dom);
        }
    }
    mesh_cache[path] = id;
    return id;
}

/* ---- 运行时 glTF/GLB 直接加载（plans/23/24）----
 * mesh_path 指向 .gltf/.glb 时不必先跑 Python importer：装配期解析 + 上传一次，之后每帧只读缓存。
 * 资源（buffer/image）的相对 URI 按**模型文件所在目录**经 VFS 解析（远程挂载同样适用）。 */
bool Engine::read_vfs_file(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    if (!vfs) return false;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != 0 || !data) return false;
    out.assign((const uint8_t*)data, (const uint8_t*)data + n);
    mq_vfs_free(data);
    return true;
}

int32_t Engine::upload_texture_rgba(const uint8_t* rgba, int w, int h, bool srgb,
                                    int filter, int wrap_u, int wrap_v) {
    if (!rapi || !rself || !rgba || w <= 0 || h <= 0) return -1;
    int32_t id = -1;
    if (rapi->create_texture2) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, w, h, levels, srgb);
        mq_renderer_texture_desc td{};
        td.width = w;
        td.height = h;
        td.pixel_format = srgb ? 1 : 0;       /* 1 = RGBA8_SRGB，0 = RGBA8（线性数据） */
        td.kind = 0;
        td.mip_levels = levels;
        td.filter = filter;
        td.wrap_u = wrap_u;
        td.wrap_v = wrap_v;
        td.data = mips.data();
        td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    if (id < 0 && rapi->create_texture) {
        id = rapi->create_texture(rself, rgba, (int32_t)w, (int32_t)h);
    }
    return id;
}

Engine::GltfGpuModel* Engine::load_gltf_model(const std::string& path) {
    auto cached = gltf_models.find(path);
    if (cached != gltf_models.end()) return &cached->second;
    GltfGpuModel gpu;
    auto fail = [&](const char* why) -> GltfGpuModel* {
        std::print(stderr, "runtime: gltf 加载失败 '{}': {}\n", path, why);
        gpu.failed = true;
        gltf_models[path] = std::move(gpu);
        return &gltf_models[path];
    };
    std::vector<uint8_t> bytes;
    if (!read_vfs_file(path, bytes))
        return fail("读不到文件（mesh_path 相对场景目录 / VFS 根）");
    const auto start = std::chrono::steady_clock::now();
    auto readfn = [this](const std::string& p, std::vector<uint8_t>& out) {
        return read_vfs_file(p, out);
    };
    gltf::Model cpu = gltf::parse(path, bytes, readfn);
    if (cpu.failed || (cpu.primitives.empty() && cpu.lights.empty() && cpu.cameras.empty()))
        return fail(cpu.error.empty() ? "没有可导入的三角网格/灯光/相机" : cpu.error.c_str());

    /* ---- 图像解码（同一 image 只解码一次）---- */
    struct Decoded {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
    };
    std::vector<Decoded> decoded(cpu.images.size());
    auto decode_image = [&](int idx) -> Decoded* {
        if (idx < 0 || (size_t)idx >= cpu.images.size()) return nullptr;
        Decoded& d = decoded[(size_t)idx];
        if (d.ok) return &d;
        if (d.w < 0) return nullptr;
        const gltf::Image& im = cpu.images[(size_t)idx];
        if (!im.valid || im.bytes.empty()) {
            d.w = -1;
            return nullptr;
        }
        uint8_t* rgba = nullptr;
        uint32_t w = 0, h = 0;
        char err[128] = { 0 };
        if (mq_image_decode(im.bytes.data(), im.bytes.size(), &rgba, &w, &h, err, sizeof err) != 0) {
            std::print(stderr, "runtime: gltf '{}' image[{}] 解码失败（{}）\n", path, idx, err);
            d.w = -1;
            return nullptr;
        }
        d.rgba.assign(rgba, rgba + (size_t)w * (size_t)h * 4u);
        d.w = (int)w;
        d.h = (int)h;
        d.ok = true;
        mq_image_free(rgba);
        return &d;
    };

    /* ---- 纹理上传（同一 image + sampler + sRGB 组合只上传一次）---- */
    std::unordered_map<uint64_t, int32_t> tex_map;
    auto tex_for = [&](const gltf::TextureRef& ref, bool srgb) -> int32_t {
        if (!ref.present) return -1;
        Decoded* d = decode_image(ref.image);
        if (!d) return -1;
        const uint64_t key = ((uint64_t)(uint32_t)ref.image << 32) |
                             ((uint64_t)(srgb ? 1u : 0u) << 20) |
                             ((uint64_t)(ref.sampler.filter & 3) << 16) |
                             ((uint64_t)(ref.sampler.wrap_u & 3) << 8) |
                             (uint64_t)(ref.sampler.wrap_v & 3);
        auto hit = tex_map.find(key);
        if (hit != tex_map.end()) return hit->second;
        const int32_t id = upload_texture_rgba(d->rgba.data(), d->w, d->h, srgb,
                                               ref.sampler.filter, ref.sampler.wrap_u,
                                               ref.sampler.wrap_v);
        tex_map[key] = id;
        if (id >= 0) gpu.textures.push_back(id);
        if (render3d_info) {
            const std::string& label = cpu.images[(size_t)ref.image].uri;
            std::print("runtime: gltf texture image={} '{}' srgb={} -> {}\n",
                       ref.image, label, srgb ? 1 : 0, id);
        }
        return id;
    };

    /* ---- 材质（核心 PBR + KHR 扩展；默认值与 extract_3d 组件路径一致）---- */
    auto defaults = [&]() {
        mq_renderer_pbr_material mat{};
        for (int ti = 0; ti < 5; ++ti) mat.textures[ti] = -1;
        for (int ti = 0; ti < 3; ++ti) mat.textures2[ti] = -1;
        for (int ti = 0; ti < 7; ++ti) mat.textures3[ti] = -1;
        for (int ti = 0; ti < 2; ++ti) {
            mat.textures4[ti] = -1;
            mat.textures6[ti] = -1;
        }
        mat.textures5[0] = -1;
        mat.textures7[0] = -1;
        for (int ti = 0; ti < 2; ++ti) {
            mat.uv_scale[ti] = 1.0F;
            mat.uv_offset[ti] = 0.0F;
            mat.normal_uv_scale[ti] = 1.0F;
            mat.normal_uv_offset[ti] = 0.0F;
        }
        mat.shading_model = 0;
        mat.side = 2;
        mat.opacity = 1.0F;
        mat.emissive_intensity = 1.0F;
        mat.env_intensity = 1.0F;
        mat.normal_scale_y = 1.0F;
        mat.shininess = 30.0F;
        mat.specular_intensity = 1.0F;
        mat.ior = 1.5F;
        mat.specular[0] = mat.specular[1] = mat.specular[2] = 1.0F;
        mat.sheen_roughness = 0.5F;
        mat.iridescence_ior = 1.3F;
        mat.iridescence_thickness_min = 100.0F;
        mat.iridescence_thickness_max = 400.0F;
        mat.clearcoat_normal_scale = 1.0F;
        mat.clearcoat_roughness = 0.03F;
        mat.toon_steps = 3;
        mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = mat.base_color[3] = 1.0F;
        mat.metallic = 0.0F;
        mat.roughness = 0.6F;
        mat.normal_scale = 1.0F;
        mat.occlusion_strength = 1.0F;
        mat.alpha_cutoff = 0.5F;
        return mat;
    };

    gpu.mats.reserve(cpu.materials.size());
    for (const gltf::Material& m : cpu.materials) {
        mq_renderer_pbr_material mat = defaults();
        for (int k = 0; k < 4; ++k) mat.base_color[k] = m.base_color[k];
        for (int k = 0; k < 3; ++k) mat.emissive[k] = m.emissive[k];
        mat.metallic = m.metallic;
        mat.roughness = m.roughness;
        mat.normal_scale = m.normal_scale;
        mat.occlusion_strength = m.occlusion_strength;
        mat.alpha_cutoff = m.alpha_cutoff;
        mat.alpha_mode = m.alpha_mode;
        mat.flags = m.double_sided ? 1 : 0;
        mat.side = m.double_sided ? 2 : 0;
        mat.shading_model = m.unlit ? 2 : 0;
        mat.emissive_intensity = m.emissive_strength;
        int32_t uv1_mask = 0;
        const int32_t t_base = tex_for(m.base_color_tex, true);
        const int32_t t_mr = tex_for(m.metallic_roughness_tex, false);
        const int32_t t_ao = tex_for(m.occlusion_tex, false);
        const int32_t t_nrm = tex_for(m.normal_tex, false);
        const int32_t t_em = tex_for(m.emissive_tex, true);
        if (t_base >= 0) mat.textures[0] = t_base;
        if (t_mr >= 0) mat.textures[1] = t_mr;
        if (t_ao >= 0) mat.textures[2] = t_ao;
        if (t_nrm >= 0) mat.textures[3] = t_nrm;
        if (t_em >= 0) mat.textures[4] = t_em;
        if (m.base_color_tex.present && m.base_color_tex.texcoord > 0) uv1_mask |= 1 << 0;
        if (m.metallic_roughness_tex.present && m.metallic_roughness_tex.texcoord > 0) uv1_mask |= 1 << 1;
        if (m.occlusion_tex.present && m.occlusion_tex.texcoord > 0) uv1_mask |= 1 << 2;
        if (m.normal_tex.present && m.normal_tex.texcoord > 0) uv1_mask |= 1 << 3;
        if (m.emissive_tex.present && m.emissive_tex.texcoord > 0) uv1_mask |= 1 << 4;
        mat.flags2 |= (uv1_mask & 0xFF) << 4;
        if (m.base_color_tex.has_transform) {
            for (int k = 0; k < 2; ++k) {
                mat.uv_scale[k] = m.base_color_tex.scale[k];
                mat.uv_offset[k] = m.base_color_tex.offset[k];
            }
            mat.uv_rotation = m.base_color_tex.rotation;
        }
        if (m.normal_tex.has_transform) {
            for (int k = 0; k < 2; ++k) {
                mat.normal_uv_scale[k] = m.normal_tex.scale[k];
                mat.normal_uv_offset[k] = m.normal_tex.offset[k];
            }
            mat.normal_uv_rotation = m.normal_tex.rotation;
        }
        /* P4 KHR 材质扩展。 */
        mat.shading_model = m.unlit ? 2 : (m.physical ? 1 : 0);
        mat.opacity = m.opacity;
        mat.clearcoat = m.clearcoat;
        mat.clearcoat_roughness = m.clearcoat_roughness > 0.0F ? m.clearcoat_roughness : 0.03F;
        mat.sheen = m.sheen;
        mat.sheen_roughness = m.sheen > 0.0F ? m.sheen_roughness : 0.5F;
        for (int k = 0; k < 3; ++k) mat.sheen_color[k] = m.sheen_color[k];
        mat.iridescence = m.iridescence;
        mat.iridescence_ior = m.iridescence_ior;
        mat.iridescence_thickness_min = m.iridescence_thickness_min;
        mat.iridescence_thickness_max = m.iridescence_thickness_max;
        mat.specular_intensity = m.specular_intensity;
        for (int k = 0; k < 3; ++k) mat.specular[k] = m.specular_color[k];
        mat.ior = m.ior;
        mat.anisotropy = m.anisotropy;
        mat.anisotropy_rotation = m.anisotropy_rotation;
        mat.volume_thickness = m.volume_thickness;
        mat.attenuation_distance = m.attenuation_distance;
        for (int k = 0; k < 3; ++k) {
            mat.attenuation_color[k] = m.attenuation_color[k];
            mat.diffuse_transmission_color[k] = m.diffuse_transmission_color[k];
            mat.multiscatter_color[k] = m.multiscatter_color[k];
        }
        mat.dispersion = m.dispersion;
        if (m.alpha_mask_in_blend) mat.flags2 |= 0x80000;
        if (m.transmission > 0.001F) {
            mat.anisotropy_rotation = m.transmission;
            mat.flags2 |= 0x100000;
        }
        if (m.diffuse_transmission > 0.001F) {
            mat.diffuse_transmission_factor = m.diffuse_transmission;
            mat.flags2 |= 0x1000000;
        }
        auto tex3 = [&](const gltf::TextureRef& ref, int slot, bool srgb) {
            const int tid = tex_for(ref, srgb);
            if (tid >= 0) {
                mat.textures3[slot] = tid;
                mat.flags2 |= 1 << (12 + slot);
            }
        };
        tex3(m.clearcoat_tex, 0, false);
        tex3(m.clearcoat_roughness_tex, 1, false);
        tex3(m.sheen_color_tex, 2, true);
        tex3(m.sheen_roughness_tex, 3, false);
        tex3(m.iridescence_tex, 4, false);
        tex3(m.iridescence_thickness_tex, 5, false);
        tex3(m.anisotropy_tex, 6, false);
        {
            const int tid = tex_for(m.clearcoat_normal_tex, false);
            if (tid >= 0) { mat.textures2[2] = tid; mat.flags2 |= 8; }
        }
        {
            const int tid = tex_for(m.specular_tex, false);
            if (tid >= 0) { mat.textures4[0] = tid; mat.flags2 |= 1 << 21; }
            const int tid2 = tex_for(m.specular_color_tex, true);
            if (tid2 >= 0) { mat.textures4[1] = tid2; mat.flags2 |= 1 << 22; }
        }
        {
            const int tid = tex_for(m.transmission_tex, false);
            if (tid >= 0) { mat.textures5[0] = tid; mat.flags2 |= 0x800000; }
        }
        {
            const int tid = tex_for(m.diffuse_transmission_tex, false);
            if (tid >= 0) { mat.textures6[0] = tid; mat.flags2 |= 1 << 25; }
            const int tid2 = tex_for(m.diffuse_transmission_color_tex, true);
            if (tid2 >= 0) { mat.textures6[1] = tid2; mat.flags2 |= 1 << 26; }
        }
        {
            const int tid = tex_for(m.volume_thickness_tex, false);
            if (tid >= 0) { mat.textures7[0] = tid; mat.flags2 |= 0x8000000; }
        }
        if (render3d_info)
            std::print("runtime: gltf material[{}] '{}' shading={} clearcoat={:.3f} sheen={:.3f} "
                       "iridescence={:.3f} specular={:.3f} ior={:.3f} anisotropy={:.3f} "
                       "transmission={:.3f} volume={:.3f} diffuse_transmission={:.3f} tex3={} "
                       "tex4={}/{} tex5={} tex6={}/{} tex7={}\n",
                       gpu.mats.size(), m.name, mat.shading_model, (double)mat.clearcoat,
                       (double)mat.sheen, (double)mat.iridescence, (double)mat.specular_intensity,
                       (double)mat.ior, (double)mat.anisotropy, (double)m.transmission,
                       (double)mat.volume_thickness, (double)mat.diffuse_transmission_factor,
                       mat.textures3[0] >= 0 || mat.textures3[1] >= 0 || mat.textures3[2] >= 0 ||
                               mat.textures3[3] >= 0 || mat.textures3[4] >= 0 ||
                               mat.textures3[5] >= 0 || mat.textures3[6] >= 0,
                       mat.textures4[0], mat.textures4[1], mat.textures5[0], mat.textures6[0],
                       mat.textures6[1], mat.textures7[0]);
        gpu.mats.push_back(mat);
    }

    /* ---- 节点 / 蒙皮 / 动画 / 灯光 / 相机 ---- */
    gpu.nodes.reserve(cpu.nodes.size());
    for (const gltf::Node& n : cpu.nodes) {
        GltfGpuModel::Node out;
        out.parent = n.parent;
        for (int k = 0; k < 3; ++k) { out.t[k] = n.translation[k]; out.s[k] = n.scale[k]; }
        for (int k = 0; k < 4; ++k) out.r[k] = n.rotation[k];
        out.has_matrix = n.has_matrix;
        std::memcpy(out.m, n.matrix, sizeof out.m);
        gpu.nodes.push_back(out);
    }
    gpu.order = cpu.node_order;
    gpu.skins.reserve(cpu.skins.size());
    for (const gltf::Skin& s : cpu.skins) {
        GltfGpuModel::Skin out;
        out.joints = s.joints;
        out.inverse_bind = s.inverse_bind;
        gpu.skins.push_back(std::move(out));
    }
    gpu.anims.reserve(cpu.animations.size());
    for (const gltf::Animation& a : cpu.animations) {
        GltfGpuModel::Anim out;
        out.name = a.name;
        out.duration = a.duration;
        out.channels.reserve(a.channels.size());
        for (const gltf::AnimationChannel& c : a.channels) {
            GltfGpuModel::AnimChannel oc;
            oc.node = c.node;
            oc.path = c.path;
            oc.interp = c.interp;
            oc.comps = c.comps;
            oc.pointer_material = c.pointer_material;
            oc.pointer_field = c.pointer_field;
            oc.times = c.times;
            oc.values = c.values;
            out.channels.push_back(std::move(oc));
        }
        gpu.anims.push_back(std::move(out));
    }
    gpu.lights.reserve(cpu.lights.size());
    for (const gltf::Light& L : cpu.lights) {
        GltfGpuModel::Light out;
        out.node = L.node;
        out.type = L.type;
        for (int k = 0; k < 3; ++k) out.color[k] = L.color[k];
        out.intensity = L.intensity;
        out.range = L.range;
        const float inner = std::clamp(L.inner_angle, 0.0F, 1.5707963F);
        const float outer = std::clamp(L.outer_angle, inner, 1.5707963F);
        out.inner_cos = std::cos(inner);
        out.outer_cos = std::cos(outer);
        gpu.lights.push_back(out);
    }
    gpu.cameras.reserve(cpu.cameras.size());
    for (const gltf::Camera& C : cpu.cameras) {
        GltfGpuModel::Camera out;
        out.node = C.node;
        out.perspective = C.perspective;
        out.yfov = C.yfov;
        out.znear = C.znear;
        out.zfar = C.zfar;
        gpu.cameras.push_back(out);
    }
    gpu.variants = cpu.variants;
    {
        float radius = 0.0F;
        for (const gltf::Primitive& p : cpu.primitives) {
            for (int ci = 0; ci < 8; ++ci) {
                const float x = (ci & 1) ? p.bounds_max[0] : p.bounds_min[0];
                const float y = (ci & 2) ? p.bounds_max[1] : p.bounds_min[1];
                const float z = (ci & 4) ? p.bounds_max[2] : p.bounds_min[2];
                radius = std::max(radius, std::sqrt(x * x + y * y + z * z));
            }
        }
        gpu.radius = std::max(radius, 1e-3F);
    }
    if (render3d_info) {
        for (size_t i = 0; i < gpu.skins.size(); ++i)
            std::print("runtime: gltf skin '{}'[{}] joints={}\n",
                       path, i, (long long)gpu.skins[i].joints.size());
        for (const auto& a : gpu.anims)
            std::print("runtime: gltf anim '{}' channels={} duration={:.3f}\n",
                       a.name, (long long)a.channels.size(), (double)a.duration);
        for (size_t i = 0; i < gpu.lights.size(); ++i)
            std::print("runtime: gltf light[{}] node={} type={} intensity={:.3f} range={:.3f} "
                       "spot_cos={:.3f}/{:.3f}\n",
                       i, gpu.lights[i].node, gpu.lights[i].type,
                       (double)gpu.lights[i].intensity, (double)gpu.lights[i].range,
                       (double)gpu.lights[i].inner_cos, (double)gpu.lights[i].outer_cos);
        for (size_t i = 0; i < gpu.cameras.size(); ++i)
            std::print("runtime: gltf camera[{}] node={} perspective={} yfov={:.3f} z=[{:.3f},{:.3f}]\n",
                       i, gpu.cameras[i].node, gpu.cameras[i].perspective ? 1 : 0,
                       (double)gpu.cameras[i].yfov, (double)gpu.cameras[i].znear,
                       (double)gpu.cameras[i].zfar);
        std::print("runtime: gltf lights={} cameras={} radius={:.3f} variants={}\n",
                   (long long)gpu.lights.size(), (long long)gpu.cameras.size(),
                   (double)gpu.radius, (long long)gpu.variants.size());
    }

    /* ---- 网格上传：无蒙皮 48/56B；蒙皮再追加 joints u16x4 + weights f32x4（stride 72/80）---- */
    struct PendingMorph { size_t prim = 0; };
    std::vector<PendingMorph> pending_morph;
    gpu.prims.reserve(cpu.primitives.size());
    for (const gltf::Primitive& p : cpu.primitives) {
        if (!rapi || !rself || !rapi->create_mesh) break;
        if (p.vertices.empty() || p.indices.empty()) continue;
        const bool skinned = p.skin >= 0 && !p.joints.empty() && !p.weights.empty();
        const uint32_t row_floats = (p.vertex_layout == 1) ? 14u : 12u;
        const uint32_t base_stride = (p.vertex_layout == 1) ? 56u : 48u;
        if (p.vertices.size() % row_floats != 0) {
            std::print(stderr, "runtime: gltf '{}' primitive 顶点行宽不符（layout={}）→ 跳过\n",
                       path, p.vertex_layout);
            continue;
        }
        const uint32_t vn = (uint32_t)(p.vertices.size() / row_floats);
        const uint32_t stride = base_stride + (skinned ? 24u : 0u);
        const int layout = p.vertex_layout + (skinned ? 2 : 0);
        std::vector<uint8_t> packed;
        const void* vptr = p.vertices.data();
        uint64_t vbytes = (uint64_t)p.vertices.size() * sizeof(float);
        if (skinned) {
            packed.resize((size_t)vn * stride);
            for (uint32_t i = 0; i < vn; ++i) {
                uint8_t* dst = packed.data() + (size_t)i * stride;
                std::memcpy(dst, p.vertices.data() + (size_t)i * row_floats, base_stride);
                uint16_t* jdst = (uint16_t*)(dst + base_stride);
                const float* wdst = (const float*)(dst + base_stride + 8);
                for (int k = 0; k < 4; ++k)
                    jdst[k] = (uint16_t)p.joints[(size_t)i * 4 + (size_t)k];
                std::memcpy((void*)wdst, p.weights.data() + (size_t)i * 4, 16);
            }
            vptr = packed.data();
            vbytes = packed.size();
        }
        mq_renderer_mesh_gpu md{};
        md.vertices = vptr;
        md.vertex_bytes = vbytes;
        md.vertex_stride = stride;
        md.vertex_layout = layout;
        md.indices = p.indices.data();
        md.index_count = (uint32_t)p.indices.size();
        for (int k = 0; k < 3; ++k) {
            md.bounds_min[k] = p.bounds_min[k];
            md.bounds_max[k] = p.bounds_max[k];
        }
        int32_t mesh_id = -1;
        if (rapi->create_mesh(rself, &md, &mesh_id) != 0 || mesh_id < 0) {
            std::print(stderr, "runtime: gltf '{}' primitive 上传失败（node={}）\n", path, p.node);
            continue;
        }
        mesh_stats[mesh_id] = { vn, (uint32_t)p.indices.size() };
        GltfGpuModel::Prim pr;
        pr.mesh = mesh_id;
        pr.material = p.material;
        pr.node = p.node;
        pr.skin = skinned ? p.skin : -1;
        pr.layout = layout;
        pr.vertex_count = vn;
        pr.stride = stride;
        pr.default_weights = p.morph_weights;
        pr.applied_weights = p.morph_weights;
        pr.variant_materials = p.variant_materials;
        pr.morph_targets.reserve(p.morph_targets.size());
        for (const gltf::MorphTarget& mt : p.morph_targets) {
            GltfGpuModel::MorphTarget out;
            out.positions = mt.positions;
            out.normals = mt.normals;
            pr.morph_targets.push_back(std::move(out));
        }
        if (!pr.morph_targets.empty()) {
            pr.base.resize((size_t)vn * stride);
            std::memcpy(pr.base.data(), vptr, pr.base.size());
            pending_morph.push_back(PendingMorph{ gpu.prims.size() });
        }
        gpu.prims.push_back(std::move(pr));
    }
    if (gpu.prims.empty() && gpu.lights.empty() && gpu.cameras.empty())
        return fail("没有可上传的网格/灯光/相机");

    bool need_default = false;
    for (const auto& pr : gpu.prims)
        if (pr.material < 0 || (size_t)pr.material >= gpu.mats.size()) need_default = true;
    if (need_default) {
        gpu.mats.push_back(defaults());
        const int32_t def = (int32_t)gpu.mats.size() - 1;
        for (auto& pr : gpu.prims)
            if (pr.material < 0 || (size_t)pr.material >= gpu.mats.size()) pr.material = def;
    }
    if (morph_enabled) {
        for (const PendingMorph& pm : pending_morph) {
            GltfGpuModel::Prim& pr = gpu.prims[pm.prim];
            bool any = false;
            for (float w : pr.default_weights) if (w != 0.0F) { any = true; break; }
            if (any) (void)apply_gltf_morph(-1, gpu, pm.prim, pr.default_weights);
        }
    }

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start).count();
    if (render3d_info) {
        size_t n_morph = 0, n_targets = 0;
        for (const auto& pr : gpu.prims) {
            if (!pr.morph_targets.empty()) { ++n_morph; n_targets += pr.morph_targets.size(); }
        }
        std::print("runtime: gltf model '{}' primitives={} materials={} textures={} nodes={} "
                   "skins={} anims={} morph_prims={} morph_targets={} ({:.1f} ms)\n",
                   path, (long long)gpu.prims.size(), (long long)gpu.mats.size(),
                   (long long)gpu.textures.size(), (long long)gpu.nodes.size(),
                   (long long)gpu.skins.size(), (long long)gpu.anims.size(),
                   (long long)n_morph, (long long)n_targets, ms);
        if (n_morph > 0)
            std::print("runtime: gltf morph '{}' primitives={} targets={}\n",
                       path, (long long)n_morph, (long long)n_targets);
    }
    gltf_models[path] = std::move(gpu);
    return &gltf_models[path];
}

/* ---- glTF 片段 / 蒙皮 / morph 解析与采样（plans/22 §6）----
 * 资产是派生文本 JSON（.skel.json / .clip.json），按 VFS 路径缓存；失败也缓存（failed=true），
 * 成功/失败都只告警一次。装配期预解析（setup_gltf_assets）→ 模拟帧内只读缓存。 */

/* 读组件里的 string[] 字段（反射 container 算子；用于 scn.AnimationPlayer.clips）。 */
static std::vector<std::string> read_string_array(const mq_field_desc* f, void* field) {
    std::vector<std::string> out;
    if (!f || f->kind != MQ_TK_ARRAY_DYN || !f->container_size || !f->container_elem) return out;
    const uint32_t n = f->container_size(field);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const std::string* s = (const std::string*)f->container_elem(field, i);
        if (s && !s->empty()) out.push_back(*s);
    }
    return out;
}

bool Engine::load_skeleton(const std::string& path) {
    auto it = skeletons.find(path);
    if (it != skeletons.end()) return !it->second.failed;
    Skeleton sk;
    sk.name = path;
    sk.failed = true;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data) {
        std::print(stderr, "runtime: skeleton 未找到 '{}'\n", path);
        skeletons[path] = std::move(sk);
        return false;
    }
    mq_json* dom = mq_json_parse((const char*)data, n);
    mq_vfs_free(data);
    if (!dom) {
        std::print(stderr, "runtime: skeleton json 解析失败 '{}'\n", path);
        skeletons[path] = std::move(sk);
        return false;
    }
    const mq_json* jn = mq_json_get(dom, "joint_names");
    const mq_json* jb = mq_json_get(dom, "inverse_bind");
    const uint32_t nj = mq_json_len(jn);
    const uint32_t nb = mq_json_len(jb);
    bool ok = nj > 0 && nj == nb;
    if (!ok)
        std::print(stderr, "runtime: skeleton '{}' 无效（joint_names={} inverse_bind={}）\n", path, nj, nb);
    if (ok) {
        sk.joint_names.reserve(nj);
        for (uint32_t i = 0; i < nj; ++i) {
            const char* nm = mq_json_str(mq_json_at(jn, i), "");
            sk.joint_names.emplace_back(nm ? nm : "");
        }
        sk.inverse_bind.resize((size_t)nj * 16);
        for (uint32_t i = 0; i < nj && ok; ++i) {
            const mq_json* row = mq_json_at(jb, i);
            if (mq_json_len(row) != 16) { ok = false; break; }
            for (uint32_t k = 0; k < 16; ++k)
                sk.inverse_bind[(size_t)i * 16 + k] = (float)mq_json_num(mq_json_at(row, k), 0.0);
        }
        if (!ok) sk.inverse_bind.clear();
    }
    const char* nm = mq_json_str(mq_json_get(dom, "name"), nullptr);
    if (nm && *nm) sk.name = nm;
    mq_json_free(dom);
    if (!ok) {
        skeletons[path] = std::move(sk);
        return false;
    }
    sk.failed = false;
    const size_t joints = sk.joint_names.size();
    skeletons[path] = std::move(sk);
    if (render3d_info)
        std::print("runtime: skeleton '{}' joints={}\n", path, (long long)joints);
    return true;
}

bool Engine::load_gclip(const std::string& path) {
    auto it = gclips.find(path);
    if (it != gclips.end()) return !it->second.failed;
    GClip clip;
    clip.name = path;
    clip.failed = true;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data) {
        std::print(stderr, "runtime: clip 未找到 '{}'\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    mq_json* dom = mq_json_parse((const char*)data, n);
    mq_vfs_free(data);
    if (!dom) {
        std::print(stderr, "runtime: clip json 解析失败 '{}'\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    clip.duration = (float)mq_json_num(mq_json_get(dom, "duration"), 0.0);
    const mq_json* jtracks = mq_json_get(dom, "tracks");
    const uint32_t nt = mq_json_len(jtracks);
    float max_time = 0.0F;
    for (uint32_t ti = 0; ti < nt; ++ti) {
        const mq_json* jt = mq_json_at(jtracks, ti);
        const char* node = mq_json_str(mq_json_get(jt, "node"), "");
        const char* pathname = mq_json_str(mq_json_get(jt, "path"), "");
        if (!node || !*node || !pathname) continue;
        int tp = -1;
        if (std::strcmp(pathname, "translation") == 0) tp = 0;
        else if (std::strcmp(pathname, "rotation") == 0) tp = 1;
        else if (std::strcmp(pathname, "scale") == 0) tp = 2;
        else if (std::strcmp(pathname, "weights") == 0) tp = 3;
        if (tp < 0) continue;                          /* 未知通道（pointer 等）跳过 */
        GClipTrack tr;
        tr.node = node;
        tr.path = tp;
        const char* interp = mq_json_str(mq_json_get(jt, "interpolation"), "LINEAR");
        tr.step = interp && std::strcmp(interp, "STEP") == 0;
        const mq_json* jtimes = mq_json_get(jt, "times");
        const mq_json* jvals = mq_json_get(jt, "values");
        const uint32_t nk = mq_json_len(jtimes);
        const uint32_t nv = mq_json_len(jvals);
        if (nk == 0 || nk != nv) {                     /* 关键帧/值数量不一致：跳过该轨道 */
            std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 关键帧数不符（{} vs {}），跳过\n",
                       path, tr.node, pathname, nk, nv);
            continue;
        }
        const uint32_t comps = mq_json_len(mq_json_at(jvals, 0));
        if (comps == 0 || (tp == 1 && comps != 4) ||
            ((tp == 0 || tp == 2) && comps != 3)) {
            std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 分量数 {} 不符，跳过\n",
                       path, tr.node, pathname, comps);
            continue;
        }
        tr.comps = comps;
        tr.times.resize(nk);
        tr.values.resize((size_t)nk * comps);
        bool ok = true;
        for (uint32_t k = 0; k < nk && ok; ++k) {
            tr.times[k] = (float)mq_json_num(mq_json_at(jtimes, k), 0.0);
            const mq_json* row = mq_json_at(jvals, k);
            if (mq_json_len(row) != comps) { ok = false; break; }
            for (uint32_t c = 0; c < comps; ++c)
                tr.values[(size_t)k * comps + c] = (float)mq_json_num(mq_json_at(row, c), 0.0);
        }
        if (!ok) { std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 值行宽不符，跳过\n", path, tr.node, pathname); continue; }
        if (!tr.times.empty() && tr.times.back() > max_time) max_time = tr.times.back();
        clip.tracks.push_back(std::move(tr));
    }
    const char* nm = mq_json_str(mq_json_get(dom, "name"), nullptr);
    if (nm && *nm) clip.name = nm;
    mq_json_free(dom);
    if (clip.tracks.empty()) {
        std::print(stderr, "runtime: clip '{}' 无可采样轨道\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    if (clip.duration <= 0.0F) clip.duration = max_time;   /* 缺省由关键帧推导 */
    clip.failed = false;
    const int ntracks = (int)clip.tracks.size();
    const float dur = clip.duration;
    gclips[path] = std::move(clip);
    if (render3d_info)
        std::print("runtime: clip '{}' tracks={} duration={:.3f}\n", path, ntracks, (double)dur);
    return true;
}

/* 采样一条轨道：STEP 取前关键帧；LINEAR 线性（rotation 用四元数 slerp）。 */
static void sample_gclip_track(const GClipTrack& tr, float time, std::vector<float>& out) {
    out.assign(tr.comps, 0.0F);
    const uint32_t nk = (uint32_t)tr.times.size();
    if (nk == 0 || tr.comps == 0) return;
    if (time <= tr.times[0]) {
        std::memcpy(out.data(), tr.values.data(), sizeof(float) * tr.comps);
        return;
    }
    if (time >= tr.times[nk - 1]) {
        std::memcpy(out.data(), tr.values.data() + (size_t)(nk - 1) * tr.comps,
                    sizeof(float) * tr.comps);
        return;
    }
    uint32_t k = 0;
    while (k + 1 < nk && tr.times[k + 1] <= time) ++k;
    const float* a = tr.values.data() + (size_t)k * tr.comps;
    if (tr.step) {
        std::memcpy(out.data(), a, sizeof(float) * tr.comps);
        return;
    }
    const float* b = tr.values.data() + (size_t)(k + 1) * tr.comps;
    const float span = tr.times[k + 1] - tr.times[k];
    float alpha = span > 0.0F ? (time - tr.times[k]) / span : 0.0F;
    if (alpha < 0.0F) alpha = 0.0F;
    if (alpha > 1.0F) alpha = 1.0F;
    if (tr.path == 1 && tr.comps == 4) {
        /* quat：dot<0 先翻转；dot>0.9995 用 nlerp（近似共线避免除零）；否则 slerp */
        float qb[4] = { b[0], b[1], b[2], b[3] };
        float dot = a[0] * qb[0] + a[1] * qb[1] + a[2] * qb[2] + a[3] * qb[3];
        if (dot < 0.0F) { dot = -dot; for (int i = 0; i < 4; ++i) qb[i] = -qb[i]; }
        float q[4];
        if (dot > 0.9995F) {
            for (int i = 0; i < 4; ++i) q[i] = a[i] + (qb[i] - a[i]) * alpha;
        } else {
            const float theta = std::acos(std::min(1.0F, dot));
            const float s = std::sin(theta);
            const float wa = std::sin((1.0F - alpha) * theta) / s;
            const float wb = std::sin(alpha * theta) / s;
            for (int i = 0; i < 4; ++i) q[i] = a[i] * wa + qb[i] * wb;
        }
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (ql > 0.0F) { const float inv = 1.0F / ql; for (int i = 0; i < 4; ++i) q[i] *= inv; }
        std::memcpy(out.data(), q, sizeof q);
        return;
    }
    for (uint32_t c = 0; c < tr.comps; ++c) out[c] = a[c] + (b[c] - a[c]) * alpha;
}

/* 按权重 CPU 重算 morph 顶点并上传（rapi->update_mesh）。权重没变不应调用（见 extract_3d）。 */
bool Engine::apply_morph(const std::string& path, const std::vector<float>& weights) {
    auto it = morph_meshes.find(path);
    if (it == morph_meshes.end()) return false;
    MorphMesh& mm = it->second;
    if (mm.mesh < 0 || mm.targets.empty() || mm.base.empty()) return false;
    if (!rapi || !rself || !rapi->update_mesh) {
        if (!mm.warned_no_update) {
            mm.warned_no_update = true;
            std::print(stderr, "runtime: 渲染器不支持 update_mesh，morph 跳过 '{}'\n", path);
        }
        return false;
    }
    std::vector<uint8_t> out = mm.base;
    for (uint32_t vi = 0; vi < mm.vertex_count; ++vi) {
        float* pos = (float*)(out.data() + (size_t)vi * mm.stride);
        float* nrm = pos + 3;
        bool normal_touched = false;
        for (size_t ti = 0; ti < mm.targets.size(); ++ti) {
            const float w = ti < weights.size() ? weights[ti] : 0.0F;
            if (w == 0.0F) continue;
            const MorphTarget& tgt = mm.targets[ti];
            const size_t o = (size_t)vi * 3;
            if (o + 2 < tgt.positions.size()) {
                pos[0] += w * tgt.positions[o];
                pos[1] += w * tgt.positions[o + 1];
                pos[2] += w * tgt.positions[o + 2];
            }
            if (o + 2 < tgt.normals.size()) {
                nrm[0] += w * tgt.normals[o];
                nrm[1] += w * tgt.normals[o + 1];
                nrm[2] += w * tgt.normals[o + 2];
                normal_touched = true;
            }
        }
        if (normal_touched) {
            const float len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            if (len2 > 1.0e-12F) {
                const float inv = 1.0F / std::sqrt(len2);
                nrm[0] *= inv; nrm[1] *= inv; nrm[2] *= inv;
            }
        }
    }
    if (rapi->update_mesh(rself, mm.mesh, out.data(), (uint64_t)out.size()) != 0) {
        if (!mm.warned_no_update) {
            mm.warned_no_update = true;
            std::print(stderr, "runtime: update_mesh 失败，morph 跳过 '{}'\n", path);
        }
        return false;
    }
    mm.applied_weights = weights;
    return true;
}

/* 装配期预解析 glTF 运行时资源（plans/22 §6.1）：实体名表 → 骨架/片段/morph 网格。
 * 装配期做 IO 合法（不在模拟帧内，docs/10 D7）；失败只告警一次（缓存 failed）。 */
void Engine::setup_gltf_assets() {
    read_gltf_env();
    entity_by_name.clear();
    entity_morph_mesh.clear();
    if (!scene) return;
    std::vector<int> ids;
    ids.reserve(scene->ents.size());
    for (auto& [k, e] : scene->ents) ids.push_back(k);
    std::sort(ids.begin(), ids.end());
    /* 名字表：唯一名 → 最小 id（重名只告警一次；动画轨道/骨架关节按名字引用实体） */
    int dup = 0;
    std::string dup_example;
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e || e->name.empty()) continue;
        auto [nit, inserted] = entity_by_name.emplace(e->name, id);
        (void)nit;
        if (!inserted) { ++dup; if (dup_example.empty()) dup_example = e->name; }
    }
    if (dup > 0)
        std::print(stderr, "runtime: 实体重名 {} 处（例如 '{}'）；按名字解析取最小 id\n",
                   dup, dup_example);
    int n_skin = 0, n_clip = 0, n_morph = 0, n_gltf = 0;
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* dmesh = nullptr;
        if (auto* mb = comp_by_name(*this, id, "render.MeshRenderer", &dmesh)) {
            const std::string* mp = read_str(dmesh, mb->data(), "mesh_path");
            if (mp && !mp->empty()) {
                if (gltf::is_gltf_path(*mp)) {
                    if (auto* gm = load_gltf_model(*mp); gm && !gm->failed) {
                        gltf_state_ensure(id, *mp, *gm);
                        ++n_gltf;
                    }
                } else {
                    (void)load_mesh(*mp);                     /* 缓存；失败只告警一次 */
                    auto mit = morph_meshes.find(*mp);
                    if (mit != morph_meshes.end() && mit->second.mesh >= 0 && !mit->second.targets.empty()) {
                        entity_morph_mesh[id] = *mp;
                        ++n_morph;
                    }
                }
            }
            const std::string* sp = read_str(dmesh, mb->data(), "skin_path");
            if (sp && !sp->empty()) {
                ++n_skin;
                (void)load_skeleton(*sp);
            }
        }
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_ANIM_PLAYER)) continue;
            const mq_field_desc* fc = mq_reflect_field(d, "clips");
            if (!fc) continue;
            for (const std::string& cp : read_string_array(fc, blob.data() + fc->offset)) {
                ++n_clip;
                (void)load_gclip(cp);
            }
        }
    }
    if (!gltf_states.empty()) gltf_anim_step(0.0F);
    if (render3d_info || n_skin > 0 || n_clip > 0 || n_morph > 0 || n_gltf > 0)
        std::print("runtime: gltf assets skins={} clips={} morph_meshes={} runtime_gltf={} names={}\n",
                   n_skin, n_clip, n_morph, n_gltf, (long long)entity_by_name.size());
}

void Engine::read_gltf_env() {
    if (gltf_env_read) return;
    gltf_env_read = true;
    auto flag = [](const char* name, bool def) {
        const char* v = std::getenv(name);
        if (!v || !*v) return def;
        return !(v[0] == '0' || v[0] == 'o' || v[0] == 'O' ||
                 v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
    };
    anim_enabled = flag("MAQUE_ANIM", true);
    skin_enabled = flag("MAQUE_SKIN", true);
    morph_enabled = flag("MAQUE_MORPH", true);
    render3d_info = std::getenv("MAQUE_RENDER3D_INFO") != nullptr;
}

/* glTF 片段采样（plans/22 §6.2）：仅处理 clips 非空的 scn.AnimationPlayer（旧 clip 通路不变）。
 * 推进播放头 → 采样（纯函数）→ 写 Engine::anim_pose（pos/rot/scale）与 morph 权重覆盖。 */
void Engine::anim_step_gclip(int entity, const mq_type_desc* d, std::vector<uint8_t>& blob, float dt) {
    const mq_field_desc* fc = mq_reflect_field(d, "clips");
    if (!fc) return;
    std::vector<std::string> paths = read_string_array(fc, blob.data() + fc->offset);
    if (paths.empty()) return;
    const int32_t ci = (int32_t)read_num(d, blob.data(), "clip_index", 0.0F);
    const std::string& clip_path = paths[(ci >= 0 && (size_t)ci < paths.size()) ? (size_t)ci : 0];
    if (!load_gclip(clip_path)) return;
    auto cit = gclips.find(clip_path);
    if (cit == gclips.end() || cit->second.failed || cit->second.tracks.empty()) return;
    const GClip& clip = cit->second;
    bool playing = true;
    if (const mq_field_desc* f = mq_reflect_field(d, "playing"))
        playing = *(const bool*)(blob.data() + f->offset);
    bool loop = true;
    if (const mq_field_desc* f = mq_reflect_field(d, "loop"))
        loop = *(const bool*)(blob.data() + f->offset);
    float speed = 1.0F;
    read_f32(d, blob.data(), "speed", &speed);
    float time = 0.0F;
    read_f32(d, blob.data(), "time", &time);
    if (playing) time += dt * speed;
    if (clip.duration > 0.0F) {
        if (loop) {
            /* 取模（fmod 而非反复相减：退化片段 duration 极小时不会卡住；确定性纯函数） */
            time = std::fmod(time, clip.duration);
            if (time < 0.0F) time += clip.duration;
        } else if (time > clip.duration) {
            time = clip.duration;
        } else if (time < 0.0F) {
            time = 0.0F;
        }
    }
    if (const mq_field_desc* f = mq_reflect_field(d, "time"))
        *(float*)(blob.data() + f->offset) = time;
    if (const mq_field_desc* f = mq_reflect_field(d, "duration"))
        *(float*)(blob.data() + f->offset) = clip.duration;
    if (!anim_enabled) return;                    /* MAQUE_ANIM=0：保持 bind pose（不写派生姿态） */
    std::vector<float> v;
    for (const GClipTrack& tr : clip.tracks) {
        auto hit = entity_by_name.find(tr.node);
        if (hit == entity_by_name.end()) continue;
        const int te = hit->second;
        sample_gclip_track(tr, time, v);
        if (tr.path == 0 && v.size() >= 3) {
            AnimPose& p = anim_pose[te];
            p.has_pos = true;
            p.pos = math::Vec3{ v[0], v[1], v[2] };
        } else if (tr.path == 1 && v.size() >= 4) {
            AnimPose& p = anim_pose[te];
            p.has_rot = true;
            std::memcpy(p.rot, v.data(), sizeof p.rot);
        } else if (tr.path == 2 && v.size() >= 3) {
            AnimPose& p = anim_pose[te];
            p.has_scale = true;
            p.scale = math::Vec3{ v[0], v[1], v[2] };
        } else if (tr.path == 3) {
            /* weights：写到该节点（含整棵子树）下所有 morph mesh 实体的权重覆盖 */
            std::vector<int> stack2{ te };
            while (!stack2.empty()) {
                const int cur = stack2.back();
                stack2.pop_back();
                if (entity_morph_mesh.find(cur) != entity_morph_mesh.end())
                    morph_weight_override[cur] = v;
                std::vector<int> kids;
                for (auto& [k2, c2] : scene->ents) if (c2.parent == cur) kids.push_back(k2);
                std::sort(kids.begin(), kids.end());
                for (int k2 : kids) stack2.push_back(k2);
            }
        }
    }
    if (render3d_info && !gltf_anim_info) {
        gltf_anim_info = true;
        std::print("runtime: anim entity={} clip='{}' tracks={} duration={:.3f} time={:.3f} pose={}\n",
                   entity, clip_path, (int)clip.tracks.size(), (double)clip.duration,
                   (double)time, (int)anim_pose.size());
    }
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
    encode_globals(w);                                // 全局状态（docs/32 §16.3；旧快照无此块）
    encode_instances(w);                              // 实例表 + 实体归属（docs/32 §6.3）
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
        /* 全局状态（docs/32 §16.3）：必须在实例 on_start 之前恢复（脚本可能读它）。
         * 旧快照没有该块 → restore_globals 探测 magic 失败、原样跳过。 */
        (void)restore_globals((const uint8_t*)bytes, n, off);
        /* 实例表（docs/32 §6.3）：结构相同时保留 VM 状态；结构变化才重建并 on_start 重注册。 */
        const int inst_mode = restore_instances((const uint8_t*)bytes, n, off);
        if (inst_mode == 1) {
            (void)dispatch_instance_on_start(0);
            if (global_bus) (void)call_global_event("on_start", 0.0, 0.0, 0.0, nullptr);
            if (overlay_bus) (void)call_overlay_event("on_start", 0.0, 0.0, 0.0, nullptr);
        }
    }
    scene->flatten_world(*this);
}

/* 全局状态校验和（docs/32 §16.3）：空 → 0（老场景的 world_trail 零变化）。
 * 键有序（std::map）→ 遍历确定；数值按位混入（-0.0 与 +0.0 不同，符合快照字节语义）。 */
uint64_t Engine::global_checksum() {
    if (global_nums.empty() && global_texts.empty()) return 0;
    uint64_t h = 1469598103934665603ull;
    auto mix_bytes = [&](const std::string& s) {
        for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ull; }
    };
    for (auto& [k, v] : global_nums) {
        mix_bytes(k);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h ^= bits; h *= 1099511628211ull;
    }
    for (auto& [k, v] : global_texts) {
        h ^= 0x9E3779B97F4A7C15ull; h *= 1099511628211ull;   // 数值/文本段分开
        mix_bytes(k);
        mix_bytes(v);
    }
    return h;
}

/* 世界校验和：场景（实体/组件标量）+ 物理求解器状态 + 粒子（都是模拟状态，docs/10 §5）。 */
uint64_t Engine::world_checksum() {
    uint64_t h = scene ? scene->checksum(*this) : 1469598103934665603ull;
    h ^= instance_checksum() * 1099511628211ull;      // 实例结构（docs/32 §6.3/§8）
    h ^= global_checksum() * 1099511628211ull;        // 全局状态（空 → 0，零回归）
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
/* ============================================================================
 * M2 3D 基础（plans/18-pbr-3d.md M2）：render.* 组件 → mq_renderer_frame3d
 *   · 矩阵列主序；右手系、Y 向上、相机看 -Z；WebGPU NDC z∈[0,1]
 *   · M2 光照 = 方向光 Lambert + 环境色（PBR 在 M3）；所有 mesh 共用内建 cube（M4 换资产）
 *   · 提取只读（渲染不回写模拟，ADR-0005）
 * ==========================================================================*/
static void m4_identity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0F;
}
/* 局部 TRS（Euler XYZ 弧度）→ 列主序 mat4；R = Rz * Ry * Rx（与 docs/components/02 约定一致）。 */
static void m4_from_trs(const math::Transform3D& t, float* m) {
    const float cx = std::cos(t.rotation.x), sx = std::sin(t.rotation.x);
    const float cy = std::cos(t.rotation.y), sy = std::sin(t.rotation.y);
    const float cz = std::cos(t.rotation.z), sz = std::sin(t.rotation.z);
    const float r00 = cy * cz, r10 = cy * sz, r20 = -sy;
    const float r01 = sx * sy * cz - cx * sz, r11 = sx * sy * sz + cx * cz, r21 = sx * cy;
    const float r02 = cx * sy * cz + sx * sz, r12 = cx * sy * sz - sx * cz, r22 = cx * cy;
    m[0] = r00 * t.scale.x; m[1] = r10 * t.scale.x; m[2] = r20 * t.scale.x; m[3] = 0.0F;
    m[4] = r01 * t.scale.y; m[5] = r11 * t.scale.y; m[6] = r21 * t.scale.y; m[7] = 0.0F;
    m[8] = r02 * t.scale.z; m[9] = r12 * t.scale.z; m[10] = r22 * t.scale.z; m[11] = 0.0F;
    m[12] = t.position.x; m[13] = t.position.y; m[14] = t.position.z; m[15] = 1.0F;
}
/* 透视投影（右手系，WebGPU NDC z∈[0,1]）。 */
static void m4_perspective(float fovy_rad, float aspect, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    const float f = 1.0F / std::tan(fovy_rad * 0.5F);
    m[0] = f / aspect; m[5] = f;
    m[10] = zf / (zn - zf); m[11] = -1.0F; m[14] = (zn * zf) / (zn - zf);
}
/* 正交投影（ortho_height = 垂直可见范围；右手系，WebGPU z∈[0,1]）。 */
static void m4_orthographic(float height, float aspect, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    const float hh = std::max(height * 0.5F, 1e-5F);
    const float hw = std::max(hh * aspect, 1e-5F);
    m[0] = 1.0F / hw;
    m[5] = 1.0F / hh;
    m[10] = 1.0F / (zn - zf);
    m[14] = zn / (zn - zf);
    m[15] = 1.0F;
}
/* 世界矩阵的旋转轴（列 c）：单位化后返回。列长≈1 时**不做除法** ——
 * 保证平级场景（world_m = m4_from_trs(local)，scale=1）与旧的 Euler 旋转逐字节一致；
 * 真正带缩放的节点（列长明显偏离 1）才归一化（相机/灯光忽略缩放）。 */
static void m4_axis(const float* m, int c, float out[3]) {
    float x = m[c * 4], y = m[c * 4 + 1], z = m[c * 4 + 2];
    const float len2 = x * x + y * y + z * z;
    if (std::fabs(std::sqrt(len2) - 1.0F) > 1.0e-4F) {
        const float inv = len2 > 0.0F ? 1.0F / std::sqrt(len2) : 1.0F;
        x *= inv; y *= inv; z *= inv;
    }
    out[0] = x; out[1] = y; out[2] = z;
}
/* 相机世界矩阵 → view（R^T * T(-pos)）；平级场景（scale=1）与旧 m4_view(trs) 逐字节一致。 */
static void m4_view_from_m(const float* wm, float* m) {
    float R[16];
    for (int c = 0; c < 3; ++c) m4_axis(wm, c, R + c * 4);
    R[3] = R[7] = R[11] = 0.0F;
    R[12] = R[13] = R[14] = 0.0F; R[15] = 1.0F;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m[c * 4 + r] = R[r * 4 + c];
    m[3] = 0.0F; m[7] = 0.0F; m[11] = 0.0F; m[15] = 1.0F;
    const float px = wm[12], py = wm[13], pz = wm[14];
    m[12] = -(m[0] * px + m[4] * py + m[8] * pz);
    m[13] = -(m[1] * px + m[5] * py + m[9] * pz);
    m[14] = -(m[2] * px + m[6] * py + m[10] * pz);
}
/* 四元数 (x,y,z,w) + 平移/缩放 → 列主序 mat4（glTF rotation 轨道；无万向锁）。 */
static void m4_from_quat_trs(const math::Vec3& pos, const float q_in[4], const math::Vec3& scale, float* m) {
    float q[4] = { q_in[0], q_in[1], q_in[2], q_in[3] };
    const float qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (qlen > 0.0F) { const float inv = 1.0F / qlen; for (int i = 0; i < 4; ++i) q[i] *= inv; }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1.0F - 2.0F * (yy + zz)) * scale.x; m[1] = (2.0F * (xy + wz)) * scale.x;
    m[2] = (2.0F * (xz - wy)) * scale.x; m[3] = 0.0F;
    m[4] = (2.0F * (xy - wz)) * scale.y; m[5] = (1.0F - 2.0F * (xx + zz)) * scale.y;
    m[6] = (2.0F * (yz + wx)) * scale.y; m[7] = 0.0F;
    m[8] = (2.0F * (xz + wy)) * scale.z; m[9] = (2.0F * (yz - wx)) * scale.z;
    m[10] = (1.0F - 2.0F * (xx + yy)) * scale.z; m[11] = 0.0F;
    m[12] = pos.x; m[13] = pos.y; m[14] = pos.z; m[15] = 1.0F;
}
static void m4_mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                           a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

/* glTF 节点局部 TRS（四元数）→ 列主序 mat4；T * R * S（glTF 规范）。 */
static void m4_from_qtrs(const float t[3], const float q[4], const float s[3], float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1.0F - 2.0F * (yy + zz)) * s[0];
    m[1] = (2.0F * (xy + wz)) * s[0];
    m[2] = (2.0F * (xz - wy)) * s[0];
    m[3] = 0.0F;
    m[4] = (2.0F * (xy - wz)) * s[1];
    m[5] = (1.0F - 2.0F * (xx + zz)) * s[1];
    m[6] = (2.0F * (yz + wx)) * s[1];
    m[7] = 0.0F;
    m[8] = (2.0F * (xz + wy)) * s[2];
    m[9] = (2.0F * (yz - wx)) * s[2];
    m[10] = (1.0F - 2.0F * (xx + yy)) * s[2];
    m[11] = 0.0F;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0F;
}

/* 动画通道采样：out 长度 = ch.comps（weights 通道可 >4）。 */
template <typename Ch>
static void sample_gltf_channel(const Ch& ch, float time, std::vector<float>& out) {
    const size_t n = ch.times.size();
    out.assign((size_t)ch.comps, 0.0F);
    if (n == 0 || ch.comps <= 0 || ch.values.empty()) return;
    const int comps = ch.comps;
    const int groups = ch.interp == 2 ? 3 : 1;
    const size_t fcount = (size_t)comps * (size_t)groups;
    auto val = [&](size_t key, int grp, int c) -> float {
        const size_t o = key * fcount + (size_t)grp * (size_t)comps + (size_t)c;
        return o < ch.values.size() ? ch.values[o] : 0.0F;
    };
    size_t i = 0;
    if (time <= ch.times[0]) {
        i = 0;
        if (ch.interp == 0 || n == 1) {
            for (int c = 0; c < comps; ++c) out[c] = val(0, ch.interp == 2 ? 1 : 0, c);
            return;
        }
    } else if (time >= ch.times[n - 1]) {
        i = n - 1;
        if (ch.interp == 0 || n == 1) {
            for (int c = 0; c < comps; ++c) out[c] = val(i, ch.interp == 2 ? 1 : 0, c);
            return;
        }
    } else {
        while (i + 1 < n && ch.times[i + 1] <= time) ++i;
    }
    if (ch.interp == 0) {
        for (int c = 0; c < comps; ++c) out[c] = val(i, 0, c);
        return;
    }
    const size_t j = i + 1 < n ? i + 1 : i;
    const float t0 = ch.times[i];
    const float t1 = ch.times[j];
    const float dt = t1 - t0;
    const float u = dt > 0.0F ? std::clamp((time - t0) / dt, 0.0F, 1.0F) : 0.0F;
    if (ch.interp == 1) {
        for (int c = 0; c < comps; ++c)
            out[c] = val(i, 0, c) + (val(j, 0, c) - val(i, 0, c)) * u;
        return;
    }
    /* CUBICSPLINE：key = (in-tangent, value, out-tangent)，切线按秒缩放。 */
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2.0F * u3 - 3.0F * u2 + 1.0F;
    const float h10 = u3 - 2.0F * u2 + u;
    const float h01 = -2.0F * u3 + 3.0F * u2;
    const float h11 = u3 - u2;
    for (int c = 0; c < comps; ++c) {
        const float p0 = val(i, 1, c);
        const float p1 = val(j, 1, c);
        const float m0 = dt * val(i, 2, c);
        const float m1 = dt * val(j, 0, c);
        out[c] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
    }
}

bool Engine::apply_gltf_morph(int entity, GltfGpuModel& gm, size_t prim_idx,
                              const std::vector<float>& weights) {
    (void)entity;
    if (prim_idx >= gm.prims.size()) return false;
    GltfGpuModel::Prim& pr = gm.prims[prim_idx];
    if (pr.mesh < 0 || pr.morph_targets.empty() || pr.base.empty()) return false;
    if (!rapi || !rself || !rapi->update_mesh) {
        if (!pr.warned_update) {
            pr.warned_update = true;
            std::print(stderr, "runtime: 渲染器不支持 update_mesh，glTF morph 跳过\n");
        }
        return false;
    }
    std::vector<uint8_t> out = pr.base;
    const uint32_t row_floats = pr.layout >= 2 ? (pr.layout == 3 ? 14u : 12u)
                                               : (pr.layout == 1 ? 14u : 12u);
    for (uint32_t vi = 0; vi < pr.vertex_count; ++vi) {
        float* pos = (float*)(out.data() + (size_t)vi * pr.stride);
        if ((size_t)(vi * row_floats + 2) >= pr.base.size() / sizeof(float)) break;
        float* nrm = pos + 3;
        bool normal_touched = false;
        for (size_t ti = 0; ti < pr.morph_targets.size(); ++ti) {
            const float w = ti < weights.size() ? weights[ti] : 0.0F;
            if (w == 0.0F) continue;
            const GltfGpuModel::MorphTarget& tgt = pr.morph_targets[ti];
            const size_t o = (size_t)vi * 3;
            if (o + 2 < tgt.positions.size()) {
                pos[0] += w * tgt.positions[o];
                pos[1] += w * tgt.positions[o + 1];
                pos[2] += w * tgt.positions[o + 2];
            }
            if (o + 2 < tgt.normals.size()) {
                nrm[0] += w * tgt.normals[o];
                nrm[1] += w * tgt.normals[o + 1];
                nrm[2] += w * tgt.normals[o + 2];
                normal_touched = true;
            }
        }
        if (normal_touched) {
            const float len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            if (len2 > 1.0e-12F) {
                const float inv = 1.0F / std::sqrt(len2);
                nrm[0] *= inv; nrm[1] *= inv; nrm[2] *= inv;
            }
        }
    }
    if (rapi->update_mesh(rself, pr.mesh, out.data(), (uint64_t)out.size()) != 0) {
        if (!pr.warned_update) {
            pr.warned_update = true;
            std::print(stderr, "runtime: glTF update_mesh 失败，morph 跳过\n");
        }
        return false;
    }
    pr.applied_weights = weights;
    return true;
}

void Engine::gltf_state_ensure(int entity, const std::string& path, const GltfGpuModel& gm) {
    if (gltf_states.find(entity) != gltf_states.end()) return;
    GltfEntityState st;
    st.path = path;
    const size_t n = gm.nodes.size();
    st.node_local.assign(n * 10, 0.0F);
    st.node_animated.assign(n, 0);
    st.node_world.assign(n * 16, 0.0F);
    st.node_weights.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& nd = gm.nodes[i];
        float* L = &st.node_local[i * 10];
        for (int k = 0; k < 3; ++k) { L[k] = nd.t[k]; L[7 + k] = nd.s[k]; }
        for (int k = 0; k < 4; ++k) L[3 + k] = nd.r[k];
        float lm[16];
        if (nd.has_matrix) std::memcpy(lm, nd.m, sizeof lm);
        else m4_from_qtrs(nd.t, nd.r, nd.s, lm);
        if (nd.parent >= 0 && (size_t)nd.parent < i)
            m4_mul(&st.node_world[(size_t)nd.parent * 16], lm, &st.node_world[i * 16]);
        else
            std::memcpy(&st.node_world[i * 16], lm, sizeof lm);
    }
    st.palettes.resize(gm.skins.size());
    for (size_t si = 0; si < gm.skins.size(); ++si) {
        const auto& sk = gm.skins[si];
        st.palettes[si].assign(sk.joints.size() * 16, 0.0F);
        for (size_t ji = 0; ji < sk.joints.size(); ++ji) {
            const int jn = sk.joints[ji];
            if (jn < 0 || (size_t)jn >= n) continue;
            const float* ibm = sk.inverse_bind.data() + ji * 16;
            m4_mul(&st.node_world[(size_t)jn * 16], ibm, &st.palettes[si][ji * 16]);
        }
    }
    st.morph_weights.resize(gm.prims.size());
    for (size_t pi = 0; pi < gm.prims.size(); ++pi)
        st.morph_weights[pi] = gm.prims[pi].default_weights;
    if (!gm.anims.empty()) {
        st.duration = gm.anims.front().duration;
        st.clip = 0;
    }
    st.inited = true;
    gltf_states[entity] = std::move(st);
}

void Engine::gltf_anim_step(float dt) {
    for (auto& kv : gltf_states) {
        const int entity = kv.first;
        GltfEntityState& st = kv.second;
        auto mit = gltf_models.find(st.path);
        if (mit == gltf_models.end() || mit->second.failed) continue;
        GltfGpuModel& gm = mit->second;
        const mq_type_desc* md = nullptr;
        std::vector<uint8_t>* mb = comp_by_name(*this, entity, "render.MeshRenderer", &md);
        void* mbase = mb ? mb->data() : nullptr;
        if (md && mbase) {
            const std::string* clip_name = read_str(md, mbase, "clip");
            if (clip_name && !clip_name->empty()) {
                for (size_t ai = 0; ai < gm.anims.size(); ++ai) {
                    if (gm.anims[ai].name == *clip_name) { st.clip = (int)ai; break; }
                }
            }
            const float speed = read_num(md, mbase, "speed", 1.0F);
            const bool playing = read_num(md, mbase, "playing", 1.0F) >= 0.5F;
            const bool loop = read_num(md, mbase, "loop", 1.0F) >= 0.5F;
            if (!gm.anims.empty() && st.clip >= 0 && (size_t)st.clip < gm.anims.size())
                st.duration = gm.anims[(size_t)st.clip].duration;
            if (playing && !gm.anims.empty()) {
                st.time += dt * speed;
                if (loop && st.duration > 0.0F) {
                    st.time = std::fmod(st.time, st.duration);
                    if (st.time < 0.0F) st.time += st.duration;
                } else if (st.time < 0.0F) {
                    st.time = 0.0F;
                } else if (st.duration > 0.0F && st.time > st.duration) {
                    st.time = st.duration;
                }
            }
            write_num(md, mbase, "time", (double)st.time);
            write_num(md, mbase, "duration", (double)st.duration);
        }
        const size_t n = gm.nodes.size();
        const bool anim_on = anim_enabled && !gm.anims.empty() && st.clip >= 0 &&
                             (size_t)st.clip < gm.anims.size();
        auto apply_pointer = [](GltfGpuModel& model, int mi, int field,
                                const std::vector<float>& v) {
            if (mi < 0 || (size_t)mi >= model.mats.size() || v.empty()) return;
            auto& m = model.mats[(size_t)mi];
            const float a = v[0];
            const float b = v.size() > 1 ? v[1] : 0.0F;
            const float c = v.size() > 2 ? v[2] : 0.0F;
            const float d = v.size() > 3 ? v[3] : 0.0F;
            switch (field) {
            case gltf::PF_BASE_COLOR:
                m.base_color[0] = a; m.base_color[1] = b; m.base_color[2] = c; m.base_color[3] = d;
                break;
            case gltf::PF_METALLIC: m.metallic = a; break;
            case gltf::PF_ROUGHNESS: m.roughness = a; break;
            case gltf::PF_EMISSIVE: m.emissive[0] = a; m.emissive[1] = b; m.emissive[2] = c; break;
            case gltf::PF_EMISSIVE_STRENGTH: m.emissive_intensity = a; break;
            case gltf::PF_ALPHA_CUTOFF: m.alpha_cutoff = a; break;
            case gltf::PF_CLEARCOAT: m.clearcoat = a; break;
            case gltf::PF_CLEARCOAT_ROUGHNESS: m.clearcoat_roughness = a; break;
            case gltf::PF_SHEEN_COLOR: m.sheen_color[0] = a; m.sheen_color[1] = b; m.sheen_color[2] = c; break;
            case gltf::PF_SHEEN_ROUGHNESS: m.sheen_roughness = a; break;
            case gltf::PF_IOR: m.ior = a; break;
            case gltf::PF_TRANSMISSION:
                m.anisotropy_rotation = a;
                if (a > 0.001F) m.flags2 |= 0x100000;
                break;
            case gltf::PF_VOLUME_THICKNESS: m.volume_thickness = a; break;
            case gltf::PF_ATTENUATION_DISTANCE: m.attenuation_distance = a; break;
            case gltf::PF_DIFFUSE_TRANSMISSION:
                m.diffuse_transmission_factor = a;
                if (a > 0.001F) m.flags2 |= 0x1000000;
                break;
            case gltf::PF_SPECULAR: m.specular_intensity = a; break;
            case gltf::PF_IRIDESCENCE: m.iridescence = a; break;
            case gltf::PF_ANISOTROPY: m.anisotropy = a; break;
            case gltf::PF_DISPERSION: m.dispersion = a; break;
            default: break;
            }
        };
        for (size_t i = 0; i < n; ++i) {
            const auto& nd = gm.nodes[i];
            float* L = &st.node_local[i * 10];
            for (int k = 0; k < 3; ++k) { L[k] = nd.t[k]; L[7 + k] = nd.s[k]; }
            for (int k = 0; k < 4; ++k) L[k + 3] = nd.r[k];
            st.node_animated[i] = 0;
            st.node_weights[i].clear();
        }
        if (anim_on) {
            const auto& anim = gm.anims[(size_t)st.clip];
            std::vector<float> vals;
            for (const auto& ch : anim.channels) {
                if (ch.path == 4) {
                    sample_gltf_channel(ch, st.time, vals);
                    apply_pointer(gm, ch.pointer_material, ch.pointer_field, vals);
                    continue;
                }
                if (ch.node < 0 || (size_t)ch.node >= n) continue;
                float* L = &st.node_local[(size_t)ch.node * 10];
                if (ch.path == 0 || ch.path == 1 || ch.path == 2) {
                    sample_gltf_channel(ch, st.time, vals);
                    if (ch.path == 0) {
                        for (int k = 0; k < 3 && k < ch.comps; ++k) L[k] = vals[(size_t)k];
                    } else if (ch.path == 1) {
                        for (int k = 0; k < 4 && k < ch.comps; ++k) L[k + 3] = vals[(size_t)k];
                        const float qx = L[3], qy = L[4], qz = L[5], qw = L[6];
                        const float ql = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
                        if (ql > 1.0e-8F) { L[3] /= ql; L[4] /= ql; L[5] /= ql; L[6] /= ql; }
                    } else {
                        for (int k = 0; k < 3 && k < ch.comps; ++k) L[7 + k] = vals[(size_t)k];
                    }
                    st.node_animated[(size_t)ch.node] = 1;
                } else if (ch.path == 3) {
                    sample_gltf_channel(ch, st.time, st.node_weights[(size_t)ch.node]);
                }
            }
        }
        for (int i : gm.order) {
            if (i < 0 || (size_t)i >= n) continue;
            const auto& nd = gm.nodes[(size_t)i];
            float lm[16];
            if (nd.has_matrix) std::memcpy(lm, nd.m, sizeof lm);
            else m4_from_qtrs(&st.node_local[(size_t)i * 10], &st.node_local[(size_t)i * 10 + 3],
                              &st.node_local[(size_t)i * 10 + 7], lm);
            if (nd.parent >= 0)
                m4_mul(&st.node_world[(size_t)nd.parent * 16], lm, &st.node_world[(size_t)i * 16]);
            else
                std::memcpy(&st.node_world[(size_t)i * 16], lm, sizeof lm);
        }
        if (skin_enabled) {
            for (size_t si = 0; si < gm.skins.size(); ++si) {
                const auto& sk = gm.skins[si];
                auto& pal = st.palettes[si];
                pal.assign(sk.joints.size() * 16, 0.0F);
                for (size_t ji = 0; ji < sk.joints.size(); ++ji) {
                    const int jn = sk.joints[ji];
                    if (jn < 0 || (size_t)jn >= n) continue;
                    m4_mul(&st.node_world[(size_t)jn * 16], sk.inverse_bind.data() + ji * 16,
                           &pal[ji * 16]);
                }
            }
        } else {
            for (auto& pal : st.palettes) {
                for (size_t ji = 0; ji + 16 <= pal.size(); ji += 16) {
                    m4_identity(&pal[ji]);
                }
            }
        }
        if (morph_enabled) {
            for (size_t pi = 0; pi < gm.prims.size(); ++pi) {
                auto& pr = gm.prims[pi];
                if (pr.morph_targets.empty()) continue;
                const auto& nw = st.node_weights[(size_t)std::max(pr.node, 0)];
                std::vector<float> w = nw.size() == pr.morph_targets.size() ? nw : pr.default_weights;
                if (w == pr.applied_weights) {
                    st.morph_weights[pi] = w;
                    continue;
                }
                if (pr.morph_owner < 0) pr.morph_owner = entity;
                if (pr.morph_owner != entity) {
                    if (!pr.warned_shared) {
                        pr.warned_shared = true;
                        std::print(stderr, "runtime: glTF morph 网格被多实体共享，仅首实体 '{}' 驱动\n",
                                   pr.morph_owner);
                    }
                    continue;
                }
                apply_gltf_morph(entity, gm, pi, w);
                st.morph_weights[pi] = w;
            }
        }
        if (render3d_info && !st.logged) {
            st.logged = true;
            std::print("runtime: gltf anim entity={} '{}' clip={} time={:.3f}/{:.3f} "
                       "channels={} skins={} morph={}\n",
                       entity, st.path, st.clip, (double)st.time, (double)st.duration,
                       gm.anims.empty() ? 0 : (long long)gm.anims[(size_t)std::clamp(st.clip, 0, (int)gm.anims.size() - 1)].channels.size(),
                       (long long)gm.skins.size(),
                       (long long)std::count_if(gm.prims.begin(), gm.prims.end(),
                                                [](const GltfGpuModel::Prim& p) { return !p.morph_targets.empty(); }));
        }
    }
}
static void read_color(const mq_type_desc* d, const void* base, const char* field, math::Color* out) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (f) std::memcpy(out, (const uint8_t*)base + f->offset, sizeof *out);
}

/* 内建 cube：24 顶点（每面 4 个，法线独立）/ 36 索引；48B 规范布局（pos3|normal3|tangent4|uv2）。
 * M2 用它验证 depth/相机/提取/直呈链路；M4 由 glTF mesh 资产取代。 */
static void build_cube(std::vector<float>& verts, std::vector<uint32_t>& indices) {
    const float s = 0.6F;
    struct Face { float n[3]; float p[4][3]; };
    static const Face faces[6] = {
        { { 0, 0, 1 }, { { -s, -s, s }, { s, -s, s }, { s, s, s }, { -s, s, s } } },
        { { 0, 0, -1 }, { { s, -s, -s }, { -s, -s, -s }, { -s, s, -s }, { s, s, -s } } },
        { { 1, 0, 0 }, { { s, -s, s }, { s, -s, -s }, { s, s, -s }, { s, s, s } } },
        { { -1, 0, 0 }, { { -s, -s, -s }, { -s, -s, s }, { -s, s, s }, { -s, s, -s } } },
        { { 0, 1, 0 }, { { -s, s, s }, { s, s, s }, { s, s, -s }, { -s, s, -s } } },
        { { 0, -1, 0 }, { { -s, -s, -s }, { s, -s, -s }, { s, -s, s }, { -s, -s, s } } },
    };
    verts.clear();
    indices.clear();
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = (uint32_t)verts.size() / 12;
        for (int i = 0; i < 4; ++i) {
            /* 每面 4 角 UV：0=(-s,-s) 1=(s,-s) 2=(s,s) 3=(-s,s)（贴图 PBR 用；M5 法线贴图同 UV） */
            const float u = (i == 0 || i == 3) ? 0.0F : 1.0F;
            const float v = (i >= 2) ? 1.0F : 0.0F;
            verts.insert(verts.end(), { faces[f].p[i][0], faces[f].p[i][1], faces[f].p[i][2],
                                        faces[f].n[0], faces[f].n[1], faces[f].n[2],
                                        1.0F, 0.0F, 0.0F, 1.0F, u, v });
        }
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

Engine::RenderTextureState* Engine::render_texture_for(int entity) {
    for (auto& rt : render_textures) if (rt.entity == entity) return &rt;
    return nullptr;
}
const Engine::RenderTextureState* Engine::render_texture_for(int entity) const {
    for (const auto& rt : render_textures) if (rt.entity == entity) return &rt;
    return nullptr;
}

void Engine::destroy_render_textures() {
    for (auto& rt : render_textures) {
        auto erase_key = [&](const std::string& key) {
            auto it = tex_cache.find(key);
            if (it != tex_cache.end() && it->second == rt.handle) tex_cache.erase(it);
        };
        erase_key(rt.uri);
        const int owner = entity_instance.count(rt.entity) ? entity_instance[rt.entity] : 0;
        if (owner == 0) erase_key("rendertexture://" + rt.name);
        if (rt.handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt.handle);
    }
    render_textures.clear();
    camera_captures.clear();
}

void Engine::setup_render_textures() {
    destroy_render_textures();
    sync_render_textures();
}

void Engine::sync_render_textures() {
    if (!scene || !reflect) return;
    const mq_type_desc* rtd = mq_reflect_type_by_name(reflect, "render.RenderTexture");
    if (!rtd) return;
    const bool supported = MQ_RENDERER_HAS(rapi, create_render_texture) &&
                           MQ_RENDERER_HAS(rapi, configure_render_texture) &&
                           MQ_RENDERER_HAS(rapi, render_to_texture) &&
                           MQ_RENDERER_HAS(rapi, read_texture);
    if (!supported) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::print(stderr, "runtime: renderer 不支持 RenderTexture ABI（需要 create/configure/render/read）\n");
        }
    }
    std::vector<int> ids;
    for (const auto& [id, e] : scene->ents)
        if (e.comps.find(rtd->type_id) != e.comps.end()) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::unordered_set<int> live(ids.begin(), ids.end());

    /* 先清理已销毁节点/已移除组件，且从 tex_cache 擦除别名，避免句柄重复释放。 */
    for (size_t i = 0; i < render_textures.size();) {
        RenderTextureState& rt = render_textures[i];
        if (live.count(rt.entity)) { ++i; continue; }
        auto erase_key = [&](const std::string& key) {
            auto it = tex_cache.find(key);
            if (it != tex_cache.end() && it->second == rt.handle) tex_cache.erase(it);
        };
        erase_key(rt.uri);
        const int owner = entity_instance.count(rt.entity) ? entity_instance[rt.entity] : 0;
        if (owner == 0) erase_key("rendertexture://" + rt.name);
        if (rt.handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt.handle);
        render_textures.erase(render_textures.begin() + (ptrdiff_t)i);
    }

    std::unordered_set<std::string> used;
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        std::vector<uint8_t>& blob = e->comps[rtd->type_id];
        const mq_type_desc* dummy = nullptr;
        const bool has3d_camera = comp_by_name(*this, id, "render.Camera3D", &dummy) != nullptr;
        const bool has2d_camera = comp_by_name(*this, id, "render.Camera2D", &dummy) != nullptr ||
                                  comp_by_name(*this, id, "scn.Camera", &dummy) != nullptr;
        const std::string* configured = read_str(rtd, blob.data(), "name");
        std::string name = configured && !configured->empty() ? *configured : e->name;
        if (name.empty()) name = "camera-" + std::to_string(id);
        const int owner = entity_instance.count(id) ? entity_instance[id] : 0;
        const std::string uri = "rendertexture://" + std::to_string(owner) + "/" + name;
        const bool valid_target = (has3d_camera || has2d_camera) && supported;
        const bool duplicate = valid_target && !used.insert(uri).second;
        RenderTextureState* rt = render_texture_for(id);

        auto write_transients = [&](const std::string& texture, bool ready, uint64_t rendered) {
            if (const mq_field_desc* f = mq_reflect_field(rtd, "texture"))
                *(std::string*)(blob.data() + f->offset) = texture;
            if (const mq_field_desc* f = mq_reflect_field(rtd, "ready"))
                *(bool*)(blob.data() + f->offset) = ready;
            if (const mq_field_desc* f = mq_reflect_field(rtd, "last_frame"))
                *(uint32_t*)(blob.data() + f->offset) = (uint32_t)rendered;
        };
        if (duplicate || (!has3d_camera && !has2d_camera) || !supported) {
            if (duplicate)
                std::print(stderr, "runtime: RenderTexture '{}' 在实例 {} 内重名（entity={} 跳过）\n", name, owner, id);
            else if (!has3d_camera && !has2d_camera)
                std::print(stderr, "runtime: RenderTexture entity={} 必须与 scn.Camera/render.Camera2D/Camera3D 同节点\n", id);
            if (rt) {
                auto erase_key = [&](const std::string& key) {
                    auto it = tex_cache.find(key);
                    if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                };
                erase_key(rt->uri);
                erase_key("rendertexture://" + rt->name);
                if (rt->handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt->handle);
                render_textures.erase(std::remove_if(render_textures.begin(), render_textures.end(),
                                                     [&](const RenderTextureState& s) { return s.entity == id; }),
                                      render_textures.end());
            }
            write_transients("", false, 0);
            continue;
        }

        const int32_t requested_w = (int32_t)read_num(rtd, blob.data(), "width", 0.0F);
        const int32_t requested_h = (int32_t)read_num(rtd, blob.data(), "height", 0.0F);
        const int32_t width = std::max(1, requested_w > 0 ? requested_w : W);
        const int32_t height = std::max(1, requested_h > 0 ? requested_h : H);
        const int32_t filter = (int32_t)read_num(rtd, blob.data(), "filter", 1.0F);
        math::Color cc{ 0.0F, 0.0F, 0.0F, 0.0F };
        read_color(rtd, blob.data(), "clear_color", &cc);
        mq_renderer_render_texture_desc desc{};
        desc.struct_size = sizeof desc;
        desc.width = width; desc.height = height; desc.pixel_format = 0;
        desc.filter = filter; desc.wrap_u = 1; desc.wrap_v = 1;
        desc.clear_color[0] = cc.r; desc.clear_color[1] = cc.g;
        desc.clear_color[2] = cc.b; desc.clear_color[3] = cc.a;

        if (!rt) {
            int32_t handle = -1;
            if (rapi->create_render_texture(rself, &desc, &handle) != 0 || handle < 0) {
                std::print(stderr, "runtime: RenderTexture '{}' 创建失败（{}x{}）\n", uri, width, height);
                write_transients("", false, 0);
                continue;
            }
            RenderTextureState state;
            state.entity = id; state.handle = handle; state.width = width; state.height = height;
            state.filter = filter; state.name = name; state.uri = uri; state.is_3d = has3d_camera;
            render_textures.push_back(std::move(state));
            rt = &render_textures.back();
            std::print("runtime: RenderTexture '{}' {}x{} handle={} camera={}\n",
                       uri, width, height, (long long)handle, has3d_camera ? "3d" : "2d");
        } else {
            if (rt->uri != uri) {
                auto it = tex_cache.find(rt->uri);
                if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                if (owner == 0) {
                    const std::string old_short = "rendertexture://" + rt->name;
                    it = tex_cache.find(old_short);
                    if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                }
                rt->uri = uri; rt->name = name;
            }
            if (rt->width != width || rt->height != height || rt->filter != filter) {
                if (rapi->configure_render_texture(rself, rt->handle, &desc) == 0) {
                    rt->width = width; rt->height = height; rt->filter = filter;
                    rt->dirty = true; rt->ready = false; rt->capture_pixels.clear();
                } else {
                    std::print(stderr, "runtime: RenderTexture '{}' 重配 {}x{} 失败，保留旧目标 {}x{}\n",
                               uri, width, height, rt->width, rt->height);
                }
            }
            if (rt->is_3d != has3d_camera) rt->dirty = true;
            rt->is_3d = has3d_camera;
        }
        rt->update_mode = (int32_t)read_num(rtd, blob.data(), "update_mode", 0.0F);
        const bool include_ui = read_num(rtd, blob.data(), "include_ui", 0.0F) >= 0.5F;
        const bool enabled = read_num(rtd, blob.data(), "enabled", 1.0F) >= 0.5F;
        if (rt->include_ui != include_ui || (!rt->enabled && enabled)) rt->dirty = true;
        rt->include_ui = include_ui;
        rt->enabled = enabled;
        tex_cache[rt->uri] = rt->handle;
        if (owner == 0) tex_cache["rendertexture://" + rt->name] = rt->handle;
        write_transients(rt->uri, rt->ready, rt->last_frame);
    }
    std::sort(render_textures.begin(), render_textures.end(),
              [](const RenderTextureState& a, const RenderTextureState& b) { return a.entity < b.entity; });
}

void Engine::setup_3d() {
    has_3d = false;
    if (!scene) return;
    int n_cam = 0, n_mesh = 0, n_light = 0;
    for (auto& kv : scene->ents) {
        const mq_type_desc* d = nullptr;
        if (comp_by_name(*this, kv.first, "render.Camera3D", &d)) ++n_cam;
        if (comp_by_name(*this, kv.first, "render.MeshRenderer", &d)) ++n_mesh;
        if (comp_by_name(*this, kv.first, "render.Light", &d)) ++n_light;
    }
    if (!n_cam && !n_mesh) return;
    has_3d = true;
    if (cube_mesh < 0 && rapi && rself && rapi->create_mesh) {
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        build_cube(verts, indices);
        mq_renderer_mesh_gpu md{};
        md.vertices = verts.data();
        md.vertex_bytes = (uint64_t)verts.size() * sizeof(float);
        md.vertex_stride = 48;
        md.vertex_layout = 0;
        md.indices = indices.data();
        md.index_count = (uint32_t)indices.size();
        md.bounds_min[0] = md.bounds_min[1] = md.bounds_min[2] = -0.6F;
        md.bounds_max[0] = md.bounds_max[1] = md.bounds_max[2] = 0.6F;
        int32_t id = -1;
        if (rapi->create_mesh(rself, &md, &id) == 0) {
            cube_mesh = id;
            mesh_stats[id] = { (uint32_t)(verts.size() / 12), (uint32_t)indices.size() };
        }
    }
    std::print("runtime: 3d scene cams={} meshes={} lights={} cube_mesh={}\n",
               n_cam, n_mesh, n_light, (long long)cube_mesh);
    /* plans/22 §6.1：实体名表 / 骨架 / 片段 / morph 网格预解析（装配期 IO，不在模拟帧内） */
    setup_gltf_assets();
}

bool Engine::make_camera_view2d(int entity, int width, int height, mq_renderer_view& view,
                                uint32_t* layer_mask) {
    Entity* e = scene ? scene->at(entity) : nullptr;
    if (!e || !node_active_effective(*this, entity)) return false;
    if (layer_mask) *layer_mask = 0xFFFFFFFFu;
    view = mq_renderer_view{};
    view.vw = (float)width; view.vh = (float)height;
    const mq_type_desc* d = nullptr;
    if (auto* b = comp_by_name(*this, entity, "render.Camera2D", &d); b && d) {
        if (read_num(d, b->data(), "active", 1.0F) < 0.5F) return false;
        math::Vec2 zoom{ 1.0F, 1.0F }, offset{};
        math::Rect viewport{ 0.0F, 0.0F, 1.0F, 1.0F };
        if (const mq_field_desc* f = mq_reflect_field(d, "zoom"))
            std::memcpy(&zoom, b->data() + f->offset, sizeof zoom);
        if (const mq_field_desc* f = mq_reflect_field(d, "offset"))
            std::memcpy(&offset, b->data() + f->offset, sizeof offset);
        if (const mq_field_desc* f = mq_reflect_field(d, "viewport"))
            std::memcpy(&viewport, b->data() + f->offset, sizeof viewport);
        if (layer_mask) if (const mq_field_desc* f = mq_reflect_field(d, "layer_mask"))
            *layer_mask = *(const uint32_t*)(b->data() + f->offset);
        const float zx = std::max(zoom.x, 1e-5F);
        if (std::fabs(zoom.y - zoom.x) > 1e-5F) {
            static bool warned_zoom = false;
            if (!warned_zoom) {
                warned_zoom = true;
                std::print(stderr, "runtime: Camera2D.zoom 当前渲染 ABI 仅支持等比缩放；使用 zoom.x={}（zoom.y={}）\n",
                           (double)zoom.x, (double)zoom.y);
            }
        }
        const float nx = std::clamp(viewport.x, 0.0F, 1.0F);
        const float ny = std::clamp(viewport.y, 0.0F, 1.0F);
        const float nw = std::clamp(viewport.width, 0.0F, 1.0F - nx);
        const float nh = std::clamp(viewport.height, 0.0F, 1.0F - ny);
        if (nw <= 0.0F || nh <= 0.0F) return false;
        view.vx = nx * width; view.vy = ny * height;
        view.vw = std::max(1.0F, nw * width); view.vh = std::max(1.0F, nh * height);
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, entity, angle, scale, opacity);
        (void)scale; (void)opacity;
        const float camera_x = e->world_x + offset.x;
        const float camera_y = (float)H - (e->world_y + offset.y);
        view.cx = camera_x;
        view.cy = camera_y;
        view.pan = (view.vw - (float)width) * 0.5F;
        view.eye = (view.vh - (float)height) * 0.5F;
        view.zoom = zx;
        view.rot = angle * 3.14159265358979323846F / 180.0F;
        return true;
    }
    if (auto* b = comp_by_name(*this, entity, "scn.Camera", &d); b && d) {
        if (read_num(d, b->data(), "active", 1.0F) < 0.5F) return false;
        const float x = read_num(d, b->data(), "x", e->world_x);
        const float y = read_num(d, b->data(), "y", e->world_y);
        view.cx = x;
        view.cy = (float)H - y;
        view.zoom = std::max(read_num(d, b->data(), "zoom", 1.0F), 1e-5F);
        view.rot = read_num(d, b->data(), "rotation", 0.0F);
        return true;
    }
    return false;
}

void Engine::fill_frame_environment(mq_renderer_frame3d& fr) {
    fr.env = mq_renderer_environment{};
    fr.env.sky_texture = -1;
    fr.env.env_texture = -1;
    fr.env.env_irradiance_texture = -1;
    fr.env.env_cubemap = -1;
    fr.env.env_cubemap_irradiance = -1;
    fr.env.sky_mode = 1;
    if (!scene) return;
    float env_col[3] = { scene->desc.env_color.r, scene->desc.env_color.g, scene->desc.env_color.b };
    float sky_col[3] = { scene->desc.env_sky_color.r, scene->desc.env_sky_color.g, scene->desc.env_sky_color.b };
    float ground_col[3] = { scene->desc.env_ground_color.r, scene->desc.env_ground_color.g, scene->desc.env_ground_color.b };
    const bool sky_zero = sky_col[0] == 0.0F && sky_col[1] == 0.0F && sky_col[2] == 0.0F;
    const bool ground_zero = ground_col[0] == 0.0F && ground_col[1] == 0.0F && ground_col[2] == 0.0F;
    for (int i = 0; i < 3; ++i) {
        if (sky_zero) sky_col[i] = env_col[i] * 1.15F;
        if (ground_zero) ground_col[i] = env_col[i] * 0.45F;
        fr.env.env_color[i] = env_col[i];
        fr.env.env_sky_color[i] = sky_col[i];
        fr.env.env_ground_color[i] = ground_col[i];
    }
    fr.env.env_color[3] = fr.env.env_sky_color[3] = fr.env.env_ground_color[3] = 1.0F;
    fr.env.intensity = scene->desc.env_intensity;
    fr.env.env_highlight = scene->desc.env_highlight;
    fr.env.env_rotation = scene->desc.env_rotation * 3.14159265F / 180.0F;

    std::string env_path = scene->desc.env_texture;
    const char* env_force = std::getenv("MAQUE_ENV_IBL");
    if (env_force && env_force[0] == '1') env_path = "builtin:/textures/studio_env";
    if (env_force && env_force[0] == '0') env_path.clear();
    if (!env_path.empty()) load_env_textures(env_path, fr.env.env_texture, fr.env.env_irradiance_texture);
    if (!scene->desc.env_cubemap.empty())
        load_env_cubemap(scene->desc.env_cubemap, fr.env.env_cubemap, fr.env.env_cubemap_irradiance);
    if (fr.env.env_cubemap >= 0) fr.env.sky_mode = scene->desc.env_background ? 2 : 1;
    else if (fr.env.env_texture >= 0 && scene->desc.env_background) fr.env.sky_mode = 3;
}

bool Engine::read_camera_texture(RenderTextureState& rt, bool refresh) {
    if (!rt.ready || rt.handle < 0 || !MQ_RENDERER_HAS(rapi, read_texture)) return false;
    if (!refresh && !rt.capture_pixels.empty()) return true;
    rt.capture_pixels.assign((size_t)rt.width * (size_t)rt.height * 4u, 0);
    const int32_t rc = rapi->read_texture(rself, rt.handle, rt.capture_pixels.data(),
                                          (uint32_t)rt.capture_pixels.size());
    if (rc != 0) { rt.capture_pixels.clear(); return false; }
    rt.capture_frame = rt.last_frame;
    return true;
}

bool Engine::save_camera_texture(int entity, const std::string& path) {
    RenderTextureState* rt = render_texture_for(entity);
    if (!rt || !rt->ready || rt->handle < 0 || path.empty() || !MQ_RENDERER_HAS(rapi, read_texture)) return false;
    std::vector<uint8_t> pixels((size_t)rt->width * (size_t)rt->height * 4u, 0);
    if (rapi->read_texture(rself, rt->handle, pixels.data(), (uint32_t)pixels.size()) != 0) return false;
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    const int rc = png ? mq_image_write_png(path.c_str(), (uint32_t)rt->width, (uint32_t)rt->height,
                                             pixels.data(), (uint32_t)rt->width * 4u)
                       : (write_ppm(path, rt->width, rt->height, pixels) ? 0 : 1);
    if (rc == 0)
        std::print("runtime: camera shot '{}' {}x{} camera={} frame={}\n",
                   path, rt->width, rt->height, entity, (unsigned long long)rt->last_frame);
    else
        std::print(stderr, "runtime: camera shot 写入失败 '{}' camera={}\n", path, entity);
    return rc == 0;
}

void Engine::render_camera_textures(const std::vector<mq_renderer_item>& draws,
                                    const std::vector<mq_renderer_mesh>& meshes) {
    if (render_textures.empty() || !MQ_RENDERER_HAS(rapi, render_to_texture)) return;
    /* 截图请求会强制 OnDemand 目标在本帧更新。 */
    for (const auto& req : camera_captures)
        if (RenderTextureState* rt = render_texture_for(req.entity)) rt->dirty = true;

    /* 依赖排序：A 的可见 2D 输入采样 B，则 B 先于 A。同一环内按实体 id 继续，语义为
     * “较早目标读环中较晚目标的上一帧，较晚目标可读较早目标的本帧”，确定且无同-pass 反馈。 */
    std::unordered_map<int32_t, size_t> by_handle;
    for (size_t i = 0; i < render_textures.size(); ++i) by_handle[render_textures[i].handle] = i;
    std::vector<std::vector<size_t>> edges(render_textures.size());
    std::vector<int> indegree(render_textures.size(), 0);
    for (size_t i = 0; i < render_textures.size(); ++i) {
        std::unordered_set<size_t> deps;
        uint32_t dep_2d_mask = 0xFFFFFFFFu;
        uint32_t dep_3d_mask = 0xFFFFFFFFu;
        if (!render_textures[i].is_3d) {
            const mq_type_desc* camera_desc = nullptr;
            if (auto* camera = comp_by_name(*this, render_textures[i].entity, "render.Camera2D", &camera_desc);
                camera && camera_desc) {
                if (const mq_field_desc* f = mq_reflect_field(camera_desc, "layer_mask"))
                    dep_2d_mask = *(const uint32_t*)(camera->data() + f->offset);
            }
        } else {
            const mq_type_desc* camera_desc = nullptr;
            if (auto* camera = comp_by_name(*this, render_textures[i].entity, "render.Camera3D", &camera_desc);
                camera && camera_desc) {
                if (const mq_field_desc* f = mq_reflect_field(camera_desc, "culling_mask"))
                    dep_3d_mask = *(const uint32_t*)(camera->data() + f->offset);
            }
        }
        auto consider = [&](int32_t texture, int layer) {
            if (texture < 0 || layer < 0 || layer > (render_textures[i].include_ui ? 1 : 0)) return;
            if (!render_textures[i].is_3d && layer < 32 && (dep_2d_mask & (1u << layer)) == 0) return;
            auto it = by_handle.find(texture);
            if (it != by_handle.end() && it->second != i) deps.insert(it->second);
        };
        if (!render_textures[i].is_3d) {
            for (const auto& item : draws) consider(item.texture, item.layer);
            for (const auto& mesh : meshes) consider(mesh.texture, mesh.layer);
        } else {
            if (render_textures[i].include_ui) {
                for (const auto& item : draws) if (item.layer == 1) consider(item.texture, item.layer);
                for (const auto& mesh : meshes) if (mesh.layer == 1) consider(mesh.texture, mesh.layer);
            }
            /* 3D 材质在 per-camera extract_3d 中生成；这里直接扫描 PBR 的字符串贴图槽，
             * 把 rendertexture:// 引用提前加入 pass 依赖。 */
            for (const auto& [eid, ent] : scene->ents) for (const auto& [tid, blob] : ent.comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || std::strcmp(d->name, "asset.PbrMaterial") != 0) continue;
                const mq_type_desc* mesh_desc = nullptr;
                auto* mesh_blob = comp_by_name(*this, eid, "render.MeshRenderer", &mesh_desc);
                if (!mesh_blob || !mesh_desc || !node_active_effective(*this, eid) ||
                    read_num(mesh_desc, mesh_blob->data(), "visible", 1.0F) < 0.5F) continue;
                uint32_t mesh_mask = 0xFFFFFFFFu;
                if (const mq_field_desc* f = mq_reflect_field(mesh_desc, "culling_mask"))
                    mesh_mask = *(const uint32_t*)(mesh_blob->data() + f->offset);
                if ((mesh_mask & dep_3d_mask) == 0) continue;
                for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                    const mq_field_desc& f = d->fields[fi];
                    if (f.kind != MQ_TK_STRING || std::string(f.name).find("texture") == std::string::npos) continue;
                    const std::string& ref = *(const std::string*)(blob.data() + f.offset);
                    auto hit = tex_cache.find(scoped_texture_ref(eid, ref));
                    if (hit != tex_cache.end()) consider(hit->second, 0);
                }
            }
        }
        for (size_t dep : deps) { edges[dep].push_back(i); ++indegree[i]; }
    }
    std::vector<size_t> order;
    for (size_t produced = 0; produced < render_textures.size();) {
        size_t pick = render_textures.size();
        for (size_t i = 0; i < render_textures.size(); ++i)
            if (indegree[i] == 0 && std::find(order.begin(), order.end(), i) == order.end() &&
                (pick == render_textures.size() || render_textures[i].entity < render_textures[pick].entity)) pick = i;
        if (pick == render_textures.size()) break;
        order.push_back(pick); ++produced; indegree[pick] = -1;
        for (size_t to : edges[pick]) --indegree[to];
    }
    if (order.size() != render_textures.size()) {
        static bool warned_cycle = false;
        if (!warned_cycle) {
            warned_cycle = true;
            std::print(stderr, "runtime: RenderTexture 依赖含环 → 环内按相机 entity id 使用确定的上一帧/本帧顺序\n");
        }
        std::vector<size_t> rest;
        for (size_t i = 0; i < render_textures.size(); ++i)
            if (std::find(order.begin(), order.end(), i) == order.end()) rest.push_back(i);
        std::sort(rest.begin(), rest.end(), [&](size_t a, size_t b) {
            return render_textures[a].entity < render_textures[b].entity;
        });
        order.insert(order.end(), rest.begin(), rest.end());
    }

    for (size_t oi : order) {
        RenderTextureState& rt = render_textures[oi];
        const bool capture_due = std::any_of(camera_captures.begin(), camera_captures.end(),
                                             [&](const CameraCaptureRequest& r) { return r.entity == rt.entity; });
        if ((!rt.enabled && !capture_due) || !node_active_effective(*this, rt.entity)) continue;
        if (rt.update_mode != 0 && !rt.dirty) continue;
        const mq_type_desc* rtd = nullptr;
        auto* rtb = comp_by_name(*this, rt.entity, "render.RenderTexture", &rtd);
        if (!rtb || !rtd) continue;
        mq_renderer_frame3d fr{};
        fr.width = rt.width; fr.height = rt.height;
        fr.env.sky_texture = fr.env.env_texture = fr.env.env_irradiance_texture = -1;
        fr.env.env_cubemap = fr.env.env_cubemap_irradiance = -1;
        fr.msaa_samples = scene && scene->desc.msaa_samples >= 4 ? 4 : 1;
        math::Color cc{};
        read_color(rtd, rtb->data(), "clear_color", &cc);
        fr.bg[0] = cc.r; fr.bg[1] = cc.g; fr.bg[2] = cc.b; fr.bg[3] = cc.a;

        std::vector<mq_renderer_item> target_items;
        std::vector<mq_renderer_mesh> target_meshes;
        auto keep_layer = [&](int layer) { return layer == 0 || (rt.include_ui && layer == 1); };
        if (!rt.is_3d) {
            const mq_type_desc* camera2d_desc = nullptr;
            if (auto* camera2d = comp_by_name(*this, rt.entity, "render.Camera2D", &camera2d_desc);
                camera2d && camera2d_desc) {
                math::Color camera_clear{};
                read_color(camera2d_desc, camera2d->data(), "clear_color", &camera_clear);
                fr.bg[0] = camera_clear.r; fr.bg[1] = camera_clear.g;
                fr.bg[2] = camera_clear.b; fr.bg[3] = camera_clear.a;
            }
            mq_renderer_view view{};
            uint32_t camera_mask = 0xFFFFFFFFu;
            if (!make_camera_view2d(rt.entity, rt.width, rt.height, view, &camera_mask)) continue;
            auto keep_2d = [&](int layer) {
                return keep_layer(layer) && layer >= 0 && layer < 32 && (camera_mask & (1u << layer)) != 0;
            };
            for (const auto& item : draws) if (keep_2d(item.layer)) target_items.push_back(item);
            for (const auto& mesh : meshes) if (keep_2d(mesh.layer)) target_meshes.push_back(mesh);
            fr.views2d = &view; fr.view2d_count = 1;
            fr.items = target_items.data(); fr.item_count = (uint32_t)target_items.size();
            fr.meshes = target_meshes.data(); fr.mesh_count = (uint32_t)target_meshes.size();
            const int32_t rc = rapi->render_to_texture(rself, rt.handle, &fr);
            if (rc != 0) { std::print(stderr, "runtime: RenderTexture '{}' 2D render rc={}\n", rt.uri, rc); continue; }
        } else {
            std::vector<mq_renderer_view3d> v3;
            std::vector<mq_renderer_light> l3;
            std::vector<mq_renderer_pbr_material> m3;
            std::vector<mq_renderer_draw3d> d3;
            std::vector<mq_renderer_skin> s3;
            extract_3d(rt.width, rt.height, v3, l3, m3, d3, s3, rt.entity, false);
            if (v3.empty()) continue;
            if (rt.include_ui) {
                for (const auto& item : draws) if (item.layer == 1) target_items.push_back(item);
                for (const auto& mesh : meshes) if (mesh.layer == 1) target_meshes.push_back(mesh);
            }
            std::vector<mq_renderer_shader_pass> passes;
            extract_shader_passes(passes);
            fr.views = v3.data(); fr.view_count = (uint32_t)v3.size();
            fr.lights = l3.data(); fr.light_count = (uint32_t)l3.size();
            fr.materials = m3.data(); fr.material_count = (uint32_t)m3.size();
            fr.draws = d3.data(); fr.draw_count = (uint32_t)d3.size();
            fr.skins = s3.data(); fr.skin_count = (uint32_t)s3.size();
            fr.items = target_items.data(); fr.item_count = (uint32_t)target_items.size();
            fr.meshes = target_meshes.data(); fr.mesh_count = (uint32_t)target_meshes.size();
            fr.views2d = nullptr; fr.view2d_count = 0;
            fr.passes = passes.data(); fr.pass_count = (uint32_t)passes.size();
            fill_frame_environment(fr);
            const int32_t rc = rapi->render_to_texture(rself, rt.handle, &fr);
            if (rc != 0) { std::print(stderr, "runtime: RenderTexture '{}' 3D render rc={}\n", rt.uri, rc); continue; }
        }
        rt.ready = true;
        rt.dirty = false;
        rt.last_frame = frame;
        if (const mq_field_desc* f = mq_reflect_field(rtd, "ready")) *(bool*)(rtb->data() + f->offset) = true;
        if (const mq_field_desc* f = mq_reflect_field(rtd, "last_frame")) *(uint32_t*)(rtb->data() + f->offset) = (uint32_t)frame;
    }
    for (const auto& req : camera_captures) {
        RenderTextureState* rt = render_texture_for(req.entity);
        if (rt && rt->ready && rt->last_frame == frame) {
            (void)save_camera_texture(req.entity, req.path);
        } else {
            std::print(stderr, "runtime: camera shot 跳过 '{}' camera={}（本帧 RenderTexture 更新失败）\n",
                       req.path, req.entity);
        }
    }
    camera_captures.clear();
}

/* ==========================================================================
 * 场景资源全屏 shader pass（docs/13）
 *   · 资源是场景目录里的 .wgsl（相对路径、VFS 根 = 场景目录）——**不是** builtin shader；
 *   · 装配期加载/编译一次；每帧只推进 time（模拟 dt，确定性）并提交给渲染器；
 *   · 失败（文件缺失/编译错）打印明确原因并跳过，场景其余部分照常运行。
 * ==========================================================================*/
void Engine::destroy_shader_passes() {
    if (rapi && rself && rapi->destroy_shader_pass)
        for (auto& p : loaded_shader_passes)
            if (p.handle >= 0) rapi->destroy_shader_pass(rself, p.handle);
    loaded_shader_passes.clear();
    has_shader_pass = false;
}

void Engine::setup_shader_passes() {
    destroy_shader_passes();
    if (!scene || !vfs || !rapi || !rself || !rapi->create_shader_pass) return;
    std::vector<int> ids;
    ids.reserve(scene->ents.size());
    for (auto& kv : scene->ents) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());            // unordered_map 顺序不可依赖（确定性）
    for (int id : ids) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, id, "render.ShaderPass", &d);
        if (!blob || !d) continue;
        const std::string* path = read_str(d, blob->data(), "shader");
        if (!path || path->empty()) continue;
        void* src = nullptr;
        size_t n = 0;
        const std::string vpath = "/" + *path;
        if (mq_vfs_read(vfs, vpath.c_str(), &src, &n) != MQ_VFS_OK || !src || n == 0) {
            std::print(stderr, "runtime: shader pass '{}' 未找到（相对场景目录，经 VFS 读取）\n", *path);
            if (src) mq_vfs_free(src);
            continue;
        }
        std::string wgsl((const char*)src, n);
        mq_vfs_free(src);
        int32_t handle = -1;
        const int32_t rc = rapi->create_shader_pass(rself, wgsl.c_str(), &handle);
        if (rc != 0 || handle < 0) {
            std::print(stderr, "runtime: shader pass '{}' 编译失败 rc={}"
                               "（需 @fragment fs_main + group0 binding0 的 32B uniform，见 docs/13）\n",
                       *path, (int)rc);
            continue;
        }
        loaded_shader_passes.push_back({ id, handle });
        std::print("runtime: shader pass '{}' loaded (entity={}, id={})\n",
                   *path, id, (long long)handle);
    }
    has_shader_pass = !loaded_shader_passes.empty();
}

void Engine::shader_pass_step(float dt) {
    if (!scene) return;
    std::vector<int> ids;
    ids.reserve(scene->ents.size());
    for (auto& kv : scene->ents) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, id, "render.ShaderPass", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "paused", 0.0F) >= 0.5F) continue;
        const float speed = read_num(d, blob->data(), "speed", 1.0F);
        if (speed == 0.0F) continue;
        const float time = read_num(d, blob->data(), "time", 0.0F) + dt * speed;
        if (const mq_field_desc* f = mq_reflect_field(d, "time"))
            *(float*)(blob->data() + f->offset) = time;
    }
}

void Engine::extract_shader_passes(std::vector<mq_renderer_shader_pass>& out) {
    out.clear();
    if (!scene || loaded_shader_passes.empty()) return;
    /* 指针位置：世界坐标（设计分辨率像素、y 向上）→ 渲染目标像素（原点左上，与 frag_coord 同向）。 */
    const float mx = mouse_x * ss;
    const float my = ((float)H - mouse_y) * ss;
    for (const auto& p : loaded_shader_passes) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, p.entity, "render.ShaderPass", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        mq_renderer_shader_pass sp{};
        sp.shader = p.handle;
        sp.time = read_num(d, blob->data(), "time", 0.0F);
        sp.frame = (float)frame;
        sp.mouse[0] = mx;
        sp.mouse[1] = my;
        out.push_back(sp);
    }
}

void Engine::extract_3d(int32_t tw, int32_t th,
                        std::vector<mq_renderer_view3d>& out_views,
                        std::vector<mq_renderer_light>& out_lights,
                        std::vector<mq_renderer_pbr_material>& out_mats,
                        std::vector<mq_renderer_draw3d>& out_draws,
                        std::vector<mq_renderer_skin>& out_skins,
                        int camera_entity, bool allow_orbit) {
    out_views.clear(); out_lights.clear(); out_mats.clear(); out_draws.clear();
    out_skins.clear();
    skin_palettes.clear();                       /* 上一帧调色板已随上一帧渲染用完 */
    if (!gltf_env_read) read_gltf_env();
    if (!scene || tw <= 0 || th <= 0) return;
    /* plans/22 §6.3：每帧先由节点树 + 片段派生姿态算世界矩阵（extract 只读派生数据）。
     * 纯 shader-pass 场景（has_3d=false）没有 3D 组件，不必付这次遍历。 */
    if (has_3d) scene->flatten_world3d(*this);
    /* 实体 id 排序：unordered_map 迭代顺序不可依赖（确定性） */
    std::vector<int> ids;
    ids.reserve(scene->ents.size());
    for (auto& kv : scene->ents) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());

    /* 材质：M3 从**同节点的 asset.PbrMaterial 组件**读取（M4 换成共享 .material.json 资产）；
     * 无材质组件的节点用默认灰（金属度 0 / 粗糙度 0.6）。 */
    math::Transform3D cam_trs{};
    cam_trs.position = math::Vec3{ 0.0F, 1.6F, 5.0F };
    cam_trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
    float cam_wm[16];                            /* 相机世界矩阵（plans/22：由 world_m 派生 view） */
    m4_identity(cam_wm);
    bool has_cam_wm = false;
    bool has_cam = false;
    bool gltf_lights_added = false;
    bool gltf_light_logged = false;
    float cam_priority = -1.0e9F;
    float fov_y = 60.0F, ortho_height = 2.0F, near_z = 0.1F, far_z = 100.0F, exposure = 1.0F;
    int32_t projection = 0;
    int32_t clear_mode = 1;
    int32_t camera_mask = -1;
    math::Rect camera_viewport{ 0.0F, 0.0F, 1.0F, 1.0F };
    math::Color clear{ 0.06F, 0.06F, 0.09F, 1.0F };

    for (int idx : ids) {
        auto it = scene->ents.find(idx);
        if (it == scene->ents.end()) continue;
        const float* wm = it->second.world_m;    /* flatten_world3d 的派生结果（列主序） */
        const mq_type_desc* dt = nullptr;
        const mq_type_desc* dcam = nullptr;
        const mq_type_desc* dmesh = nullptr;
        const mq_type_desc* dlight = nullptr;
        auto* tb = comp_by_name(*this, idx, "scene.Transform3D", &dt);
        math::Transform3D trs{};
        trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
        if (tb && dt) {
            if (const mq_field_desc* lf = mq_reflect_field(dt, "local"))
                std::memcpy(&trs, tb->data() + lf->offset, sizeof trs);
        }
        auto* cb = comp_by_name(*this, idx, "render.Camera3D", &dcam);
        const bool rt_camera = comp_by_name(*this, idx, "render.RenderTexture", nullptr) != nullptr;
        if (cb && dcam && (camera_entity < 0 || idx == camera_entity) &&
            !(camera_entity < 0 && rt_camera)) {
            const float active = read_num(dcam, cb->data(), "active", 1.0F);
            const float prio = read_num(dcam, cb->data(), "priority", 0.0F);
            if (active >= 0.5F && node_active_effective(*this, idx) &&
                (!has_cam || prio > cam_priority)) {
                has_cam = true;
                cam_priority = prio;
                cam_trs = trs;
                std::memcpy(cam_wm, wm, sizeof cam_wm);   /* 相机用世界矩阵（平级场景 == 旧 trs） */
                has_cam_wm = true;
                projection = (int32_t)read_num(dcam, cb->data(), "projection", 0.0F);
                fov_y = read_num(dcam, cb->data(), "fov_y", 60.0F);
                ortho_height = read_num(dcam, cb->data(), "ortho_height", 2.0F);
                near_z = read_num(dcam, cb->data(), "near", 0.1F);
                far_z = read_num(dcam, cb->data(), "far", 100.0F);
                exposure = read_num(dcam, cb->data(), "exposure", 1.0F);
                clear_mode = (int32_t)read_num(dcam, cb->data(), "clear_mode", 1.0F);
                if (const mq_field_desc* f = mq_reflect_field(dcam, "culling_mask"))
                    camera_mask = (int32_t)*(const uint32_t*)(cb->data() + f->offset);
                if (const mq_field_desc* f = mq_reflect_field(dcam, "viewport"))
                    std::memcpy(&camera_viewport, cb->data() + f->offset, sizeof camera_viewport);
                read_color(dcam, cb->data(), "clear_color", &clear);
            }
        }
        auto* lb = comp_by_name(*this, idx, "render.Light", &dlight);
        if (lb && dlight) {
            /* M5b：Directional / Point / Spot 全类型（后两种用节点世界位置；方向 = 节点 -Z 轴） */
            mq_renderer_light L{};
            const int type = (int)read_num(dlight, lb->data(), "type", 0.0F);
            L.type = type;
            math::Color lc{ 1.0F, 1.0F, 1.0F, 1.0F };
            read_color(dlight, lb->data(), "color", &lc);
            L.color[0] = lc.r; L.color[1] = lc.g; L.color[2] = lc.b;
            L.intensity = read_num(dlight, lb->data(), "intensity", 1.0F);
            L.range = read_num(dlight, lb->data(), "range", 10.0F);
            constexpr float kDeg = 3.14159265F / 180.0F;
            const float inner_d = read_num(dlight, lb->data(), "inner_angle", 20.0F);
            const float outer_d = read_num(dlight, lb->data(), "outer_angle", 30.0F);
            L.inner_cos = std::cos(inner_d * kDeg);
            L.outer_cos = std::cos(outer_d * kDeg);
            /* plans/22 §6.3：位置/朝向取节点世界矩阵（平级场景与旧 trs 逐字节一致） */
            float az[3];
            m4_axis(wm, 2, az);
            L.pos[0] = wm[12]; L.pos[1] = wm[13]; L.pos[2] = wm[14];
            L.dir[0] = -az[0]; L.dir[1] = -az[1]; L.dir[2] = -az[2];
            out_lights.push_back(L);
        }
        auto* mb = comp_by_name(*this, idx, "render.MeshRenderer", &dmesh);
        const std::string* gmp = (mb && dmesh) ? read_str(dmesh, mb->data(), "mesh_path") : nullptr;
        if (mb && dmesh && gmp && !gmp->empty() && gltf::is_gltf_path(*gmp)) {
            if (read_num(dmesh, mb->data(), "visible", 1.0F) < 0.5F) continue;
            GltfGpuModel* gm = load_gltf_model(*gmp);
            if (gm && !gm->failed) {
                gltf_state_ensure(idx, *gmp, *gm);
                GltfEntityState& gst = gltf_states[idx];
                /* plans/24 P6：KHR_materials_variants 选择（组件 variant 名称）。 */
                int selected_variant = -1;
                if (!gm->variants.empty()) {
                    const std::string* vn = read_str(dmesh, mb->data(), "variant");
                    if (vn && !vn->empty()) {
                        for (size_t vi = 0; vi < gm->variants.size(); ++vi)
                            if (gm->variants[vi] == *vn) { selected_variant = (int)vi; break; }
                    }
                }
                /* plans/24 P5：KHR_lights_punctual → frame3d 灯光（按模型尺度归一化）。 */
                for (const GltfGpuModel::Light& gl : gm->lights) {
                    if (gl.node < 0) continue;
                    const size_t nw = (size_t)gl.node * 16u;
                    if (nw + 16u > gst.node_world.size()) continue;
                    float lm[16];
                    m4_mul(wm, &gst.node_world[nw], lm);
                    mq_renderer_light L{};
                    L.type = gl.type;
                    L.pos[0] = lm[12]; L.pos[1] = lm[13]; L.pos[2] = lm[14];
                    float laz[3];
                    m4_axis(lm, 2, laz);
                    L.dir[0] = -laz[0]; L.dir[1] = -laz[1]; L.dir[2] = -laz[2];
                    for (int k = 0; k < 3; ++k) L.color[k] = gl.color[k];
                    if (gl.type == 0) {
                        L.intensity = std::clamp(2.6F * gl.intensity, 0.0F, 20.0F);
                    } else {
                        const float dref = std::max(0.5F * gm->radius, 1e-3F);
                        L.intensity = std::clamp(2.6F * (dref * dref + 1.0F) * gl.intensity,
                                                 0.0F, 2000.0F);
                    }
                    L.range = gl.type == 0 ? 0.0F : gl.range;
                    L.inner_cos = gl.inner_cos;
                    L.outer_cos = gl.outer_cos;
                    out_lights.push_back(L);
                    gltf_lights_added = true;
                    if (render3d_info && !gltf_light_logged) {
                        gltf_light_logged = true;
                        std::print("runtime: gltf light on entity={} node={} type={} intensity={:.3f}\n",
                                   idx, gl.node, gl.type, (double)L.intensity);
                    }
                }
                if (!has_cam && !has_cam_wm && !gm->cameras.empty()) {
                    for (const auto& gc : gm->cameras) {
                        if (!gc.perspective || gc.node < 0) continue;
                        const size_t nw = (size_t)gc.node * 16u;
                        if (nw + 16u > gst.node_world.size()) continue;
                        m4_mul(wm, &gst.node_world[nw], cam_wm);
                        has_cam_wm = true;
                        fov_y = gc.yfov * 57.2957795F;
                        near_z = std::max(gc.znear, 1e-4F);
                        far_z = std::max(gc.zfar, near_z + 1.0F);
                        if (render3d_info)
                            std::print("runtime: gltf camera entity={} node={} fov={:.2f} near={:.3f} far={:.3f}\n",
                                       idx, gc.node, (double)fov_y, (double)near_z, (double)far_z);
                        break;
                    }
                }
                for (size_t pi = 0; pi < gm->prims.size(); ++pi) {
                    const GltfGpuModel::Prim& gp = gm->prims[pi];
                    int mat_index = gp.material;
                    if (selected_variant >= 0 &&
                        (size_t)selected_variant < gp.variant_materials.size() &&
                        gp.variant_materials[(size_t)selected_variant] >= 0)
                        mat_index = gp.variant_materials[(size_t)selected_variant];
                    if (gp.mesh < 0 || mat_index < 0 || (size_t)mat_index >= gm->mats.size())
                        continue;
                    mq_renderer_draw3d dr{};
                    dr.mesh = gp.mesh;
                    dr.material = (int32_t)out_mats.size();
                    out_mats.push_back(gm->mats[(size_t)mat_index]);
                    dr.skin = -1;
                    int skin_index = -1;
                    const bool skinned = gp.skin >= 0 && (size_t)gp.skin < gst.palettes.size() &&
                                         !gst.palettes[(size_t)gp.skin].empty();
                    if (skinned) {
                        const auto& pal = gst.palettes[(size_t)gp.skin];
                        skin_palettes.push_back(pal);
                        mq_renderer_skin rs{};
                        rs.palette = skin_palettes.back().data();
                        rs.joint_count = (uint32_t)(pal.size() / 16);
                        out_skins.push_back(rs);
                        skin_index = (int32_t)out_skins.size() - 1;
                        std::memcpy(dr.model, wm, sizeof dr.model);
                    } else if (gp.node >= 0 && (size_t)gp.node * 16u + 16u <= gst.node_world.size()) {
                        m4_mul(wm, &gst.node_world[(size_t)gp.node * 16u], dr.model);
                    } else {
                        std::memcpy(dr.model, wm, sizeof dr.model);
                    }
                    dr.skin = skin_index;
                    dr.flags |= 0x40000000;            /* draw.layer 是有效 culling mask（旧调用方无此位） */
                    dr.layer = -1;
                    if (const mq_field_desc* f = mq_reflect_field(dmesh, "culling_mask"))
                        dr.layer = (int32_t)*(const uint32_t*)(mb->data() + f->offset);
                    dr.sort_key = (uint64_t)idx;
                    out_draws.push_back(dr);
                }
            }
            continue;
        }
        if (mb && dmesh && cube_mesh >= 0) {
            if (read_num(dmesh, mb->data(), "visible", 1.0F) < 0.5F) continue;
            /* M3 材质：同节点的 asset.PbrMaterial 组件（M4 起由共享 .material.json 资产引用） */
            mq_renderer_pbr_material mat{};
            for (int ti = 0; ti < 5; ++ti) mat.textures[ti] = -1;   /* -1 = 未给贴图 → 渲染器用 1×1 白 */
            for (int ti = 0; ti < 3; ++ti) mat.textures2[ti] = -1;  /* matcap / gradient / clearcoat-normal */
            for (int ti = 0; ti < 7; ++ti) mat.textures3[ti] = -1;  /* KHR 扩展 7 槽（clearcoat/sheen/iridescence/anisotropy） */
            for (int ti = 0; ti < 2; ++ti) mat.textures4[ti] = -1;  /* KHR_materials_specular 2 槽（factor/color） */
            mat.textures5[0] = -1;                                  /* KHR_materials_transmission 贴图槽（R=透射率掩码） */
            for (int ti = 0; ti < 2; ++ti) mat.textures6[ti] = -1;  /* diffuse transmission（A 乘子 / RGB sRGB 色） */
            mat.textures7[0] = -1;                                  /* KHR_materials_volume thicknessTexture.G */
            for (int ti = 0; ti < 2; ++ti) {
                mat.uv_scale[ti] = 1.0F; mat.uv_offset[ti] = 0.0F;       /* KHR_texture_transform 缺省 */
                mat.normal_uv_scale[ti] = 1.0F; mat.normal_uv_offset[ti] = 0.0F;
            }
            mat.shading_model = 0;                                  /* Standard（与旧 PbrMaterial 语义一致） */
            mat.side = 2;                                           /* Double：兼容旧 cullMode=None */
            mat.opacity = 1.0F;
            mat.emissive_intensity = 1.0F;
            mat.env_intensity = 1.0F;
            mat.normal_scale_y = 1.0F;
            mat.shininess = 30.0F;
            mat.specular_intensity = 1.0F;
            mat.ior = 1.5F;
            mat.specular[0] = mat.specular[1] = mat.specular[2] = 1.0F;
            mat.sheen_roughness = 0.5F;
            mat.iridescence_ior = 1.3F;
            mat.iridescence_thickness_min = 100.0F;
            mat.iridescence_thickness_max = 400.0F;
            mat.clearcoat_normal_scale = 1.0F;
            mat.toon_steps = 3;
            mat.depth_packing = 0;
            mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = mat.base_color[3] = 1.0F;
            mat.metallic = 0.0F;
            mat.roughness = 0.6F;
            mat.normal_scale = 1.0F;
            mat.occlusion_strength = 1.0F;
            const mq_type_desc* dmat = nullptr;
            auto* matb = comp_by_name(*this, idx, "asset.PbrMaterial", &dmat);
            if (matb && dmat) {
                const uint8_t* base = matb->data();
                math::Color bc{ 1.0F, 1.0F, 1.0F, 1.0F };
                if (const mq_field_desc* f = mq_reflect_field(dmat, "base_color"))
                    std::memcpy(&bc, base + f->offset, sizeof bc);
                mat.base_color[0] = bc.r; mat.base_color[1] = bc.g;
                mat.base_color[2] = bc.b; mat.base_color[3] = bc.a;
                math::Vec3 em{ 0.0F, 0.0F, 0.0F };
                if (const mq_field_desc* f = mq_reflect_field(dmat, "emissive"))
                    std::memcpy(&em, base + f->offset, sizeof em);
                mat.emissive[0] = em.x; mat.emissive[1] = em.y; mat.emissive[2] = em.z;
                mat.metallic = read_num(dmat, base, "metallic", 0.0F);
                mat.roughness = read_num(dmat, base, "roughness", 0.5F);
                mat.normal_scale = read_num(dmat, base, "normal_scale", 1.0F);
                mat.occlusion_strength = read_num(dmat, base, "occlusion_strength", 1.0F);
                mat.alpha_cutoff = read_num(dmat, base, "alpha_cutoff", 0.5F);
                mat.alpha_mode = (int32_t)read_num(dmat, base, "alpha_mode", 0.0F);
    /* transmission/diffuse 近似把 MASK 改成 BLEND 时保留 alpha 镂空（shader 用 flags2 bit19） */
    if (read_num(dmat, base, "alpha_mask_in_blend", 0.0F) >= 0.5F) mat.flags2 |= 0x80000;

                mat.flags = read_num(dmat, base, "double_sided", 0.0F) >= 0.5F ? 1 : 0;
                mat.clearcoat = read_num(dmat, base, "clearcoat", 0.0F);
                mat.clearcoat_roughness = read_num(dmat, base, "clearcoat_roughness", 0.03F);
                /* plans/19：多着色模型参数（缺省 = Standard 观感，旧场景零变化） */
                mat.shading_model = (int32_t)read_num(dmat, base, "shading_model", 0.0F);
                mat.side = (int32_t)read_num(dmat, base, "side", 2.0F);
                mat.opacity = read_num(dmat, base, "opacity", 1.0F);
                mat.emissive_intensity = read_num(dmat, base, "emissive_intensity", 1.0F);
                mat.env_intensity = read_num(dmat, base, "env_intensity", 1.0F);
                mat.normal_scale_y = read_num(dmat, base, "normal_scale_y", 1.0F);
                if (const mq_field_desc* f = mq_reflect_field(dmat, "specular"))
                    std::memcpy(mat.specular, base + f->offset, sizeof mat.specular);
                mat.shininess = read_num(dmat, base, "shininess", 30.0F);
                mat.specular_intensity = read_num(dmat, base, "specular_intensity", 1.0F);
                mat.ior = read_num(dmat, base, "ior", 1.5F);
                mat.sheen = read_num(dmat, base, "sheen", 0.0F);
                mat.sheen_roughness = read_num(dmat, base, "sheen_roughness", 0.5F);
                if (const mq_field_desc* f = mq_reflect_field(dmat, "sheen_color"))
                    std::memcpy(mat.sheen_color, base + f->offset, sizeof mat.sheen_color);
                mat.iridescence = read_num(dmat, base, "iridescence", 0.0F);
                mat.iridescence_ior = read_num(dmat, base, "iridescence_ior", 1.3F);
                mat.iridescence_thickness_min = read_num(dmat, base, "iridescence_thickness_min", 100.0F);
                mat.iridescence_thickness_max = read_num(dmat, base, "iridescence_thickness_max", 400.0F);
                mat.anisotropy = read_num(dmat, base, "anisotropy", 0.0F);
                mat.anisotropy_rotation = read_num(dmat, base, "anisotropy_rotation", 0.0F);
                /* KHR_materials_transmission 近似参数：shader 用 Fresnel 视角 alpha、gfx 画两遍（背面→正面）。
                 * Draw 的 anisotropy_rotation 槽 shader 未使用，复用它传 transmission factor（避免扩 uniform）。
                 * ⚠️ 必须写在上面读取材质自身 anisotropy_rotation **之后**：否则会被无条件覆盖为 0，
                 *    shader 收到的 Fresnel 权重恒 0（玻璃退化为均匀半透"幽灵"，CompareDispersion 即此坑）。 */
                const float transmission_factor = read_num(dmat, base, "transmission_factor", 0.0F);
                if (transmission_factor > 0.001F) {
                    mat.anisotropy_rotation = transmission_factor;
                    mat.flags2 |= 0x100000;             /* bit20 = transmission approx */
                }
                /* KHR_materials_volume / KHR_materials_dispersion：折射厚度与色散近似参数
                 * （gfx 打包进 Draw.clearcoat.z / Draw.alpha.w，材质 uniform 仍是 288B 不扩）。
                 * 缺省 0 = 无体积折射/色散（旧材质零行为变化）。 */
                mat.volume_thickness = read_num(dmat, base, "volume_thickness", 0.0F);
                mat.dispersion = read_num(dmat, base, "dispersion", 0.0F);
                /* KHR_materials_volume：thicknessFactor/thicknessTexture 给的是 **mesh 空间** 厚度，
                 * 规范要求随节点变换缩放；attenuationDistance 是**世界空间**。gfx/shader 把它当世界
                 * 空间厚度用（Beer-Lambert 路径长 + 折射偏移），这里先乘节点世界缩放（非均匀用几何均值
                 * 近似路径长）。漏乘时小尺度资产（USDShaderBall 节点 scale=0.01）光学厚度 ×100 → 发黑。 */
                if (mat.volume_thickness > 0.0F) {
                    const float vscale = std::cbrt(std::abs(trs.scale.x * trs.scale.y * trs.scale.z));
                    if (vscale > 1.0e-6F) mat.volume_thickness *= vscale;
                }
                /* KHR_materials_diffuse_transmission：**不透明**薄壁透光（不再 alpha BLEND，
                 * 否则陶瓷/叶片会变幽灵玻璃）。factor → shader 背光透射项；flags2 bit24 标记。 */
                const float diffuse_transmission_factor =
                    read_num(dmat, base, "diffuse_transmission_factor", 0.0F);
                if (diffuse_transmission_factor > 0.001F) {
                    mat.diffuse_transmission_factor = diffuse_transmission_factor;
                    math::Vec3 dtc{ 1.0F, 1.0F, 1.0F };
                    if (const mq_field_desc* f = mq_reflect_field(dmat, "diffuse_transmission_color"))
                        std::memcpy(&dtc, base + f->offset, sizeof dtc);
                    mat.diffuse_transmission_color[0] = dtc.x;
                    mat.diffuse_transmission_color[1] = dtc.y;
                    mat.diffuse_transmission_color[2] = dtc.z;
                    mat.flags2 |= 0x1000000;            /* bit24 = diffuse transmission approx */
                }
                /* KHR_materials_volume（第 24 轮）：Beer-Lambert 吸收（attenuationColor/Distance）+
                 * thicknessTexture.G（厚度乘子）+ volume_scatter 的 multiscatterColorFactor。 */
                {
                    math::Vec3 att{ 1.0F, 1.0F, 1.0F };
                    if (const mq_field_desc* f = mq_reflect_field(dmat, "attenuation_color"))
                        std::memcpy(&att, base + f->offset, sizeof att);
                    mat.attenuation_color[0] = att.x;
                    mat.attenuation_color[1] = att.y;
                    mat.attenuation_color[2] = att.z;
                    mat.attenuation_distance = read_num(dmat, base, "attenuation_distance", 0.0F);
                    math::Vec3 msc{ 0.0F, 0.0F, 0.0F };
                    if (const mq_field_desc* f = mq_reflect_field(dmat, "multiscatter_color"))
                        std::memcpy(&msc, base + f->offset, sizeof msc);
                    mat.multiscatter_color[0] = msc.x;
                    mat.multiscatter_color[1] = msc.y;
                    mat.multiscatter_color[2] = msc.z;
                }
                mat.clearcoat_normal_scale = read_num(dmat, base, "clearcoat_normal_scale", 1.0F);
                mat.toon_steps = (int32_t)read_num(dmat, base, "toon_steps", 3.0F);
                mat.depth_packing = (int32_t)read_num(dmat, base, "depth_packing", 0.0F);
                if (read_num(dmat, base, "flat_shading", 0.0F) >= 0.5F) mat.flags2 |= 1;
                /* KHR_texture_transform（plans/19）：基础贴图用 uv_scale/uv_offset，法线单独一组 */
                if (const mq_field_desc* f = mq_reflect_field(dmat, "uv_scale")) {
                    math::Vec2 v{ 1.0F, 1.0F };
                    std::memcpy(&v, base + f->offset, sizeof v);
                    mat.uv_scale[0] = v.x; mat.uv_scale[1] = v.y;
                }
                if (const mq_field_desc* f = mq_reflect_field(dmat, "uv_offset")) {
                    math::Vec2 v{ 0.0F, 0.0F };
                    std::memcpy(&v, base + f->offset, sizeof v);
                    mat.uv_offset[0] = v.x; mat.uv_offset[1] = v.y;
                }
                if (const mq_field_desc* f = mq_reflect_field(dmat, "normal_uv_scale")) {
                    math::Vec2 v{ 1.0F, 1.0F };
                    std::memcpy(&v, base + f->offset, sizeof v);
                    mat.normal_uv_scale[0] = v.x; mat.normal_uv_scale[1] = v.y;
                }
                if (const mq_field_desc* f = mq_reflect_field(dmat, "normal_uv_offset")) {
                    math::Vec2 v{ 0.0F, 0.0F };
                    std::memcpy(&v, base + f->offset, sizeof v);
                    mat.normal_uv_offset[0] = v.x; mat.normal_uv_offset[1] = v.y;
                }
                /* glTF TEXCOORD_1（多 UV 集）：uv1_mask bit0..7 = 8 个**基础**贴图槽是否用第二套 UV；
                 * 折进 flags2 bit4..11（uv1_sel），bit12..18 留给 KHR 扩展贴图 presence（见下），
                 * 故这里只取低 8 位（0xFF）——用 0x1FF 会占用 bit12。
                 * 只有 vertex_layout=1 的网格才有有效 uv1；layout0 的 vs 写 uv1=0，mask=0 时无影响。 */
                {
                    const int32_t uv1_mask = (int32_t)read_num(dmat, base, "uv1_mask", 0.0F);
                    mat.flags2 |= (uv1_mask & 0xFF) << 4;
                }
                /* M4：PBR 贴图槽（albedo / metallic-roughness / occlusion）；normal 在 M5（需要切线空间） */
                if (const std::string* texref = read_str(dmat, base, "base_color_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[0] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr albedo texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = read_str(dmat, base, "metallic_roughness_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[1] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr mr texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = read_str(dmat, base, "occlusion_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[2] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr ao texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = read_str(dmat, base, "normal_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[3] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr normal texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = read_str(dmat, base, "emissive_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[4] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr emissive texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* plans/19：matcap / toon 渐变 / clearcoat 法线三个新贴图槽（flags2 位告知 shader 是否有图） */
                /* KHR_texture_transform.rotation（弧度；glTF T*R*S 顺序，shader 里先缩放再旋转再平移） */
    mat.uv_rotation = read_num(dmat, base, "uv_rotation", 0.0F);
    mat.normal_uv_rotation = read_num(dmat, base, "normal_uv_rotation", 0.0F);
    if (const std::string* texref = read_str(dmat, base, "matcap_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[0] = tid; mat.flags2 |= 2; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: matcap texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = read_str(dmat, base, "gradient_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_ref(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[1] = tid; mat.flags2 |= 4; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: toon gradient texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = read_str(dmat, base, "clearcoat_normal_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[2] = tid; mat.flags2 |= 8; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: clearcoat normal texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* KHR 扩展材质贴图槽（plans/20）：7 槽 → mat.textures3[]，存在项置 flags2 presence bit12..18。
                 * 槽序（与 gfx binding 10..16 / shader 一致）：
                 *   0 clearcoat(R)          1 clearcoat_roughness(G)  2 sheen_color(RGB, sRGB)
                 *   3 sheen_roughness(A)    4 iridescence(R)          5 iridescence_thickness(G)
                 *   6 anisotropy(RG=方向, B=强度)
                 * 除 sheen_color 是颜色贴图（sRGB）外，其余都是线性数据贴图（G/A/R 通道语义由 shader 解释）。
                 * 缺失/空 → textures3 保持 -1、presence=0：shader 走旧路径（旧材质零行为变化）。 */
                {
                    auto load_ext_tex = [&](const char* field, int slot, bool srgb) {
                        const std::string* texref = read_str(dmat, base, field);
                        if (!texref || texref->empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, *texref)) : load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) {
                            mat.textures3[slot] = tid;
                            mat.flags2 |= 1 << (12 + slot);          /* presence：bit12..18 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr ext texture {} '{}' -> {}\n", field, *texref, tid);
                    };
                    load_ext_tex("clearcoat_texture", 0, false);
                    load_ext_tex("clearcoat_roughness_texture", 1, false);
                    load_ext_tex("sheen_color_texture", 2, true);
                    load_ext_tex("sheen_roughness_texture", 3, false);
                    load_ext_tex("iridescence_texture", 4, false);
                    load_ext_tex("iridescence_thickness_texture", 5, false);
                    load_ext_tex("anisotropy_texture", 6, false);
                }
                /* KHR_materials_specular 两槽（flags2 bit21/22）：specularTexture.A=specularFactor（线性）、
                 * specularColorTexture.RGB=specularColorFactor（sRGB）；shader 只作用于电介质 F0。 */
                {
                    auto load_spec_tex = [&](const char* field, int slot, bool srgb) {
                        const std::string* texref = read_str(dmat, base, field);
                        if (!texref || texref->empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, *texref)) : load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) {
                            mat.textures4[slot] = tid;
                            mat.flags2 |= 1 << (21 + slot);          /* presence：bit21/22 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr specular texture {} '{}' -> {}\n", field, *texref, tid);
                    };
                    load_spec_tex("specular_texture", 0, false);
                    load_spec_tex("specular_color_texture", 1, true);
                }
                /* KHR_materials_transmission 贴图槽（尾部追加）：R = 透射率掩码（0=不透、1=全透）。
                 * 线性数据贴图 → load_texture_mips（不是 sRGB）；句柄放 textures5[0]，
                 * 存在时置 flags2 bit23（presence；gfx 在 group1 新 binding 绑定）。空/失败保持 -1、presence=0。 */
                if (const std::string* texref = read_str(dmat, base, "transmission_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures5[0] = tid; mat.flags2 |= 0x800000; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: transmission texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* KHR_materials_diffuse_transmission 两槽（尾部追加）：
                 * diffuseTransmissionTexture.A = 透射率乘子（线性数据，load_texture_mips）；
                 * diffuseTransmissionColorTexture.RGB = 透射色（sRGB）。presence = flags2 bit25/26，
                 * gfx 绑定在 group1 binding 20/21；空/失败保持 -1、presence=0。 */
                {
                    auto load_dt_tex = [&](const char* field, int slot, bool srgb) {
                        const std::string* texref = read_str(dmat, base, field);
                        if (!texref || texref->empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, *texref)) : load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) {
                            mat.textures6[slot] = tid;
                            mat.flags2 |= 1 << (25 + slot);          /* presence：bit25/26 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr diffuse-transmission texture {} '{}' -> {}\n",
                                       field, *texref, tid);
                    };
                    load_dt_tex("diffuse_transmission_texture", 0, false);
                    load_dt_tex("diffuse_transmission_color_texture", 1, true);
                }
                /* KHR_materials_volume thicknessTexture.G（线性数据；flags2 bit27 presence，
                 * gfx 绑定在 group1 binding 22）。 */
                if (const std::string* texref = read_str(dmat, base, "volume_thickness_texture")) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures7[0] = tid; mat.flags2 |= 0x8000000; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: volume thickness texture '{}' -> {}\n", *texref, tid);
                    }
                }
            }
            if (std::getenv("MAQUE_RENDER3D_INFO"))
                std::print("runtime: 3d material node={} model={} side={} flags2={} tex2=({},{},{})\n",
                           idx, mat.shading_model, mat.side, mat.flags2,
                           mat.textures2[0], mat.textures2[1], mat.textures2[2]);
            const int32_t mat_index = (int32_t)out_mats.size();
            out_mats.push_back(mat);
            /* M4c：mesh_path 非空时用离线导入网格（glTF → .mesh.json），否则内建 cube */
            int32_t mesh_id = cube_mesh;
            std::string mesh_path_str;
            if (const std::string* mp = read_str(dmesh, mb->data(), "mesh_path")) {
                if (!mp->empty()) {
                    mesh_path_str = *mp;
                    const int32_t loaded = load_mesh(*mp);
                    if (loaded >= 0) mesh_id = loaded;
                    if (std::getenv("MAQUE_RENDER3D_INFO"))
                        std::print("runtime: mesh renderer node={} path='{}' → mesh={}\n", idx, *mp, loaded);
                }
            }
            /* ---- 蒙皮（plans/22 §6.3）：调色板 palette[j] = world_m(joint) * inverse_bind[j] ----
             * 蒙皮网格忽略节点变换（glTF 规范）→ dr.model = identity、dr.skin = frame3d.skins 下标。
             * 解析失败 → 告警一次并回退静态（dr.model = world_m、skin = -1）；
             * MAQUE_SKIN=0 → 调色板填 identity（bind pose 不变量，用于 e2e A/B 对照）。 */
            int32_t skin_index = -1;
            float model_m[16];
            std::memcpy(model_m, wm, sizeof model_m);
            const std::string* skin_path = read_str(dmesh, mb->data(), "skin_path");
            if (skin_path && !skin_path->empty()) {
                auto lay_it = mesh_layout.find(mesh_path_str);
                const bool skinned_ok = !mesh_path_str.empty() && lay_it != mesh_layout.end() &&
                                        lay_it->second >= 2;
                if (!skinned_ok) {
                    if (!gltf_warned.count("skin-layout:" + *skin_path)) {
                        gltf_warned.insert("skin-layout:" + *skin_path);
                        std::print(stderr, "runtime: 蒙皮网格 '{}/{}' 布局缺失或载入失败，回退静态\n",
                                   mesh_path_str, *skin_path);
                    }
                } else {
                    const bool sk_loaded = load_skeleton(*skin_path);
                    auto sit = skeletons.find(*skin_path);
                    const Skeleton* sk = (sk_loaded && sit != skeletons.end() && !sit->second.failed)
                                             ? &sit->second : nullptr;
                    const size_t nj = sk ? sk->joint_names.size() : 0;
                    const bool sk_valid = sk && nj > 0 && sk->inverse_bind.size() == nj * 16;
                    bool built = false;
                    if (sk_valid) {
                        std::vector<float> pal(nj * 16);
                        if (!skin_enabled) {
                            /* MAQUE_SKIN=0：identity 调色板（joint_count 保留）→ bind pose 不变量 */
                            for (size_t j = 0; j < nj; ++j) m4_identity(pal.data() + j * 16);
                            built = true;
                        } else if (!entity_by_name.empty()) {
                            built = true;
                            for (size_t j = 0; j < nj; ++j) {
                                auto jhit = entity_by_name.find(sk->joint_names[j]);
                                const Entity* je = jhit == entity_by_name.end()
                                                       ? nullptr : scene->at(jhit->second);
                                if (!je) { built = false; break; }
                                m4_mul(je->world_m, sk->inverse_bind.data() + j * 16,
                                       pal.data() + j * 16);
                            }
                        }
                        if (built) {
                            skin_palettes.push_back(std::move(pal));
                            mq_renderer_skin rs{};
                            rs.palette = skin_palettes.back().data();
                            rs.joint_count = (uint32_t)nj;
                            out_skins.push_back(rs);
                            skin_index = (int32_t)out_skins.size() - 1;
                            m4_identity(model_m);            /* 蒙皮网格忽略节点世界变换 */
                        }
                    }
                    if (!built && !gltf_warned.count("skin-joint:" + *skin_path)) {
                        gltf_warned.insert("skin-joint:" + *skin_path);
                        std::print(stderr, "runtime: 蒙皮 '{}/{}' 关节解析失败，回退静态\n",
                                   mesh_path_str, *skin_path);
                    }
                }
            }
            /* ---- CPU morph（plans/22 §6.4）：权重 = 动画覆盖 > 默认；权变才重算 + update_mesh ---- */
            if (!mesh_path_str.empty()) {
                auto mit = morph_meshes.find(mesh_path_str);
                if (mit != morph_meshes.end() && mit->second.mesh >= 0 && !mit->second.targets.empty()) {
                    MorphMesh& mm = mit->second;
                    auto oit = morph_weight_override.find(idx);
                    const std::vector<float>& mw = (oit != morph_weight_override.end())
                                                       ? oit->second : mm.default_weights;
                    if (mm.owner_entity < 0) mm.owner_entity = idx;
                    if (mm.owner_entity == idx) {
                        if (mm.applied_weights != mw) (void)apply_morph(mesh_path_str, mw);
                    } else if (!mm.warned_shared) {
                        mm.warned_shared = true;
                        std::print(stderr, "runtime: mesh '{}' 被多个实体共享，morph 只取首实体 entity={}\n",
                                   mesh_path_str, mm.owner_entity);
                    }
                }
            }
            mq_renderer_draw3d dr{};
            dr.mesh = mesh_id;
            dr.material = mat_index;
            std::memcpy(dr.model, model_m, sizeof dr.model);
            dr.skin = skin_index;
            dr.flags |= 0x40000000;            /* draw.layer 是有效 culling mask（旧调用方无此位） */
            dr.layer = -1;
            if (const mq_field_desc* f = mq_reflect_field(dmesh, "culling_mask"))
                dr.layer = (int32_t)*(const uint32_t*)(mb->data() + f->offset);
            dr.sort_key = (uint64_t)idx;
            out_draws.push_back(dr);
        }
    }

    /* OrbitControls：仅主相机绕世界原点环绕；RenderTexture 相机保持节点自身变换。 */
    if (allow_orbit) if (const char* oo = std::getenv("MAQUE_ORBIT")) {
        float oy = 0.0F, op = 0.3F, od = 1.0F;
        if (std::sscanf(oo, "%f,%f,%f", &oy, &op, &od) >= 1) {
            orbit_yaw = oy; orbit_pitch = op; orbit_dist = od > 0.0F ? od : 1.0F;
            orbit_enabled = true; orbit_inited = true;
        }
    }
    if (allow_orbit && orbit_enabled) {
        if (!orbit_inited) {
            const float ex = cam_trs.position.x, ey = cam_trs.position.y, ez = cam_trs.position.z;
            float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (d < 1e-4F) d = 1.0F;
            orbit_dist = d;
            orbit_pitch = std::asin(std::max(-1.0F, std::min(1.0F, ey / d)));
            orbit_yaw = std::atan2(ex, ez);
            orbit_inited = true;
        }
        const float cp = std::cos(orbit_pitch), sp = std::sin(orbit_pitch);
        const float sy = std::sin(orbit_yaw), cy = std::cos(orbit_yaw);
        const float px = sy * cp * orbit_dist, py = sp * orbit_dist, pz = cy * cp * orbit_dist;
        cam_trs.position = math::Vec3{ px, py, pz };
        cam_trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
        const float fl = std::sqrt(px * px + py * py + pz * pz);
        if (fl > 1e-6F) {
            const float fx = -px / fl, fy = -py / fl, fz = -pz / fl;
            cam_trs.rotation = math::Vec3{ std::asin(std::max(-1.0F, std::min(1.0F, fy))),
                                           std::atan2(-fx, -fz), 0.0F };
        }
        m4_from_trs(cam_trs, cam_wm);        /* Orbit 覆盖相机：世界矩阵 = 绕世界原点环绕 */
        has_cam_wm = true;
        (void)has_cam;
    }
    if (gltf_lights_added) {
        std::stable_partition(out_lights.begin(), out_lights.end(),
                              [](const mq_renderer_light& L) { return L.type == 0; });
        if (out_lights.size() > 8) out_lights.resize(8);
    }
    opt_last_sun = false;
    if (out_lights.empty()) {                              /* 无方向光：给一盏默认主光，避免全黑 */
        /* F4 引擎选项：默认太阳光可关 / 调强度 / 调方向（仅"场景没有任何光源"这条回退路径）。 */
        if (opt_sun_enabled) {
            mq_renderer_light L{};
            L.type = 0;
            /* 方向全 0 会让 shader 的 normalize 退化 → 给一个合法方向（面板已夹，环境变量入口也兜住） */
            float dx = opt_sun_dir[0], dy = opt_sun_dir[1], dz = opt_sun_dir[2];
            if (std::fabs(dx) + std::fabs(dy) + std::fabs(dz) < 1e-4F) dz = -1.0F;
            L.dir[0] = dx; L.dir[1] = dy; L.dir[2] = dz;
            L.color[0] = L.color[1] = L.color[2] = 1.0F;
            L.intensity = opt_sun_intensity;
            out_lights.push_back(L);
            opt_last_sun = true;
        }
    }

    /* F4 引擎选项：主相机 near/far 覆盖（不写场景组件）。
     * 覆盖关闭时把**场景实际值**回填进选项字段 —— 面板里一勾选"覆盖"就等同当前画面，不跳变。 */
    if (allow_orbit && opt_cam_override) {
        near_z = std::max(opt_cam_near, 1e-4F);
        far_z = std::max(opt_cam_far, near_z + 1e-3F);
    } else if (allow_orbit) {
        opt_cam_near = near_z;
        opt_cam_far = far_z;
    }
    if (allow_orbit) {
        opt_last_near = near_z;
        opt_last_far = far_z;
    }

    mq_renderer_view3d v{};
    if (camera_entity >= 0 && !has_cam) return;       /* RenderTexture 必须由同节点的有效 Camera3D 驱动 */
    /* 相机 view 由**世界矩阵**派生（plans/22 §6.3）：默认相机 / 平级场景与旧 m4_view(trs) 逐字节一致 */
    if (!has_cam_wm) m4_from_trs(cam_trs, cam_wm);
    float view[16], proj[16];
    m4_view_from_m(cam_wm, view);
    const float vx = std::clamp(camera_viewport.x, 0.0F, 1.0F);
    const float vy = std::clamp(camera_viewport.y, 0.0F, 1.0F);
    const float vw = std::clamp(camera_viewport.width, 0.0F, 1.0F - vx);
    const float vh = std::clamp(camera_viewport.height, 0.0F, 1.0F - vy);
    const float aspect = ((float)tw * std::max(vw, 1e-5F)) /
                         ((float)th * std::max(vh, 1e-5F));
    if (projection == 1) m4_orthographic(ortho_height, aspect, near_z, far_z, proj);
    else m4_perspective(fov_y * 3.14159265F / 180.0F, aspect, near_z, far_z, proj);
    std::memcpy(v.view, view, sizeof view);
    std::memcpy(v.proj, proj, sizeof proj);
    v.eye[0] = cam_wm[12]; v.eye[1] = cam_wm[13]; v.eye[2] = cam_wm[14];
    v.viewport[0] = vx * tw; v.viewport[1] = vy * th;
    v.viewport[2] = std::max(1.0F, vw * tw); v.viewport[3] = std::max(1.0F, vh * th);
    v.exposure = exposure;
    v.clear_mode = clear_mode;
    v.clear_color[0] = clear.r; v.clear_color[1] = clear.g;
    v.clear_color[2] = clear.b; v.clear_color[3] = clear.a;
    v.layer_mask = camera_mask;
    out_views.push_back(v);
    if (render3d_info && !gltf_frame_info) {
        gltf_frame_info = true;
        std::print("runtime: 3d frame0 draws={} skins={} morph={} anim_pose={} names={} "
                   "lights={} mats={}\n",
                   (long long)out_draws.size(), (long long)out_skins.size(),
                   (long long)morph_meshes.size(), (long long)anim_pose.size(),
                   (long long)entity_by_name.size(), (long long)out_lights.size(),
                   (long long)out_mats.size());
    }
}

void Engine::step_frame(bool render_frame) {
    MQ_TRACY_ZONE(runtime);                          // Tracy：一次模拟步（含渲染+present）总 zone
    tracy_tick();                                    // Tracy：MAQUE_TRACY 环境启动 + 1Hz CPU/内存采样
    engine_options_parse_env();                      // F4 引擎选项的启动初值（MAQUE_ENGINE_OPTIONS；只解析一次）
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
            if (k == MQ_KEY_F2 || k == MQ_KEY_F4) continue;   // 引擎调试面板热键（不传给游戏）
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
            MQ_TRACY_ZONE_N(anim, "tween");          // Tween（补间）
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
        { MQ_TRACY_ZONE_N(anim, "particles"); particles_step(dt); }      // 粒子推进（docs/15 §4）
        { MQ_TRACY_ZONE_N(nav, "nav.step"); nav_step(); }                // 导航代理推进（A* + 追踪）
        { MQ_TRACY_ZONE_N(physics, "physics.step"); physics_components_step(dt); }   // 组件驱动物理
        flush_pending_destroy();                         // node.destroy()：帧边界统一销毁
        { MQ_TRACY_ZONE_N(ui, "ui.step"); ui_step(); }   // UI 组件语义（layout/progress/slider/...）
        { MQ_TRACY_ZONE_N(runtime, "flatten"); scene->flatten_world(*this); }
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
    if (!render_frame) {
        ++frame;
        return;
    }
    // ⑤ 渲染（只读快照 → 渲染不回写，docs/13 §8）
    const int SWpx = (int)(W * ss + 0.5F), SHpx = (int)(H * ss + 0.5F);
    const auto stat_render0 = std::chrono::steady_clock::now();
    {
        MQ_TRACY_ZONE_N(render, "render_textures");
        render_camera_textures(draws, spine_meshes);
    }
    std::vector<uint8_t> pixels((size_t)TW * SHpx * 4);
    /* 直接呈现（plans/18 M1）：wgpu 交换链直呈，**不经 CPU readback**。
     * 截图/抓帧（--shot / --shot-seq / frames_dir）仍走离屏回读（out 非空）→ CI 像素断言不变。 */
    const bool need_pixels = !shot_path.empty() || !shot_seq.empty() || !frames_dir.empty();
    if ((has_3d || has_shader_pass) && rapi && rself && rapi->render_frame3d) {
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
        fr.present_x = (int32_t)std::lround(canvas.offset_x);
        fr.present_y = (int32_t)std::lround(canvas.offset_y);
        fr.present_w = (int32_t)std::lround(canvas.viewport_w);
        fr.present_h = (int32_t)std::lround(canvas.viewport_h);
        fr.render_mode = opt_render_mode;
        for (int i = 0; i < 4; ++i) fr.bg[i] = bg[i];
        /* MSAA：场景字段 scn.Scene.msaa_samples（0/1=关、4=4x），MAQUE_MSAA 覆盖（0/1/4）。
         * 默认关 → 148 模型批量预览与旧场景逐像素不变；gfx 不支持 4x 时自动回退。
         * F4 引擎选项（opt_msaa > 0）优先级最高：面板上显式选择的 关/4x 覆盖场景与 MAQUE_MSAA。 */
        int msaa_samples = scene ? scene->desc.msaa_samples : 0;
        if (const char* msaa_env = std::getenv("MAQUE_MSAA")) msaa_samples = std::atoi(msaa_env);
        if (opt_msaa > 0) msaa_samples = opt_msaa;
        fr.msaa_samples = (msaa_samples >= 4) ? 4 : 1;
        opt_last_msaa = fr.msaa_samples;
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
        const bool direct3d = direct_present && !need_pixels;
        fr.out = direct3d ? nullptr : pixels.data();
        fr.out_size = (uint32_t)pixels.size();
        const int32_t rc = rapi->render_frame3d(rself, &fr);
        if (rc == 0 && direct3d) {
            if (rapi->present) rapi->present(rself);
        } else if (rc != 0 && !(direct3d && rc == -7)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::print(stderr, "runtime: 3D frame 渲染失败 rc={} → 本帧回退 2D\n", rc);
            }
            if (direct3d) {
                mq_renderer_window none{};
                (void)rapi->attach_window(rself, &none);
                direct_present = false;
                direct_requested = false;
                direct_window_handle = nullptr;
            } else {
                has_3d = false;
            }
        }
    } else if (direct_present && !need_pixels && rapi && rself && rapi->render_frame3d) {
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
        f.present_x = (int32_t)std::lround(canvas.offset_x);
        f.present_y = (int32_t)std::lround(canvas.offset_y);
        f.present_w = (int32_t)std::lround(canvas.viewport_w);
        f.present_h = (int32_t)std::lround(canvas.viewport_h);
        for (int i = 0; i < 4; ++i) f.bg[i] = bg[i];
        f.env.sky_mode = 1;                       /* M1：背景色直出（3D 天空盒在 M3） */
        f.env.env_texture = -1;                   /* 2D 直呈路径无 3D env（避免 0 号纹理被误当 env） */
        f.env.env_irradiance_texture = -1;
        for (int i = 0; i < 4; ++i) f.env.sky_color[i] = bg[i];
        const int32_t rc = rapi->render_frame3d(rself, &f);
        if (rc == 0 && rapi->present) rapi->present(rself);
        else if (rc != -7) {
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
        /* 目标矩形按**画布策略**（缩放/留边由分辨率策略决定；整倍放大用最近邻） */
        const int dx = (int)(canvas.offset_x + 0.5F), dy = (int)(canvas.offset_y + 0.5F);
        const int dw = (int)(canvas.viewport_w + 0.5F), dh = (int)(canvas.viewport_h + 0.5F);
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
    if (!gltf_env_read) read_gltf_env();
    gltf_anim_step(dt);                                   /* plans/24：直读 glTF 的实体级动画/蒙皮/morph */
    /* plans/22 §6.2：派生姿态表每帧重建（纯函数 = (time, clip)；MAQUE_ANIM=0 时保持空表 = bind pose）。 */
    anim_pose.clear();
    morph_weight_override.clear();
    std::vector<int> ids;
    for (auto& [idx, e] : scene->ents) ids.push_back(idx);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d || !(d->role & MQ_ROLE_ANIM_PLAYER)) continue;
            /* glTF 片段通路：clips 非空 → 只走新逻辑（旧 clip 通路完全不变） */
            if (const mq_field_desc* fc = mq_reflect_field(d, "clips")) {
                if (fc->kind == MQ_TK_ARRAY_DYN && fc->container_size &&
                    fc->container_size(blob.data() + fc->offset) > 0) {
                    anim_step_gclip(id, d, blob, dt);
                    continue;
                }
            }
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

/* 音频组件（docs/30 §6）：AudioSource/AudioListener 按 role 扫描（不硬编码类型），
 * 所有字段走反射；只写 transient（state/playhead/voice），不改配置字段。 */
void Engine::audio_step(float dt) {
    (void)dt;
    if (!audio || !scene) return;
    std::vector<int> ids;
    ids.reserve(scene->ents.size());
    for (auto& [k, e] : scene->ents) ids.push_back(k);
    std::sort(ids.begin(), ids.end());   // 稳定顺序（docs/10 D4）

    /* 听者：每 instance 取稳定排序后的第一个 active */
    const mq_type_desc* listener_desc = nullptr;
    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        auto* lb = comp_by_role(*this, id, MQ_ROLE_AUDIO_LISTENER);
        if (!lb) continue;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (d && (d->role & MQ_ROLE_AUDIO_LISTENER)) { listener_desc = d; break; }
        }
        if (!listener_desc) continue;
        if (read_num(listener_desc, lb->data(), "active", 1.0F) <= 0.0F) continue;
        const float rad = e->angle * 3.14159265358979F / 180.0F;
        mq_audio_listener l{};
        l.struct_size = sizeof l;
        l.x = e->world_x;
        l.y = e->world_y;
        l.z = 0.0F;
        l.dir_x = std::cos(rad);
        l.dir_y = std::sin(rad);
        l.dir_z = 0.0F;
        mq_audio_set_listener(audio, &l);
        const float master = read_num(listener_desc, lb->data(), "master_volume", 1.0F);
        if (master != last_listener_master) {
            mq_audio_set_master(audio, master, 0);
            last_listener_master = master;
        }
        break;
    }

    for (int id : ids) {
        Entity* e = scene->at(id);
        if (!e) continue;
        auto* sb = comp_by_role(*this, id, MQ_ROLE_AUDIO_SOURCE);
        if (!sb) continue;
        const mq_type_desc* d = nullptr;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* t = mq_reflect_type_by_id(reflect, tid);
            if (t && (t->role & MQ_ROLE_AUDIO_SOURCE)) { d = t; break; }
        }
        if (!d) continue;
        uint8_t* data = sb->data();
        const mq_field_desc* fv = mq_reflect_field(d, "voice");
        uint64_t voice = fv ? *(uint64_t*)(data + fv->offset) : 0;
        const std::string* clip_path = read_str(d, data, "clip");
        if (!clip_path || clip_path->empty()) {
            if (voice) {
                mq_audio_stop(audio, voice);
                if (fv) *(uint64_t*)(data + fv->offset) = 0;
            }
            continue;
        }
        const bool autoplay = read_num(d, data, "autoplay", 0.0F) > 0.0F;
        const bool loop = read_num(d, data, "loop", 0.0F) > 0.0F;
        const float volume = read_num(d, data, "volume", 1.0F);
        const float pitch = read_num(d, data, "pitch", 1.0F);
        const float min_d = read_num(d, data, "min_distance", 1.0F);
        const float max_d = read_num(d, data, "max_distance", 50.0F);
        const uint32_t panning = (uint32_t)read_num(d, data, "panning", 0.0F);
        const uint32_t attenuation = (uint32_t)read_num(d, data, "attenuation", 0.0F);
        const std::string* bus = read_str(d, data, "bus");

        if (voice == 0) {
            if (!autoplay) continue;
            if (std::getenv("MAQUE_TRACE_AUDIO"))
                std::print("[audio] start id={} type={} clip='{}'\n", id, d->name, *clip_path);
            mq_audio_clip* clip = audio_clip(*clip_path);
            if (!clip) continue;
            mq_audio_voice_desc vd{};
            vd.struct_size = sizeof vd;
            vd.clip = clip;
            vd.volume = volume;
            vd.pitch = pitch;
            vd.loop = loop ? 1u : 0u;
            vd.bus = (bus && !bus->empty()) ? bus->c_str() : "sfx";
            vd.panning = panning;
            vd.attenuation = attenuation;
            vd.min_distance = min_d;
            vd.max_distance = max_d;
            vd.x = e->world_x;
            vd.y = e->world_y;
            vd.z = 0.0F;
            voice = mq_audio_play(audio, &vd);
            if (std::getenv("MAQUE_TRACE_AUDIO")) {
                uint32_t fr = 0, rt = 0, chn = 0;
                mq_audio_clip_info(clip, &fr, &rt, &chn);
                std::print("[audio] start voice={} frames={} rate={} ch={}\n", voice, fr, rt, chn);
            }
            if (!voice) continue;
            if (fv) *(uint64_t*)(data + fv->offset) = voice;
        }
        /* 每帧同步可调参数（脚本可用 node_set_number 改 volume 等） */
        mq_audio_set_volume(audio, voice, volume);
        mq_audio_set_pitch(audio, voice, pitch);
        mq_audio_set_spatial(audio, voice, e->world_x, e->world_y, 0.0F, panning, attenuation,
                             min_d, max_d);
        uint32_t st = 0;
        float head = 0.0F;
        if (mq_audio_voice_state(audio, voice, &st, &head) == MQ_AUDIO_OK) {
            if (const mq_field_desc* fs = mq_reflect_field(d, "state"))
                *(int32_t*)(data + fs->offset) = (int32_t)st;
            if (const mq_field_desc* fh = mq_reflect_field(d, "playhead"))
                *(float*)(data + fh->offset) = head;
            (void)0;
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
/* 切换场景时**必须**清空资源缓存（docs/32 §5.1 资源基准）：缓存键是组件里的**相对路径**
 *（如 khronos 每个模型的 "textures/image0.png"），而 VFS 根挂载点已换成新场景目录 ——
 * 不清会直接命中上一个场景的纹理/网格，表现为“换了场景但贴图还是上一个模型”。 */
void Engine::clear_asset_caches() {
    destroy_render_textures();   /* 先擦除 tex_cache 中的虚拟 URI/别名并释放一次，避免下方重复销毁 */
    const size_t nt = tex_cache.size(), nm = mesh_cache.size(), ne = env_tex_cache.size();
    const size_t nec = env_cube_cache.size();
    const size_t ng = gclips.size(), ns = skeletons.size(), nmo = morph_meshes.size();
    const size_t ngl = gltf_models.size();
    if (rapi && rself) {
        if (rapi->destroy_texture) {
            for (auto& [k, id] : tex_cache) if (id >= 0) rapi->destroy_texture(rself, id);
            for (auto& [k, pair] : env_tex_cache) {
                if (pair.first >= 0) rapi->destroy_texture(rself, pair.first);
                if (pair.second >= 0) rapi->destroy_texture(rself, pair.second);
            }
            for (auto& [k, pair] : env_cube_cache) {
                if (pair.first >= 0) rapi->destroy_texture(rself, pair.first);
                if (pair.second >= 0) rapi->destroy_texture(rself, pair.second);
            }
            for (auto& [k, gm] : gltf_models)
                for (int32_t t : gm.textures)
                    if (t >= 0) rapi->destroy_texture(rself, t);
        }
        if (rapi->destroy_mesh) {
            for (auto& [k, id] : mesh_cache) if (id >= 0) rapi->destroy_mesh(rself, id);
            for (auto& [k, gm] : gltf_models)
                for (const auto& pr : gm.prims)
                    if (pr.mesh >= 0) rapi->destroy_mesh(rself, pr.mesh);
        }
    }
    for (const auto& [k, id] : mesh_cache) mesh_stats.erase(id);
    for (const auto& [k, gm] : gltf_models)
        for (const auto& pr : gm.prims) mesh_stats.erase(pr.mesh);
    /* cube_mesh 跨场景保留，其 stats 也保留；其余句柄随缓存销毁同步失效。 */
    tex_cache.clear();
    material_cache.clear();
    mesh_cache.clear();
    env_tex_cache.clear();
    env_cube_cache.clear();
    gltf_models.clear();
    gltf_states.clear();
    /* plans/22 §6.5：glTF 动画/蒙皮/morph 缓存与名字表也按**相对路径/场景**失效 */
    gclips.clear();
    skeletons.clear();
    morph_meshes.clear();
    mesh_layout.clear();
    anim_pose.clear();
    morph_weight_override.clear();
    entity_morph_mesh.clear();
    entity_by_name.clear();
    skin_palettes.clear();
    gltf_warned.clear();
    gltf_frame_info = false;
    gltf_anim_info = false;
    if (nt + nm + ne + nec + ng + ns + nmo + ngl > 0)
        std::print("runtime: asset caches cleared on scene switch ({} textures, {} meshes, {} env, {} env_cube, "
                   "{} clips, {} skeletons, {} morph, {} gltf)\n",
                   nt, nm, ne, nec, ng, ns, nmo, ngl);
}

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
                    load_texture_ref(scoped_texture_ref(id, *tp));
                if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty()) {
                    scn::SpriteMaterial m{};
                    if (load_material(*mp, m)) {
                        material_cache[*mp] = m;
                        if (!m.texture.empty()) load_texture_ref(scoped_texture_ref(id, m.texture));
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
    /* 场景文件热重载：Spine/资源缓存按旧世界释放（相对路径缓存键在新挂载点下会串场景）。 */
    spine_release();
    clear_asset_caches();
    /* 场景文件热重载（docs/32 §5.3）：根实例的 VM 保留（与旧行为一致），
     * 子实例随旧世界销毁并在装配时按新文件重建；实体归属/节点事件重新登记。 */
    for (auto& up : instances) {
        if (up && up->id != 0 && up->state == 1) destroy_instance_content(*up, true);
    }
    if (SceneInstanceRuntime* root = instance_at(0)) {
        root->children.clear();
        root->roots.clear();
    }
    entity_instance.clear();
    node_events.clear();
    const size_t instance_mark = instances.size();
    /* 热重载：新世界沿用旧世界的实体 id 水位 —— `Scene::spawn` 的"永不复用"契约
     * 必须跨场景重建也成立，否则常驻脚本持有的旧 id 会别名到新场景实体。 */
    Scene* fresh = new Scene();
    fresh->next_id = scene ? scene->next_id : 0;
    scene_chain.clear();
    scene_chain.push_back(scn_vfs_path);
    building_instance = 0;
    building_base.clear();
    fresh->deserialize(*this, text);
    fresh->instantiate(*this);
    scene_chain.pop_back();
    delete scene;                       // 重建世界（编辑器/AI 场景迭代路径）
    scene = fresh;
    scene->flatten_world(*this);
    if (SceneInstanceRuntime* root = instance_at(0)) {
        for (auto& [eid, ent] : scene->ents)
            if (ent.parent < 0 && entity_instance.find(eid) != entity_instance.end() && entity_instance[eid] == 0)
                root->roots.push_back(eid);
        std::sort(root->roots.begin(), root->roots.end());
    }
    /* 新装配出的子实例需要一次 on_start（后序）；根实例保持"世界重建但 VM 状态不丢"的旧语义。 */
    for (size_t i = instance_mark; i < instances.size(); ++i) {
        SceneInstanceRuntime* up = instance_at((int)i);
        if (up && up->state == 1 && up->parent == 0) (void)dispatch_instance_on_start(up->id);
    }
    setup_render_textures();               // 相机离屏目标/URI 随新世界重建
    preload_scene_assets();                // 新世界的 Sprite/材质引用重新解析
    setup_3d();                         // 重载可能改变 2D/3D 组成 → 重算 has_3d/cube_mesh
    setup_shader_passes();              // 场景资源 shader pass（.wgsl）随场景重载重建
    /* 热重载重建了世界：全局/覆盖层 UI 实体属于旧世界 → 让常驻脚本重建（VM 状态保留）。 */
    if (global_bus) mq_script_bus_call3(global_bus, "on_start", 0.0, 0.0, 0.0, nullptr);
    if (overlay_bus) mq_script_bus_call3(overlay_bus, "on_start", 0.0, 0.0, 0.0, nullptr);
    ++reloads;
    mq_obs_event(obs, "runtime", 1, "scene_reloaded");
    std::print("runtime: hot reload scene '{}'\n", scn_vfs_path);
    return true;
}

/* ---- 常驻覆盖层脚本（scn.Scene.overlay_script）----
 * 独立 VM/总线：世界重置（director.loadScene）时主脚本随 `bus` 重建，覆盖层 VM 不动 →
 * 外壳/HUD/导航的 JS 状态跨场景保留；UI 实体仍属于当前世界（切场景即销毁），
 * 所以每次场景装配完成后由 on_start 让脚本重建 UI。 */
void Engine::unload_overlay() {
    if (overlay_bus) { mq_script_bus_destroy(overlay_bus); overlay_bus = nullptr; }
    overlay_rel.clear();
    overlay_abs.clear();
    overlay_hash = 0;
}

bool Engine::ensure_overlay_script(const std::string& rel) {
    if (rel.empty()) return false;
    /* 同一脚本：复用已有 VM（状态跨场景保留，只重新收 on_start）。 */
    if (overlay_bus && rel == overlay_rel) return true;
    unload_overlay();
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + rel;
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK || !d) {
        std::print(stderr, "runtime: overlay script not found '{}'\n", rel);
        mq_obs_event(obs, "runtime", 3, "overlay_script_missing");
        return false;
    }
    std::string src((char*)d, n);
    mq_vfs_free(d);
    const char* lang = mq_script_lang_for_path(rel.c_str());
    mq_script_vm* one = mq_script_vm_create_lang(lang);
    if (!one) {
        std::print(stderr, "runtime: overlay script backend '{}' unavailable for '{}'\n", lang, rel);
        mq_obs_event(obs, "runtime", 3, "overlay_script_backend");
        return false;
    }
    mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(one, this);
    if (mq_script_eval(one, src.c_str(), rel.c_str()) != 0) {
        char ebuf[512] = { 0 };
        mq_script_last_error(one, ebuf, (int)sizeof ebuf);
        std::print(stderr, "runtime: overlay script eval failed '{}': {}\n", rel, ebuf);
        mq_script_vm_destroy(one);
        mq_obs_event(obs, "runtime", 3, "overlay_script_failed");
        return false;
    }
    overlay_bus = mq_script_bus_create();
    if (!overlay_bus) { mq_script_vm_destroy(one); return false; }
    mq_script_bus_add(overlay_bus, one, rel.c_str());
    overlay_rel = rel;
    overlay_abs = scn_dir.empty() ? rel : (std::filesystem::path(scn_dir) / rel).string();
    overlay_hash = mq_vfs_hash(src.data(), src.size());
    if (watcher) mq_watcher_add(watcher, overlay_abs.c_str());
    std::print("runtime: overlay script '{}' active (persists across scene loads)\n", rel);
    mq_obs_event(obs, "runtime", 1, "overlay_script_loaded");
    return true;
}

bool Engine::reload_overlay_script() {
    if (!overlay_bus || overlay_abs.empty() || !scene) return false;
    std::ifstream f(overlay_abs, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (src.empty()) return false;
    const uint64_t h = mq_vfs_hash(src.data(), src.size());
    if (h == overlay_hash) return false;
    const char* lang = mq_script_lang_for_path(overlay_rel.c_str());
    mq_script_vm* fresh = mq_script_vm_create_lang(lang);
    if (!fresh) return false;
    mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(fresh, this);
    if (mq_script_eval(fresh, src.c_str(), overlay_rel.c_str()) != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
        mq_script_vm_destroy(fresh);                 // 保留旧 VM：写坏脚本不打断运行时
        std::print(stderr, "runtime: overlay hot reload failed '{}': {} (旧脚本保留)\n", overlay_rel, e);
        return false;
    }
    mq_script_bus_swap(overlay_bus, 0, fresh, overlay_rel.c_str());
    overlay_hash = h;
    /* 换入后只对新 VM 派发 on_start：重建 UI 并重新初始化。 */
    if (mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr) != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "on_start failed";
        std::print(stderr, "runtime: overlay hot reload on_start failed '{}': {}\n", overlay_rel, e);
    }
    std::print("runtime: hot reload overlay script '{}' ({} bytes)\n", overlay_rel, src.size());
    mq_obs_event(obs, "runtime", 1, "overlay_script_reloaded");
    return true;
}

/* ---- 全局脚本（scn.Scene.global_script，docs/32 §16.1）----
 * 与覆盖层同形：独立 VM/总线、跨场景保留、每次装配后 on_start。差别在**语义与顺序**：
 * 它是游戏级服务（进度/存档/跨场景规则），生命周期在实例之后、覆盖层之前；
 * 其他 VM 可用 global_emit(event, value) 显式调用它的命名处理器。
 * 同一路径复用已有 VM（跨场景状态保留）；不同路径 → 换脚本（状态重置）。 */
void Engine::unload_global() {
    if (global_bus) { mq_script_bus_destroy(global_bus); global_bus = nullptr; }
    global_rel.clear();
    global_abs.clear();
    global_hash = 0;
}

bool Engine::ensure_global_script(const std::string& rel) {
    if (rel.empty()) return false;
    if (global_bus && rel == global_rel) return true;
    unload_global();
    void* d = nullptr; size_t n = 0;
    const std::string vpath = "/" + rel;
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK || !d) {
        std::print(stderr, "runtime: global script not found '{}'\n", rel);
        mq_obs_event(obs, "runtime", 3, "global_script_missing");
        return false;
    }
    std::string src((char*)d, n);
    mq_vfs_free(d);
    const char* lang = mq_script_lang_for_path(rel.c_str());
    mq_script_vm* one = mq_script_vm_create_lang(lang);
    if (!one) {
        std::print(stderr, "runtime: global script backend '{}' unavailable for '{}'\n", lang, rel);
        mq_obs_event(obs, "runtime", 3, "global_script_backend");
        return false;
    }
    mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(one, this);
    /* 顶层代码（默认值/初始化）也在"全局脚本上下文"里执行：node_on 归属 -3，
     * 且不能误以为自己是某个场景实例。 */
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_g = in_global;
    const bool prev_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const int rc = mq_script_eval(one, src.c_str(), rel.c_str());
    active_instance = prev;
    in_global = prev_g;
    in_overlay = prev_ov;
    if (rc != 0) {
        char ebuf[512] = { 0 };
        mq_script_last_error(one, ebuf, (int)sizeof ebuf);
        std::print(stderr, "runtime: global script eval failed '{}': {}\n", rel, ebuf);
        mq_script_vm_destroy(one);
        mq_obs_event(obs, "runtime", 3, "global_script_failed");
        return false;
    }
    global_bus = mq_script_bus_create();
    if (!global_bus) { mq_script_vm_destroy(one); return false; }
    mq_script_bus_add(global_bus, one, rel.c_str());
    global_rel = rel;
    global_abs = scn_dir.empty() ? rel : (std::filesystem::path(scn_dir) / rel).string();
    global_hash = mq_vfs_hash(src.data(), src.size());
    if (watcher) mq_watcher_add(watcher, global_abs.c_str());
    std::print("runtime: global script '{}' active (persists across scene loads)\n", rel);
    mq_obs_event(obs, "runtime", 1, "global_script_loaded");
    return true;
}

bool Engine::reload_global_script() {
    if (!global_bus || global_abs.empty() || !scene) return false;
    std::ifstream f(global_abs, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (src.empty()) return false;
    const uint64_t h = mq_vfs_hash(src.data(), src.size());
    if (h == global_hash) return false;
    const char* lang = mq_script_lang_for_path(global_rel.c_str());
    mq_script_vm* fresh = mq_script_vm_create_lang(lang);
    if (!fresh) return false;
    mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(fresh, this);
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_g = in_global;
    const bool prev_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const int rc = mq_script_eval(fresh, src.c_str(), global_rel.c_str());
    active_instance = prev;
    in_global = prev_g;
    in_overlay = prev_ov;
    if (rc != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
        mq_script_vm_destroy(fresh);
        std::print(stderr, "runtime: global hot reload failed '{}': {} (旧脚本保留)\n", global_rel, e);
        return false;
    }
    mq_script_bus_swap(global_bus, 0, fresh, global_rel.c_str());
    global_hash = h;
    /* 换入后只对新 VM 派发 on_start（重建 HUD / 重新注册事件）；上下文标记为全局脚本，
     * 否则 on_start 里的 node_on/entity_spawn 会落到错误的作用域。 */
    SceneInstanceRuntime* prev2 = active_instance;
    const bool prev2_g = in_global;
    const bool prev2_ov = in_overlay;
    active_instance = nullptr;
    in_global = true;
    in_overlay = false;
    const int hs = mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr);
    active_instance = prev2;
    in_global = prev2_g;
    in_overlay = prev2_ov;
    if (hs != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(fresh, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "on_start failed";
        std::print(stderr, "runtime: global hot reload on_start failed '{}': {}\n", global_rel, e);
    }
    std::print("runtime: hot reload global script '{}' ({} bytes)\n", global_rel, src.size());
    mq_obs_event(obs, "runtime", 1, "global_script_reloaded");
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
        /* 全局脚本 / 常驻覆盖层脚本优先（它们不在 script_paths 里，跨场景保留）。 */
        if (!c->eng->global_abs.empty() && p.size() >= c->eng->global_abs.size() &&
            p.compare(p.size() - c->eng->global_abs.size(), c->eng->global_abs.size(),
                      c->eng->global_abs) == 0) {
            c->eng->reload_global_script();
            return 0;
        }
        if (!c->eng->overlay_abs.empty() && p.size() >= c->eng->overlay_abs.size() &&
            p.compare(p.size() - c->eng->overlay_abs.size(), c->eng->overlay_abs.size(),
                      c->eng->overlay_abs) == 0) {
            c->eng->reload_overlay_script();
            return 0;
        }
        /* 脚本热重载（docs/32 §5.3）：根场景与嵌套实例按文件增量，只重建对应实例的那个槽位。 */
        const auto ends_with = [&](const std::string& full, const std::string& tail) {
            return !tail.empty() && full.size() >= tail.size() &&
                   full.compare(full.size() - tail.size(), tail.size(), tail) == 0;
        };
        bool handled = false;
        for (auto& up : c->eng->instances) {
            if (!up || up->state != 1) continue;
            for (const std::string& sp : up->scripts) {
                const std::string local = (std::filesystem::path(c->eng->scn_dir) /
                                           vfs_join(up->base, sp).substr(1)).string();
                if (ends_with(p, local)) {
                    c->eng->reload_instance_script(up->id, sp);
                    handled = true;
                    break;
                }
            }
        }
        if (!handled) {
            /* 嵌套场景源文件变化：帧边界重建该实例（保留旧实例直到重建成功）。 */
            for (auto& up : c->eng->instances) {
                if (!up || up->id == 0 || up->state != 1) continue;
                const std::string local = (std::filesystem::path(c->eng->scn_dir) / up->source.substr(1)).string();
                if (ends_with(p, local)) {
                    c->eng->instance_ops.push_back({ 2, up->id, -1, std::string() });
                    handled = true;
                    break;
                }
            }
        }
        if (!handled && !c->eng->scn_vfs_path.empty() &&
            ends_with(p, c->eng->scn_vfs_path))
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
    for (uint64_t i = 0; i < n && !quit; ++i) {
#ifdef MQ_RENDERER_STATIC_NULL
        step_frame(false);
#else
        step_frame(true);
#endif
    }
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
        MQ_TRACY_ZONE_N(runtime, "loop");   // Tracy：一轮主循环（含可能的多步补偿 + 墙钟 sleep）
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
    if (eng) {
        eng->entity_instance.erase(index);
        eng->node_events.erase(index);
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

/* VFS 路径拼接（docs/32 §5.1）：`base` 是相对 VFS 根的目录（"" 或 "dir/"）；
 * `rel` 为绝对路径（/x、http(s)://、builtin:/…）时原样返回，否则相对 base 解析并做
 * "." / ".." 归一化（不允许逃出 VFS 根）。 */
static std::string vfs_join(const std::string& base, const std::string& rel) {
    if (rel.empty()) return std::string();
    if (rel[0] == '/' || rel.find("://") != std::string::npos || rel.rfind("builtin:", 0) == 0)
        return rel;
    std::string joined = (base.empty() ? std::string() : base) + rel;
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= joined.size()) {
        size_t slash = joined.find('/', i);
        std::string seg = joined.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "..") { if (!segs.empty()) segs.pop_back(); }
        else if (!seg.empty() && seg != ".") segs.push_back(seg);
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    std::string out = "/";
    for (size_t k = 0; k < segs.size(); ++k) { if (k) out += '/'; out += segs[k]; }
    return out;
}

/* 场景资产所在目录（相对 VFS 根，含结尾 '/'）——用于嵌套实例的资源基准。 */
static std::string vfs_dirname(const std::string& vfs_path) {
    const size_t slash = vfs_path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return vfs_path.substr(1, slash == 0 ? 0 : slash - 1) + (slash > 1 ? "/" : "");
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
    /* 相对路径按**当前场景实例的资源基准**解析（嵌套场景内的 prefab 不再按根场景目录解析）。 */
    const std::string src_vfs = vfs_join(eng.building_base, src);
    if (chain) {
        for (const auto& c : *chain)
            if (c == src_vfs) {
                std::print(stderr, "runtime: cyclic prefab reference '{}' — skipped\n", src);
                mq_obs_event(eng.obs, "runtime", 3, "prefab_cycle");
                return;
            }
    }
    void* pd = nullptr; size_t pn = 0;
    if (mq_vfs_read(eng.vfs, src_vfs.c_str(), &pd, &pn) != MQ_VFS_OK) {
        std::print(stderr, "runtime: prefab not found '{}' (resolved '{}')\n", src, src_vfs);
        return;
    }
    std::string ptext((char*)pd, pn);
    mq_vfs_free(pd);
    mq_json* pdom = mq_json_parse(ptext.c_str(), ptext.size());
    if (!pdom) return;
    std::vector<std::string> child_chain = chain ? *chain : std::vector<std::string>{};
    child_chain.push_back(src_vfs);
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
    eng.entity_instance[idx] = eng.building_instance;    // 归属（事件作用域/序列化过滤用）
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
     * detached=true 表示**已展开**（内容已是本场景的一部分），不再展开。
     * ⚠️ 只认 `scn.PrefabInstance`：`scn.SceneInstance` 也有 source/detached 字段，
     * 若按字段名盲扫会被当成 prefab 再展开一次（docs/32 §5 的两类实例必须分开）。 */
    for (auto& [tid, blob] : ent->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d || std::strcmp(d->name, "scn.PrefabInstance") != 0) continue;
        const std::string* src = read_str(d, blob.data(), "source");
        if (!src || src->empty()) continue;
        bool detached = false;
        if (const mq_field_desc* fd = mq_reflect_field(d, "detached")) detached = *(const bool*)(blob.data() + fd->offset);
        if (detached) continue;
        expand_prefab(eng, sc, *src, idx, chain, depth);
    }

    /* 场景实例（docs/32 §5）：`scn.SceneInstance.source` 指向一个 .scn → 在该实体下实例化
     * 子场景（独立 VM 组/生命周期/事件作用域）。detached=true 表示已烘焙，不再加载源。 */
    {
        const mq_type_desc* sid = mq_reflect_type_by_name(eng.reflect, "scn.SceneInstance");
        if (sid) {
            auto it = ent->comps.find(sid->type_id);
            if (it != ent->comps.end()) {
                std::vector<uint8_t>& blob = it->second;
                const std::string* src = read_str(sid, blob.data(), "source");
                const mq_field_desc* det = mq_reflect_field(sid, "detached");
                const bool detached = det && *(const bool*)(blob.data() + det->offset);
                if (!detached && src && !src->empty()) {
                    std::string src_vfs;
                    const int iid = eng.resolve_instance_path(*src, eng.building_base, src_vfs)
                                        ? eng.create_instance(eng.building_instance, idx, src_vfs)
                                        : -1;
                    int state = 0; uint16_t err = 0;
                    if (iid < 0) { state = 3; err = 1; }
                    else {
                        SceneInstanceRuntime* ci = eng.instance_at(iid);
                        /* 静态实例参数（docs/32 §16.2）：构建前写入，子场景 on_start 可见。 */
                        if (ci) {
                            if (const std::string* pp = read_str(sid, blob.data(), "params"))
                                ci->params = *pp;
                        }
                        if (!ci || !eng.build_instance(*ci, (int)eng.scene_chain.size())) {
                            state = 3; err = (uint16_t)(ci ? 2 : 1);
                            if (ci) { ci->state = 3; ci->error = "instance build failed"; }
                        } else {
                            state = 1;
                            if (const mq_field_desc* f = mq_reflect_field(sid, "paused"))
                                ci->paused = *(bool*)(blob.data() + f->offset);
                            if (const mq_field_desc* f = mq_reflect_field(sid, "active"))
                                ci->active = *(bool*)(blob.data() + f->offset);
                            if (!ci->active) { if (Entity* se = eng.scene->at(idx)) se->active = false; }
                        }
                    }
                    if (const mq_field_desc* f = mq_reflect_field(sid, "instance_id"))
                        *(int32_t*)(blob.data() + f->offset) = (int32_t)iid;
                    if (const mq_field_desc* f = mq_reflect_field(sid, "state"))
                        *(int32_t*)(blob.data() + f->offset) = state;
                    if (const mq_field_desc* f = mq_reflect_field(sid, "error"))
                        *(uint16_t*)(blob.data() + f->offset) = err;
                }
            }
        }
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

/* =====================================================================================
 * 场景实例（docs/32）
 * ===================================================================================== */

SceneInstanceRuntime* Engine::instance_at(int id) {
    if (id < 0 || id >= (int)instances.size()) return nullptr;
    return instances[(size_t)id].get();
}

SceneInstanceRuntime* Engine::instance_of_entity(int entity) {
    auto it = entity_instance.find(entity);
    return it == entity_instance.end() ? nullptr : instance_at(it->second);
}

SceneInstanceRuntime* Engine::instance_by_attach(int entity) {
    for (auto& up : instances)
        if (up && up->attach_entity == entity && up->state != 2) return up.get();
    return nullptr;
}

bool Engine::entity_in_instance_subtree(int instance, int entity) const {
    if (instance <= 0) return true;                 // 根实例：整棵树可见
    int cur = entity;
    for (int guard = 0; cur >= 0 && guard < 100000; ++guard) {
        auto it = entity_instance.find(cur);
        if (it != entity_instance.end() && it->second == instance) return true;
        auto* e = scene ? scene->at(cur) : nullptr;
        cur = e ? e->parent : -1;
    }
    return false;
}

int Engine::create_instance(int parent, int attach, const std::string& vfs_source) {
    if ((int)instances.size() >= std::max(1, instance_limit)) {
        scene_last_error = "instance limit reached";
        std::print(stderr, "runtime: scene instance limit reached ({}) — '{}' skipped\n",
                   instance_limit, vfs_source);
        mq_obs_event(obs, "runtime", 3, "scene_instance_limit");
        return -1;
    }
    auto up = std::make_unique<SceneInstanceRuntime>();
    const int id = (int)instances.size();
    up->id = id;
    up->parent = parent;
    up->attach_entity = attach;
    up->source = vfs_source;
    up->base = vfs_dirname(vfs_source);
    up->state = 0;
    instances.push_back(std::move(up));
    SceneInstanceRuntime* parent_inst = instance_at(parent);
    if (parent_inst) parent_inst->children.push_back(id);
    return id;
}

bool Engine::resolve_instance_path(const std::string& rel, const std::string& base,
                                   std::string& out_vfs) const {
    out_vfs = vfs_join(base, rel);
    return !out_vfs.empty() && out_vfs.size() > 1;
}

int Engine::call_instance_event(SceneInstanceRuntime& inst, const char* name,
                                double a, double b, double c, const char* text) {
    if (!inst.bus || !name) return 0;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = &inst;
    in_overlay = false;
    in_global = false;
    const int hits = mq_script_bus_call3(inst.bus, name, a, b, c, text);
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    return hits;
}

int Engine::call_overlay_event(const char* name, double a, double b, double c, const char* text) {
    if (!overlay_bus || !name) return 0;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = nullptr;
    in_overlay = true;
    in_global = false;
    const int hits = mq_script_bus_call3(overlay_bus, name, a, b, c, text);
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    return hits;
}

int Engine::call_global_event(const char* name, double a, double b, double c, const char* text) {
    if (!global_bus || !name) return 0;
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    const bool prev_g = in_global;
    active_instance = nullptr;
    in_overlay = false;
    in_global = true;
    const int hits = mq_script_bus_call3(global_bus, name, a, b, c, text);
    active_instance = prev;
    in_overlay = prev_ov;
    in_global = prev_g;
    return hits;
}

void Engine::collect_instance_preorder(int id, std::vector<int>& out, bool skip_paused) const {
    if (id < 0 || id >= (int)instances.size() || !instances[(size_t)id]) return;
    const SceneInstanceRuntime* inst = instances[(size_t)id].get();
    if (inst->state != 1) return;
    if (!inst->active) return;
    if (skip_paused && inst->paused) return;
    out.push_back(id);
    for (int c : inst->children) collect_instance_preorder(c, out, skip_paused);
}

void Engine::collect_instance_postorder(int id, std::vector<int>& out) const {
    if (id < 0 || id >= (int)instances.size() || !instances[(size_t)id]) return;
    const SceneInstanceRuntime* inst = instances[(size_t)id].get();
    if (inst->state != 1) return;
    for (int c : inst->children) collect_instance_postorder(c, out);
    out.push_back(id);
}

bool Engine::load_instance_scripts(SceneInstanceRuntime& inst, std::vector<std::string> rels, bool is_root) {
    mq_script_bus* bus = mq_script_bus_create();
    if (!bus) {
        inst.error = "script bus create failed";
        scene_last_error = inst.error;
        return false;
    }
    inst.bus = bus;
    inst.scripts = rels;
    inst.script_hashes.clear();
    if (is_root) { this->bus = bus; }
    for (const std::string& rel : rels) {
        const char* want = mq_script_lang_for_path(rel.c_str());
        mq_script_vm* one = mq_script_vm_create_lang(want);
        if (!one) {
            std::print(stderr, "runtime: script backend '{}' unavailable for '{}' (instance {})\n",
                       want, rel, inst.id);
            inst.error = "script backend unavailable";
            scene_last_error = inst.error;
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        std::print("runtime: script backend '{}' for '{}' (instance {})\n", mq_script_lang(one), rel, inst.id);
        mq_script_set_rng(one, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
        mq_bind_generated(one, this);
        std::string src;
        const std::string vfs_path = vfs_join(inst.base, rel);
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, vfs_path.c_str(), &d, &n) == MQ_VFS_OK) { src.assign((char*)d, n); mq_vfs_free(d); } }
        if (src.empty()) {
            std::print("runtime: cannot read script '{}' (resolved '{}', instance {})\n", rel, vfs_path, inst.id);
            inst.error = "cannot read script";
            scene_last_error = inst.error;
            mq_script_vm_destroy(one);
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        inst.script_hashes.push_back(mq_vfs_hash(src.data(), src.size()));
        SceneInstanceRuntime* prev = active_instance;
        const bool prev_ov = in_overlay;
        active_instance = &inst;
        in_overlay = false;
        const int rc = mq_script_eval(one, src.c_str(), rel.c_str());
        active_instance = prev;
        in_overlay = prev_ov;
        if (rc != 0) {
            std::print("runtime: script error in '{}' (instance {})\n", rel, inst.id);
            inst.error = "script eval failed";
            scene_last_error = inst.error;
            mq_script_vm_destroy(one);
            if (is_root) this->bus = nullptr;
            inst.bus = nullptr;
            mq_script_bus_destroy(bus);
            return false;
        }
        mq_script_bus_add(bus, one, rel.c_str());
    }
    if (is_root) {
        script_paths = rels;
        script_hashes = inst.script_hashes;
        script_path = rels.empty() ? std::string() : rels[0];
        script_hash = inst.script_hashes.empty() ? 0 : inst.script_hashes[0];
        vm = mq_script_bus_vm_at(bus, 0);
        if (rels.size() > 1)
            std::print("runtime: multi-script bus with {} VM(s) (dispatch order = declaration order)\n",
                       mq_script_bus_count(bus));
    }
    return true;
}

bool Engine::build_instance(SceneInstanceRuntime& inst, int depth) {
    (void)depth;
    inst.roots.clear();
    if (!scene) { inst.error = "no world"; inst.state = 3; return false; }
    for (const auto& s : scene_chain) {
        if (s == inst.source) {
            inst.error = "cyclic scene instance reference";
            scene_last_error = "cyclic scene instance: " + inst.source;
            std::print(stderr, "runtime: cyclic scene instance reference '{}' — skipped\n", inst.source);
            mq_obs_event(obs, "runtime", 3, "scene_instance_cycle");
            inst.state = 3;
            return false;
        }
    }
    if ((int)scene_chain.size() >= 16) {
        inst.error = "scene instance nesting too deep (>16)";
        scene_last_error = inst.error;
        std::print(stderr, "runtime: scene instance nesting too deep (>16) — '{}' skipped\n", inst.source);
        inst.state = 3;
        return false;
    }
    void* pd = nullptr; size_t pn = 0;
    if (mq_vfs_read(vfs, inst.source.c_str(), &pd, &pn) != MQ_VFS_OK) {
        inst.error = "scene not found: " + inst.source;
        scene_last_error = inst.error;
        std::print(stderr, "runtime: scene instance source not found '{}'\n", inst.source);
        mq_obs_event(obs, "runtime", 3, "scene_instance_missing");
        inst.state = 3;
        return false;
    }
    std::string text((char*)pd, pn);
    mq_vfs_free(pd);
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) {
        inst.error = "scene json parse failed";
        scene_last_error = inst.error;
        inst.state = 3;
        return false;
    }
    scn::Scene nd{};
    if (scene_type) (void)mq_persist_apply_json(reflect, scene_type, dom, &nd);

    std::vector<std::string> rel_scripts;
    if (!nd.scripts.empty()) rel_scripts = nd.scripts;
    else if (!nd.script.empty()) rel_scripts.push_back(nd.script);

    /* docs/32 §5.1 D1：嵌套场景只有“行为/节点”生效；世界级字段忽略并**显式警告**（每个源一次）。 */
    if ((nd.width != 320 || nd.height != 240 || nd.seed != 0 || nd.gravity_y != -900.0F ||
         nd.views != 1 || nd.msaa_samples != 0 || !nd.overlay_script.empty() ||
         !nd.global_script.empty()) &&
        instance_warned.insert(inst.source).second) {
        std::print(stderr, "runtime: nested scene '{}' ignores world-level fields "
                           "(width/height/seed/gravity/views/msaa/overlay_script/global_script) — one world only\n",
                   inst.source);
    }

    scene_chain.push_back(inst.source);
    const int prev_inst = building_instance;
    const std::string prev_base = building_base;
    building_instance = inst.id;
    building_base = inst.base;
    const int attach = inst.attach_entity;
    const mq_json* ents = mq_json_get(dom, "entities");
    const uint32_t count = mq_json_len(ents);
    for (uint32_t i = 0; i < count; ++i) load_entity(*this, *scene, mq_json_at(ents, i), attach, nullptr, 1);
    bool ok = true;
    if (!rel_scripts.empty()) ok = load_instance_scripts(inst, rel_scripts, false);
    building_instance = prev_inst;
    building_base = prev_base;
    scene_chain.pop_back();
    mq_json_free(dom);

    for (auto& [eid, ent] : scene->ents)
        if (ent.parent == attach && entity_instance.find(eid) != entity_instance.end()
            && entity_instance[eid] == inst.id)
            inst.roots.push_back(eid);
    std::sort(inst.roots.begin(), inst.roots.end());
    if (!ok) { inst.state = 3; return false; }
    inst.state = 1;
    rebase_instance_assets(inst.id);                 // 嵌套场景内资源路径 → 实例 base 前缀
    std::print("runtime: scene instance #{} '{}' built ({} roots, {} scripts)\n",
               inst.id, inst.source, inst.roots.size(), inst.scripts.size());
    mq_obs_event(obs, "runtime", 1, "scene_instance_built");
    return true;
}

int Engine::dispatch_instance_on_start(int id) {
    SceneInstanceRuntime* inst = instance_at(id);
    if (!inst || inst->state != 1) return 0;
    int hits = 0;
    std::vector<int> kids = inst->children;         // 复制：回调里不会立即改结构（动态请求排队）
    for (int c : kids) hits += dispatch_instance_on_start(c);
    hits += call_instance_event(*inst, "on_start", 0.0, 0.0, 0.0, nullptr);
    return hits;
}

int Engine::dispatch_instance_on_destroy(int id) {
    SceneInstanceRuntime* inst = instance_at(id);
    if (!inst) return 0;
    if (inst->state != 1 && inst->state != 4) return 0;
    int hits = 0;
    std::vector<int> kids = inst->children;
    for (int c : kids) hits += dispatch_instance_on_destroy(c);
    hits += call_instance_event(*inst, "on_destroy", 0.0, 0.0, 0.0, nullptr);
    return hits;
}

void Engine::gc_all_instance_scripts() {
    for (auto& up : instances)
        if (up && up->bus) mq_script_bus_gc(up->bus);
    if (global_bus) mq_script_bus_gc(global_bus);
    if (overlay_bus) mq_script_bus_gc(overlay_bus);
}

uint32_t Engine::instance_script_errors() const {
    uint32_t n = 0;
    for (auto& up : instances)
        if (up && up->bus) n += mq_script_bus_error_count(up->bus);
    if (global_bus) n += mq_script_bus_error_count(global_bus);
    if (overlay_bus) n += mq_script_bus_error_count(overlay_bus);
    return n;
}

void Engine::destroy_instance_content(SceneInstanceRuntime& inst, bool fire_destroy) {
    std::vector<int> kids = inst.children;
    for (int c : kids) {
        if (SceneInstanceRuntime* ci = instance_at(c)) destroy_instance_content(*ci, fire_destroy);
    }
    if (fire_destroy && (inst.state == 1 || inst.state == 4)) (void)dispatch_instance_on_destroy(inst.id);
    if (inst.bus) {
        if (inst.bus == bus) { bus = nullptr; vm = nullptr; }
        mq_script_bus_destroy(inst.bus);
        inst.bus = nullptr;
    }
    std::vector<int> owned;
    for (auto& [eid, owner] : entity_instance)
        if (owner == inst.id) owned.push_back(eid);
    std::sort(owned.begin(), owned.end());
    if (scene) {
        for (int eid : owned) scene->remove(eid);
    }
    for (int eid : owned) {
        entity_instance.erase(eid);
        node_events.erase(eid);
    }
    /* 动态实例自动建的挂载壳属于**父**实例；卸载子实例后必须一并删除，
     * 否则每波敌人都会在世界根留下一个空壳（组合机制的资源泄漏，docs/32 §16.4）。 */
    if (inst.owns_shell && inst.attach_entity >= 0 && scene) {
        scene->remove(inst.attach_entity);
        inst.attach_entity = -1;
    }
    inst.owns_shell = false;
    inst.roots.clear();
    inst.state = 2;
}

void Engine::reset_instances() {
    active_instance = nullptr;
    in_overlay = false;
    in_global = false;
    for (auto& up : instances) {
        if (!up) continue;
        if (up->bus) {
            if (up->bus == bus) { bus = nullptr; vm = nullptr; }
            mq_script_bus_destroy(up->bus);
            up->bus = nullptr;
        }
        up->state = 2;
    }
    instances.clear();
    entity_instance.clear();
    node_events.clear();                 // 处理器记录带实例 id，重建实例后必须重注册
    scene_chain.clear();
    instance_ops.clear();
    instance_warned.clear();
    active_instance = nullptr;
    in_overlay = false;
    in_global = false;
    building_instance = 0;
    building_base.clear();
}

/* 帧边界：提交动态实例化/卸载/重载请求（docs/32 §7.1）。
 * 本边界内新产生的请求（如新实例 on_start 里再 scene_instantiate）留到下一帧边界。 */
void Engine::apply_instance_ops() {
    if (instance_ops.empty()) return;
    std::vector<InstanceOp> ops;
    ops.swap(instance_ops);
    for (const InstanceOp& op : ops) {
        SceneInstanceRuntime* inst = instance_at(op.instance);
        if (!inst) continue;
        if (op.kind == 0) {                                  // instantiate
            if (inst->state != 0) continue;
            /* 动态实例（parent=-1）：自动建一个挂载壳（归属父实例），让子场景根节点
             * 仍然只有一个统一的 `scene_self_root()`；以后落盘/定位也一致。 */
            if (inst->attach_entity < 0 && inst->id != 0 && scene) {
                const int shell = scene->spawn();
                if (shell >= 0) {
                    entity_instance[shell] = inst->parent >= 0 ? inst->parent : 0;
                    inst->attach_entity = shell;
                    inst->owns_shell = true;
                    if (Entity* se = scene->at(shell)) {
                        se->parent = -1;
                        se->name = "SceneInstance#" + std::to_string(inst->id);
                    }
                }
            }
            if (inst->attach_entity >= 0 && (!scene || !scene->at(inst->attach_entity))) {
                inst->state = 3;
                inst->error = "attach entity was destroyed";
                scene_last_error = inst->error;
                continue;
            }
            if (!build_instance(*inst, 1)) continue;
            if (inst->attach_entity >= 0) ensure_instance_component(inst->attach_entity, inst->id);
            (void)dispatch_instance_on_start(inst->id);
        } else if (op.kind == 1) {                            // unload
            SceneInstanceRuntime* parent = instance_at(inst->parent);
            if (parent) parent->children.erase(std::remove(parent->children.begin(), parent->children.end(), inst->id),
                                               parent->children.end());
            destroy_instance_content(*inst, true);
            inst->state = 2;
        } else if (op.kind == 2) {                            // reload（状态重置；失败进入 error）
            const std::string src = inst->source;
            destroy_instance_content(*inst, true);
            inst->children.clear();
            inst->state = 0;
            inst->error.clear();
            inst->source = src;
            if (build_instance(*inst, 1)) {
                if (inst->attach_entity >= 0) ensure_instance_component(inst->attach_entity, inst->id);
                (void)dispatch_instance_on_start(inst->id);
            }
        }
    }
}


/* ---- 实例资源基址 / 动态壳组件 / 嵌套脚本热重载 / 快照实例表（docs/32 §5/§6） ---- */

/* 组件里的字符串资源路径字段（显式名单 + PBR 的 *_texture 槽）。 */
static bool is_instance_asset_field(const std::string& name) {
    if (name == "texture" || name == "material" || name == "mesh_path" || name == "clip" ||
        name == "shader" || name == "font" || name == "skeleton" || name == "atlas" ||
        name == "env_texture")
        return true;
    return name.size() > 8 && name.compare(name.size() - 8, 8, "_texture") == 0;
}
static bool is_absolute_asset(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p.rfind("builtin:", 0) == 0 || p.find("://") != std::string::npos ||
                          p.rfind("mem:", 0) == 0 || p.rfind("zip:", 0) == 0);
}
static void snapshot_str(SnapshotWriter& w, const std::string& s) {
    w.pod((uint32_t)s.size());
    if (!s.empty()) w.bytes(s.data(), s.size());
}

/* 实例 source 的**可序列化形式**：根实例相对场景目录；嵌套实例相对父场景 base；
 * 落在 base 之外的路径保留绝对 VFS 路径（远程/共享资源）。 */
std::string Engine::serialized_instance_source(const SceneInstanceRuntime& inst) {
    const std::string& src = inst.source;
    if (src.empty() || src[0] != '/') return src;
    if (inst.parent > 0) {
        const SceneInstanceRuntime* p = instance_at(inst.parent);
        if (p && !p->base.empty()) {
            const std::string prefix = "/" + p->base;
            if (src.rfind(prefix, 0) == 0) return src.substr(prefix.size());
        }
        return src;
    }
    return src.substr(1);                        // 根场景挂载点 = 场景目录
}

void Engine::ensure_instance_component(int shell, int instance_id) {
    if (!scene || !reflect) return;
    Entity* se = scene->at(shell);
    SceneInstanceRuntime* inst = instance_at(instance_id);
    if (!se || !inst) return;
    const mq_type_desc* sid = mq_reflect_type_by_name(reflect, "scn.SceneInstance");
    if (!sid) return;
    if (se->comps.find(sid->type_id) == se->comps.end())
        attach_component(*this, shell, "scn.SceneInstance");
    auto it = se->comps.find(sid->type_id);
    if (it == se->comps.end()) return;
    std::vector<uint8_t>& blob = it->second;
    if (const mq_field_desc* f = mq_reflect_field(sid, "source"))
        *(std::string*)(blob.data() + f->offset) = serialized_instance_source(*inst);
    if (const mq_field_desc* f = mq_reflect_field(sid, "params"))
        *(std::string*)(blob.data() + f->offset) = inst->params;
    if (const mq_field_desc* f = mq_reflect_field(sid, "paused"))
        *(bool*)(blob.data() + f->offset) = inst->paused;
    if (const mq_field_desc* f = mq_reflect_field(sid, "active"))
        *(bool*)(blob.data() + f->offset) = inst->active;
    if (const mq_field_desc* f = mq_reflect_field(sid, "detached"))
        *(bool*)(blob.data() + f->offset) = inst->detached;
    if (const mq_field_desc* f = mq_reflect_field(sid, "instance_id"))
        *(int32_t*)(blob.data() + f->offset) = inst->id;
    if (const mq_field_desc* f = mq_reflect_field(sid, "state"))
        *(int32_t*)(blob.data() + f->offset) = inst->state;
}

/* 把实例拥有的组件资源路径改写为「实例 base 前缀」——让既有的 VFS 相对根场景目录的
 * 加载路径（preload / mesh / spine / audio / text）无需逐个改函数即可找到嵌套资源。 */
void Engine::rebase_instance_assets(int instance_id) {
    SceneInstanceRuntime* inst = instance_at(instance_id);
    if (!inst || inst->base.empty() || !scene) return;
    for (auto& [eid, owner] : entity_instance) {
        if (owner != instance_id) continue;
        Entity* e = scene->at(eid);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d) continue;
            for (uint32_t i = 0; i < d->field_count; ++i) {
                const mq_field_desc* f = &d->fields[i];
                if (f->kind != MQ_TK_STRING || !is_instance_asset_field(f->name)) continue;
                std::string* s = (std::string*)(blob.data() + f->offset);
                if (s->empty()) continue;
                static constexpr const char* kRt = "rendertexture://";
                if (s->rfind(kRt, 0) == 0) continue;  /* 运行期按 consumer instance 解析，不改写持久化文本 */
                if (is_absolute_asset(*s)) continue;
                const std::string joined = vfs_join(inst->base, *s);   // "/base/rel"
                if (joined.size() > 1) *s = joined.substr(1);          // 去前导 '/'（既有代码会补）
            }
        }
    }
}

bool Engine::reload_instance_script(int instance_id, const std::string& rel) {
    SceneInstanceRuntime* inst = instance_at(instance_id);
    if (!inst || !inst->bus || rel.empty()) return false;
    int slot = -1;
    for (size_t i = 0; i < inst->scripts.size(); ++i)
        if (inst->scripts[i] == rel) { slot = (int)i; break; }    // 按文件增量：只重建该槽位
    if (slot < 0) return false;
    void* d = nullptr; size_t n = 0;
    const std::string vpath = vfs_join(inst->base, rel);
    if (mq_vfs_read(vfs, vpath.c_str(), &d, &n) != MQ_VFS_OK || !d) return false;
    std::string src((char*)d, n);
    mq_vfs_free(d);
    const uint64_t h = mq_vfs_hash(src.data(), src.size());
    if ((size_t)slot < inst->script_hashes.size() && inst->script_hashes[(size_t)slot] == h) return false;
    const char* lang = mq_script_lang_for_path(rel.c_str());
    mq_script_vm* fresh = mq_script_vm_create_lang(lang);
    if (!fresh) return false;
    mq_script_set_rng(fresh, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(fresh, this);
    SceneInstanceRuntime* prev = active_instance;
    const bool prev_ov = in_overlay;
    active_instance = inst;
    in_overlay = false;
    const int rc = mq_script_eval(fresh, src.c_str(), rel.c_str());
    active_instance = prev;
    in_overlay = prev_ov;
    if (rc != 0) {                                   // 原子：eval 失败保留旧 VM
        char ebuf[512] = { 0 };
        mq_script_last_error(fresh, ebuf, (int)sizeof ebuf);
        std::print(stderr, "runtime: instance {} script hot reload failed '{}': {} (旧脚本保留)\n",
                   inst->id, rel, ebuf);
        mq_script_vm_destroy(fresh);
        return false;
    }
    mq_script_bus_swap(inst->bus, (uint32_t)slot, fresh, rel.c_str());
    while (inst->script_hashes.size() <= (size_t)slot) inst->script_hashes.push_back(0);
    inst->script_hashes[(size_t)slot] = h;
    if (inst->id == 0) {
        script_hashes = inst->script_hashes;
        if (!script_hashes.empty()) script_hash = script_hashes.front();
        vm = mq_script_bus_vm_at(inst->bus, 0);
    }
    /* 换入后只对新 VM 派发 on_start（重新注册事件处理器）。 */
    prev = active_instance; active_instance = inst;
    (void)mq_event_dispatch(fresh, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr);
    active_instance = prev;
    std::print("runtime: hot reload instance #{} script '{}' (slot {})\n", inst->id, rel, slot);
    mq_obs_event(obs, "runtime", 1, "instance_script_reloaded");
    return true;
}

/* 快照扩展：全局状态块（docs/32 §16.3）。放在实例表**之前**：restore 时先恢复状态，
 * 再重建实例 VM 并派发 on_start（脚本能读到恢复后的全局状态）。
 * 旧快照没有该块 → restore_globals 探测 magic 失败，原样跳过。 */
void Engine::encode_globals(SnapshotWriter& w) const {
    w.pod((uint32_t)0x4D51474Cu);                  // magic "MQGL"
    w.pod((uint32_t)1u);                           // 版本
    w.pod((uint32_t)global_nums.size());
    for (auto& [k, v] : global_nums) { snapshot_str(w, k); w.pod(v); }
    w.pod((uint32_t)global_texts.size());
    for (auto& [k, v] : global_texts) { snapshot_str(w, k); snapshot_str(w, v); }
}

int Engine::restore_globals(const uint8_t* data, size_t n, size_t& off) {
    auto need = [&](size_t bytes) { return off + bytes <= n; };
    uint32_t magic = 0, ver = 0;
    if (!need(8)) return 0;
    std::memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4D51474Cu) { off -= 4; return 0; }      // 旧快照：无该块
    std::memcpy(&ver, data + off, 4); off += 4;
    if (ver != 1) return 0;
    auto rstr = [&](std::string& out) {
        uint32_t len = 0;
        if (!need(4)) return false;
        std::memcpy(&len, data + off, 4); off += 4;
        if (!need(len) || len > (1u << 20)) return false;
        out.assign((const char*)data + off, len); off += len;
        return true;
    };
    uint32_t nums = 0;
    if (!need(4)) return 0;
    std::memcpy(&nums, data + off, 4); off += 4;
    if (nums > (1u << 20)) return 0;
    std::map<std::string, double> new_nums;
    for (uint32_t i = 0; i < nums; ++i) {
        std::string k; double v = 0;
        if (!rstr(k) || !need(8)) return 0;
        std::memcpy(&v, data + off, 8); off += 8;
        new_nums[std::move(k)] = v;
    }
    uint32_t texts = 0;
    if (!need(4)) return 0;
    std::memcpy(&texts, data + off, 4); off += 4;
    if (texts > (1u << 20)) return 0;
    std::map<std::string, std::string> new_texts;
    for (uint32_t i = 0; i < texts; ++i) {
        std::string k, v;
        if (!rstr(k) || !rstr(v)) return 0;
        new_texts[std::move(k)] = std::move(v);
    }
    global_nums = std::move(new_nums);
    global_texts = std::move(new_texts);
    return 1;
}

/* 快照扩展：实例表 + 实体归属。放在快照末尾（旧快照没有该块 → 跳过）。 */
void Engine::encode_instances(SnapshotWriter& w) const {
    w.pod((uint32_t)0x4D515349u);                  // magic "MQSI"
    w.pod((uint32_t)2u);                           // 版本（v2 = +params；v1 仍可读）
    uint32_t live = 0;
    for (auto& up : instances) if (up && up->state != 2) ++live;
    w.pod(live);
    for (auto& up : instances) {
        if (!up || up->state == 2) continue;
        w.pod((int32_t)up->id);
        w.pod((int32_t)up->parent);
        w.pod((int32_t)up->attach_entity);
        w.pod((int32_t)up->state);
        w.pod((uint8_t)(up->paused ? 1 : 0));
        w.pod((uint8_t)(up->active ? 1 : 0));
        w.pod((uint8_t)(up->detached ? 1 : 0));
        w.pod((uint8_t)(up->owns_shell ? 1 : 0));
        snapshot_str(w, up->source);
        snapshot_str(w, up->base);
        snapshot_str(w, up->params);
        w.pod((uint32_t)up->roots.size());
        for (int r : up->roots) w.pod((int32_t)r);
        w.pod((uint32_t)up->scripts.size());
        for (const auto& s : up->scripts) snapshot_str(w, s);
    }
    std::vector<std::pair<int, int>> owners(entity_instance.begin(), entity_instance.end());
    std::sort(owners.begin(), owners.end());
    w.pod((uint32_t)owners.size());
    for (auto& [eid, oid] : owners) { w.pod((int32_t)eid); w.pod((int32_t)oid); }
}

int Engine::restore_instances(const uint8_t* data, size_t n, size_t& off) {
    auto need = [&](size_t bytes) { return off + bytes <= n; };
    uint32_t magic = 0, ver = 0;
    if (!need(8)) return 0;
    std::memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4D515349u) { off -= 4; return 0; }
    std::memcpy(&ver, data + off, 4); off += 4;
    if (ver != 1 && ver != 2) return 0;                 // v1 = 无 params/owns_shell（旧快照）
    uint32_t count = 0;
    if (!need(4)) return 0;
    std::memcpy(&count, data + off, 4); off += 4;
    if (count > (uint32_t)std::max(instance_limit * 4, 1024)) return 0;

    /* 先解析到临时表；再决定"保留现有 VM（结构完全相同）"还是重建。 */
    std::vector<std::unique_ptr<SceneInstanceRuntime>> recs;
    recs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto up = std::make_unique<SceneInstanceRuntime>();
        int32_t id = -1, parent = -1, attach = -1, state = 1;
        uint8_t paused = 0, active = 1, detached = 0, owns_shell = 0;
        if (!need(4 * 4 + 3)) return 0;
        std::memcpy(&id, data + off, 4); off += 4;
        std::memcpy(&parent, data + off, 4); off += 4;
        std::memcpy(&attach, data + off, 4); off += 4;
        std::memcpy(&state, data + off, 4); off += 4;
        paused = data[off++]; active = data[off++]; detached = data[off++];
        if (ver >= 2) {
            if (!need(1)) return 0;
            owns_shell = data[off++];
        }
        auto rstr = [&](std::string& out) {
            uint32_t len = 0;
            if (!need(4)) return false;
            std::memcpy(&len, data + off, 4); off += 4;
            if (!need(len)) return false;
            out.assign((const char*)data + off, len); off += len;
            return true;
        };
        if (!rstr(up->source) || !rstr(up->base)) return 0;
        if (ver >= 2 && !rstr(up->params)) return 0;
        uint32_t rn = 0;
        if (!need(4)) return 0;
        std::memcpy(&rn, data + off, 4); off += 4;
        if (rn > 1u << 20) return 0;
        for (uint32_t k = 0; k < rn; ++k) {
            int32_t r = -1;
            if (!need(4)) return 0;
            std::memcpy(&r, data + off, 4); off += 4;
            up->roots.push_back(r);
        }
        uint32_t sn = 0;
        if (!need(4)) return 0;
        std::memcpy(&sn, data + off, 4); off += 4;
        if (sn > 1024) return 0;
        for (uint32_t k = 0; k < sn; ++k) {
            std::string s;
            if (!rstr(s)) return 0;
            up->scripts.push_back(s);
        }
        up->id = id; up->parent = parent; up->attach_entity = attach;
        up->state = state; up->paused = paused != 0; up->active = active != 0; up->detached = detached != 0;
        up->owns_shell = owns_shell != 0;
        recs.push_back(std::move(up));
    }
    std::vector<std::pair<int, int>> owners;
    uint32_t owner_count = 0;
    if (!need(4)) return 0;
    std::memcpy(&owner_count, data + off, 4); off += 4;
    if (owner_count > (1u << 22)) return 0;
    owners.reserve(owner_count);
    for (uint32_t i = 0; i < owner_count; ++i) {
        int32_t eid = -1, oid = -1;
        if (!need(8)) return 0;
        std::memcpy(&eid, data + off, 4); off += 4;
        std::memcpy(&oid, data + off, 4); off += 4;
        owners.emplace_back(eid, oid);
    }

    /* 结构相同 → **保留现有 VM/脚本状态**（纯 undo/回滚更自然；docs/32 §6.3）。
     * 实体表已由 Scene::decode 恢复（id 不变），因此只需刷新归属/暂停/显隐/roots。 */
    instance_ops.clear();                              // 回滚取消尚未提交的动态增删请求
    int live_now = 0;
    for (auto& up : instances) if (up && up->state != 2) ++live_now;
    bool same = (size_t)live_now == recs.size();
    for (auto& rec : recs) {
        if (!same) break;
        SceneInstanceRuntime* cur = instance_at(rec->id);
        if (!cur || cur->state == 2) { same = false; break; }
        if (cur->parent != rec->parent || cur->attach_entity != rec->attach_entity ||
            cur->source != rec->source || cur->base != rec->base || cur->params != rec->params ||
            cur->scripts != rec->scripts || cur->roots != rec->roots) { same = false; break; }
    }
    if (same) {
        for (auto& rec : recs) {
            SceneInstanceRuntime* cur = instance_at(rec->id);
            if (!cur) { same = false; break; }
            cur->paused = rec->paused;
            cur->active = rec->active;
            cur->detached = rec->detached;
            cur->state = rec->state;
            cur->roots = rec->roots;
            cur->owns_shell = rec->owns_shell;
        }
        if (same) {
            entity_instance.clear();
            for (auto& [eid, oid] : owners) entity_instance[eid] = oid;
            return 2;                                  // VM 状态保留，无需 on_start
        }
    }

    reset_instances();                                 // 结构变化：重建（VM 状态不落盘）
    instances = std::move(recs);
    entity_instance.clear();
    for (auto& [eid, oid] : owners) entity_instance[eid] = oid;
    for (auto& up : instances) {
        up->children.clear();
        if (up->parent > 0) {
            SceneInstanceRuntime* p = instance_at(up->parent);
            if (p) p->children.push_back(up->id);
        }
    }
    for (auto& up : instances) {
        if (!up) continue;
        if (!up->scripts.empty()) {
            if (!load_instance_scripts(*up, up->scripts, up->id == 0)) up->state = 3;
        } else {
            up->bus = nullptr;
            up->state = 1;
        }
    }
    return 1;                                          // 已重建 → 调用方派发 on_start
}

uint64_t Engine::instance_checksum() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (auto& up : instances) {
        if (!up || up->state == 2) continue;
        mix((uint64_t)(uint32_t)up->id);
        mix((uint64_t)(uint32_t)up->parent);
        mix((uint64_t)(uint32_t)up->attach_entity);
        mix((uint64_t)(uint32_t)up->state);
        mix(up->paused ? 1u : 0u);
        mix(up->active ? 1u : 0u);
        /* 参数是实例组成的一部分（静态/动态都算）；空串不混入 → 老场景 checksum 零变化。 */
        if (!up->params.empty()) {
            mix(0x9E3779B97F4A7C15ull);
            for (unsigned char c : up->params) mix((uint64_t)c);
        }
        for (int r : up->roots) mix((uint64_t)(uint32_t)r);
    }
    std::vector<std::pair<int, int>> owners(entity_instance.begin(), entity_instance.end());
    std::sort(owners.begin(), owners.end());
    for (auto& [eid, oid] : owners) { mix((uint64_t)(uint32_t)eid); mix((uint64_t)(uint32_t)oid); }
    return h;
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
    const int id_floor = next_id;      // 回滚也不回收已发放的 id（单调契约）
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
    if (id_floor > next_id) next_id = id_floor;
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

/* 3D 世界矩阵（plans/22 §6.3）：遍历顺序与 flatten_world 完全一致（根 id 升序、子 id 升序）。
 * local = scene.Transform3D.local 叠加 Engine::anim_pose（clips 采样结果）；
 * 有 rotation 轨道用四元数路径（m4_from_quat_trs，无万向锁），否则走 Euler TRS（m4_from_trs）；
 * world_m = parent.world_m * local。平级场景 world_m == m4_from_trs(local)，旧场景零漂移。 */
void Scene::flatten_world3d(Engine& eng) {
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
        math::Transform3D trs{};
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (!d || std::strcmp(d->name, "scene.Transform3D") != 0) continue;
            if (const mq_field_desc* lf = mq_reflect_field(d, "local"))
                std::memcpy(&trs, blob.data() + lf->offset, sizeof trs);
            break;
        }
        AnimPose* pose = nullptr;
        auto pit = eng.anim_pose.find(idx);
        if (pit != eng.anim_pose.end()) pose = &pit->second;
        if (pose && pose->has_pos) trs.position = pose->pos;
        if (pose && pose->has_scale) trs.scale = pose->scale;
        float local[16];
        if (pose && pose->has_rot) m4_from_quat_trs(trs.position, pose->rot, trs.scale, local);
        else m4_from_trs(trs, local);
        if (pidx >= 0) {
            if (Entity* pe = at(pidx)) m4_mul(pe->world_m, local, e->world_m);
            else std::memcpy(e->world_m, local, sizeof local);
        } else {
            std::memcpy(e->world_m, local, sizeof local);
        }
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
                // 矩形控件以"中心 + 半尺寸"图元表达（half_w/half_h 都 >0 时渲染器画矩形；
                // 只填 size 会退化成正方形 —— 96x28 的按钮曾显示成 96x96 方块）
                mq_renderer_item it{};
                it.x = e.world_x + x + w * 0.5F; it.y = (float)eng.H - (e.world_y + y) - h * 0.5F;
                it.size = w * 0.5F > h * 0.5F ? w * 0.5F : h * 0.5F;
                it.half_w = w * 0.5F;
                it.half_h = h * 0.5F;
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
                const std::string resolved = eng.scoped_texture_ref(idx, texref);
                auto ti = eng.tex_cache.find(resolved);
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
            if (tex >= 0) {
                for (const auto& rt : eng.render_textures)
                    if (rt.handle == tex) { it.filter = rt.filter == 0 ? 1 : 0; break; }
            }
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

/* 事件 → 脚本回调：映射与参数个数全部由 DSL 声明、codegen 生成（docs/14 §4、docs/19 §8）。
 * 事件 kind → 处理器名（生成的事件表是单一真相）；docs/32 §4.3 的**作用域路由**：
 *   · 生命周期/帧回调：按实例树顺序（on_start 后序、update 前序、on_destroy 后序）；
 *   · 指针/滚轮：root → … → 命中实体的所属实例（祖先链），最后 overlay；未命中/键盘：root + 所有实例；
 *   · overlay 恒在最后（老场景"主脚本之后"的语义不变）。 */
int dispatch_event(Engine& eng, int kind, double a, double b, double c, const char* text) {
    uint32_t n = 0;
    const mq_event_desc* tab = mq_event_table(&n);
    if (!tab || kind < 0 || (uint32_t)kind >= n) return 0;
    const char* name = tab[kind].name;
    MQ_TRACY_ZONE_NAMED(__tz_evt, script, name);
    int hits = 0;

    if (kind == MQ_EVENT_ON_START) {
        hits += eng.dispatch_instance_on_start(0);
        hits += eng.call_global_event(name, a, b, c, text);
        hits += eng.call_overlay_event(name, a, b, c, text);
        return hits;
    }
    if (kind == MQ_EVENT_ON_DESTROY) {
        hits += eng.dispatch_instance_on_destroy(0);
        hits += eng.call_global_event(name, a, b, c, text);
        hits += eng.call_overlay_event(name, a, b, c, text);
        return hits;
    }
    if (kind == MQ_EVENT_UPDATE) {
        std::vector<int> order;
        eng.collect_instance_preorder(0, order, true);       // 前序；paused/active=false 整棵子树跳过
        for (int id : order)
            if (SceneInstanceRuntime* inst = eng.instance_at(id))
                hits += eng.call_instance_event(*inst, name, a, b, c, text);
        hits += eng.call_global_event(name, a, b, c, text);   // 全局服务先于覆盖层（先算状态，再画 HUD）
        hits += eng.call_overlay_event(name, a, b, c, text);
        return hits;
    }

    /* 输入事件：指针类按 target 的实例祖先链做作用域路由；键盘/文本给全部 active 实例。 */
    const bool pointerish = kind >= MQ_EVENT_ON_POINTER_DOWN && kind <= MQ_EVENT_ON_CLICK;
    std::vector<int> order;
    if (pointerish) {
        const int target = (int)a;
        SceneInstanceRuntime* owner = target >= 0 ? eng.instance_of_entity(target) : nullptr;
        std::vector<int> chain;
        for (SceneInstanceRuntime* cur = owner; cur; cur = eng.instance_at(cur->parent))
            chain.push_back(cur->id);
        std::reverse(chain.begin(), chain.end());            // root → … → owner
        if (chain.empty() && eng.instance_at(0)) chain.push_back(0);
        order = chain;
    } else {
        eng.collect_instance_preorder(0, order, false);      // 键盘/文本：全部 active（root 在内）
    }
    for (int id : order) {
        SceneInstanceRuntime* inst = eng.instance_at(id);
        if (!inst || inst->state != 1 || !inst->active) continue;
        hits += eng.call_instance_event(*inst, name, a, b, c, text);
    }
    hits += eng.call_global_event(name, a, b, c, text);
    hits += eng.call_overlay_event(name, a, b, c, text);
    return hits;
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
    if (!want) {
        /* 通用短名：任意 namespace 的 "Ns.Type" 后缀匹配（audio.AudioSource 等），
         * 避免每加一个模块都要在这里补一个前缀。重名时取注册顺序第一个。 */
        const std::string suffix = std::string(".") + type_name;
        const uint32_t n = mq_reflect_count(eng.reflect);
        for (uint32_t i = 0; i < n && !want; ++i) {
            const mq_type_desc* t = mq_reflect_type_at(eng.reflect, i);
            if (!t || !t->name) continue;
            const std::string nm(t->name);
            if (nm.size() > suffix.size() &&
                nm.compare(nm.size() - suffix.size(), suffix.size(), suffix) == 0) {
                want = t;
            }
        }
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
    float size = 16.0F, r = 1, g = 1, b = 1, a = 1, max_w = 0, max_h = 0, line_spacing = 0;
    int align = 0, wrap = 0, overflow = 0;
    bool bold = false, italic = false;
    if (const std::string* tp = read_str(d, e.comps[d->type_id].data(), "text")) text = *tp;
    if (const std::string* fp = read_str(d, e.comps[d->type_id].data(), "font")) font_path_comp = *fp;
    read_f32(d, e.comps[d->type_id].data(), "font_size", &size);
    read_f32(d, e.comps[d->type_id].data(), "max_width", &max_w);
    read_f32(d, e.comps[d->type_id].data(), "max_height", &max_h);
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
    overflow = (int)read_num(d, e.comps[d->type_id].data(), "overflow", 0.0F);
    bold = read_num(d, e.comps[d->type_id].data(), "bold", 0.0F) >= 0.5F;
    italic = read_num(d, e.comps[d->type_id].data(), "italic", 0.0F) >= 0.5F;
    /* 装饰：outline / shadow（字段不存在 → 关闭；ui.* 描边用 outline_color Vec4） */
    float outline_size = 0.0F, orr = 0.0F, ogg = 0.0F, obb = 0.0F, oaa = 1.0F;
    read_f32(d, e.comps[d->type_id].data(), "outline_size", &outline_size);
    if (const mq_field_desc* foc = mq_reflect_field(d, "outline_color")) {
        const float* v = (const float*)(e.comps[d->type_id].data() + foc->offset);
        orr = v[0]; ogg = v[1]; obb = v[2]; oaa = v[3];
    } else {
        read_f32(d, e.comps[d->type_id].data(), "outline_r", &orr);
        read_f32(d, e.comps[d->type_id].data(), "outline_g", &ogg);
        read_f32(d, e.comps[d->type_id].data(), "outline_b", &obb);
        read_f32(d, e.comps[d->type_id].data(), "outline_a", &oaa);
    }
    /* 阴影：偏移为文本空间像素（y 向下）；a=0 关闭。仅 scn.Label 声明这些字段。 */
    float shx = 0.0F, shy = 0.0F, shr = 0.0F, shg = 0.0F, shb = 0.0F, sha = 0.0F;
    read_f32(d, e.comps[d->type_id].data(), "shadow_x", &shx);
    read_f32(d, e.comps[d->type_id].data(), "shadow_y", &shy);
    read_f32(d, e.comps[d->type_id].data(), "shadow_r", &shr);
    read_f32(d, e.comps[d->type_id].data(), "shadow_g", &shg);
    read_f32(d, e.comps[d->type_id].data(), "shadow_b", &shb);
    read_f32(d, e.comps[d->type_id].data(), "shadow_a", &sha);
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
    if (const mq_field_desc* fw = mq_reflect_field(d, "text_w"))
        *(float*)(e.comps[d->type_id].data() + fw->offset) = shown_w;
    if (const mq_field_desc* fh = mq_reflect_field(d, "text_h"))
        *(float*)(e.comps[d->type_id].data() + fh->offset) = shown_h;
    if (const mq_field_desc* flc = mq_reflect_field(d, "line_count"))
        *(int32_t*)(e.comps[d->type_id].data() + flc->offset) = (int32_t)m.line_count;
    if (const mq_field_desc* fts = mq_reflect_field(d, "text_size")) {
        float* v = (float*)(e.comps[d->type_id].data() + fts->offset);
        v[0] = shown_w; v[1] = shown_h;
    }
    /* render.RichText 用 content_size 承载实测尺寸 */
    if (const mq_field_desc* fcs = mq_reflect_field(d, "content_size")) {
        float* v = (float*)(e.comps[d->type_id].data() + fcs->offset);
        v[0] = shown_w; v[1] = shown_h;
    }
    /* 高度限制 + Shrink 的实际字号（transient；0 = 未生效） */
    if (const mq_field_desc* ffs = mq_reflect_field(d, "fit_font_size"))
        *(float*)(e.comps[d->type_id].data() + ffs->offset) = shrink_mode ? st.font_size : 0.0F;

    /* 锚点：scn.Label 的 x/y 为左上角偏移；ui.* 以节点位置为左上角 */
    float ox = 0, oy = 0;
    read_f32(d, e.comps[d->type_id].data(), "x", &ox);
    read_f32(d, e.comps[d->type_id].data(), "y", &oy);
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
        q.x = e.world_x + ox + gl.x0 + half_w + dx;
        q.y = (float)eng.H - (e.world_y + oy) + gl.y0 + half_h + dy;
        q.half_w = half_w; q.half_h = half_h;
        q.r = cr; q.g = cg; q.b = cb; q.a = ca;
        q.z = 100 + zi;
        eng.pending_quads.push_back(q);
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
#ifdef MQ_PLATFORM_WASM
    else spec = "mem://";
#else
    else if (!remote) {
        /* 默认：**场景目录**旁的 .maque（跑 `scenes/sokoban/sokoban.scn` → scenes/sokoban/.maque；
         * 场景就在 CWD 时 → <CWD>/.maque）。远程场景没有可写目录 → 交给模块的 XDG 兜底。 */
        std::error_code ec;
        const auto abs = std::filesystem::absolute(std::filesystem::path(scn_dir.empty() ? "." : scn_dir) / ".maque", ec);
        spec = "file://" + (ec ? (std::filesystem::path(scn_dir.empty() ? "." : scn_dir) / ".maque").string() : abs.string());
    } else {
        spec = "file://";
    }
#endif

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
    /* 像素倍率：
     *   · 官方后端：显示空间 = 窗口**设备像素** → 只按 UI 缩放 × DPI 缩放；
     *   · 网格链：显示空间 = **渲染目标像素**（设计分辨率 × 渲染倍率）→ 再乘 ss，
     *     这样 --shot 里面板与画面的比例和窗口模式一致。 */
    const float ps = imgui_ui_scale * imgui_content_scale * (pre_ren ? 1.0F : (ss > 0.0F ? ss : 1.0F));
    imgui_pixel_scale = ps > 0.0F ? ps : 1.0F;
    imgui_input_scale = imgui_content_scale;
    if (mq_imgui_init_with_fonts(font_path.empty() ? nullptr : font_path.c_str(),
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
        std::print("runtime: imgui panel OFF\n");
        if (had_opts) std::print("runtime: engine options panel OFF\n");
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

void Engine::imgui_frame_begin(float dt) {
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

    /* 显示空间：
     * · 官方后端模式：ImGui 直接画在 SDL_Renderer 上 → 显示空间 = 窗口**设备像素**
     *   （HiDPI 下大于逻辑尺寸；鼠标逻辑坐标按 imgui_input_scale 换算）。
     * · 自研网格链模式（headless / present off）：显示空间 = 目标像素（面板进 --shot）。
     * 字号/面板尺寸都是**固定 UI 度量**（imgui_resolve_metrics），与窗口/目标大小无关。 */
    float show_w = (float)target_w;
    float show_h = (float)target_h;
    if (imgui_backend) {
        int dw = 0, dh = 0;
        if (platform) mq_platform_drawable_size(platform, &dw, &dh);
        if (dw > 0 && dh > 0) { show_w = (float)dw; show_h = (float)dh; }
        else if (win_w > 0 && win_h > 0) { show_w = (float)win_w; show_h = (float)win_h; }
    }
    const float scale = 1.0F, off_x = 0.0F, off_y = 0.0F;
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
                mq_imgui_set_pos(pad, pad);
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
                std::snprintf(line, sizeof line, "frame %llu  target %dx%d",
                              (unsigned long long)frame, target_w, target_h);
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

/* ---- F4 引擎选项（docs/28）----------------------------------------------------
 * 「引擎选项」面板：渲染模式 / 游戏 3D、2D/UI 图层 / MSAA / 默认太阳光 / 主相机 near·far。
 *   · 纯视图层：不写场景组件、不进快照/校验和、不参与模拟（docs/10 确定性不受影响）；
 *   · 值在 extract_3d / 成帧时读取（Engine 的 opt_* 字段），切场景后仍保留；
 *   · 启动初值可用 `MAQUE_ENGINE_OPTIONS`（CI/截图/演示不必依赖鼠标拖控件）。
 * 环境变量格式（逗号分隔；向量分量用 ':'）：
 *   MAQUE_ENGINE_OPTIONS="mode=wireframe,3d=1,2d=0,msaa=4,sun=0,near=0.01,far=500"
 * 键：mode(rendered|solid|wireframe) / 3d(0|1) / 2d(0|1) / msaa(0|1|4) /
 * sun(0|1) / sun_intensity / sun_dir(x:y:z) / near / far；
 * near 或 far 任一出场即启用主相机 near/far 覆盖。 */
void Engine::engine_options_parse_env() {
    if (opt_env_parsed) return;
    opt_env_parsed = true;
    const char* e = std::getenv("MAQUE_ENGINE_OPTIONS");
    if (!e || !*e) return;
    std::string s(e), item;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = i < s.size() ? s[i] : ',';
        if (c != ',') { if (c != ' ' && c != '\t') item.push_back(c); continue; }
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        const std::string key = eq == std::string::npos ? item : item.substr(0, eq);
        const std::string val = eq == std::string::npos ? std::string() : item.substr(eq + 1);
        if (key == "mode") {
            opt_render_mode = val == "wireframe" ? 2 : (val == "solid" ? 1 : 0);
        } else if (key == "3d") {
            opt_3d_enabled = !(val == "0" || val == "off" || val == "false");
        } else if (key == "2d") {
            opt_2d_enabled = !(val == "0" || val == "off" || val == "false");
        } else if (key == "msaa") {
            const int m = std::atoi(val.c_str());
            opt_msaa = m >= 4 ? 4 : (m == 1 ? 1 : 0);
        } else if (key == "sun") {
            opt_sun_enabled = !(val == "0" || val == "off" || val == "false");
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
        }
        item.clear();
    }
    static const char* const kModeNames[3] = { "rendered", "solid", "wireframe" };
    std::print("runtime: engine options (env) mode={} 3d={} 2d={} msaa={} sun={} intensity={:.2f} cam={}",
               kModeNames[opt_render_mode], opt_3d_enabled ? "on" : "off", opt_2d_enabled ? "on" : "off",
               opt_msaa == 0 ? "scene" : (opt_msaa == 4 ? "4x" : "off"),
               opt_sun_enabled ? "on" : "off", (double)opt_sun_intensity,
               opt_cam_override ? "override" : "scene");
    if (opt_cam_override)
        std::print(" near={:.3f} far={:.3f}", (double)opt_cam_near, (double)opt_cam_far);
    std::print("\n");
}

/* F4 面板内容。调用方保证本帧 ImGui 已 begin（`imgui_frame_begin`）。
 * 布局：默认贴在 stats 面板右侧；窗口太窄时夹紧在窗口内。
 * 位置/尺寸用 FirstUseEver 语义（`mq_imgui_begin_at`）→ 用户拖动/缩放后不会被每帧复位。 */
void Engine::engine_options_panel(float show_w, float show_h) {
    const float pad = 8.0F * imgui_pixel_scale;
    const float gap = 8.0F * imgui_pixel_scale;
    const float w = std::min(imgui_panel_w * 1.30F, std::max(show_w * 0.90F, 120.0F));
    const float h = std::min(imgui_panel_h * 3.65F, std::max(show_h * 0.85F, 100.0F));
    float x = imgui_panel_w > 0.0F ? pad + imgui_panel_w + gap : pad;
    if (x + w > show_w - pad) x = std::max(pad, show_w - w - pad);
    if (!mq_imgui_begin_at("engine options", x, pad, w, h)) { mq_imgui_end(); return; }
    mq_imgui_push_small_font();
    mq_imgui_push_item_width(std::min(w * 0.50F, 150.0F * imgui_pixel_scale));

    mq_imgui_text("选项：只影响本机画面，不改场景");
    mq_imgui_text_disabled("F4 开关本面板 / F2 开关 stats");
    mq_imgui_separator();

    /* ① 调试渲染与游戏图层：只改提交路径，不隐藏 F4 面板自身。 */
    const int mode_sel = opt_render_mode == 2 ? 0 : (opt_render_mode == 1 ? 1 : 2);
    const int new_mode_sel = (int)mq_imgui_combo("渲染模式", (float)mode_sel, "wireframe,solid,rendered");
    const int new_mode = new_mode_sel == 0 ? 2 : (new_mode_sel == 1 ? 1 : 0);
    if (new_mode != opt_render_mode) {
        opt_render_mode = new_mode;
        static const char* const kModeNames[3] = { "rendered", "solid", "wireframe" };
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

    /* ② MSAA：0=跟随场景（与 MAQUE_MSAA），1=关，4=4x */
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
    if (mq_imgui_button("重置为默认")) {
        opt_render_mode = 0;
        opt_3d_enabled = true;
        opt_2d_enabled = true;
        opt_msaa = 0;
        opt_sun_enabled = true;
        opt_sun_intensity = 1.1F;
        opt_sun_dir[0] = 0.35F; opt_sun_dir[1] = -0.85F; opt_sun_dir[2] = -0.4F;
        opt_cam_override = false;            // near/far 下一帧自动跟随场景
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

/* ---- Tracy 性能分析（docs/29）------------------------------------------------------
 * 设计：客户端**手动启动**（F3 / MAQUE_TRACY=1）；未启动时所有宏 no-op。
 * 基础数据：frame/logic/render/present/FPS/draw/实体（每步）+ CPU%/内存（1Hz）。
 * F3 还会拉起 tracy-profiler GUI（独立进程；退出游戏后 trace 仍可看）。
 */
static std::string find_tracy_profiler() {
    namespace fs = std::filesystem;
    std::error_code ec;
#if defined(_WIN32)
    const char* exe_name = "tracy-profiler.exe";
#elif defined(__APPLE__)
    const char* exe_name = "Tracy.app/Contents/MacOS/Tracy";
#else
    const char* exe_name = "tracy-profiler";
#endif
    std::vector<fs::path> cands;
    if (const char* e = std::getenv("MAQUE_TRACY_PROFILER"); e && *e) cands.emplace_back(fs::path(e));
    /* 从 CWD 逐级向上找 `.deps/tracy/<exe>`（典型：仓库根 或 build-win/samples/player 启动）。 */
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 6 && !dir.empty() && !ec; ++i) {
        cands.push_back(dir / ".deps" / "tracy" / exe_name);
        cands.push_back(dir / "tools" / "tracy" / exe_name);
        dir = dir.parent_path();
    }
    for (const auto& c : cands)
        if (fs::exists(c, ec) && !ec) return c.string();
    return {};
}

void Engine::tracy_start(const char* why) {
    if (maque::tracy::started()) return;
    if (!maque::tracy::startup()) {
        std::print(stderr, "runtime: [tracy] 本构建未编入 Tracy（MAQUE_TRACY=OFF）\n");
        return;
    }
    /* 基础数据曲线：颜色与模块色一致（frametime=蓝 / logic=绿 / render=棕 / present=紫 …）。 */
    MQ_TRACY_PLOT_CONFIG("frametime", 0, false, true, 0x4E79A7);
    MQ_TRACY_PLOT_CONFIG("logic", 0, false, true, 0x59A14F);
    MQ_TRACY_PLOT_CONFIG("render", 0, false, true, 0x9C755F);
    MQ_TRACY_PLOT_CONFIG("present", 0, false, true, 0xB07AA1);
    MQ_TRACY_PLOT_CONFIG("fps", 0, false, true, 0xEDC948);
    MQ_TRACY_PLOT_CONFIG("draws", 0, false, true, 0x9E9E9E);
    MQ_TRACY_PLOT_CONFIG("entities", 0, false, true, 0x8CD17D);
    MQ_TRACY_PLOT_CONFIG("cpu", 2 /*percentage*/, false, true, 0xF28E2B);
    MQ_TRACY_PLOT_CONFIG("memory", 1 /*memory*/, false, true, 0x76B7B2);
    maque::tracy::thread_name("main");
    MQ_TRACY_MESSAGE("Tracy client started");
    const char* port = std::getenv("MAQUE_TRACY_PORT");
    std::print("runtime: [tracy] client started ({}) — listening on 127.0.0.1:{}\n", why,
               port && *port ? port : "8086");
}

void Engine::tracy_launch_gui() {
    if (tracy_gui_launched) return;
    if (!maque::tracy::started()) return;          // 未编入/启动失败：不弹 GUI
    const std::string exe = find_tracy_profiler();
    if (exe.empty()) {
        std::print(stderr,
                   "runtime: [tracy] 未找到 tracy-profiler；Windows 跑 tools/tracy/get-profiler.ps1 下载、"
                   "Linux/macOS 跑 tools/tracy/build-profiler.sh 构建 GUI，"
                   "或设 MAQUE_TRACY_PROFILER=<path>\n");
        return;
    }
    const char* env_port = std::getenv("MAQUE_TRACY_PORT");
    const std::string port = (env_port && *env_port) ? env_port : "8086";
    const std::string args = "-a 127.0.0.1 -p " + port;
    const int rc = platform ? mq_platform_spawn_detached(platform, exe.c_str(), args.c_str()) : 1;
    if (rc == 0) {
        tracy_gui_launched = true;
        std::print("runtime: [tracy] launched profiler '{} {}'\n", exe, args);
    } else {
        std::print(stderr, "runtime: [tracy] 启动 profiler 失败（rc={}）: '{}'\n", rc, exe);
    }
}

void Engine::tracy_tick() {
    if (!tracy_env_checked) {
        tracy_env_checked = true;
        const char* e = std::getenv("MAQUE_TRACY");
        if (e && *e && std::strcmp(e, "0") != 0) tracy_start("MAQUE_TRACY");
    }
    if (!maque::tracy::started()) return;
    /* 1Hz：进程 CPU%/常驻内存（Tracy 基础数据）。 */
    if (frame % 60 != 0 || !platform) return;
    double cpu = 0.0;
    uint64_t mem = 0;
    if (mq_platform_process_stats(platform, &cpu, &mem) == 0) {
        MQ_TRACY_PLOT("cpu", cpu);
        MQ_TRACY_PLOT("memory", (double)mem);
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
    std::string p = path ? path : "";
    static constexpr const char* kRt = "rendertexture://";
    if (p.rfind(kRt, 0) == 0) {
        const std::string tail = p.substr(std::strlen(kRt));
        if (tail.find('/') == std::string::npos) {
            const int owner = e->active_instance ? e->active_instance->id : 0;
            p = std::string(kRt) + std::to_string(owner) + "/" + tail;
        }
        return (float)e->load_texture(p);
    }
    if (p.empty() || p[0] != '/') p.insert(p.begin(), '/');
    return (float)e->load_texture(p);
}
const char* camera_texture(void* ctx, float camera) {
    auto* e = (Engine*)ctx;
    const int id = (int)camera;
    if (const Engine::RenderTextureState* rt = e->render_texture_for(id)) {
        e->script_ret = rt->uri;
        return e->script_ret.c_str();
    }
    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*e, id, "render.RenderTexture", &d);
    Entity* ent = e->scene ? e->scene->at(id) : nullptr;
    if (!b || !d || !ent) { e->script_ret.clear(); return e->script_ret.c_str(); }
    const std::string* configured = read_str(d, b->data(), "name");
    const std::string name = configured && !configured->empty() ? *configured
                                                                 : (!ent->name.empty() ? ent->name : "camera-" + std::to_string(id));
    const int owner = e->entity_instance.count(id) ? e->entity_instance[id] : 0;
    e->script_ret = "rendertexture://" + std::to_string(owner) + "/" + name;
    return e->script_ret.c_str();
}
float camera_update(void* ctx, float camera) {
    auto* e = (Engine*)ctx;
    const int id = (int)camera;
    if (Engine::RenderTextureState* rt = e->render_texture_for(id)) { rt->dirty = true; return 1.0F; }
    return comp_by_name(*e, id, "render.RenderTexture", nullptr) ? 1.0F : 0.0F;
}
float camera_capture(void* ctx, float camera, const char* path) {
    auto* e = (Engine*)ctx;
    const int id = (int)camera;
    if (!path || !*path || !comp_by_name(*e, id, "render.RenderTexture", nullptr)) return 0.0F;
    if (Engine::RenderTextureState* rt = e->render_texture_for(id)) rt->dirty = true;
    e->camera_captures.push_back({ id, path });
    return 1.0F;
}
float entity_spawn(void* ctx) {
    auto* eng = (Engine*)ctx;
    const int id = eng->scene->spawn();
    if (id >= 0) eng->entity_instance[id] = eng->active_instance ? eng->active_instance->id : 0;
    return (float)id;
}
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
    std::string assigned = value ? value : "";
    *(std::string*)(blob->data() + f->offset) = assigned;
    /* 改了资源引用就**当场解析并缓存**。
     * 为什么必须在这里做：渲染提取（Scene::extract）是**只读**的（docs/13 §8 / docs/15 §1：
     * 不得触发加载），它只查 material_cache / tex_cache。装配期会预解析场景自带实体的资源，
     * 但**脚本运行时生成的**实体没有那次机会 —— 于是贴图缺失、图元退化成纯色方块
     *（实测：动态生成的水管全是 sprite 默认蓝色，而不是贴图的绿色）。
     * 所以"赋值即解析"是这条路径上唯一能补齐的时机。 */
    if (d->role & MQ_ROLE_SPRITE) {
        const std::string& ref = assigned;
        if (!ref.empty()) {
            if (f == mq_reflect_field(d, "material")) {
                scn::SpriteMaterial m{};
                if (eng->load_material(ref, m)) {
                    eng->material_cache[ref] = m;
                    if (!m.texture.empty()) eng->load_texture_ref(eng->scoped_texture_ref((int)id, m.texture));
                }
            } else if (f == mq_reflect_field(d, "texture")) {
                eng->load_texture_ref(eng->scoped_texture_ref((int)id, ref));
            }
        }
    }
    return 1.0F;
}

/* ---- 事件（node.on/off/emit；emit 沿父链冒泡，docs/32 §4.2/§4.3）---- */
float node_on(void* ctx, float id, const char* event, const char* handler) {
    auto* eng = (Engine*)ctx;
    if (!eng->scene || !eng->scene->at((int)id) || !event || !handler) return 0.0F;
    /* 归属：全局脚本 = -3、常驻覆盖层 = -2（两者都可监听任意节点，保持旧能力）；
     * 否则 = 当前脚本所属实例。
     * 子实例脚本只能注册自己子树内的节点（docs/32 §4.2：越界显式拒绝，不静默生效）。 */
    const int owner = eng->in_global ? -3
                      : eng->in_overlay ? -2
                      : (eng->active_instance ? eng->active_instance->id : 0);
    if (owner > 0 && !eng->entity_in_instance_subtree(owner, (int)id)) {
        std::print(stderr, "runtime: node_on rejected — entity {} is outside scene instance {} scope\n",
                   (int)id, owner);
        return 0.0F;
    }
    eng->node_events[(int)id].push_back({ event, handler, owner });
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
    if ((!eng->bus && !eng->overlay_bus && !eng->global_bus && eng->instances.empty()) || !event) return 0.0F;
    const std::string ev = event;
    int hits = 0;
    /* 目标 → 祖先（冒泡，跨实例边界不截断）；处理器只在其注册者所属实例的 bus 里调用。
     * 每层按注册顺序派发（确定性）。 */
    for (int cur = (int)id; cur >= 0; ) {
        auto it = eng->node_events.find(cur);
        if (it != eng->node_events.end()) {
            for (const Engine::NodeHandler& h : it->second) {
                if (h.event != ev) continue;
                if (h.instance == -3) {
                    eng->call_global_event(h.handler.c_str(), (double)cur, (double)value, 0.0, nullptr);
                } else if (h.instance == -2) {
                    eng->call_overlay_event(h.handler.c_str(), (double)cur, (double)value, 0.0, nullptr);
                } else if (SceneInstanceRuntime* ci = eng->instance_at(h.instance)) {
                    eng->call_instance_event(*ci, h.handler.c_str(), (double)cur, (double)value, 0.0, nullptr);
                }
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

/* 场景自省（常驻覆盖层/导航脚本用）：当前场景标题/目录/路径。
 * 绑定层立即拷贝返回串（std::string 成员的生命周期由引擎保证）。 */
const char* scene_title(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng->scene ? eng->scene->desc.title.c_str() : "";
}
const char* scene_dir(void* ctx) { return ((Engine*)ctx)->scn_dir.c_str(); }
const char* scene_path(void* ctx) { return ((Engine*)ctx)->scn_full_path.c_str(); }

/* ---- 场景实例（docs/32 §7.1）：句柄立即分配，构建/卸载在帧边界（apply_instance_ops） ---- */
float scene_instantiate(void* ctx, float parent, const char* path, const char* params) {
    auto* eng = (Engine*)ctx;
    if (!path || !*path) { eng->scene_last_error = "scene_instantiate: missing path"; return -1.0F; }
    const int attach = (int)parent;
    if (attach >= 0 && (!eng->scene || !eng->scene->at(attach))) {
        eng->scene_last_error = "scene_instantiate: parent entity not found";
        return -1.0F;
    }
    const std::string base = eng->active_instance ? eng->active_instance->base : std::string();
    std::string vfs;
    if (!eng->resolve_instance_path(path, base, vfs)) {
        eng->scene_last_error = "scene_instantiate: invalid path";
        return -1.0F;
    }
    int parent_inst = 0;
    if (attach >= 0) {
        SceneInstanceRuntime* owner = eng->instance_of_entity(attach);
        if (owner) parent_inst = owner->id;
    }
    const int id = eng->create_instance(parent_inst, attach, vfs);
    if (id < 0) return -1.0F;
    /* 参数在**入队时**就写入实例记录：帧边界构建 → 子场景 on_start 一定能读到
     * （docs/32 §16.2；省略第三参数时 params 为空串）。 */
    if (SceneInstanceRuntime* inst = eng->instance_at(id); inst && params) inst->params = params;
    eng->instance_ops.push_back({ 0, id, attach, vfs });
    std::print("runtime: scene_instantiate queued #{} '{}' under entity {}\n", id, vfs, attach);
    return (float)id;
}

float scene_unload(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || inst->id == 0 || inst->state == 2 || inst->state == 4) return 0.0F;
    inst->state = 4;                                // pending unload：立刻停止 update
    eng->instance_ops.push_back({ 1, inst->id, -1, std::string() });
    return 1.0F;
}

float scene_reload(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || inst->id == 0 || inst->state != 1) return 0.0F;
    eng->instance_ops.push_back({ 2, inst->id, -1, std::string() });
    return 1.0F;
}

void scene_set_instance_paused(void* ctx, float instance, float paused) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst) return;
    inst->paused = paused != 0.0F;
    if (inst->attach_entity >= 0) eng->ensure_instance_component(inst->attach_entity, inst->id);
}

void scene_set_instance_active(void* ctx, float instance, float active) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst) return;
    inst->active = active != 0.0F;
    if (inst->attach_entity >= 0) {
        if (Entity* e = eng->scene ? eng->scene->at(inst->attach_entity) : nullptr) e->active = inst->active;
        eng->ensure_instance_component(inst->attach_entity, inst->id);
    }
}

float scene_instance_valid(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst && inst->state == 1 ? 1.0F : 0.0F;
}
float scene_instance_root(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst) return -1.0F;
    if (inst->attach_entity >= 0) return (float)inst->attach_entity;
    return inst->roots.empty() ? -1.0F : (float)inst->roots.front();
}
float scene_instance_parent(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst ? (float)inst->parent : -1.0F;
}
const char* scene_instance_source(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst ? inst->source.c_str() : "";
}
float scene_instance_paused(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst && inst->paused ? 1.0F : 0.0F;
}
float scene_instance_count(void* ctx) {
    auto* eng = (Engine*)ctx;
    int n = 0;
    for (auto& up : eng->instances) if (up && up->state == 1) ++n;
    return (float)n;
}
float scene_instance_at(void* ctx, float index) {
    auto* eng = (Engine*)ctx;
    int i = (int)index, n = 0;
    for (auto& up : eng->instances) {
        if (!up || up->state != 1) continue;
        if (n++ == i) return (float)up->id;
    }
    return -1.0F;
}
float scene_instance_of(void* ctx, float entity) {
    auto* eng = (Engine*)ctx;
    auto it = eng->entity_instance.find((int)entity);
    return it == eng->entity_instance.end() ? -1.0F : (float)it->second;
}
float scene_self(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng->active_instance ? (float)eng->active_instance->id : -1.0F;
}
float scene_self_root(void* ctx) {
    auto* eng = (Engine*)ctx;
    if (!eng->active_instance) return -1.0F;
    return eng->active_instance->attach_entity >= 0
               ? (float)eng->active_instance->attach_entity
               : (eng->active_instance->roots.empty() ? -1.0F : (float)eng->active_instance->roots.front());
}
float scene_emit(void* ctx, float instance, const char* event, float value) {
    auto* eng = (Engine*)ctx;
    if (!event || !*event) return 0.0F;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || inst->state != 1) return 0.0F;
    return (float)eng->call_instance_event(*inst, event, 0.0, (double)value, 0.0, nullptr);
}
const char* scene_last_error(void* ctx) { return ((Engine*)ctx)->scene_last_error.c_str(); }

/* ---- 实例参数（docs/32 §16.2）：父场景 → 子场景的配置 ---- */
const char* scene_self_params(void* ctx) {
    auto* eng = (Engine*)ctx;
    return eng->active_instance ? eng->active_instance->params.c_str() : "";
}
const char* scene_instance_params(void* ctx, float instance) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    return inst ? inst->params.c_str() : "";
}
float scene_set_instance_params(void* ctx, float instance, const char* params) {
    auto* eng = (Engine*)ctx;
    SceneInstanceRuntime* inst = eng->instance_at((int)instance);
    if (!inst || !params) return 0.0F;
    inst->params = params;
    if (inst->attach_entity >= 0) eng->ensure_instance_component(inst->attach_entity, inst->id);
    return 1.0F;
}

/* ---- 全局脚本 / 全局状态（docs/32 §16）---- */
float global_emit(void* ctx, const char* event, float value) {
    auto* eng = (Engine*)ctx;
    if (!event || !*event) return 0.0F;
    return (float)eng->call_global_event(event, 0.0, (double)value, 0.0, nullptr);
}
float global_set(void* ctx, const char* name, float value) {
    auto* eng = (Engine*)ctx;
    if (!name || !*name) return 0.0F;
    eng->global_nums[name] = (double)value;
    return 1.0F;
}
float global_get(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return 0.0F;
    auto it = eng->global_nums.find(name);
    return it == eng->global_nums.end() ? 0.0F : (float)it->second;
}
float global_set_text(void* ctx, const char* name, const char* value) {
    auto* eng = (Engine*)ctx;
    if (!name || !*name) return 0.0F;
    eng->global_texts[name] = value ? value : "";
    return 1.0F;
}
const char* global_get_text(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return "";
    auto it = eng->global_texts.find(name);
    return it == eng->global_texts.end() ? "" : it->second.c_str();
}
float global_has(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return 0.0F;
    return (eng->global_nums.count(name) || eng->global_texts.count(name)) ? 1.0F : 0.0F;
}
float global_remove(void* ctx, const char* name) {
    auto* eng = (Engine*)ctx;
    if (!name) return 0.0F;
    const bool had = eng->global_nums.erase(name) + eng->global_texts.erase(name) > 0;
    return had ? 1.0F : 0.0F;
}
void global_clear(void* ctx) {
    auto* eng = (Engine*)ctx;
    eng->global_nums.clear();
    eng->global_texts.clear();
}
const char* global_keys(void* ctx) {
    auto* eng = (Engine*)ctx;
    std::vector<std::string> keys;
    keys.reserve(eng->global_nums.size() + eng->global_texts.size());
    for (auto& [k, v] : eng->global_nums) keys.push_back(k);
    for (auto& [k, v] : eng->global_texts)
        if (!eng->global_nums.count(k)) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    eng->global_ret.clear();
    eng->global_ret += '[';
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) eng->global_ret += ',';
        write_json_string_pub(eng->global_ret, keys[i]);
    }
    eng->global_ret += ']';
    return eng->global_ret.c_str();
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

/* ---- 音频（docs/30）：一次性音效；场景常驻声源用 audio.AudioSource 组件 ---- */
double audio_play(void* ctx, const char* path, float volume, float loop, const char* bus) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio || !path || !*path) return 0.0;
    mq_audio_clip* clip = eng->audio_clip(path);
    if (!clip) return 0.0;
    mq_audio_voice_desc d{};
    d.struct_size = sizeof d;
    d.clip = clip;
    d.volume = volume;
    d.pitch = 1.0F;
    d.loop = (loop != 0.0F) ? 1u : 0u;
    d.bus = (bus && *bus) ? bus : "sfx";
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    return (double)mq_audio_play(eng->audio, &d);
}
void audio_stop(void* ctx, double voice) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_stop(eng->audio, (uint64_t)voice);
}
void audio_pause(void* ctx, double voice, float paused) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_pause(eng->audio, (uint64_t)voice, paused != 0.0F);
}
void audio_set_volume(void* ctx, double voice, float volume) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_set_volume(eng->audio, (uint64_t)voice, volume);
}
void audio_set_pitch(void* ctx, double voice, float pitch) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_set_pitch(eng->audio, (uint64_t)voice, pitch);
}
float audio_state(void* ctx, double voice) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio || voice <= 0.0) return -1.0F;
    uint32_t state = 0;
    if (mq_audio_voice_state(eng->audio, (uint64_t)voice, &state, nullptr) != MQ_AUDIO_OK)
        return -1.0F;
    return (float)state;
}
float audio_playhead(void* ctx, double voice) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio || voice <= 0.0) return -1.0F;
    float head = 0.0F;
    if (mq_audio_voice_state(eng->audio, (uint64_t)voice, nullptr, &head) != MQ_AUDIO_OK)
        return -1.0F;
    return head;
}
float audio_enabled(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->audio) ? 1.0F : 0.0F;
}
void audio_set_listener(void* ctx, float x, float y, float dir_x, float dir_y) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio) return;
    mq_audio_listener l{};
    l.struct_size = sizeof l;
    l.x = x; l.y = y; l.z = 0.0F;
    l.dir_x = dir_x; l.dir_y = dir_y; l.dir_z = 0.0F;
    mq_audio_set_listener(eng->audio, &l);
}
void audio_master_volume(void* ctx, float volume) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio) mq_audio_set_master(eng->audio, volume, 0);
}
void audio_bus_volume(void* ctx, const char* bus, float volume) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && bus && *bus) mq_audio_set_bus(eng->audio, bus, volume, 0);
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

/* Capture.camera：按需回读指定相机的最新 RenderTexture，并以 PPM/base64 分块返回。 */
extern "C" int mq_control_Capture_camera(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int camera = (int)mq_json_num(mq_json_get(params, "camera"), -1);
    Engine::RenderTextureState* rt = eng->render_texture_for(camera);
    if (!rt || !rt->ready) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"camera texture not ready\"}"); return 0; }
    const size_t offset = (size_t)mq_json_num(mq_json_get(params, "offset"), 0);
    size_t max_bytes = (size_t)mq_json_num(mq_json_get(params, "max"), 24 * 1024);
    if (max_bytes == 0 || max_bytes > (1u << 20)) max_bytes = 24 * 1024;
    if (offset > 0 && rt->capture_pixels.empty()) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"snapshot expired; restart at offset 0\"}");
        return 0;
    }
    if (!eng->read_camera_texture(*rt, offset == 0)) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"readback failed\"}"); return 0;
    }
    const int src_w = rt->width, src_h = rt->height;
    const int max_dim = (int)mq_json_num(mq_json_get(params, "max_dim"), 0);
    int w = src_w, h = src_h;
    std::vector<uint8_t> small;
    const uint8_t* src = rt->capture_pixels.data();
    if (max_dim > 0 && (src_w > max_dim || src_h > max_dim)) {
        const int f = src_w > src_h ? (src_w + max_dim - 1) / max_dim : (src_h + max_dim - 1) / max_dim;
        w = std::max(1, src_w / f); h = std::max(1, src_h / f);
        small.assign((size_t)w * h * 4u, 0);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t acc[4] = { 0, 0, 0, 0 }, n = 0;
            for (int dy = 0; dy < f; ++dy) for (int dx = 0; dx < f; ++dx) {
                const int sx = x * f + dx, sy = y * f + dy;
                if (sx >= src_w || sy >= src_h) continue;
                const uint8_t* p = src + ((size_t)sy * src_w + sx) * 4u;
                for (int c = 0; c < 4; ++c) acc[c] += p[c];
                ++n;
            }
            for (int c = 0; c < 4; ++c) small[((size_t)y * w + x) * 4u + c] = (uint8_t)(acc[c] / std::max(1u, n));
        }
        src = small.data();
    }
    char header[48];
    const int hlen = std::snprintf(header, sizeof header, "P6\n%d %d\n255\n", w, h);
    const size_t total = (size_t)hlen + (size_t)w * h * 3u;
    auto byte_at = [&](size_t i) -> uint8_t {
        if (i < (size_t)hlen) return (uint8_t)header[i];
        const size_t p = i - (size_t)hlen, px = p / 3u, ch = p % 3u;
        return src[px * 4u + ch];
    };
    const size_t take = std::min(max_bytes, offset < total ? total - offset : 0);
    std::string raw; raw.reserve(take);
    for (size_t i = 0; i < take; ++i) raw += (char)byte_at(offset + i);
    const std::string b64 = b64_encode((const uint8_t*)raw.data(), raw.size());
    std::snprintf(out, cap, "{\"ok\":true,\"camera\":%d,\"w\":%d,\"h\":%d,\"frame\":%llu,"
                            "\"total\":%zu,\"offset\":%zu,\"size\":%zu,\"data\":\"%s\"}",
                  camera, w, h, (unsigned long long)rt->capture_frame, total, offset, take, b64.c_str());
    return 0;
}

extern "C" int mq_control_Capture_camera_save(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int camera = (int)mq_json_num(mq_json_get(params, "camera"), -1);
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!path || !*path || !comp_by_name(*eng, camera, "render.RenderTexture", nullptr)) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"bad camera/path\"}"); return 0;
    }
    if (Engine::RenderTextureState* rt = eng->render_texture_for(camera)) rt->dirty = true;
    eng->camera_captures.push_back({ camera, path });
    std::snprintf(out, cap, "{\"ok\":true,\"deferred\":true,\"camera\":%d}", camera);
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
    e->entity_instance[id] = 0;                 // 控制面编辑：归世界（根实例）
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
            if (!d || std::strcmp(d->name, "scn.PrefabInstance") != 0) continue;
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

/* ---- 音频控制面（docs/30）：只读查询 + 变更类（D2 由 control_mutating 统一限制）---- */
extern "C" int mq_control_Audio_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->audio) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"audio unavailable\"}"); return 0; }
    mq_audio_stats st{};
    st.struct_size = sizeof st;
    mq_audio_get_stats(e->audio, &st);
    std::snprintf(out, cap,
                  "{\"ok\":true,\"sample_rate\":%u,\"channels\":%u,\"voices\":%u,"
                  "\"callbacks\":%llu,\"dropped\":%llu}",
                  st.sample_rate, st.channels, st.voices,
                  (unsigned long long)st.callbacks, (unsigned long long)st.dropped);
    return 0;
}

extern "C" int mq_control_Audio_voices(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->audio) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"audio unavailable\"}"); return 0; }
    std::string json = "{\"ok\":true,\"voices\":[";
    uint64_t voice = 0;
    uint32_t state = 0;
    float head = 0.0F;
    for (uint32_t i = 0; i < 256; ++i) {
        if (!mq_audio_voice_at(e->audio, i, &voice, &state, &head)) break;
        if (i) json += ',';
        char b[192];
        std::snprintf(b, sizeof b,
                      "{\"voice\":\"%llu\",\"state\":%u,\"playhead\":%.6f}",
                      (unsigned long long)voice, state, (double)head);
        json += b;
    }
    json += "]}";
    std::snprintf(out, cap, "%s", json.c_str());
    return 0;
}

extern "C" int mq_control_Audio_play(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!e->audio || !path || !*path) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path/audio\"}");
        return 0;
    }
    mq_audio_clip* clip = e->audio_clip(path);
    if (!clip) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"load failed\"}"); return 0; }
    mq_audio_voice_desc d{};
    d.struct_size = sizeof d;
    d.clip = clip;
    d.volume = (float)mq_json_num(mq_json_get(params, "volume"), 1.0);
    d.pitch = 1.0F;
    d.loop = mq_json_num(mq_json_get(params, "loop"), 0.0) != 0.0 ? 1u : 0u;
    const char* bus = mq_json_str(mq_json_get(params, "bus"), "sfx");
    d.bus = (bus && *bus) ? bus : "sfx";
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    const uint64_t voice = mq_audio_play(e->audio, &d);
    if (!voice) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"voice pool full\"}"); return 0; }
    std::snprintf(out, cap, "{\"ok\":true,\"voice\":\"%llu\"}", (unsigned long long)voice);
    return 0;
}

extern "C" int mq_control_Audio_stop(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* vs = mq_json_str(mq_json_get(params, "voice"), nullptr);
    const uint64_t voice = (vs && *vs) ? std::strtoull(vs, nullptr, 10) : 0;
    if (!e->audio || !voice) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing voice\"}"); return 0; }
    const int rc = mq_audio_stop(e->audio, voice);
    std::snprintf(out, cap, "{\"ok\":%s}", rc == MQ_AUDIO_OK ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Audio_set_master(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->audio) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"audio unavailable\"}"); return 0; }
    const float volume = (float)mq_json_num(mq_json_get(params, "volume"), 1.0);
    const bool mute = mq_json_num(mq_json_get(params, "mute"), 0.0) != 0.0;
    const int rc = mq_audio_set_master(e->audio, volume, mute ? 1u : 0u);
    std::snprintf(out, cap, "{\"ok\":%s}", rc == MQ_AUDIO_OK ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Audio_set_bus(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* bus = mq_json_str(mq_json_get(params, "bus"), nullptr);
    if (!e->audio || !bus || !*bus) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing bus\"}");
        return 0;
    }
    const float volume = (float)mq_json_num(mq_json_get(params, "volume"), 1.0);
    const bool mute = mq_json_num(mq_json_get(params, "mute"), 0.0) != 0.0;
    const int rc = mq_audio_set_bus(e->audio, bus, volume, mute ? 1u : 0u);
    std::snprintf(out, cap, "{\"ok\":%s}", rc == MQ_AUDIO_OK ? "true" : "false");
    return 0;
}

/* Scene.bake：Scene.expand_prefabs 的兼容别名（已弃用；docs/12 §5 改名） */
extern "C" int mq_control_Scene_bake(void* u, const mq_json* params, char* out, size_t cap) {
    return mq_control_Scene_expand_prefabs(u, params, out, cap);
}

/* Scene.save：把当前世界写回 .scn（编辑器落盘；docs/11 §4）。
 * docs/32 §6.2：场景描述符走**反射**（全部字段零遗漏）；嵌套实例默认**保引用不烘焙**
 * （只写 SceneInstance 组件，生成的子树不落盘）；detached/已烘焙实例才写子树。 */
extern "C" int mq_control_Scene_save(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!path) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path\"}"); return 0; }
    if (!e->scene || !e->scene_type) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no scene\"}"); return 0; }

    /* 1) 描述符：反射序列化全部字段（含 scripts/overlay_script/env/views/...；docs/32 §6.1） */
    std::vector<char> dbuf(8192);
    size_t dneed = mq_persist_write_json(e->reflect, e->scene_type, &e->scene->desc, dbuf.data(), dbuf.size());
    if (dneed + 1 > dbuf.size()) {
        dbuf.resize(dneed + 1);
        dneed = mq_persist_write_json(e->reflect, e->scene_type, &e->scene->desc, dbuf.data(), dbuf.size());
    }
    std::string json;
    if (dneed >= 2 && dbuf[0] == '{') json.assign(dbuf.data(), dneed - 1);   // 去掉最后的 '}'
    else json = "{\"version\":2";

    /* 2) 实体：只写根实例拥有的节点；未被烘焙的子实例内容不写（由 source 重建）。 */
    const auto included = [&](int id) {
        auto it = e->entity_instance.find(id);
        if (it == e->entity_instance.end()) return true;    // 无归属信息（老场景/宿主手工节点）
        int owner = it->second;
        if (owner <= 0) return true;
        /* 只有**本实例已烘焙**才把内容写进父场景；未烘焙实例的内容由 source 重建，
         * 即使其祖先已烘焙也不写（否则重载会重复实例化）。 */
        SceneInstanceRuntime* cur = e->instance_at(owner);
        return cur && cur->detached;
    };
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
        for (auto& [k, c] : e->scene->ents) if (c.parent == id && included(k)) kids.push_back(k);
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
        if (!included(id)) continue;
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

/* ---- 场景实例控制面（docs/32 §7.2）：查询只读；变更类走与脚本同一条帧边界队列 ---- */
extern "C" int mq_control_Scene_instances(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    int live = 0;
    for (auto& up : e->instances) if (up && up->state != 2) ++live;
    std::string json = "{\"ok\":true,\"count\":" + std::to_string(live) + ",\"instances\":[";
    bool first = true;
    for (auto& up : e->instances) {
        if (!up || up->state == 2) continue;
        if (!first) json += ',';
        first = false;
        char head[256];
        std::snprintf(head, sizeof head,
                      "{\"id\":%d,\"parent\":%d,\"attach\":%d,\"paused\":%s,\"active\":%s,\"state\":%d,"
                      "\"error\":\"%s\",\"source\":",
                      up->id, up->parent, up->attach_entity, up->paused ? "true" : "false",
                      up->active ? "true" : "false", up->state, up->error.c_str());
        json += head;
        write_json_string_pub(json, up->source);
        json += ",\"scripts\":[";
        for (size_t i = 0; i < up->scripts.size(); ++i) {
            if (i) json += ',';
            write_json_string_pub(json, up->scripts[i]);
        }
        json += "],\"roots\":[";
        for (size_t i = 0; i < up->roots.size(); ++i) {
            if (i) json += ',';
            json += std::to_string(up->roots[i]);
        }
        json += "]}";
    }
    json += "]}";
    std::snprintf(out, cap, "%s", json.c_str());
    return 0;
}

extern "C" int mq_control_Scene_instance_info(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "instance"), -1);
    SceneInstanceRuntime* up = e->instance_at(id);
    if (!up) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no such instance\"}"); return 0; }
    std::string json = "{\"ok\":true,\"instance\":{";
    char head[320];
    std::snprintf(head, sizeof head,
                  "\"id\":%d,\"parent\":%d,\"attach\":%d,\"paused\":%s,\"active\":%s,\"state\":%d,"
                  "\"error\":\"%s\",\"source\":",
                  up->id, up->parent, up->attach_entity, up->paused ? "true" : "false",
                  up->active ? "true" : "false", up->state, up->error.c_str());
    json += head;
    write_json_string_pub(json, up->source);
    json += ",\"scripts\":[";
    for (size_t i = 0; i < up->scripts.size(); ++i) {
        if (i) json += ',';
        write_json_string_pub(json, up->scripts[i]);
    }
    json += "],\"roots\":[";
    for (size_t i = 0; i < up->roots.size(); ++i) { if (i) json += ','; json += std::to_string(up->roots[i]); }
    json += "]}}";
    std::snprintf(out, cap, "%s", json.c_str());
    return 0;
}

extern "C" int mq_control_Scene_instantiate(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    const char* iparams = mq_json_str(mq_json_get(params, "params"), nullptr);
    const int parent = (int)mq_json_num(mq_json_get(params, "parent"), -1);
    if (!path || !*path) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path\"}"); return 0; }
    const float id = host::scene_instantiate(e, (float)parent, path, iparams);
    if (id < 0) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e->scene_last_error.c_str());
        return 0;
    }
    std::snprintf(out, cap, "{\"ok\":true,\"instance\":%d,\"deferred\":true}", (int)id);
    return 0;
}

extern "C" int mq_control_Scene_unload_instance(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "instance"), -1);
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    SceneInstanceRuntime* up = e->instance_at(id);
    if (!up || up->state == 2) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no such instance\"}"); return 0; }
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true}"); return 0; }
    if (id == 0) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"root instance cannot be unloaded\"}"); return 0; }
    const float ok = host::scene_unload(e, (float)id);
    std::snprintf(out, cap, "{\"ok\":%s,\"deferred\":true}", ok != 0.0F ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Scene_reload_instance(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "instance"), -1);
    const float ok = host::scene_reload(e, (float)id);
    std::snprintf(out, cap, "{\"ok\":%s,\"deferred\":true}", ok != 0.0F ? "true" : "false");
    return 0;
}

/* 把 SceneInstance 标记 detached=true（烘焙；与 Scene.expand_prefabs 对齐）。 */
extern "C" int mq_control_Scene_bake_instances(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    const mq_type_desc* sid = mq_reflect_type_by_name(e->reflect, "scn.SceneInstance");
    int marked = 0;
    if (sid && e->scene) {
        for (auto& [eid, ent] : e->scene->ents) {
            auto it = ent.comps.find(sid->type_id);
            if (it == ent.comps.end()) continue;
            const mq_field_desc* f = mq_reflect_field(sid, "detached");
            if (!f) continue;
            bool& det = *(bool*)(it->second.data() + f->offset);
            if (det) continue;
            if (!dry) det = true;
            if (!dry) {
                if (SceneInstanceRuntime* up = e->instance_by_attach(eid)) up->detached = true;
            }
            ++marked;
        }
    }
    std::snprintf(out, cap, "{\"ok\":true,\"marked\":%d,\"dry_run\":%s}", marked, dry ? "true" : "false");
    return 0;
}

/* World.snapshot：抓当前世界快照（返回字节数与校验和；AI undo/回滚点）。 */
extern "C" int mq_control_World_snapshot(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->snap_manual) e->snap_manual = mq_snapshot_create();     // 独立于周期池（不被 60 帧环覆盖）
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
 *   · http(s)://…  → 挂 URL 目录（基址 = 场景 URL 的父目录）到 "/"
 *   · asset://… → 挂 Android APK asset 目录到 "/"
 * 输出场景在 VFS 里的路径（"/<文件名>"）与来源目录。
 * 返回 false：路径非法或挂载失败（未链接 libcurl 时 URL 挂载返回 UNSUPPORTED）。 */
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
    rapi = (const mq_renderer_v1*)mq_module_query(MQ_RENDERER_API_NAME, 1);
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
        !(rapi = (const mq_renderer_v1*)mq_host_query(host, MQ_RENDERER_API_NAME, 1))) {
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
    /* ---- 3D/PBR：能力自省 + 直接呈现探测（plans/18-pbr-3d.md M0）----
     *   · caps 总是调用（记录适配器能力，MAQUE_RENDER3D_INFO/wgpu 时打印）；
     *   · 直接呈现只在显式 MAQUE_PRESENT=wgpu 时尝试：M1 接通前 attach_window 明确返回失败
     *     → 保持现有「离屏 + platform(SDL_Renderer) 呈现」路径，老 2D 行为不变。 */
    if (rapi->caps && rself) {
        const bool want_wgpu = [] {
            const char* p = std::getenv("MAQUE_PRESENT");
            return p && std::strcmp(p, "wgpu") == 0;
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

/* ---- 窗口 resize / 最大化：自动重建渲染目标（FBO） --------------------------
 * 渲染目标（离屏回读的颜色目标 / 直呈交换链 / gfx 内部 depth + MSAA）尺寸 =
 *   设计分辨率 × ss（× 视图数）。
 * 窗口可 resize/最大化（platform 建窗即带 SDL_WINDOW_RESIZABLE）→ 用户拖拽或系统最大化后
 * SDL 发 SIZE_CHANGED → runtime 每帧对比窗口逻辑尺寸：
 *   · **自动倍率**（默认；未设 MAQUE_RENDER_SCALE / ui.Canvas.render_scale）：
 *     ss 重新取"窗口逻辑宽 / 设计宽" → TW/SHpx/views 全量重建 → gfx 下一帧按新尺寸
 *     建目标（depth/MSAA 按尺寸失效重建；直呈路径显式 resize_surface）= **自动重建 FBO**；
 *   · **显式倍率**：目标固定（像素风/固定分辨率），窗口内由 present 等比缩放 + 留边。
 * headless / MAQUE_WINDOW=0x0 没有窗口 → 整条路径不触发（截图尺寸、文本折行稳定）。
 */

/* 渲染倍率请求：显式来源优先；<=0 表示自动跟随窗口。
 * 只在装配期解析一次（运行期窗口变化由 sync_window_size 走自动分支）。 */
void Engine::resolve_render_scale() {
    render_scale_req = 0.0F;
    if (const char* e = std::getenv("MAQUE_RENDER_SCALE")) render_scale_req = (float)std::atof(e);
    if (render_scale_req <= 0.0F && scene && canvas_entity >= 0) {   /* 画布组件的 render_scale */
        if (Entity* ce = scene->at(canvas_entity)) {
            for (auto& [tid, blob] : ce->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (!d || !(d->role & MQ_ROLE_UI_CANVAS)) continue;
                read_f32(d, blob.data(), "render_scale", &render_scale_req);
                break;
            }
        }
    }
    /* 自动倍率只在"**确实会有窗口**且不是一次性任务"时跟随窗口像素：
     * headless / 截图 / present=off → 1.0（= 设计分辨率），否则截图尺寸与文本折行会漂。 */
#ifdef MQ_RENDERER_STATIC_NULL
    window_intent = false;
#else
    const char* present_env = std::getenv("MAQUE_PRESENT");
    window_intent = !std::getenv("MAQUE_HEADLESS") && !std::getenv("MAQUE_SHOT") &&
                    !(present_env && (std::strcmp(present_env, "off") == 0 || std::strcmp(present_env, "0") == 0));
#endif
}

/* 解析 ss + 渲染目标尺寸 + 每视图 view（含场景 Camera 覆盖）。
 * 装配期调用一次；窗口 resize 时再次调用 = **重建 FBO**。 */
void Engine::rebuild_render_target() {
    if (!scene) return;
    float rs = render_scale_req;
    if (rs <= 0.0F) {
        /* 判据必须是"真的有窗口"：装配期 platform 还没创建 → 用 window_intent + win_w。
         * 不能只看 win_w（默认已是 800x650）：离屏/headless 下若仍按它算，目标会被放大成
         * 800x… → 截图尺寸、文本折行行数全变（实测 shot_e2e / text_e2e 因此失败）。
         * 嵌入宿主（mq_host_create）根本没有 platform → 这里判 window_intent，不碰 platform。 */
        const bool real_window = window || (window_intent && win_w > 0);
        if (real_window && win_w > 0) rs = (float)win_w / (float)(W > 0 ? W : 1);
    }
    if (rs <= 0.0F) rs = 1.0F;
    /* 倍率允许 **< 1**（下限 0.25）：窗口比场景小时目标正好等于窗口像素，present 1:1 更清晰；
     * 上限 4（超采样）。⚠️ 无窗口/headless 时 rs 恒为 1.0（上面已处理）。 */
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
        if (comp_by_name(*this, idx, "render.RenderTexture", nullptr)) continue;
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
}

/* 窗口逻辑尺寸变化（用户拖拽 / 最大化 / 宿主 set_window_size）→ 自动重建渲染目标。
 * 只在有真实窗口时生效；显式倍率时不改目标（present 缩放），但同样打印一行便于诊断。 */
void Engine::sync_window_size(int32_t log_w, int32_t log_h) {
    if (!platform || !window) return;
    if (log_w <= 0 || log_h <= 0) return;              // 最小化/隐藏时部分平台给 0
    if (win_last_w <= 0) { win_last_w = log_w; win_last_h = log_h; return; }   // 首帧采样
    if (log_w == win_last_w && log_h == win_last_h) return;
    const int32_t old_w = win_last_w, old_h = win_last_h;
    const int old_tw = TW, old_sh = (int)(H * ss + 0.5F);
    const float old_ss = ss;
    win_last_w = log_w; win_last_h = log_h;
    win_w = log_w; win_h = log_h;
    rebuild_render_target();
    const int new_sh = (int)(H * ss + 0.5F);
    if (TW != old_tw || new_sh != old_sh) {
        /* 直呈：显式重建交换链/中间目标（ABI 约定"下一帧生效"）。
         * 离屏路径：gfx 每帧按 (TW, SHpx) 建颜色目标，depth/MSAA 缓存按尺寸失效重建。 */
        if (direct_present && rapi && rself && rapi->resize_surface) {
            int32_t surface_w = 0, surface_h = 0;
            mq_platform_drawable_size(platform, &surface_w, &surface_h);
            const int32_t rc = rapi->resize_surface(rself, surface_w, surface_h);
            if (rc != 0) std::print(stderr, "runtime: resize_surface({}x{}) 失败 rc={}\n",
                                    surface_w, surface_h, rc);
        }
        /* 控制面预览缓存与旧尺寸绑定 → 作废（下一帧由渲染路径覆盖） */
        frame_pixels.clear(); frame_w = frame_h = 0;
    }
    std::print("runtime: window resized {}x{} → {}x{}: render target {}x{} → {}x{} (ss {:.3f} → {:.3f}, views={})\n",
               old_w, old_h, log_w, log_h, old_tw, old_sh, TW, new_sh, old_ss, ss, nviews);
}

/* CI/演示钩子：MAQUE_WINDOW_EVENT_AT="<frame>:<op>[;...]" —— 在指定模拟帧对**真实窗口**
 * 下发操作（op = WxH 设尺寸 / max 最大化 / restore 还原），走真实 SDL 窗口事件路径
 * （SDL_SetWindowSize / SDL_MaximizeWindow → SIZE_CHANGED → 平台队列 → sync_window_size），
 * 与用户拖拽标题栏/点最大化按钮**同一条链路**。 */
void Engine::window_event_hooks() {
    if (!platform || !window) return;
    const char* spec = std::getenv("MAQUE_WINDOW_EVENT_AT");
    if (!spec || !*spec) return;
    std::string s(spec), item;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] != ';') { item.push_back(s[i]); continue; }
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const int at = std::atoi(item.substr(0, colon).c_str());
            const std::string op = item.substr(colon + 1);
            if ((int)frame == at) {
                if (op == "max") {
                    const int rc = mq_platform_set_window_maximized(platform, 1);
                    std::print("runtime: window event frame={} maximize (rc={})\n", (long long)frame, rc);
                } else if (op == "restore") {
                    const int rc = mq_platform_set_window_maximized(platform, 0);
                    std::print("runtime: window event frame={} restore (rc={})\n", (long long)frame, rc);
                } else {
                    int w = 0, h = 0;
                    if (std::sscanf(op.c_str(), "%dx%d", &w, &h) == 2) {
                        const int rc = mq_platform_set_window_size(platform, w, h);
                        std::print("runtime: window event frame={} resize {}x{} (rc={})\n", (long long)frame, w, h, rc);
                    } else {
                        std::print(stderr, "runtime: MAQUE_WINDOW_EVENT_AT 帧 {} 操作 '{}' 无法识别\n", at, op);
                    }
                }
            }
        }
        item.clear();
    }
}

/* 场景装配（setup 与 director.loadScene 共用）：解析 → 实例化 → 尺寸/视图 → 物理世界
 * → 脚本 VM/绑定 → 监视 → 资源预解析 → 物理组件建体。切场景同样走此处（docs/12/20）。 */
bool Engine::load_scene_into(const std::string& scn_path) {
    MQ_TRACY_ZONE_N(runtime, "scene.load");   // Tracy：场景加载（资源/实体/脚本装配）
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
    reset_instances();                   // 实例树/全部实例 VM（覆盖层是独立总线，不在此列）
    spine_release();                     // Spine 实例随世界一起释放（派生数据，重建即可）
    clear_asset_caches();                // ⚠️ 纹理/网格缓存键是相对路径，换场景挂载点后必须清空
    agent_paths.clear();
    node_events.clear();
    physics_role_bodies.clear();
    particles.clear();
    if (audio) {
        for (auto& kv : audio_clips) mq_audio_clip_release(kv.second);
        audio_clips.clear();
        last_listener_master = 1.0F;
    }
    director_paused = false;
    /* 实体 id 水位跨场景保留：旧场景的 id 永不复用（常驻脚本/快照引用的陈旧 id 必须
     * 保持无效，绝不能别名到新场景的同号实体）。 */
    const int preserved_entity_id = scene ? scene->next_id : 0;
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
    if (create_instance(-1, -1, scn_vfs) < 0) return false;
    if (SceneInstanceRuntime* root = instance_at(0)) { root->state = 1; }
    if (!scene->deserialize(*this, text)) return false;
    scene->instantiate(*this);
    scene_chain.pop_back();
    scene->flatten_world(*this);
    {
        SceneInstanceRuntime* root = instance_at(0);
        if (root) {
            for (auto& [eid, ent] : scene->ents)
                if (ent.parent < 0 && entity_instance.find(eid) != entity_instance.end()
                    && entity_instance[eid] == 0)
                    root->roots.push_back(eid);
            std::sort(root->roots.begin(), root->roots.end());
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
    /* 渲染倍率 + 渲染目标（ss/TW/views）：解析一次；窗口 resize 时用同一条路径重建 FBO。 */
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
            for (auto& up : instances) {
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
    destroy_render_textures();           // 相机离屏目标（公共纹理句柄）
    destroy_shader_passes();             // 场景资源 shader pass（句柄归渲染器，先于销毁渲染器释放）
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

/* ---- 音频生命周期（docs/30）---- */
void Engine::audio_setup() {
    mq_audio_desc d{};
    d.struct_size = sizeof d;
    d.sample_rate = 48000;
    d.channels = 2;
    d.buffer_frames = 1024;
    d.max_voices = 64;
    d.backend = MQ_AUDIO_BACKEND_NULL;   // 默认静音：逻辑/脚本可用，CI 不弹设备
    bool want_device = false;
    if (const char* env = std::getenv("MAQUE_AUDIO"); env && *env) {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0 ||
            std::strcmp(env, "null") == 0) {
            d.backend = MQ_AUDIO_BACKEND_NULL;
        } else if (std::strcmp(env, "sdl") == 0) {
            d.backend = MQ_AUDIO_BACKEND_SDL;
            want_device = true;
        } else {   // 1 / on / auto
            d.backend = MQ_AUDIO_BACKEND_AUTO;
            want_device = true;
        }
    }
    if (const char* dev = std::getenv("MAQUE_AUDIO_DEVICE"); dev && *dev) d.device = dev;
    if (const char* buf = std::getenv("MAQUE_AUDIO_BUFFER"); buf && *buf) {
        const int v = std::atoi(buf);
        if (v > 0) d.buffer_frames = (uint32_t)v;
    }
    const char* cap_path = std::getenv("MAQUE_AUDIO_CAPTURE");
    const bool capture = cap_path && *cap_path;
    if (capture) {
        /* 抓音必须独占混音推进（否则设备回调 + 抓音各 mix 一次 → playhead 双倍消耗） */
        d.backend = MQ_AUDIO_BACKEND_NULL;
        want_device = false;
    }
    audio = mq_audio_create(&d);
    if (audio) {
        mq_audio_stats st{};
        st.struct_size = sizeof st;
        mq_audio_get_stats(audio, &st);
        audio_cap_rate = st.sample_rate;
        audio_cap_channels = st.channels;
        std::print("runtime: audio {} backend rate={} ch={}\n",
                   capture ? "capture(null pump)"
                           : (want_device ? "device requested" : "null (MAQUE_AUDIO=off/null)"),
                   st.sample_rate, st.channels);
        if (capture) audio_capture_start(cap_path);
    } else {
        std::print(stderr, "runtime: audio init failed（脚本 audio_* 将返回失败）\n");
    }
}

void Engine::audio_teardown() {
    audio_capture_finish();
    if (audio) {
        mq_audio_destroy(audio);   // 内含 stop/close（join 音频线程）
        audio = nullptr;
    }
    for (auto& kv : audio_clips) mq_audio_clip_release(kv.second);
    audio_clips.clear();
}

void Engine::audio_capture_start(const char* path) {
    audio_cap = std::fopen(path, "wb");
    if (!audio_cap) {
        std::print(stderr, "runtime: audio capture open failed '{}'\n", path);
        return;
    }
    std::array<uint8_t, 44> header{};   // 先占位，退出时回填
    std::fwrite(header.data(), 1, header.size(), audio_cap);
    audio_cap_path = path;
    audio_cap_frames = 0;
    audio_cap_frac = 0.0;
    std::print("runtime: audio capture -> {}\n", path);
}

void Engine::audio_capture_step(float dt) {
    if (!audio_cap || !audio) return;
    audio_cap_frac += (double)dt * (double)audio_cap_rate;
    uint32_t frames = (uint32_t)audio_cap_frac;
    if (frames == 0) return;
    audio_cap_frac -= (double)frames;
    if (frames > 4800) frames = 4800;                 // 单帧上限（防 dt 异常）
    audio_cap_buf.assign((size_t)frames * 2, 0.0F);
    if (mq_audio_mix(audio, audio_cap_buf.data(), frames) != MQ_AUDIO_OK) return;
    std::vector<int16_t> pcm((size_t)frames * 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float v = std::clamp(audio_cap_buf[i], -1.0F, 1.0F);
        pcm[i] = (int16_t)std::lround(v * 32767.0F);
    }
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), audio_cap);
    audio_cap_frames += frames;
}

void Engine::audio_capture_finish() {
    if (!audio_cap) return;
    const uint32_t channels = audio_cap_channels ? audio_cap_channels : 2;
    const uint32_t data_bytes = (uint32_t)(audio_cap_frames * channels * 2u);
    const uint32_t byte_rate = audio_cap_rate * channels * 2u;
    std::array<uint8_t, 44> h{};
    auto w16 = [&](size_t off, uint16_t v) { h[off] = (uint8_t)(v & 0xFF); h[off + 1] = (uint8_t)(v >> 8); };
    auto w32 = [&](size_t off, uint32_t v) {
        h[off] = (uint8_t)(v & 0xFF); h[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        h[off + 2] = (uint8_t)((v >> 16) & 0xFF); h[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    };
    std::memcpy(h.data(), "RIFF", 4);
    w32(4, 36u + data_bytes);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);                       // PCM
    w16(22, (uint16_t)channels);
    w32(24, audio_cap_rate);
    w32(28, byte_rate);
    w16(32, (uint16_t)(channels * 2u));
    w16(34, 16);
    std::memcpy(h.data() + 36, "data", 4);
    w32(40, data_bytes);
    std::fseek(audio_cap, 0, SEEK_SET);
    std::fwrite(h.data(), 1, h.size(), audio_cap);
    std::fclose(audio_cap);
    audio_cap = nullptr;
    std::print("runtime: audio capture done '{}' frames={}\n", audio_cap_path,
               (unsigned long long)audio_cap_frames);
}

mq_audio_clip* Engine::audio_clip(const std::string& path) {
    if (!audio || path.empty()) return nullptr;
    auto it = audio_clips.find(path);
    if (it != audio_clips.end()) return it->second;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data || n == 0) {
        std::print(stderr, "runtime: audio clip not found '{}'\n", path);
        return nullptr;
    }
    mq_audio_clip* clip = nullptr;
    const int rc = mq_audio_load_memory(audio, data, n, path.c_str(), &clip);
    mq_vfs_free(data);
    if (rc != MQ_AUDIO_OK || !clip) return nullptr;
    audio_clips[path] = clip;
    return clip;
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
        /* stdout 接管道时是块缓冲：逐行控制（inspector/e2e 的"写一条读一条"）必须每次响应后
         * flush，否则对端读不到 → 交互式驱动永久阻塞。 */
        auto serve = [&](const char* r) {
            std::print("{}\n", mq_control_dispatch(eng->control, r));
            std::fflush(stdout);
        };
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
        maque::tracy::shutdown();        // Tracy：冲刷数据（若本次运行启动过；普通运行是 no-op）
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
    maque::tracy::shutdown();            // Tracy：冲刷数据（未启动时 no-op）
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
    /* 嵌入宿主：如果本进程启动过 Tracy（F3/MAQUE_TRACY），退出时冲刷。多实例同时销毁时
     * 第一个 destroy 就会 shutdown —— 嵌入式 profile 默认按单实例使用（docs/29）。 */
    maque::tracy::shutdown();
}

extern "C" int mq_host_step(mq_host_scene* s) {
    if (!s || !s->eng) return 1;
    s->eng->step_frame(false);
    return 0;
}

extern "C" int mq_host_tick(mq_host_scene* s, uint8_t* out, size_t out_size) {
    if (!s || !s->eng) return 1;
    s->eng->step_frame(true);
    Engine& e = *s->eng;
    // 渲染该帧到宿主缓冲（step_frame 内部已渲染；这里补一次输出到调用方缓冲）
    std::vector<mq_renderer_item> draws;
    std::vector<mq_renderer_mesh> meshes;
    if (e.scene) { e.scene->extract(e, draws); e.spine_extract(meshes); }
    /* 渲染目标是 **缩放后** 的像素尺寸（W×ss, H×ss），与 step_frame 的 SHpx 同口径；
     * 之前误用 e.H → 窗口缩放（ss≠1）时 views 是 800x600、目标却按 800x240 建 → wgpu 报
     * "Scissor Rect ... is not contained in the render target" 直接 abort（嵌入宿主必现）。 */
    const int SHpx = (int)(e.H * e.ss + 0.5F);
    if (!out || out_size < (size_t)e.TW * (size_t)SHpx * 4) return 2;
    MQ_TRACY_ZONE_N(render, "frame");   // Tracy：宿主侧补渲染（mq_host_tick 的额外一次成帧）
    float bg[4] = { e.bg[0], e.bg[1], e.bg[2], e.bg[3] };
    const int rc = (!meshes.empty() && e.rapi->render_mesh_views)
        ? e.rapi->render_mesh_views(e.rself, e.TW, SHpx, e.views.data(), (uint32_t)e.views.size(),
                                    draws.data(), (uint32_t)draws.size(),
                                    meshes.data(), (uint32_t)meshes.size(), bg, out, (uint32_t)out_size)
        : e.rapi->render_views(e.rself, e.TW, SHpx, e.views.data(), (uint32_t)e.views.size(),
                               draws.data(), (uint32_t)draws.size(), bg, out, (uint32_t)out_size);
    return rc == 0 ? 0 : 3;
}

extern "C" void mq_host_size(mq_host_scene* s, int* w, int* h) {
    if (!s || !s->eng) return;
    if (w) *w = s->eng->TW;
    if (h) *h = (int)(s->eng->H * s->eng->ss + 0.5F);   // 缩放后的目标高度（与 mq_host_tick 一致）
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
