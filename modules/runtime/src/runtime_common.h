/* maque 运行时（DSL 驱动）：
 *   Engine：设备(Renderer) / VFS / 脚本 / 反射 ctx / 视图。
 *   Scene ：deserialize(反射) + instantiate(实体/组件) + dispatch/extract。
 *   实体 = name + 组件；组件语义由 schema 的 roles 声明，引擎**不硬编码任何组件类型**。
 *   宿主 API 由 schema 的 api_list(ctx:true) 生成（绑定/声明），引擎只实现语义。 */

/* runtime 内部共享面 —— 只有 modules/runtime 的 .cpp 包含（不对外，不进安装头）。
 * 拆自原 runtime.cpp 的单一编译单元：这里是**类型 + 辅助声明**，实现留在各 .cpp。
 * 公开 ABI（mq_runtime.h / mq_host.h / maque_api.h / maque_control.h）不受影响。 */
#pragma once

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
#include "maque_events.h"                 // 生成：mq_renderer_v2（插件接口 ABI）
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
#include <deque>
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

namespace maque::rt {

/* std::filesystem 短别名：本模块内部共用（runtime.cpp 不再单独声明）。 */
namespace fs = std::filesystem;

#define MQ_RENDERER_HAS(api, member) \
    ((api) && (api)->header.struct_size >= offsetof(mq_renderer_v2, member) + sizeof((api)->member) && \
     (api)->member)

/* 场景树节点（Node + Components）——**不是 ECS 实体**。
 * 层级是显式的 parent + 有序 children（升序 id = 声明/创建序），组件是反射 blob。
 * 没有 archetype/系统调度：每帧只做一次确定性的树访问（Scene::visit），
 * 由访问者按角色读取组件。id 只是脚本/快照/控制面使用的稳定键，不是存储槽。 */
struct SceneNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;                 // 有序：id 升序（声明序；旧 childAt 语义不变）
    /* 无 Transform2D 的节点（纯 UI/空节点）的局部位置兜底。
     * 有 Transform2D 时位置真相在组件里（与 docs/12 一致）。 */
    float local_x = 0.0F, local_y = 0.0F;
    /* 世界坐标：**每帧由 Scene::update_world() 的树访问写到这里的派生缓存**（不是位置真相！
     * 位置真相 = Transform 组件里的局部坐标；改位置请经 node/entity API 写组件，
     * 直接改 world_x/y 会在下一帧被覆盖）。 */
    float world_x = 0, world_y = 0;
    /* 3D 世界矩阵（列主序，派生缓存）：同一趟树访问（+ glTF 片段派生姿态）
     * 重算；不是位置真相（真相在 scene.Transform3D.local / .clip.json 采样里）。
     * 平级场景 world_m == m4_from_trs(local)，旧场景零漂移（plans/22 §6.3）。 */
    float world_m[16] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                          0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
    /* 节点属性（active/scale/angle/opacity）：影响自身与子树；z_index 参与 UI/绘制稳定排序。
     * local_* 进快照与校验和；world_* 是同一趟树访问派生的有效值，不进快照。 */
    bool  active = true;
    float scale = 1.0F;
    float angle = 0.0F;                                    // 角度（度）
    float opacity = 1.0F;                                  // 0..1
    int   z_index = 0;
    /* 派生：从根累积到本节点（update_world 写；O(1) 读取，不再沿父链重复上溯）。 */
    bool  visible = true;
    float world_angle = 0.0F;
    float world_scale = 1.0F;
    float world_opacity = 1.0F;
    int   depth = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> comps;   // type_id → blob
};

/* 已知内置组件：按 schema 生成的 type_id 判型后直接访问生成 struct。
 * 调用方应优先用它，而不是 mq_reflect_field + offset。 */
template <class T>
inline const T* as_component(const mq_type_desc* d, const void* p) {
    if (!d || !p || d->type_id != mq::schema::type_id_v<T>) return nullptr;
    return std::launder(reinterpret_cast<const T*>(p));
}
template <class T>
inline T* as_component_mut(const mq_type_desc* d, void* p) {
    if (!d || !p || d->type_id != mq::schema::type_id_v<T>) return nullptr;
    return std::launder(reinterpret_cast<T*>(p));
}

/* 按**编译期类型 id** 直接取组件槽（免描述符查找、免字段反射）：
 *   component_of<MQ_TID_SCN_BUTTON>(node)  ≡  component<scn::Button>(node)
 * 槽长度必须等于该类型尺寸（construct_blob 保证）；不匹配视为缺失（空槽/陈旧数据）。
 * 调用点因此不再需要「遍历 comps + mq_reflect_type_by_id + 比 role」。 */
template <uint32_t Id>
inline auto* component_of(SceneNode& n) {
    using T = typename ::mq::schema::type_of<Id>::type;
    auto it = n.comps.find(Id);
    if (it == n.comps.end() || it->second.size() != sizeof(T)) return static_cast<T*>(nullptr);
    return std::launder(reinterpret_cast<T*>(it->second.data()));
}
template <class T>
inline T* component(SceneNode& n) { return component_of<::mq::schema::type_id_v<T>>(n); }

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

/* 已解析字段的直读/直写（生成访问器优先；容器/未知类型回退 offset）。 */
static inline bool field_get_bool(const mq_field_desc* f, const void* p, bool def = false) {
    if (!f) return def;
    /* bool 先按 offset 直读：accessor 是不可内联的间接调用，实测比一次 byte load 贵约 4ns；
     * 标量字段的 offset 由 codegen 用 offsetof 生成，永远准确。 */
    if (f->kind == MQ_TK_BOOL) return *(const bool*)((const uint8_t*)p + f->offset);
    if (f->accessor && f->accessor->get_f64) return f->accessor->get_f64(p) != 0.0;
    return def;
}
static inline void field_set_bool(const mq_field_desc* f, void* p, bool v) {
    if (!f) return;
    if (f->accessor && f->accessor->set_f64) { f->accessor->set_f64(p, v ? 1.0 : 0.0); return; }
    if (f->kind == MQ_TK_BOOL) *(bool*)((uint8_t*)p + f->offset) = v;
}
static inline int32_t field_get_i32(const mq_field_desc* f, const void* p, int32_t def = 0) {
    if (!f) return def;
    if (f->accessor && f->accessor->get_f64) return (int32_t)f->accessor->get_f64(p);
    return f->kind == MQ_TK_I32 ? *(const int32_t*)((const uint8_t*)p + f->offset) : def;
}
static inline void field_set_i32(const mq_field_desc* f, void* p, int32_t v) {
    if (!f) return;
    if (f->accessor && f->accessor->set_f64) { f->accessor->set_f64(p, (double)v); return; }
    if (f->kind == MQ_TK_I32) *(int32_t*)((uint8_t*)p + f->offset) = v;
}
static inline uint32_t field_get_u32(const mq_field_desc* f, const void* p, uint32_t def = 0) {
    if (!f) return def;
    if (f->accessor && f->accessor->get_f64) return (uint32_t)f->accessor->get_f64(p);
    return f->kind == MQ_TK_U32 ? *(const uint32_t*)((const uint8_t*)p + f->offset) : def;
}
static inline void field_set_u32(const mq_field_desc* f, void* p, uint32_t v) {
    if (!f) return;
    if (f->accessor && f->accessor->set_f64) { f->accessor->set_f64(p, (double)v); return; }
    if (f->kind == MQ_TK_U32) *(uint32_t*)((uint8_t*)p + f->offset) = v;
}
static inline float field_get_f32(const mq_field_desc* f, const void* p, float def = 0.0F) {
    if (!f) return def;
    if (f->accessor && f->accessor->get_f64) return (float)f->accessor->get_f64(p);
    return f->kind == MQ_TK_F32 ? *(const float*)((const uint8_t*)p + f->offset) : def;
}
static inline void field_set_f32(const mq_field_desc* f, void* p, float v) {
    if (!f) return;
    if (f->accessor && f->accessor->set_f64) { f->accessor->set_f64(p, (double)v); return; }
    if (f->kind == MQ_TK_F32) *(float*)((uint8_t*)p + f->offset) = v;
}
static inline bool field_set_str(const mq_field_desc* f, void* p, const char* v) {
    if (!f) return false;
    if (f->accessor && f->accessor->set_str) { f->accessor->set_str(p, v); return true; }
    if (f->kind == MQ_TK_STRING) { *(std::string*)((uint8_t*)p + f->offset) = v ? v : ""; return true; }
    return false;
}
/* 结构体/字符串/数组字段的指针（生成访问器优先；无访问器时回退 offset）。 */
static inline const void* field_ptr(const mq_field_desc* f, const void* p) {
    if (!f) return nullptr;
    if (f->accessor && f->accessor->get_ptr) return f->accessor->get_ptr(p);
    return (const uint8_t*)p + f->offset;
}
static inline void* field_ptr_mut(const mq_field_desc* f, void* p) {
    if (!f) return nullptr;
    if (f->accessor && f->accessor->get_mut_ptr) return f->accessor->get_mut_ptr(p);
    return (uint8_t*)p + f->offset;
}
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
extern bool keep_open;
/* 组件语义辅助（定义见 host API 之前）：供输入回调使用。 */
std::vector<uint8_t>* comp_by_role(Engine& eng, int id, uint32_t role);
/* VFS 相对路径拼接（定义见场景实例装配段；watcher 等前置代码也要用）。 */
std::string vfs_join(const std::string& base, const std::string& rel);
std::vector<uint8_t>* comp_by_name(Engine& eng, int id, const char* type_name, const mq_type_desc** out_desc);
void accumulate_node_attrs(Engine& eng, int idx, float& angle_deg, float& scale, float& opacity);
bool node_active_effective(Engine& eng, int idx);
/* 热路径布局缓存（定义见 Engine 之后）：按 type_id 解析一次，热路径不再查反射。 */
float read_num(const mq_type_desc* d, const void* base, const char* field, float def);
bool  write_num(const mq_type_desc* d, void* base, const char* field, double v);
void  slider_apply_pointer(Engine& eng, int id, const mq_type_desc* d, void* base, float px);
void  window_to_world(const Engine& eng, float* x, float* y);   // 窗口像素 → 世界坐标（含 y 翻转）
void  device_event_to_world(const Engine& eng, mq_platform_pointer_event* p);  // 位置 + 方向量
/* node_emit_number / node_width 由生成的 maque_api.h 在 ::host 中声明（api_list）。 */
void attach_component(Engine& eng, int idx, const char* type_name);
void construct_blob(Engine& eng, const mq_type_desc* d, std::vector<uint8_t>& blob);
void destroy_blob(Engine& eng, uint32_t type_id, std::vector<uint8_t>& blob);

/* 贴图路径里的 sampler 后缀（plans/20）：
 * `textures/x.png#wrap=clamp,mirror&filter=nearest` —— '#' 前是 VFS 路径，
 * wrap/filter 落到 create_texture2。缓存 key 用完整字符串（同图不同 sampler = 两个纹理）。 */
struct TexPathParams {
    std::string vfs;
    int wrap_u = 0, wrap_v = 0;      // 0=repeat 1=clamp 2=mirror
    int filter = 1;                  // 0=nearest 1=linear
    bool wrap_set = false, filter_set = false;
};
TexPathParams parse_tex_path(const std::string& ref);

/* ---- 跨 TU 的内部辅助（原匿名命名空间里"到处可见"，拆分后必须显式声明）---- */
const std::string* read_str(const mq_type_desc* d, const void* blob, const char* name);
bool resolve_field_path(Engine& eng, int entity, const std::string& path,
                        const mq_type_desc** out_type, const mq_field_desc** out_field,
                        void** out_base);
int  ui_hit_entity(Engine& eng, float px, float py, mq_input_target* out);
int  runtime_hit_test(void* user, const mq_input_event* ev, mq_input_target* out);
int  runtime_parent(void* user, int entity);
int  runtime_input_handler(void* user, const mq_input_event* ev, mq_input_target* target);
double cfg_field(Engine* eng, const char* path);
bool read_sim_scenario(mq_vfs* vfs, const char* path, std::string& out);
bool read_f32(const mq_type_desc* d, const void* blob, const char* name, float* out);
bool write_ppm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba);
/* 事件 kind → 处理器名 → 总线广播（多脚本/多语言的唯一事件入口；定义在输入分发段）。 */
int dispatch_event(Engine& eng, int kind, double a, double b, double c, const char* text);
/* 模块/包路径规范化（docs/34 P0；定义在 script/module.cpp）。 */
std::string normalize_module_rel(const std::string& in);
/* 终止信号 / 崩溃落盘（docs/09 §5；定义在 engine/crash.cpp）。 */
extern volatile sig_atomic_t g_signal_hit;
extern const char* g_signal_name;
extern int  g_crash_fd;
extern char g_crash_buf[16384];
extern volatile sig_atomic_t g_crash_len;
extern volatile sig_atomic_t g_quit_requested;
void on_quit_signal(int);
void on_fatal_signal(int sig);
/* 场景加载（docs/12 §5）：实体树反序列化 + prefab 展开（定义在 scene/loader.cpp）。 */
std::string vfs_dirname(const std::string& vfs_path);
int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent,
                std::vector<std::string>* chain = nullptr, int depth = 0);
void read_color(const mq_type_desc* d, const void* base, const char* field, math::Color* out);
void build_cube(std::vector<float>& verts, std::vector<uint32_t>& indices);

}  // namespace maque::rt
