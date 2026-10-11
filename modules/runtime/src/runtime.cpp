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
#include "core/control/mq_control.h"
#include "core/api/mq_api.h"
#include "maque_api.h"                 // 生成：host:: 声明（api_list）
#include "maque_ifaces.h"                 // 生成：mq_renderer_v1（插件接口 ABI）
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

namespace {

struct Entity {
    std::string name;
    int parent = -1;
    float x = 0, y = 0;                                    // 世界（每帧计算）
    std::unordered_map<uint32_t, std::vector<uint8_t>> comps;   // type_id → blob
};

struct Engine;
struct Scene;
struct SnapshotWriter;

/* 引擎内辅助（定义见 Engine 之后；Scene 与 Engine 互相引用）。 */
void write_json_string_pub(std::string& out, const std::string& s);
void attach_component(Engine& eng, int idx, const char* type_name);
void construct_blob(Engine& eng, const mq_type_desc* d, std::vector<uint8_t>& blob);
void destroy_blob(Engine& eng, uint32_t type_id, std::vector<uint8_t>& blob);

struct Scene {
    scn::Scene desc;
    std::string text_;
    mq_handle_table* table = nullptr;
    Engine* eng = nullptr;                  // 组件析构需要反射 ctx（remove/重建时）
    std::unordered_map<int, Entity> ents;

    Scene() { table = mq_handle_table_create(sizeof(uint8_t), 1); }
    ~Scene() { destroy_all(); if (table) mq_handle_table_destroy(table); }
    Scene(const Scene&) = delete; Scene& operator=(const Scene&) = delete;
    Scene(Scene&& o) noexcept { *this = std::move(o); }
    Scene& operator=(Scene&& o) noexcept {
        if (this == &o) return *this;
        destroy_all();
        if (table) mq_handle_table_destroy(table);
        desc = std::move(o.desc);
        text_ = std::move(o.text_);
        table = o.table; o.table = nullptr;
        eng = o.eng;
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
    uint64_t stop_frame = 0;                                // >0 时到该帧停止（CI 固定长度跑）
    std::string frames_dir;                                 // 每帧 PPM 输出目录（""=不输出）
    std::string name;
    const mq_type_desc* scene_type = nullptr;
    Scene               scene_store;                        // 场景状态（移动后失效）
    std::unique_ptr<Scene> scene_owned;                     // 宿主模式：堆上场景
    Scene*              scene = &scene_store;

    std::unordered_map<std::string, int> tex_cache;
    uint32_t rng = 0;
    bool     quit = false;
    float    bg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    int W = 0, H = 0, nviews = 1, TW = 0;
    std::vector<mq_renderer_view> views;

    uint32_t next_rng() { rng = rng * 1664525u + 1013904223u; return rng; }
    int load_texture(const std::string& path);                 // assets（VFS 编码文件）
    int load_texture_ref(const std::string& ref);              // builtin: 或 assets
    bool load_material(const std::string& ref, scn::SpriteMaterial& out);
    void main_loop(const std::string& frames_dir, const std::string& name);
    bool prepare_replay();                                  // 载入回放日志并设定期望校验和（docs/10 §3）
    bool load_clip(const std::string& ref);                 // 载入动画片段（反射验证轨道路径）
    void nav_step();                                        // 导航代理推进（寻路 + 追踪）
    void anim_step(float dt);                               // 关键帧采样 → 写回字段（docs/15 §2）
    void particles_step(float dt);                          // 确定性粒子推进（docs/15 §4）
    bool setup(const std::string& scn_path, const char* renderer_override);   // 加载并装配引擎
    void teardown();                                        // 释放全部实例资源（幂等）
    void step_frame();                                      // 推进一个模拟帧（单一帧序，docs/10 D1）
    bool run_frames(uint64_t n);                            // 推进 n 帧（控制面 Runtime.step）
    bool reload_script(std::string* err = nullptr);          // 重载脚本（帧边界调用，docs/14）
    bool reload_scene();                                    // 重载场景（重建世界，docs/08）
    uint32_t poll_watcher();                                // 帧边界检查资源变化
    void checkpoint_push();                                 // 记录一个检查点（周期性）
    void write_crash_context(const char* why);              // 崩溃/异常上下文（docs/09 §5、docs/10 §7）
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
        for (uint32_t i = 0; i < d->field_count; ++i) {
            const mq_field_desc* f = &d->fields[i];
            if (f->kind == MQ_TK_STRING) reinterpret_cast<std::string*>(blob.data() + f->offset)->~basic_string();
            else if (f->kind == MQ_TK_ARRAY_DYN && f->container_resize) f->container_resize(blob.data() + f->offset, 0);
        }
    }
    blob.clear();
}

/* 默认构造组件 blob（走"空 JSON"反序列化，schema 默认值生效）。 */
void construct_blob(Engine& eng, const mq_type_desc* d, std::vector<uint8_t>& blob) {
    blob.assign(d->size, 0);
    if (d->flags & MQ_TYPE_DYNAMIC) {
        for (uint32_t i = 0; i < d->field_count; ++i) {
            const mq_field_desc* f = &d->fields[i];
            if (f->kind == MQ_TK_STRING) new (blob.data() + f->offset) std::string();
        }
    }
    mq_json* empty = mq_json_parse("{}", 2);
    if (empty) { mq_persist_apply_json(eng.reflect, d, empty, blob.data()); mq_json_free(empty); }
}

/* 组件挂载：由**描述符**构造默认实例（schema 默认值生效）→ 引擎不硬编码组件类型。 */
void attach_component(Engine& eng, int idx, const char* type_name) {
    Entity* e = eng.scene->at(idx);
    const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, type_name);
    if (!e || !d) return;
    e->comps[d->type_id].assign(d->size, 0);
    /* 默认值：用描述符字段默认（反射无默认值 API）→ 走"空 JSON"反序列化保持与场景一致 */
    mq_json* dom = mq_json_parse("{}", 2);
    if (dom) { mq_persist_apply_json(eng.reflect, d, dom, e->comps[d->type_id].data()); mq_json_free(dom); }
    else e->comps[d->type_id].assign(d->size, 0);
}

/* ---- 终止信号（docs/09 §5）：处理器只置位并恢复默认行为，写盘在帧边界 ---- */
volatile sig_atomic_t g_signal_hit = 0;
const char* g_signal_name = nullptr;
inline void on_fatal_signal(int sig) {
    g_signal_hit = sig;
    g_signal_name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT"
                  : sig == SIGFPE ? "SIGFPE" : sig == SIGINT ? "SIGINT" : "signal";
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
    int rc = mq_image_decode(data, n, &rgba, &w, &h);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, mq_image_last_error()); return -1; }
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
    static thread_local mq_input_frame iframe{};   // 缓冲由平台写入（宿主预分配语义）
    // ① 采样（设备事件随时入队，固定处理点统一消费，docs/19 §3）
    if (platform) {
        mq_platform_pump(platform);
        if (window) mq_platform_poll(platform, &iframe);
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
    // ③ 脚本：update(dt) → 帧边界 GC（确定性，docs/14）
    std::vector<mq_renderer_item> draws;
    const float dt = 1.0F / 60.0F;
    if (scene) {
        if (mq_script_has(vm, "update") && mq_script_call1(vm, "update", dt, nullptr) != 0) {
            char ebuf[512] = { 0 };
            mq_script_last_error(vm, ebuf, (int)sizeof ebuf);
            mq_obs_event(obs, "script", 3, "update_failed");
            write_crash_context(ebuf[0] ? ebuf : "script update failed");      // 可精确重放（docs/09 §5）
        }
        mq_script_gc(vm);
        anim_step(dt);                                   // 动画采样写回（docs/15 §2）
        particles_step(dt);                              // 粒子推进（docs/15 §4）
        nav_step();                                      // 导航代理推进（A* + 追踪）
        scene->compute_world(*this);
        scene->extract(*this, draws);
    }
    // ④ 世界校验和 + 周期快照（确定性验证 + 回滚点，docs/10 §4/§5）
    if (scene) {
        last_checksum = world_checksum();
        trail.emplace_back(frame, last_checksum);
        if (snaps && (frame % 60 == 0)) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
        if (cp_active && (frame % cp_interval == 0)) checkpoint_push();
    }
    mq_obs_counter_add(obs, "frames", 1.0);
    mq_obs_gauge_set(obs, "entities", scene ? (double)scene->count() : 0.0);
    mq_obs_gauge_set(obs, "draws", (double)draws.size());
    // ⑤ 渲染（只读快照 → 渲染不回写，docs/13 §8）
    std::vector<uint8_t> pixels((size_t)TW * H * 4);
    if (rapi && rself)
        rapi->render_views(rself, TW, H, views.data(), (uint32_t)views.size(),
                           draws.data(), (uint32_t)draws.size(), bg, pixels.data(), (uint32_t)pixels.size());
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
    clip.blob.assign(ct->size, 0);
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
        if (const char* mf = input ? mq_input_log_get_meta(input, "frames") : nullptr)
            cap = (uint64_t)std::strtoull(mf, nullptr, 10) + 1;
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
    void* p = nullptr;
    mq_handle h = mq_handle_table_alloc(table, &p);
    if (h.index == MQ_HANDLE_INDEX_NONE) return -1;
    ents.emplace((int)h.index, Entity{});
    return (int)h.index;
}
Entity* Scene::at(int index) {
    if (index < 0) return nullptr;
    auto it = ents.find(index);
    return it == ents.end() ? nullptr : &it->second;
}
void Scene::remove(int index) {
    auto it = ents.find(index);
    if (it != ents.end())
        for (auto& [t, b] : it->second.comps) if (eng) destroy_blob(*eng, t, b);
    ents.erase(index);
    mq_handle_table_free(table, mq_handle_table_handle_at(table, (uint32_t)index));
}

bool Scene::deserialize(Engine& eng, const std::string& text) {
    text_ = text;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    int rc = mq_persist_apply_json(eng.reflect, eng.scene_type, dom, &desc);
    mq_json_free(dom);
    return rc == 0;
}

static int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent) {
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
            const mq_json* pents = mq_json_get(pdom, "entities");
            const uint32_t pcount = mq_json_len(pents);
            for (uint32_t i = 0; i < pcount; ++i) load_entity(eng, sc, mq_json_at(pents, i), idx);
            mq_json_free(pdom);
            std::print("runtime: prefab '{}' instantiated under entity {}\n", *src, idx);
        }
    }
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
    for (uint32_t i = 0; i < kn; ++i) load_entity(eng, sc, mq_json_at(ch, i), idx);
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
    for (auto& [k, ent] : ents) {
        for (auto& [t, b] : ent.comps) destroy_blob(eng, t, b);
        mq_handle_table_free(table, mq_handle_table_handle_at(table, (uint32_t)k));
    }
    ents.clear();
    for (uint32_t i = 0; i < count; ++i) {
        int32_t k = 0, parent = -1;
        uint32_t name_len = 0;
        if (!take(&k, 4) || !take(&parent, 4) || !take(&name_len, 4)) return 0;
        if (name_len > 4096 || pos + name_len > n) return 0;
        std::string nm((const char*)in + pos, name_len);
        pos += name_len;
        uint32_t comps = 0;
        if (!take(&comps, 4) || comps > 4096) return 0;
        Entity e;
        e.name = std::move(nm);
        e.parent = parent;
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
    *lx = 0; *ly = 0;
    for (auto& [tid, blob] : e.comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d || !(d->role & MQ_ROLE_TRANSFORM2D)) continue;
        if (read_f32(d, blob.data(), "x", lx) && read_f32(d, blob.data(), "y", ly)) return;
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
            if (d->role & MQ_ROLE_UI_LABEL) {
                float x = 0, y = 0, r = 1, g = 1, b = 1, a = 1;
                read_f32(d, blob.data(), "x", &x);
                read_f32(d, blob.data(), "y", &y);
                read_f32(d, blob.data(), "r", &r);
                read_f32(d, blob.data(), "g", &g);
                read_f32(d, blob.data(), "b", &b);
                read_f32(d, blob.data(), "a", &a);
                mq_renderer_item it{};
                it.x = e.x + x; it.y = (float)eng.H - (e.y + y);
                it.size = 2.0F; it.r = r; it.g = g; it.b = b; it.a = a;
                it.texture = -1; it.layer = 1; it.z = 100;      // 文本在控件之上
                out.push_back(it);
                continue;
            }
            if (!(d->role & MQ_ROLE_SPRITE)) continue;
            float radius = 8, r = 1, g = 1, b = 1;
            read_f32(d, blob.data(), "radius", &radius);
            read_f32(d, blob.data(), "r", &r);
            read_f32(d, blob.data(), "g", &g);
            read_f32(d, blob.data(), "b", &b);
            std::string texref;
            if (const std::string* tp = read_str(d, blob.data(), "texture")) texref = *tp;
            // 材质资源覆盖颜色/贴图（builtin:/ 或 assets）
            if (const std::string* mp = read_str(d, blob.data(), "material"))
                if (!mp->empty()) {
                    scn::SpriteMaterial m{};
                    if (eng.load_material(*mp, m)) {
                        r = m.base_color.x; g = m.base_color.y; b = m.base_color.z;
                        if (!m.texture.empty()) texref = m.texture;
                    }
                }
            int tex = -1;
            if (!texref.empty()) tex = eng.load_texture_ref(texref);
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
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        if (!d) continue;
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
        /* 坐标：世界 y 向上，屏幕 y 向下（docs/19 §4）→ 控件矩形 = [sy-h, sy] × [sx, sx+w] */
        const float sx = e->x + x, sy = (float)eng.H - (e->y + y);
        const float bw = w > 0 ? w : 64, bh = h > 0 ? h : 24;
        const float bx = sx, by = sy - bh;
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
    auto* eng = (Engine*)user;
    out->entity = -1; out->handle = (uint32_t)-1;
    if (!eng->scene || !eng->scene->count()) return 0;
    const float px = ev->x, py = ev->y;   // 窗口与帧缓冲同尺度（docs/19 §4）
    return ui_hit_entity(*eng, px, py, out) >= 0 ? 1 : 0;
}

int runtime_parent(void* user, int entity) {
    Entity* e = ((Engine*)user)->scene->at(entity);
    return e ? e->parent : -1;
}

/* 把语义事件路由给脚本（docs/19 §8）：回调名固定，参数为标量（生成绑定约束，docs/21）。 */
void call_script_event(Engine& eng, const char* fn, const mq_input_event* ev) {
    if (!mq_script_has(eng.vm, fn)) return;
    mq_script_call3(eng.vm, fn, (double)ev->target, (double)ev->x, (double)ev->y, nullptr);
}

int runtime_input_handler(void* user, const mq_input_event* ev, mq_input_target* target) {
    auto* eng = (Engine*)user;
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
    }
    switch (ev->type) {
        case MQ_INPUT_POINTER_DOWN:   call_script_event(*eng, "on_pointer_down", ev); break;
        case MQ_INPUT_POINTER_UP:     call_script_event(*eng, "on_pointer_up", ev); break;
        case MQ_INPUT_POINTER_MOVE:   call_script_event(*eng, "on_pointer_move", ev); break;
        case MQ_INPUT_POINTER_WHEEL:  call_script_event(*eng, "on_wheel", ev); break;
        case MQ_INPUT_CLICK:          call_script_event(*eng, "on_click", ev); break;
        case MQ_INPUT_KEY_DOWN:       call_script_event(*eng, "on_key_down", ev); break;
        case MQ_INPUT_KEY_UP:         call_script_event(*eng, "on_key_up", ev); break;
        case MQ_INPUT_TEXT:           call_script_event(*eng, "on_text", ev); break;
        default: break;
    }
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

} // namespace

/* ---------------- 宿主 API 实现（声明来自生成的 maque_api.h） ---------------- */
namespace host {

float screen_w(void* ctx) { return (float)((Engine*)ctx)->W; }
float screen_h(void* ctx) { return (float)((Engine*)ctx)->H; }
float rng(void* ctx) { return (float)((Engine*)ctx)->next_rng() / 4294967296.0f; }
void  quit(void* ctx) { ((Engine*)ctx)->quit = true; }
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
static int h_perf(void* u, const char*, const mq_json*, char* out, size_t cap) {
    std::snprintf(out, cap, "%s", mq_obs_dump_json(((Engine*)u)->obs));
    return 0;
}
static int h_scene(void* u, const char*, const mq_json*, char* out, size_t cap) {
    auto& d = ((Engine*)u)->scene->desc;
    std::snprintf(out, cap, "{\"title\":\"%s\",\"script\":\"%s\",\"width\":%d,\"height\":%d,\"views\":%d}",
                  d.title.c_str(), d.script.c_str(), d.width, d.height, d.views);
    return 0;
}
static int h_checksum(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_types(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_entities(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_scene_get(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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

static int h_scene_set(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_scene_spawn(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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

static int h_scene_destroy(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_scene_bake(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_scene_save(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_snapshot(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_restore(void* u, const char*, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    mq_snapshot* src = e->snap_manual ? e->snap_manual : e->snap_last;
    if (!src) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no snapshot\"}"); return 0; }
    e->world_restore(src);
    std::snprintf(out, cap, "{\"ok\":true,\"entities\":%d}", e->scene->count());
    return 0;
}

static int h_trail(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_inject(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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

/* Control.mcp：MCP 语义（initialize/tools/list/tools/call）经控制平面进出。
 * 请求体放 params.request（字符串）；也可直接传 params.method/params.name。 */
/* Reload.script / Reload.scene：手动触发帧边界重载；dry_run=1 只报告不执行（docs/11 §4）。 */
static int h_reload_script(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_reload_scene(void* u, const char*, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    if (dry) { std::snprintf(out, cap, "{\"would_reload\":true,\"scene\":\"%s\"}", e->scn_vfs_path.c_str()); return 0; }
    const bool ok = e->reload_scene();
    std::snprintf(out, cap, "{\"reloaded\":%s,\"reloads\":%u,\"entities\":%d}",
                  ok ? "true" : "false", e->reloads, e->scene ? e->scene->count() : 0);
    return 0;
}
static int h_files(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_mcp(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_checkpoints(void* u, const char*, const mq_json*, char* out, size_t cap) {
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
static int h_seek(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_frame(void* u, const char*, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap, "{\"frame\":%llu,\"entities\":%d,\"particles\":%zu,\"quit\":%s}",
                  (unsigned long long)e->frame, e->scene ? e->scene->count() : 0,
                  e->particles.size(), e->quit ? "true" : "false");
    return 0;
}

/* Runtime.step：确定性推进 n 帧（AI/CI 逐帧驱动，docs/11 §5）。 */
static int h_step(void* u, const char*, const mq_json* params, char* out, size_t cap) {
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
static int h_replay_log(void* u, const char*, const mq_json*, char* out, size_t cap) {
    const char* json = mq_input_log_json(((Engine*)u)->input);
    std::snprintf(out, cap, "%s", json ? json : "{}");
    return 0;
}

/* 装配引擎：VFS → 反射 → 渲染插件 → 场景 → 平台/输入 → 脚本 → 控制面。
 * 两条入口（命令行 / 嵌入宿主）共用，保证行为一致。 */
bool Engine::setup(const std::string& scn_path, const char* renderer_override) {
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

    scene_store = Scene();
    scene = &scene_store;
    if (!scene->deserialize(*this, text)) { std::print("runtime: failed to deserialize scene\n"); return false; }
    scene->instantiate(*this);
    scene->compute_world(*this);

    W = scene->desc.width > 0 ? scene->desc.width : 320;
    H = scene->desc.height > 0 ? scene->desc.height : 240;
    nviews = scene->desc.views > 0 ? scene->desc.views : 1;
    TW = W * nviews;
    views.resize(nviews);
    for (int i = 0; i < nviews; ++i) {
        mq_renderer_view& v = views[i];
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
    std::print("runtime: scene '{}' title='{}' script='{}' {}x{} views={}\n",
               scn_path, scene->desc.title, scene->desc.script, W, H, nviews);

    obs = mq_obs_create();
    platform = mq_platform_create(scene->desc.title.c_str(), W, H);
    window = mq_platform_has_window(platform) != 0;
    input = mq_input_create();
    mq_input_set_hit_test(input, runtime_hit_test, this);
    mq_input_set_parent_fn(input, runtime_parent, this);
    mq_input_add_handler(input, runtime_input_handler, this);
    { const char* hs = std::getenv("MAQUE_HITS"); if (hs && *hs && *hs != '0') trace_hits = true; }
    snaps = mq_snapshot_pool_create(8, 1 << 16);
    { const char* cp = std::getenv("MAQUE_CHECKPOINTS"); if (cp && *cp && *cp != '0') cp_active = true; }
    vm = mq_script_vm_create();
    mq_bind_generated(vm, this);

    script_path = scene->desc.script;
    scn_vfs_path = "/" + scnp.filename().string();
    scn_full_path = scn_path;
    scn_dir = scn_dir2;
    if (!script_path.empty()) {
        std::string js;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(vfs, ("/" + script_path).c_str(), &d, &n) == MQ_VFS_OK) { js.assign((char*)d, n); mq_vfs_free(d); } }
        if (js.empty()) { std::print("runtime: cannot read script '{}'\n", script_path); return false; }
        script_hash = mq_vfs_hash(js.data(), js.size());
        if (mq_script_eval(vm, js.c_str(), script_path.c_str()) != 0) { std::print("runtime: script error\n"); return false; }
    }
    const char* watch = std::getenv("MAQUE_WATCH");
    if (watch && *watch && *watch != '0') {
        watcher = mq_watcher_create();
        mq_watcher_add(watcher, (std::filesystem::path(scn_dir) / scnp.filename()).string().c_str());
        if (!script_path.empty())
            mq_watcher_add(watcher, (std::filesystem::path(scn_dir) / script_path).string().c_str());
        std::print("runtime: hot reload watching {} file(s)\n", mq_watcher_count(watcher));
    }
    if (mq_script_has(vm, "on_start") && mq_script_call0(vm, "on_start", nullptr) != 0) {
        char ebuf[512] = { 0 };
        mq_script_last_error(vm, ebuf, (int)sizeof ebuf);
        write_crash_context(ebuf[0] ? ebuf : "on_start failed");
    }

    control = mq_control_create();
    mq_control_register(control, "Perf.get", h_perf, this);
    mq_control_register(control, "Scene.info", h_scene, this);
    mq_control_register(control, "World.checksum", h_checksum, this);
    mq_control_register(control, "World.trail", h_trail, this);
    mq_control_register(control, "World.snapshot", h_snapshot, this);
    mq_control_register(control, "World.restore", h_restore, this);
    mq_control_register(control, "Types.describe", h_types, this);
    mq_control_register(control, "Scene.entities", h_entities, this);
    mq_control_register(control, "Scene.get", h_scene_get, this);
    mq_control_register(control, "Scene.set", h_scene_set, this);
    mq_control_register(control, "Scene.save", h_scene_save, this);
    mq_control_register(control, "Scene.bake", h_scene_bake, this);
    mq_control_register(control, "Scene.spawn", h_scene_spawn, this);
    mq_control_register(control, "Scene.destroy", h_scene_destroy, this);
    mq_control_register(control, "Input.inject", h_inject, this);
    mq_control_register(control, "Replay.log", h_replay_log, this);
    mq_control_register(control, "Runtime.step", h_step, this);
    mq_control_register(control, "Runtime.frame", h_frame, this);
    mq_control_register(control, "Replay.checkpoints", h_checkpoints, this);
    mq_control_register(control, "Replay.seek", h_seek, this);
    mq_control_register(control, "Control.mcp", h_mcp, this);
    mq_control_register(control, "Reload.script", h_reload_script, this);
    mq_control_register(control, "Reload.scene", h_reload_scene, this);
    mq_control_register(control, "Reload.files", h_files, this);
    std::print("runtime: platform window={} input=on\n", window ? 1 : 0);
    return true;
}

void Engine::teardown() {
    if (control) { mq_control_destroy(control); control = nullptr; }
    if (vm) { mq_script_vm_destroy(vm); vm = nullptr; }
    if (obs) { mq_obs_destroy(obs); obs = nullptr; }
    if (reflect) { mq_reflect_destroy(reflect); reflect = nullptr; }
    for (auto* s : checkpoints) mq_snapshot_destroy(s);
    checkpoints.clear();
    mq_snapshot_destroy(snap_manual); snap_manual = nullptr;
    mq_snapshot_pool_destroy(snaps); snaps = nullptr;
    mq_watcher_destroy(watcher); watcher = nullptr;
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
    if (std::getenv("MAQUE_CRASH_REPORT")) {
        signal(SIGSEGV, on_fatal_signal);
        signal(SIGABRT, on_fatal_signal);
        signal(SIGFPE, on_fatal_signal);
    }
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
