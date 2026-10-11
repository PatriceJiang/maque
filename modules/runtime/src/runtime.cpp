/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */
#include "runtime/mq_runtime.h"

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
#include <utility>

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
    mq_snapshot*        snap_last = nullptr;                // 最近一次世界快照
    mq_watcher*         watcher = nullptr;                  // 资源热重载监视（docs/08）
    std::string         script_path;                        // 当前脚本（热重载用）
    uint64_t            script_hash = 0;
    std::string         scn_vfs_path;                       // 场景在 VFS 中的路径
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
    struct Ball { int id; float vx, vy, radius; };
    std::vector<Ball> balls;                                // bounce_step 的确定性模拟集合（按 id 升序）
    uint64_t stop_frame = 0;                                // >0 时到该帧停止（CI 固定长度跑）
    std::string frames_dir;                                 // 每帧 PPM 输出目录（""=不输出）
    std::string name;
    const mq_type_desc* scene_type = nullptr;
    Scene*              scene = nullptr;

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
    void step_frame();                                      // 推进一个模拟帧（单一帧序，docs/10 D1）
    bool run_frames(uint64_t n);                            // 推进 n 帧（控制面 Runtime.step）
    bool reload_script(std::string* err = nullptr);          // 重载脚本（帧边界调用，docs/14）
    bool reload_scene();                                    // 重载场景（重建世界，docs/08）
    uint32_t poll_watcher();                                // 帧边界检查资源变化
    void export_final_snapshot();                           // 导出世界快照（MAQUE_SNAPSHOT）
    void finalize_run();                                    // 导出快照 + 录制日志（含 frames/trail 元数据）
    bool world_snapshot(mq_snapshot* snap);                 // 世界 → 快照（反射驱动，docs/10 §4）
    void world_restore(const mq_snapshot* snap);            // 快照 → 世界
    uint64_t world_trail() const;                           // 校验和轨迹（失步定位）
};

struct Scene {
    scn::Scene desc;
    std::string text_;
    mq_handle_table* table = nullptr;
    std::unordered_map<int, Entity> ents;

    Scene() { table = mq_handle_table_create(sizeof(uint8_t), 1); }
    ~Scene() { if (table) mq_handle_table_destroy(table); }
    Scene(const Scene&) = delete; Scene& operator=(const Scene&) = delete;

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
    scene->decode(*this, (const uint8_t*)bytes, n);
    scene->compute_world(*this);
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
        if (mq_script_has(vm, "update")) mq_script_call1(vm, "update", dt, nullptr);
        mq_script_gc(vm);
        scene->compute_world(*this);
        scene->extract(*this, draws);
    }
    // ④ 世界校验和 + 周期快照（确定性验证 + 回滚点，docs/10 §4/§5）
    if (scene) {
        last_checksum = scene->checksum(*this);
        trail.emplace_back(frame, last_checksum);
        if (snaps && (frame % 60 == 0)) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
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
    if (const char* td = std::getenv("MAQUE_TRAIL_OUT")) {   // 失步定位：逐帧校验和（docs/10 §5）
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
    }

    finalize_run();

    if (const char* td = std::getenv("MAQUE_TRAIL_OUT")) {   // 失步定位（docs/10 §5）
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
        blob.resize(d->size);
        mq_persist_apply_json(eng.reflect, d, c, blob.data());
    }
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
    for (uint32_t i = 0; i < kn; ++i) load_entity(eng, sc, mq_json_at(ch, i), idx);
    return idx;
}

void Scene::instantiate(Engine& eng) {
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
    size_t pos = 0;
    auto take = [&](void* dst, size_t bytes) -> bool {
        if (pos + bytes > n) return false;
        std::memcpy(dst, in + pos, bytes);
        pos += bytes;
        return true;
    };
    uint32_t count = 0;
    if (!take(&count, 4) || count > (1u << 20)) return 0;
    for (auto& [k, v] : ents) mq_handle_table_free(table, mq_handle_table_handle_at(table, (uint32_t)k));
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
            std::vector<uint8_t> blob(d ? d->size : 0);
            if (d && len && mq_persist_decode(eng.reflect, d, in + pos, len, blob.data()) < 0) return 0;
            pos += len;
            if (d) e.comps[tid] = std::move(blob);
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
/* 确定性反弹模拟（ball_demo 的玩法语义移入引擎侧组合 API；走引擎 RNG 与固定步长，docs/10） */
void bounce_step(void* ctx, float dt) {
    auto* eng = (Engine*)ctx;
    std::sort(eng->balls.begin(), eng->balls.end(),
              [](const Engine::Ball& a, const Engine::Ball& b) { return a.id < b.id; });
    const float W = (float)eng->W, H = (float)eng->H;
    for (auto& b : eng->balls) {
        Entity* e = eng->scene->at(b.id);
        if (!e) continue;
        b.vy += 900.0F * dt;                        // 重力（像素/秒²）
        float nx = e->x + b.vx * dt;
        float ny = e->y + b.vy * dt;
        if (nx < b.radius) { nx = b.radius; b.vx = -b.vx * 0.86F; }
        if (nx > W - b.radius) { nx = W - b.radius; b.vx = -b.vx * 0.86F; }
        if (ny > H - b.radius) { ny = H - b.radius; b.vy = -b.vy * 0.86F; }
        if (ny < b.radius) { ny = b.radius; b.vy = -b.vy * 0.86F; }
        host::entity_set_pos(eng, (float)b.id, nx, ny);
        e->x = nx; e->y = ny;
    }
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
    eng->balls.push_back(Engine::Ball{ (int)id, vx, vy, radius > 0 ? radius : 8.0F });
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

void ball_clear(void* ctx) {
    auto* eng = (Engine*)ctx;
    for (auto& b : eng->balls) host::entity_destroy(eng, (float)b.id);
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
    std::snprintf(out, cap, "{\"trail\":\"%llu\"}", (unsigned long long)((Engine*)u)->last_checksum);
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

/* World.snapshot：抓当前世界快照（返回字节数与校验和；AI undo/回滚点）。 */
static int h_snapshot(void* u, const char*, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->snap_last) e->snap_last = mq_snapshot_pool_acquire(e->snaps);
    if (!e->world_snapshot(e->snap_last)) { std::snprintf(out, cap, "{\"ok\":false}"); return 0; }
    size_t n = 0; mq_snapshot_bytes(e->snap_last, &n);
    std::snprintf(out, cap, "{\"ok\":true,\"bytes\":%zu,\"checksum\":\"%llu\",\"frame\":%llu}",
                  n, (unsigned long long)mq_snapshot_checksum(e->snap_last),
                  (unsigned long long)e->frame);
    return 0;
}

/* World.restore：回到最近快照（A5 快迭代/undo，docs/10 §4）。 */
static int h_restore(void* u, const char*, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->snap_last) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no snapshot\"}"); return 0; }
    e->world_restore(e->snap_last);
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

extern "C" int mq_run_scene_ex(const char* scn_path, const char* frames_dir) {
    if (!scn_path) return 1;
    std::filesystem::path scnp(scn_path);
    std::string scn_dir = scnp.parent_path().empty() ? std::string(".") : scnp.parent_path().string();

    Engine eng;
    eng.vfs = mq_vfs_create();
    mq_vfs_mount(eng.vfs, "/", ("file://" + scn_dir).c_str());

    std::string text;
    { void* d = nullptr; size_t n = 0;
      if (mq_vfs_read(eng.vfs, ("/" + scnp.filename().string()).c_str(), &d, &n) == 0) { text.assign((char*)d, n); mq_vfs_free(d); } }
    if (text.empty()) { std::print("runtime: cannot read scene '{}'\n", scn_path); mq_vfs_destroy(eng.vfs); return 1; }

    // 反射 ctx（per-instance）：载入 schema
    eng.reflect = mq_reflect_create();
    mq_reflect_load_schema(eng.reflect);
    eng.scene_type = mq_reflect_type_by_name(eng.reflect, "scn.Scene");
    if (!eng.scene_type) { std::print("runtime: no scn.Scene type\n"); return 1; }

    const char* plugin = std::getenv("MAQUE_RENDERER");
#ifdef MQ_RENDERER_DEFAULT
    if (!plugin) plugin = MQ_RENDERER_DEFAULT;
#endif
    if (!plugin) plugin = "libmaque_gfx.so";
    eng.host = mq_host_create();
    mq_loaded_module* rmod = nullptr;
    if (mq_host_load(eng.host, plugin, &rmod) != 0 ||
        !(eng.rapi = (const mq_renderer_v1*)mq_host_query(eng.host, MQ_RENDERER_API_NAME, 1))) {
        std::print("runtime: cannot load renderer plugin '{}'\n", plugin);
        mq_host_destroy(eng.host); mq_vfs_destroy(eng.vfs); return 2;
    }
    const char* backend = nullptr;
    eng.rself = eng.rapi->create(&backend);
    if (!eng.rself) { std::print("runtime: renderer create failed\n"); return 2; }
    std::print("runtime: renderer plugin '{}' backend='{}'\n", plugin, backend ? backend : "?");

    Scene scene;
    eng.scene = &scene;
    if (!scene.deserialize(eng, text)) { std::print("runtime: failed to deserialize scene\n"); return 1; }
    scene.instantiate(eng);
    scene.compute_world(eng);

    eng.W = scene.desc.width > 0 ? scene.desc.width : 320;
    eng.H = scene.desc.height > 0 ? scene.desc.height : 240;
    eng.nviews = scene.desc.views > 0 ? scene.desc.views : 1;
    eng.TW = eng.W * eng.nviews;
    eng.views.resize(eng.nviews);
    for (int i = 0; i < eng.nviews; ++i) {
        mq_renderer_view& v = eng.views[i];
        v = mq_renderer_view{};
        v.vx = (float)(i * eng.W); v.vy = 0;
        v.vw = (float)eng.W; v.vh = (float)eng.H;
        v.pan = (float)(i * eng.W);
        v.eye = (i - (eng.nviews - 1) * 0.5F) * scene.desc.eye_sep;
        v.cx = eng.W * 0.5F; v.cy = eng.H * 0.5F;   // 默认：相机中心 = 视口中心（世界坐标 = 像素）
        v.zoom = 1.0F;
    }
    /* 场景中的 Camera 组件（role=camera）覆盖视图相机（docs/13 §相机/分层） */
    for (auto& [idx, e] : scene.ents) {
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (!d || !(d->role & MQ_ROLE_CAMERA)) continue;
            bool active = true;
            if (const mq_field_desc* fa = mq_reflect_field(d, "active")) active = *(const bool*)(blob.data() + fa->offset);
            if (!active) continue;
            float cx = eng.W * 0.5F, cy = eng.H * 0.5F, zoom = 1.0F, rot = 0.0F;
            read_f32(d, blob.data(), "x", &cx);
            read_f32(d, blob.data(), "y", &cy);
            read_f32(d, blob.data(), "zoom", &zoom);
            read_f32(d, blob.data(), "rotation", &rot);
            for (auto& v : eng.views) { v.cx = cx; v.cy = cy; v.zoom = zoom > 0 ? zoom : 1.0F; v.rot = rot; }
        }
    }
    eng.rng = scene.desc.seed * 2654435761u + 12345u;
    std::print("runtime: scene '{}' title='{}' script='{}' {}x{} views={}\n",
               scn_path, scene.desc.title, scene.desc.script, eng.W, eng.H, eng.nviews);

    eng.obs = mq_obs_create();
    // 平台/输入：窗口默认打开（有显示环境），MAQUE_HEADLESS=1 或无显示 → dummy。
    // 事件经同一队列进入确定性事件流（docs/19 §3）。
    eng.platform = mq_platform_create(scene.desc.title.c_str(), eng.W, eng.H);
    eng.window = mq_platform_has_window(eng.platform) != 0;
    eng.input = mq_input_create();
    mq_input_set_hit_test(eng.input, runtime_hit_test, &eng);
    mq_input_set_parent_fn(eng.input, runtime_parent, &eng);
    mq_input_add_handler(eng.input, runtime_input_handler, &eng);
    {
        const char* hs = std::getenv("MAQUE_HITS");
        if (hs && *hs && *hs != '0') eng.trace_hits = true;
    }
    std::print("runtime: platform window={} input=on\n", eng.window ? 1 : 0);
    eng.snaps = mq_snapshot_pool_create(8, 1 << 16);
    eng.vm = mq_script_vm_create();
    mq_bind_generated(eng.vm, &eng);               // 生成绑定（math.* / host.*），注入引擎 ctx
    const std::string& script_path = scene.desc.script;
    if (!script_path.empty()) {
        std::string js;
        { void* d = nullptr; size_t n = 0;
          if (mq_vfs_read(eng.vfs, ("/" + script_path).c_str(), &d, &n) == 0) { js.assign((char*)d, n); mq_vfs_free(d); } }
        if (js.empty()) { std::print("runtime: cannot read script '{}'\n", script_path); return 1; }
        if (mq_script_eval(eng.vm, js.c_str(), script_path.c_str()) != 0) { std::print("runtime: script error\n"); return 1; }
    }
    eng.script_path = script_path;
    eng.scn_vfs_path = "/" + scnp.filename().string();
    eng.scn_dir = scn_dir;
    {
        void* d = nullptr; size_t n = 0;
        if (!script_path.empty() && mq_vfs_read(eng.vfs, ("/" + script_path).c_str(), &d, &n) == MQ_VFS_OK) {
            eng.script_hash = mq_vfs_hash(d, n);
            mq_vfs_free(d);
        }
    }
    /* 热重载监视（MAQUE_WATCH=1）：只监视脚本与场景两个文件（编辑器留待 M9+） */
    const char* watch = std::getenv("MAQUE_WATCH");
    if (watch && *watch && *watch != '0') {
        eng.watcher = mq_watcher_create();
        mq_watcher_add(eng.watcher, (std::filesystem::path(scn_dir) / scnp.filename()).string().c_str());
        if (!script_path.empty())
            mq_watcher_add(eng.watcher, (std::filesystem::path(scn_dir) / script_path).string().c_str());
        std::print("runtime: hot reload watching {} file(s)\n", mq_watcher_count(eng.watcher));
    }
    if (mq_script_has(eng.vm, "on_start")) mq_script_call0(eng.vm, "on_start", nullptr);

    // 控制平面：注册方法；若无显示/AI 驱动，可 headless 服务请求
    eng.control = mq_control_create();
    mq_control_register(eng.control, "Perf.get", h_perf, &eng);
    mq_control_register(eng.control, "Scene.info", h_scene, &eng);
    mq_control_register(eng.control, "World.checksum", h_checksum, &eng);
    mq_control_register(eng.control, "World.trail", h_trail, &eng);
    mq_control_register(eng.control, "World.snapshot", h_snapshot, &eng);
    mq_control_register(eng.control, "World.restore", h_restore, &eng);
    mq_control_register(eng.control, "Types.describe", h_types, &eng);
    mq_control_register(eng.control, "Scene.entities", h_entities, &eng);
    mq_control_register(eng.control, "Input.inject", h_inject, &eng);
    mq_control_register(eng.control, "Replay.log", h_replay_log, &eng);
    mq_control_register(eng.control, "Runtime.step", h_step, &eng);
    mq_control_register(eng.control, "Control.mcp", h_mcp, &eng);
    mq_control_register(eng.control, "Reload.script", h_reload_script, &eng);
    mq_control_register(eng.control, "Reload.scene", h_reload_scene, &eng);
    mq_control_register(eng.control, "Reload.files", h_files, &eng);
    if (const char* req = std::getenv("MAQUE_CONTROL")) {
        const bool replayed = eng.prepare_replay();
        if (!replayed && std::getenv("MAQUE_RECORD")) mq_input_record_begin(eng.input);
        auto serve = [&](const char* r) { std::print("{}\n", mq_control_dispatch(eng.control, r)); };
        if (std::strcmp(req, "-") == 0) {
            char line[4096];
            while (std::fgets(line, sizeof line, stdin)) { line[std::strcspn(line, "\n")] = 0; if (*line) serve(line); }
        } else serve(req);
        eng.finalize_run();
        if (replayed) {
            const char* rt = mq_input_log_get_meta(eng.input, "trail");
            if (rt) {
                char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)eng.world_trail());
                if (rt == std::string(tb)) std::print("runtime: replay matches recorded trail ({})\n", tb);
                else std::print("runtime: DESYNC — replay trail {} != recorded {}\n", tb, rt);
            }
        }
        mq_control_destroy(eng.control);
        mq_script_vm_destroy(eng.vm); mq_obs_destroy(eng.obs); mq_reflect_destroy(eng.reflect);
        mq_snapshot_pool_destroy(eng.snaps); mq_input_destroy(eng.input);
        mq_platform_destroy(eng.platform);
        if (eng.rapi && eng.rself) eng.rapi->destroy(eng.rself);
        mq_host_destroy(eng.host); mq_vfs_destroy(eng.vfs);
        return 0;
    }

    eng.main_loop(frames_dir ? frames_dir : "", scnp.filename().string());

    mq_control_destroy(eng.control);
    mq_script_vm_destroy(eng.vm);
    mq_obs_destroy(eng.obs);
    mq_reflect_destroy(eng.reflect);
    mq_watcher_destroy(eng.watcher);
    mq_snapshot_pool_destroy(eng.snaps);
    mq_input_destroy(eng.input);
    mq_platform_destroy(eng.platform);
    if (eng.rapi && eng.rself) eng.rapi->destroy(eng.rself);
    mq_host_destroy(eng.host);
    mq_vfs_destroy(eng.vfs);
    return eng.quit ? 0 : 1;
}

extern "C" int mq_run_scene(const char* scn_path) { return mq_run_scene_ex(scn_path, nullptr); }
