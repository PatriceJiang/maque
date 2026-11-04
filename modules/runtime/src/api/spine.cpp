/* 宿主 API · Spine 2D（docs/27；实例按实体 id 寻址）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

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

}  // namespace host
