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
#include "core/profile/mq_profile.h"   // Linux perf 采样剖析（docs/35；OFF/非 Linux 时是 stub）
#include "gltf_model.h"                // 运行时 glTF/GLB 解析（plans/23/24，CPU 侧；无 GPU 依赖）

#include <fstream>
#include "core/control/mq_control.h"
#include "core/api/mq_api.h"
#include "maque_api.h"                 // 生成：host:: 声明（api_list）
#include "maque_ifaces.h"
#include "maque_control.h"
#include "maque_events.h"                 // 生成：mq_renderer_v4（插件接口 ABI）
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
/* zlib 的 zconf.h 在部分平台（Android NDK）**无条件** `#define far`（16 位时代遗留）。
 * 生成类型 render::Camera3D 有字段 `far`（schema/render/render.yaml），宏会让
 * `cam->far` 展开成 `cam->` 直接编译失败；这里用完即撤销，别把宏泄漏给引擎代码。 */
#ifdef far
#  undef far
#endif
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
    ((api) && (api)->header.struct_size >= offsetof(mq_renderer_v4, member) + sizeof((api)->member) && \
     (api)->member)

/* 组件槽：map key（type_id）+ **描述符缓存** + 字节存储。
 * 描述符是静态/模块生命周期，指针稳定；缓存它可以让所有遍历跳过
 * `mq_reflect_type_by_id`（每组件一次 unordered_map 查找）。
 * 不变量：desc->type_id == key。只经 component_slot() 写入 desc。
 * data()/size()/empty()/begin()/end() 透传，调用点的 `blob.data()` 一行不用改。 */
struct ComponentSlot {
    const mq_type_desc* desc = nullptr;
    std::vector<uint8_t> bytes;

    uint8_t*       data()        { return bytes.data(); }
    const uint8_t* data()  const { return bytes.data(); }
    size_t         size()  const { return bytes.size(); }
    bool           empty() const { return bytes.empty(); }
    auto begin()       { return bytes.begin(); }
    auto end()         { return bytes.end(); }
    auto begin() const { return bytes.begin(); }
    auto end()   const { return bytes.end(); }
};

/* ---- UI 裁剪（ui.Mask / ClippingNode）----
 * 矩形裁剪统一用**设计坐标**（世界 y 向上；提取时翻成屏幕 y 向下）。
 * ClipSet = **可见矩形集合**：普通 Mask 做交，invert（reverse）做差。
 * 差集把矩形切成最多 4 块 → 每块仍是一个 scissor；因此 reverse 不依赖 stencil，
 * 也能与合批共存（同一块内的图元仍共享一次 drawcall）。
 * 这样嵌套 reverse 也能精确表达（最多 4^深度 块；超上限退化为包围盒，宁可多画不丢内容）。 */
struct ClipRect {
    float x0 = 0.0F, y0 = 0.0F, x1 = 0.0F, y1 = 0.0F;
    bool valid() const { return x1 > x0 && y1 > y0; }
};
struct ClipSet {
    bool active = false;                 // false = 不裁剪（继承链上没有任何 Mask）
    std::vector<ClipRect> rects;         // active && empty = 全裁掉
    bool empty() const { return active && rects.empty(); }
    bool contains(float x, float y) const {
        if (!active) return true;
        for (const ClipRect& r : rects)
            if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) return true;
        return false;
    }
};

inline bool clip_intersect(const ClipRect& a, const ClipRect& b, ClipRect& out) {
    out.x0 = std::max(a.x0, b.x0); out.y0 = std::max(a.y0, b.y0);
    out.x1 = std::min(a.x1, b.x1); out.y1 = std::min(a.y1, b.y1);
    return out.valid();
}

/* r - m：不重叠保持 r；重叠切成上/下/左/右四块（互不重叠、并集 = r \ m）。 */
inline void clip_subtract(const ClipRect& r, const ClipRect& m, std::vector<ClipRect>& out) {
    ClipRect ix{};
    if (!clip_intersect(r, m, ix)) { out.push_back(r); return; }
    if (m.y1 < r.y1) out.push_back(ClipRect{ r.x0, m.y1, r.x1, r.y1 });   // 上
    if (m.y0 > r.y0) out.push_back(ClipRect{ r.x0, r.y0, r.x1, m.y0 });   // 下
    if (m.x0 > r.x0) out.push_back(ClipRect{ r.x0, ix.y0, m.x0, ix.y1 }); // 左
    if (m.x1 < r.x1) out.push_back(ClipRect{ m.x1, ix.y0, r.x1, ix.y1 }); // 右
}

inline ClipSet clip_apply(const ClipSet& parent, const ClipRect& mask, bool invert, float W, float H) {
    ClipSet out;
    out.active = true;
    if (!invert) {
        if (!parent.active) { if (mask.valid()) out.rects.push_back(mask); return out; }
        for (const ClipRect& r : parent.rects) {
            ClipRect ix{};
            if (clip_intersect(r, mask, ix)) out.rects.push_back(ix);
        }
        return out;
    }
    /* reverse：可见 = 父可见区 - mask。没有父裁剪时以设计画布为全集。 */
    if (!parent.active) {
        ClipRect full{ 0.0F, 0.0F, W, H };
        if (full.valid()) clip_subtract(full, mask, out.rects);
        return out;
    }
    for (const ClipRect& r : parent.rects) clip_subtract(r, mask, out.rects);
    /* 防御：嵌套 reverse 的块数理论上是 4^深度；超过 64 块退化为包围盒（多画、不丢内容）。 */
    if (out.rects.size() > 64) {
        ClipRect b = out.rects.front();
        for (const ClipRect& r : out.rects) {
            if (r.x0 < b.x0) b.x0 = r.x0; if (r.y0 < b.y0) b.y0 = r.y0;
            if (r.x1 > b.x1) b.x1 = r.x1; if (r.y1 > b.y1) b.y1 = r.y1;
        }
        out.rects.assign(1, b);
    }
    return out;
}

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
    /* 运行期 alpha-mask 覆盖：-2=继承父，-1=强制无 mask（滚动条/百分比），>=0=固定链。
     * 只由 ListView 自动生成节点使用；不是序列化字段。 */
    int   ui_mask_override = -2;
    std::unordered_map<uint32_t, ComponentSlot> comps;   // type_id → 组件槽（含描述符缓存）
};

/* 组件槽工厂：**唯一**允许设置 desc 的入口（不变量：desc->type_id == key）。 */
inline ComponentSlot& component_slot(SceneNode& n, const mq_type_desc* d) {
    ComponentSlot& s = n.comps[d->type_id];
    s.desc = d;
    return s;
}

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

/* 计算节点自身/子树的裁剪集合（ui.Mask = ClippingNode）。
 * 矩形来自同节点 ui.Control 的 offset_max（与 ui.Image 的左上角语义一致）。
 * P0 只支持 Rect（mask_mode 的其他值先按矩形处理；圆角/纹理/stencil 后续版本）。 */
inline void compute_ui_clip(SceneNode& e, const ClipSet& parent, float W, float H,
                            ClipSet& self, ClipSet& kids) {
    self = parent;
    kids = parent;
    const ui::Mask* mask = component<ui::Mask>(e);
    const ui::ListView* list = component<ui::ListView>(e);
    if (!mask && !list) return;
    const ui::Control* ctl = component<ui::Control>(e);
    if (!ctl) return;
    /* 节点缩放与 ui.Image 的几何保持一致（Image 用 world_scale 缩放目标尺寸）。
     * 旋转仍不参与（ui.Image 目前也不旋转）；P1 做 UI transform pass 时统一。 */
    const float s = e.world_scale > 0.0F ? e.world_scale : 1.0F;
    const float w = ctl->offset_max.x * s, h = ctl->offset_max.y * s;
    if (!(w > 0.0F) || !(h > 0.0F)) return;
    const ClipRect r{ e.world_x, e.world_y, e.world_x + w, e.world_y + h };
    if (!r.valid()) return;
    if (mask) {
        if (mask->clip_children) kids = clip_apply(parent, r, mask->invert, W, H);
        else self = clip_apply(parent, r, mask->invert, W, H);
    }
    /* ListView 的视口裁剪只作用于子树（自身不绘制）；与 Mask 叠加时继续求交。 */
    if (list && list->clip_content) kids = clip_apply(kids, r, false, W, H);
}

/* ---- ListView 滚动条几何（ui_step 与提取共用同一套，避免“能看见但点不到”）----
 * 世界坐标 y 向上；thumb 高度 = 视口/内容比例，位置由 scroll_offset 决定。 */
struct ListBarGeometry {
    bool  visible = false;
    float x0 = 0, x1 = 0;             // 轨道左右
    float y0 = 0, y1 = 0;             // 轨道上下（视口）
    float thumb_y0 = 0, thumb_y1 = 0; // 滑块上下
    float thumb_h = 0;
    float content_h = 0, view_h = 0;
};
inline ListBarGeometry list_scrollbar_geometry(const ui::ListView& lv, float x, float y,
                                               float w, float h, float s = 1.0F) {
    ListBarGeometry g{};
    g.view_h = h;
    g.content_h = lv.content_height * s;
    if (!lv.show_scrollbar || !(w > 0.0F) || !(h > 0.0F) || !(g.content_h > h + 0.5F)) return g;
    g.visible = true;
    g.y0 = y; g.y1 = y + h;
    const float bw = std::clamp(lv.scrollbar_width * s, 1.0F, w * 0.5F);
    const float margin = std::clamp(lv.scrollbar_margin * s, 0.0F, w * 0.25F);
    g.x0 = x + w - margin - bw;
    g.x1 = x + w - margin;
    const float ratio = std::clamp(h / g.content_h, 0.02F, 1.0F);
    g.thumb_h = std::max(24.0F * s, h * ratio);
    if (g.thumb_h > h) g.thumb_h = h;
    /* thumb 在轨道内的行程 = 轨道长 - thumb 长；不要用像素 scroll_offset（它是
     * content_h - view_h 的范围，和 thumb 行程不是同一个尺度）。 */
    const float travel = std::max(0.0F, h - g.thumb_h);
    const float pos = std::clamp(lv.scroll_pos, 0.0F, 1.0F);
    g.thumb_y1 = y + h - pos * travel;
    g.thumb_y0 = g.thumb_y1 - g.thumb_h;
    return g;
}

/* 屏幕坐标 AABB 与可见裁剪集合是否相交（文本/节点快速剔除用）。 */
inline bool clip_intersects_screen_aabb(const ClipSet& clip, float x0, float y0,
                                        float x1, float y1, float H) {
    if (!clip.active) return true;
    for (const ClipRect& r : clip.rects) {
        const float cx0 = r.x0, cy0 = H - r.y1, cx1 = r.x1, cy1 = H - r.y0;
        if (x0 < cx1 && x1 > cx0 && y0 < cy1 && y1 > cy0) return true;
    }
    return false;
}

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
