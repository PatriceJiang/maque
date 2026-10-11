/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */
#include "runtime/mq_runtime.h"
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
#include <utility>
#include <functional>
#include <csignal>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>

namespace {

struct Entity {
    std::string name;
    int parent = -1;
    float x = 0, y = 0;                                    // 局部坐标（父空间）
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
    void compute_world(Engine& eng);
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
    mq_script_vm*       vm = nullptr;
    mq_reflect_ctx*     reflect = nullptr;                 // per-instance
    mq_obs_ctx*         obs = nullptr;                     // 可观测（事件/指标）
    mq_control*         control = nullptr;                 // 控制平面（JSON-RPC）
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
    uint32_t            reloads = 0;
    uint64_t            frame = 0;                          // 模拟帧号（= sim time / dt）
    uint64_t            last_checksum = 0;
    std::vector<std::pair<uint64_t, uint64_t>> trail;       // (frame, checksum) —— 失步定位
    bool                window = false;                     // 是否真窗口（有显示环境）
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
    /* 输入模拟器（docs/19）：.sim.json 场景 → 帧索引输入事件流（确定性；与录制/回放一致） */
    mq_sim* sim = nullptr;
    bool sim_active = false;
    std::string sim_path;                   // MAQUE_SIM 载入的场景文件（自省用）
    void sim_setup();                       // 装配期：MAQUE_SIM / MAQUE_SIM_CLICK → 载入场景
    void sim_feed(uint64_t frame);           // 每帧：把到期事件注入输入流（窗口坐标→世界）
    /* 文本渲染（docs/13/17）：字体实例 + 按字号生成的图集纹理 id（每字号一张） */
    mq_font* font = nullptr;
    std::string font_path;
    struct FontAtlas { float size; int texture; uint32_t w, h; };
    std::vector<FontAtlas> font_atlases;
    int glyph_texture(float size);          // 取（必要时创建）该字号的字形图集纹理
    void text_setup();                      // 装配期：加载字体（MAQUE_FONT → 系统字体 → 内置）
    uint64_t stop_frame = 0;                                // >0 时到该帧停止（CI 固定长度跑）
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
    bool reload_script(std::string* err = nullptr);          // 重载脚本（帧边界调用，docs/14）
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
inline void on_fatal_signal(int sig) {
    g_signal_hit = sig;
    g_signal_name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT"
                  : sig == SIGFPE ? "SIGFPE" : sig == SIGINT ? "SIGINT" : "signal";
    if (g_crash_fd >= 0 && g_crash_len > 0) {
        const ssize_t ignored = write(g_crash_fd, g_crash_buf, (size_t)g_crash_len);
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
    scene->compute_world(*this);
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
    // ① 采样（设备事件随时入队，固定处理点统一消费，docs/19 §3）
    if (platform) {
        mq_platform_pump(platform);
        if (window) mq_platform_poll(platform, &iframe);
        /* 设备事件：窗口像素 → 世界坐标（只在此处转换一次；引擎内部一律世界坐标） */
        for (uint32_t i = 0; i < iframe.pointer_count; ++i)
            device_event_to_world(*this, &iframe.pointers[i]);
        if (std::getenv("MAQUE_TRACE_INPUT") && (iframe.pointer_count || iframe.key_count))
            std::print("runtime: polled pointers={} keys={} (frame {})\n",
                       iframe.pointer_count, iframe.key_count, frame);
    }
    // ①b 输入模拟器：把该帧到期的事件注入输入流（与设备事件同一条路径；回放时跳过）
    sim_feed(frame);
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
    const float dt = 1.0F / 60.0F;
    if (scene && director_paused) {
        /* director.pause()：跳过模拟（脚本/动画/粒子/导航/物理/UI 布局），仅保留采样与渲染
         * （暂停语义；输入仍被采样，便于 resume 后继续）。 */
    } else if (scene) {
        ++script_frames;
        step_tweens(dt);                             // Tween（补间）
        step_schedules(dt);                          // 定时器（定时器）
        if (mq_event_dispatch(vm, MQ_EVENT_UPDATE, (double)dt, 0.0, 0.0, nullptr) != 0) {
            char ebuf[512] = { 0 };
            mq_script_last_error(vm, ebuf, (int)sizeof ebuf);
            mq_obs_event(obs, "script", 3, "update_failed");
            write_crash_context(ebuf[0] ? ebuf : "script update failed");      // 可精确重放（docs/09 §5）
        }
        mq_script_gc(vm);
        anim_step(dt);                                   // 动画采样写回（docs/15 §2）
        particles_step(dt);                              // 粒子推进（docs/15 §4）
        nav_step();                                      // 导航代理推进（A* + 追踪）
        physics_components_step(dt);                     // 组件驱动物理（RigidBody2D）
        flush_pending_destroy();                         // node.destroy()：帧边界统一销毁
        ui_step();                                       // UI 组件语义（layout/progress/slider/check/safearea/scroll）
        scene->compute_world(*this);
        scene->extract(*this, draws);
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
    std::vector<uint8_t> pixels((size_t)TW * H * 4);
    if (rapi && rself)
        rapi->render_views(rself, TW, H, views.data(), (uint32_t)views.size(),
                           draws.data(), (uint32_t)draws.size(), bg, pixels.data(), (uint32_t)pixels.size());
    /* 呈现到窗口（docs/05、M5）：无窗口/dummy 驱动时为安全空操作 */
    if (platform && window && rapi && rself)
        mq_platform_present(platform, pixels.data(), TW, H);
    if (!frames_dir.empty()) {
        char fn[64]; std::snprintf(fn, sizeof(fn), "/frame_%04d.ppm", (int)frame);
        write_ppm(frames_dir + fn, TW, H, pixels);
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
                p.x = e->x; p.y = e->y;
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
                    const int rc = mq_nav_find_path(nav, e->x, e->y, tx, ty, buf.data(),
                                                   (uint32_t)buf.size() / 2, &n, &need);
                    if (rc == MQ_NAV_ERR_CAPACITY) {
                        buf.resize(need);
                        mq_nav_find_path(nav, e->x, e->y, tx, ty, buf.data(), need, &n, &need);
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
            float x = e->x, y = e->y;
            const float dt = 1.0F / 60.0F;
            const int rc = mq_nav_step_towards(nav, &x, &y, ap.path.data(),
                                               (uint32_t)(ap.path.size() / 2), &ap.index,
                                               speed, dt, arrive);
            host::entity_set_pos(this, (float)id, x, y);
            e->x = x; e->y = y;
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

bool Engine::reload_script(std::string* err) {
    if (script_path.empty() || !vm) { if (err) *err = "no script"; return false; }
    void* d = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, ("/" + script_path).c_str(), &d, &n) != MQ_VFS_OK) {
        if (err) *err = "script not readable";
        return false;
    }
    std::string js((char*)d, n);
    mq_vfs_free(d);
    const uint64_t h = mq_vfs_hash(js.data(), js.size());
    if (h == script_hash) return false;
    /* 脚本以 IIFE 形态定义全局回调（推荐写法，见 scenes/ui.js）：每次重载得到**全新的词法作用域**，
     * 回调被替换、`let/const` 不冲突；状态迁移由脚本显式经 globalThis 载体完成（docs/14 §6）。 */
    if (mq_script_eval(vm, js.c_str(), script_path.c_str()) != 0) {
        char ebuf[512] = { 0 };
        std::string e = mq_script_last_error(vm, ebuf, (int)sizeof ebuf) == 0 ? ebuf : "eval failed";
        if (err) *err = e;
        std::print(stderr, "runtime: hot reload failed for '{}': {}\n", script_path, e);
        return false;
    }
    script_hash = h;
    ++reloads;
    mq_obs_event(obs, "runtime", 1, "script_reloaded");
    std::print("runtime: hot reload script '{}' ({} bytes)\n", script_path, js.size());
    return true;
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
    scene->compute_world(*this);
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
        if (!c->eng->script_path.empty() &&
            p.size() >= c->eng->script_path.size() &&
            p.compare(p.size() - c->eng->script_path.size(), c->eng->script_path.size(), c->eng->script_path) == 0)
            c->eng->reload_script();
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
        g_crash_fd = ::open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        const ssize_t ig = pwrite(g_crash_fd, g_crash_buf, (size_t)g_crash_len, 0);
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
    for (; frame < cap && !quit; ) {
        if (stop_frame && frame > stop_frame) break;
        step_frame();
        if (g_signal_hit) {                              // 收到终止信号 → 落盘上下文后退出
            write_crash_context(g_signal_name ? g_signal_name : "signal");
            mq_obs_event(obs, "runtime", 3, g_signal_name ? g_signal_name : "signal");
            break;
        }
    }

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
                       std::vector<std::string>* chain = nullptr, int depth = 0) {
    if (depth > 64) {
        std::print(stderr, "runtime: prefab nesting too deep (>64), aborted at depth {}\n", depth);
        return -1;
    }
    int idx = sc.spawn();
    if (idx < 0) return -1;
    Entity* ent = sc.at(idx);
    ent->parent = parent;
    const char* nm = mq_json_str(mq_json_get(e, "name"), nullptr);
    if (nm) ent->name = nm;

    const mq_json* comps = mq_json_get(e, "components");
    uint32_t cn = mq_json_len(comps);
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
     * detached=true 表示已 bake（内容已是本场景的一部分），不再展开。 */
    for (auto& [tid, blob] : ent->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
        const std::string* src = read_str(d, blob.data(), "source");
        if (!src || src->empty()) continue;
        bool detached = false;
        if (const mq_field_desc* fd = mq_reflect_field(d, "detached")) detached = *(const bool*)(blob.data() + fd->offset);
        if (detached) continue;
        if (chain) {
            bool cycle = false;
            for (const auto& c : *chain) if (c == *src) { cycle = true; break; }
            if (cycle) {
                std::print(stderr, "runtime: cyclic prefab reference '{}' (entity {}) — skipped\n", *src, idx);
                mq_obs_event(eng.obs, "runtime", 3, "prefab_cycle");
                continue;
            }
        }
        void* pd = nullptr; size_t pn = 0;
        const std::string ppath = "/" + *src;
        if (mq_vfs_read(eng.vfs, ppath.c_str(), &pd, &pn) != MQ_VFS_OK) {
            std::print(stderr, "runtime: prefab not found '{}'\n", *src);
            continue;
        }
        std::string ptext((char*)pd, pn);
        mq_vfs_free(pd);
        mq_json* pdom = mq_json_parse(ptext.c_str(), ptext.size());
        if (pdom) {
            std::vector<std::string> child_chain = chain ? *chain : std::vector<std::string>{};
            child_chain.push_back(*src);
            const mq_json* pents = mq_json_get(pdom, "entities");
            const uint32_t pcount = mq_json_len(pents);
            for (uint32_t i = 0; i < pcount; ++i)
                load_entity(eng, sc, mq_json_at(pents, i), idx, &child_chain, depth + 1);
            mq_json_free(pdom);
            std::print("runtime: prefab '{}' instantiated under entity {}\n", *src, idx);
        }
    }
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
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
        mix(&k, sizeof k); mix(&e.parent, sizeof e.parent); mix(&e.x, sizeof e.x); mix(&e.y, sizeof e.y);
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
    *lx = e.x; *ly = e.y;                      /* 默认：实体自身（无 Transform2D 的纯 UI 节点） */
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
void Scene::compute_world(Engine& eng) {
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
        if (pidx >= 0) { if (Entity* pe = at(pidx)) { e->x = pe->x + lx; e->y = pe->y + ly; } }
        else { e->x = lx; e->y = ly; }
        std::vector<int> kids;
        for (auto& [k, c] : ents) if (c.parent == idx) kids.push_back(k);
        std::sort(kids.begin(), kids.end(), std::greater<int>());   // 栈序 → 最小 id 先处理（稳定）
        for (int k : kids) stack.emplace_back(k, idx);
    }
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
                it.x = e.x + x + w * 0.5F; it.y = (float)eng.H - (e.y + y) - h * 0.5F;
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
            float radius = 8, r = 1, g = 1, b = 1;
            read_f32(d, blob.data(), "radius", &radius);
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
            it.x = e.x; it.y = (float)eng.H - e.y; it.size = radius;
            it.r = r; it.g = g; it.b = b; it.a = 1.0F;
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
                const float cx = e->x, cy = e->y;
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
        const float bx = e->x + x, by = e->y + y;
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
void call_script_event(Engine& eng, const mq_input_event* ev) {
    const int kind = mq_event_for_input(ev->type);
    if (kind < 0) return;
    const char* text = ev->type == MQ_INPUT_TEXT ? ev->text : nullptr;
    mq_event_dispatch(eng.vm, kind, (double)ev->target, (double)ev->x, (double)ev->y, text);
}

/* 窗口像素 → **世界坐标**：等比缩放 + 居中留边（SDL_RenderSetLogicalSize 同规则）+ **y 轴翻转**。
 * 窗口/SDL 事件是 y 向下；引擎世界与脚本是 y 向上（docs/12 §坐标、docs/19 §4）——
 * 漏掉翻转会导致点击位置上下镜像（曾经的真实缺陷）。 */
void window_to_world(const Engine& eng, float* x, float* y) {
    if (eng.win_w <= 0 || eng.win_h <= 0) return;
    /* 呈现按目标尺寸（TW×H）等比缩放；多视图时 x 落在 [0,TW)，视图索引 = x / W */
    const float sc = std::min((float)eng.win_w / (float)eng.TW, (float)eng.win_h / (float)eng.H);
    if (sc <= 0.0F) return;
    const float off_x = ((float)eng.win_w - (float)eng.TW * sc) * 0.5F;
    const float off_y = ((float)eng.win_h - (float)eng.H * sc) * 0.5F;
    const float ty = (*y - off_y) / sc;             // 目标空间 y（向下）
    *x = (*x - off_x) / sc;
    *y = (float)eng.H - ty;                         // → 世界 y（向上）
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
    float t = (px - (e->x - w * 0.5F)) / w;
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


/* 文本：装配期选字体（MAQUE_FONT 环境变量 → 常见系统字体 → 内置 5x7 位图字体）。
 * 图集纹理按字号惰性创建并缓存（确定性：同字号同图集，docs/10）。 */
void Engine::text_setup() {
    if (font) return;
    const char* env = std::getenv("MAQUE_FONT");
    if (env && *env) {
        font = mq_font_load_file(env);
        if (font) font_path = env;
    }
    if (!font) {
        static const char* kCandidates[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/lato/Lato-Medium.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/tuffy/tuffy_bold.ttf",
        };
        for (const char* c : kCandidates) {
            font = mq_font_load_file(c);
            if (font) { font_path = c; break; }
        }
    }
    if (!font) { font = mq_font_builtin(); font_path = "(builtin 5x7)"; }
    std::print("runtime: text font '{}'\n", font_path);
}

int Engine::glyph_texture(float size) {
    const float sz = size > 0.0F ? size : 16.0F;
    for (auto& a : font_atlases) if (a.size == sz) return a.texture;
    if (!font || !rapi || !rself) return -1;
    uint32_t w = 0, h = 0;
    if (mq_font_atlas(font, sz, nullptr, 0, &w, &h) != MQ_TEXT_ERR_CAP) return -1;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    if (mq_font_atlas(font, sz, rgba.data(), rgba.size(), &w, &h) != MQ_TEXT_OK) return -1;
    /* 字形是白字 + alpha：渲染时按字形颜色调制（与材质管线一致） */
    const int tex = rapi->create_texture(rself, rgba.data(), (int32_t)w, (int32_t)h);
    if (tex < 0) return -1;
    font_atlases.push_back({ sz, tex, w, h });
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
    st.line_spacing = line_spacing;
    st.wrap = (uint32_t)(wrap < 0 ? 0 : wrap);
    st.align = (uint32_t)(align < 0 ? 0 : align);
    st.max_width = max_w;
    st.r = r; st.g = g; st.b = b; st.a = a * inherit_alpha;

    /* 先测量（写回 transient 字段，供脚本/控制面读取），再排版 */
    mq_text_metrics m{};
    const int mrc = mq_text_measure(eng.font, &st, text.c_str(), &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text] id={} type={} len={} size={:.0f} wrap={} rc={} lines={}\n",
                   idx, d->name, text.size(), st.font_size, st.wrap, mrc, m.line_count);
    if (mrc != MQ_TEXT_OK) return;
    std::vector<mq_text_glyph> glyphs(m.glyph_count ? m.glyph_count : 1);
    uint32_t n = 0;
    const int lrc = mq_text_layout(eng.font, &st, text.c_str(), glyphs.data(), (uint32_t)glyphs.size(), &n, &m);
    if (std::getenv("MAQUE_TRACE_TEXT"))
        std::print("[text]   layout rc={} glyphs={} pos=({:.0f},{:.0f}) tex={} first=({:.1f},{:.1f},{:.1f}x{:.1f})\n",
                   lrc, n, e.x, e.y, n ? glyph_texture(glyphs[0].size) : -1,
                   n ? glyphs[0].x : 0.0F, n ? glyphs[0].y : 0.0F,
                   n ? glyphs[0].w : 0.0F, n ? glyphs[0].h : 0.0F);
    if (lrc != MQ_TEXT_OK) return;
    /* transient 度量写回 */
    if (const mq_field_desc* fw = mq_reflect_field(d, "text_w"))
        *(float*)(e.comps[d->type_id].data() + fw->offset) = m.width;
    if (const mq_field_desc* fh = mq_reflect_field(d, "text_h"))
        *(float*)(e.comps[d->type_id].data() + fh->offset) = m.height;
    if (const mq_field_desc* flc = mq_reflect_field(d, "line_count"))
        *(int32_t*)(e.comps[d->type_id].data() + flc->offset) = (int32_t)m.line_count;
    if (const mq_field_desc* fts = mq_reflect_field(d, "text_size")) {
        float* v = (float*)(e.comps[d->type_id].data() + fts->offset);
        v[0] = m.width; v[1] = m.height;
    }
    /* render.RichText 用 content_size 承载实测尺寸 */
    if (const mq_field_desc* fcs = mq_reflect_field(d, "content_size")) {
        float* v = (float*)(e.comps[d->type_id].data() + fcs->offset);
        v[0] = m.width; v[1] = m.height;
    }

    /* 锚点：scn.Label 的 x/y 为左上角偏移；ui.* 以节点位置为左上角 */
    float ox = 0, oy = 0;
    read_f32(d, e.comps[d->type_id].data(), "x", &ox);
    read_f32(d, e.comps[d->type_id].data(), "y", &oy);
    for (uint32_t i = 0; i < n; ++i) {
        const mq_text_glyph& gl = glyphs[i];
        const int tex = glyph_texture(gl.size);
        mq_renderer_item it{};
        /* 字形四角：位置为左上角，尺寸取位图尺寸（无位图时跳过） */
        if (gl.w <= 0.0F || gl.h <= 0.0F) continue;
        it.x = e.x + ox + gl.x + gl.w * 0.5F;
        /* 排版坐标 y 向下（行往下排），屏幕 y 亦向下 → 基准翻转后**相加** */
        it.y = (float)eng.H - (e.y + oy) + gl.y + gl.h * 0.5F;
        it.size = gl.w * 0.5F > gl.h * 0.5F ? gl.w * 0.5F : gl.h * 0.5F;
        it.r = gl.r; it.g = gl.g; it.b = gl.b; it.a = gl.a;
        it.texture = tex;
        it.uv0 = gl.u0; it.uv1 = gl.v0; it.uv2 = gl.u1; it.uv3 = gl.v1;
        it.layer = 1; it.z = 100 + (int)i;
        if (std::getenv("MAQUE_TRACE_TEXT") && i < 2)
            std::print("[text]   item[{}] pos=({:.1f},{:.1f}) size={:.2f} col=({:.2f},{:.2f},{:.2f},{:.2f}) tex={} uv=({:.3f},{:.3f},{:.3f},{:.3f})\n",
                       i, it.x, it.y, it.size, it.r, it.g, it.b, it.a, it.texture, it.uv0, it.uv1, it.uv2, it.uv3);
        out.push_back(it);
    }
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
            ev.key = se.key;
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
                    /* 位置经 entity_set_pos 写回 Transform2D 组件（否则被 compute_world 覆盖） */
                    if ((int)mode == 1) {
                        host::entity_set_pos(this, (float)k, e->x + pad_l + cursor + kw * 0.5F,
                                             e->y + pad_t + kh * 0.5F);
                        cursor += kw + spacing;
                    } else if ((int)mode == 2) {
                        host::entity_set_pos(this, (float)k, e->x + pad_l + kw * 0.5F,
                                             e->y + pad_t + cursor + kh * 0.5F);
                        cursor += kh + spacing;
                    } else if ((int)mode == 3) {
                        const int c = columns > 0 ? columns : 1;
                        const int row = col / c, column = col % c;
                        host::entity_set_pos(this, (float)k,
                                             e->x + pad_l + (float)column * (kw + spacing) + kw * 0.5F,
                                             e->y + pad_t + (float)row * (kh + spacing) + kh * 0.5F);
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
                    host::entity_set_pos(this, (float)target, e->x + il, e->y + it);
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
                        const float kx = ke.x;
                        host::entity_set_pos(this, (float)k, kx, e->y - sv);
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
    if (schedules.empty() || !vm) return;
    std::vector<size_t> drop;
    for (size_t i = 0; i < schedules.size(); ++i) {
        Scheduled& sc = schedules[i];
        sc.remaining -= dt;
        if (sc.remaining > 0.0F) continue;
        if (mq_script_has(vm, sc.handler.c_str()))
            mq_script_call1(vm, sc.handler.c_str(), (double)dt, nullptr);
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
        if (auto* col = comp_by_role(*this, id, MQ_ROLE_PHYSICS_COLLIDER)) {
            const mq_type_desc* cd = nullptr;
            for (auto& [tid, b] : e->comps) {
                const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
                if (d && (d->role & MQ_ROLE_PHYSICS_COLLIDER)) { cd = d; break; }
            }
            if (cd) {
                radius = read_num(cd, col->data(), "radius", 8.0F);
                hw = read_num(cd, col->data(), "half_w", 8.0F);
                hh = read_num(cd, col->data(), "half_h", 8.0F);
            }
        }
        if (radius <= 0.0 && hw <= 0.0 && hh <= 0.0) radius = 8.0;   // 默认圆
        mq_physics_body_desc desc{};
        desc.struct_size = (uint32_t)sizeof desc;
        desc.type = is_static != 0.0 ? MQ_PHYSICS_BODY_STATIC : MQ_PHYSICS_BODY_DYNAMIC;
        desc.shape = radius > 0.0 ? MQ_PHYSICS_SHAPE_BALL : MQ_PHYSICS_SHAPE_CUBOID;
        desc.x = e->x; desc.y = e->y;
        desc.radius = (float)radius;
        desc.half_w = (float)hw; desc.half_h = (float)hh;
        desc.mass = 1.0F; desc.restitution = 0.5F; desc.friction = 0.2F;
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
    if (!physics || physics_role_bodies.empty()) return;
    mq_physics_step(physics);                       // 固定步长在世界配置里（docs/10 D1）
    for (auto& [id, body] : physics_role_bodies) {
        Entity* e = scene->at(id);
        if (!e) continue;
        float x = e->x, y = e->y;
        if (mq_physics_get_transform(physics, body, &x, &y) == MQ_PHYSICS_OK) {
            e->x = x; e->y = y;
            host::entity_set_pos(this, (float)id, x, y);   // 写回 Transform2D（否则被 compute_world 覆盖）
        }
    }
}

} // namespace

namespace host {

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
        if (!has_tf) { ent->x = x; ent->y = y; return; }
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
        ent->x = x; ent->y = y;
    }
}
float entity_x(void* ctx, float id) { Entity* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->x : 0.0f; }
float entity_y(void* ctx, float id) { Entity* e = ((Engine*)ctx)->scene->at((int)id); return e ? e->y : 0.0f; }
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
    if (!eng->vm || !event) return 0.0F;
    const std::string ev = event;
    int hits = 0;
    /* 目标 → 祖先（冒泡）；每层按注册顺序派发（确定性） */
    for (int cur = (int)id; cur >= 0; ) {
        auto it = eng->node_events.find(cur);
        if (it != eng->node_events.end()) {
            for (const Engine::NodeHandler& h : it->second) {
                if (h.event != ev) continue;
                if (!mq_script_has(eng->vm, h.handler.c_str())) continue;
                mq_script_call2(eng->vm, h.handler.c_str(), (double)cur, (double)value, nullptr);
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
    for (const char* fn : { "text_h", "text_size", "content_size" })
        if (const mq_field_desc* f = mq_reflect_field(d, fn))
            return *(const float*)(blob->data() + f->offset + sizeof(float));
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
                      first ? "" : ",", idx, ent.name.c_str(), ent.parent, ent.x, ent.y, ent.comps.size());
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

/* Scene.bake：把 PrefabInstance 标记为已展开（detached=true）→ 场景自包含（docs/12 §5）。 */
extern "C" int mq_control_Scene_bake(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    uint32_t baked = 0;
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
            ++baked;
        }
    }
    std::snprintf(out, cap, "{\"ok\":true,\"baked\":%u,\"dry_run\":%s}", baked, dry ? "true" : "false");
    return 0;
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

/* 装配引擎：VFS → 反射 → 渲染插件 → 场景 → 平台/输入 → 脚本 → 控制面。
 * 两条入口（命令行 / 嵌入宿主）共用，保证行为一致。 */
bool Engine::setup(const std::string& scn_path, const char* renderer_override) {
    if (const char* cr = std::getenv("MAQUE_CRASH_REPORT"); cr && *cr) {   // 两条入口共用
        signal(SIGSEGV, on_fatal_signal);
        signal(SIGABRT, on_fatal_signal);
        signal(SIGFPE, on_fatal_signal);
        signal(SIGINT, on_fatal_signal);
        if (g_crash_fd < 0) g_crash_fd = ::open(cr, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    std::filesystem::path scnp(scn_path);
    const std::string scn_dir2 = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();

    vfs = mq_vfs_create();
    mq_vfs_mount(vfs, "/", ("file://" + scn_dir2).c_str());

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, ("/" + scnp.filename().string()).c_str(), &d, &n) == MQ_VFS_OK) { text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) { std::print("runtime: cannot read scene '{}'\n", scn_path); return false; }

    reflect = mq_reflect_create();
    mq_reflect_load_schema(reflect);
    scene_type = mq_reflect_type_by_name(reflect, "scn.Scene");
    if (!scene_type) { std::print("runtime: no scn.Scene type\n"); return false; }

    const char* plugin = renderer_override ? renderer_override : std::getenv("MAQUE_RENDERER");
#ifdef MQ_RENDERER_DEFAULT
    if (!plugin) plugin = MQ_RENDERER_DEFAULT;
#endif
    if (!plugin) plugin = "libmaque_gfx.so";
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
    snaps = mq_snapshot_pool_create(8, 1 << 16);
    { const char* cp = std::getenv("MAQUE_CHECKPOINTS"); if (cp && *cp && *cp != '0') cp_active = true; }
    /* 场景/脚本/资源/物理装配统一走 load_scene_into（与 director.loadScene 同一条路径） */
    control = mq_control_create();
    /* 控制面方法由 DSL（schema.yaml control_list）声明、codegen 生成注册表 —— 引擎零手写注册
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
    const fs::path scnp(scn_path);
    const std::string dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();
    if (vfs) mq_vfs_mount(vfs, "/", ("file://" + dir).c_str());   // 后挂载者优先 → 新场景目录生效

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(vfs, ("/" + scnp.filename().string()).c_str(), &d, &n) == MQ_VFS_OK) {
          text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) return false;

    /* ---- 世界重置（切场景：旧世界全部释放；脚本状态不跨场景保留，等价  onDestroy）---- */
    if (vm) { mq_script_vm_destroy(vm); vm = nullptr; }
    agent_paths.clear();
    node_events.clear();
    physics_role_bodies.clear();
    particles.clear();
    director_paused = false;
    scene_store = Scene();
    scene = &scene_store;
    if (!scene->deserialize(*this, text)) return false;
    scene->instantiate(*this);
    scene->compute_world(*this);

    W = scene->desc.width > 0 ? scene->desc.width : 320;
    H = scene->desc.height > 0 ? scene->desc.height : 240;
    nviews = scene->desc.views > 0 ? scene->desc.views : 1;
    /* 启动期覆盖（宿主/CI 用；docs/05、docs/22）：
     *   MAQUE_WINDOW=WxH  窗口与渲染目标尺寸（世界坐标不变，仅改可视区）
     *   MAQUE_VIEWS=n     视图数（1 = 单视图；>1 = 并排多视图/立体） */
    win_w = W; win_h = H;                       // 默认：窗口 = 世界/目标尺寸
    if (const char* ws = std::getenv("MAQUE_WINDOW")) {
        int ow = 0, oh = 0;
        if (std::sscanf(ws, "%dx%d", &ow, &oh) == 2 && ow > 0 && oh > 0) {
            win_w = ow; win_h = oh;             // 只改窗口；世界/目标不变 → 窗口内等比缩放显示
            std::print("runtime: window {}x{} (world {}x{}, scale {:.2f}x)\n",
                       win_w, win_h, W, H,
                       std::min((float)win_w / (float)W, (float)win_h / (float)H));
        }
    }
    if (const char* vs = std::getenv("MAQUE_VIEWS")) {
        const int v = std::atoi(vs);
        if (v >= 1 && v <= 4) nviews = v;
    }
    TW = W * nviews;
    views.assign((size_t)nviews, mq_renderer_view{});
    for (int i = 0; i < nviews; ++i) {
        mq_renderer_view& v = views[(size_t)i];
        v = mq_renderer_view{};
        v.vx = (float)(i * W); v.vy = 0;
        v.vw = (float)W; v.vh = (float)H;
        v.pan = (float)(i * W);
        v.eye = (i - (nviews - 1) * 0.5F) * scene->desc.eye_sep;
        v.cx = W * 0.5F; v.cy = H * 0.5F;
        v.zoom = 1.0F;
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
            for (auto& v : views) { v.cx = cx; v.cy = cy; v.zoom = zoom > 0 ? zoom : 1.0F; v.rot = rot; }
        }
    }
    rng = scene->desc.seed * 2654435761u + 12345u;
    if (physics) { mq_physics_destroy(physics); physics = nullptr; }
    {
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
    std::print("runtime: scene '{}' title='{}' script='{}' {}x{} views={}\n",
               scn_path, scene->desc.title, scene->desc.script, W, H, nviews);

    script_path = scene->desc.script;
    scn_vfs_path = "/" + scnp.filename().string();
    scn_full_path = scn_path;
    scn_dir = dir;
    /* ---- 脚本 VM：切场景重建（docs/14：脚本不跨场景共享全局态）---- */
    if (!vm) {
        vm = mq_script_vm_create();
        mq_script_set_rng(vm, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
        mq_bind_generated(vm, this);
    }
    if (!script_path.empty()) {
        std::string js;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, ("/" + script_path).c_str(), &d, &n) == MQ_VFS_OK) { js.assign((char*)d, n); mq_vfs_free(d); } }
        if (js.empty()) { std::print("runtime: cannot read script '{}'\n", script_path); return false; }
        script_hash = mq_vfs_hash(js.data(), js.size());
        if (mq_script_eval(vm, js.c_str(), script_path.c_str()) != 0) { std::print("runtime: script error\n"); return false; }
    }
    if (watcher) { mq_watcher_destroy(watcher); watcher = nullptr; }
    const char* watch = std::getenv("MAQUE_WATCH");
    if (watch && *watch && *watch != '0') {
        watcher = mq_watcher_create();
        mq_watcher_add(watcher, (fs::path(scn_dir) / scnp.filename()).string().c_str());
        if (!script_path.empty())
            mq_watcher_add(watcher, (fs::path(scn_dir) / script_path).string().c_str());
        std::print("runtime: hot reload watching {} file(s)\n", mq_watcher_count(watcher));
    }
    preload_scene_assets();
    text_setup();                               // 字体（MAQUE_FONT → 系统字体 → 内置 5x7）
    sim_setup();                                // 输入模拟器（MAQUE_SIM / MAQUE_SIM_CLICK）
    physics_components_setup();
    if (mq_event_dispatch(vm, MQ_EVENT_ON_START, 0.0, 0.0, 0.0, nullptr) != 0) {
        char ebuf[512] = { 0 };
        mq_script_last_error(vm, ebuf, (int)sizeof ebuf);
        write_crash_context(ebuf[0] ? ebuf : "on_start failed");
    }
    return true;
}

void Engine::teardown() {
    /* 释放顺序：先释放"依赖 reflect 的资源"（clips 的字符串成员要经反射析构），再销毁 reflect。 */
    if (control) { mq_control_destroy(control); control = nullptr; }
    if (vm) { mq_script_vm_destroy(vm); vm = nullptr; }
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
    mq_input_destroy(input); input = nullptr;
    mq_platform_destroy(platform); platform = nullptr;
    if (rapi && rself) { rapi->destroy(rself); rself = nullptr; }
    mq_host_destroy(host); host = nullptr;
    mq_vfs_destroy(vfs); vfs = nullptr;
}

extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;
    auto* eng = new Engine();
    if (!eng->setup(scn_path, nullptr)) { eng->teardown(); delete eng; return 2; }

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
    const bool quit = eng->quit;
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
    if (e.scene) e.scene->extract(e, draws);
    if (!out || out_size < (size_t)e.TW * e.H * 4) return 2;
    float bg[4] = { e.bg[0], e.bg[1], e.bg[2], e.bg[3] };
    const int rc = e.rapi->render_views(e.rself, e.TW, e.H, e.views.data(), (uint32_t)e.views.size(),
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
