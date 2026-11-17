/* 宿主 API · DragonBones 2D（docs/36；实例按实体 id 寻址）
 *
 * 语义与 spine_* 一致：**问引擎当前状态**（组件字段 + 实例是唯一真相），脚本不自己记一份；
 * 越界 / 实体没有 DragonBonesSkeleton 组件 → 0 / "" / -1，且不崩（脚本侧无需防御）。
 * 声明由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

static DragonBonesRuntime::Instance* db_at(Engine* eng, float id) {
    return eng ? eng->dragonbones_of((int)id) : nullptr;
}

/* 组件字段 = 唯一真相：直接写组件（下一帧 dragonbones_step 按组件驱动实例）。 */
static anim::DragonBonesSkeleton* db_comp(Engine* eng, float id) {
    SceneNode* e = (eng && eng->scene) ? eng->scene->at((int)id) : nullptr;
    return e ? component<anim::DragonBonesSkeleton>(*e) : nullptr;
}

float dragonbones_animation_count(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return db ? (float)db->anim_names.size() : 0.0F;
}

const char* dragonbones_animation_name(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db) return "";
    const int i = (int)index;
    if (i < 0 || i >= (int)db->anim_names.size()) return "";
    return db->anim_names[(size_t)i].c_str();
}

float dragonbones_animation_index(void* ctx, float id, const char* name) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !name) return -1.0F;
    for (size_t i = 0; i < db->anim_names.size(); ++i)
        if (db->anim_names[i] == name) return (float)i;
    return -1.0F;
}

float dragonbones_play(void* ctx, float id, const char* name, float loop) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst || !name || !*name) return -1.0F;
    if (mq_db_instance_play(db->inst, name, loop != 0.0F ? 1 : 0) != MQ_DB_OK) return -1.0F;
    int idx = -1;
    for (size_t i = 0; i < db->anim_names.size(); ++i)
        if (db->anim_names[i] == name) { idx = (int)i; break; }
    db->last_anim = idx;
    /* 组件同步：否则下一帧 step 会按组件里的旧 animation 又切回去 */
    if (auto* sc = db_comp((Engine*)ctx, id); sc && idx >= 0) {
        sc->animation = (uint32_t)idx;
        sc->play_mode = (anim::PlayMode)(loop != 0.0F ? 1 : 0);
    }
    return 0.0F;
}

float dragonbones_stop(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return -1.0F;
    mq_db_instance_stop(db->inst);
    /* 组件字段仍是唯一真相：把 last_anim 对齐到组件现值，避免下一帧 step 又按旧 animation 重播 */
    if (auto* sc = db_comp((Engine*)ctx, id)) db->last_anim = (int)sc->animation;
    return 0.0F;
}

const char* dragonbones_current_animation(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return "";
    const char* nm = mq_db_instance_animation_name(db->inst);
    return nm ? nm : "";
}

float dragonbones_time(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return (db && db->inst) ? mq_db_instance_time(db->inst) : 0.0F;
}

float dragonbones_set_time(void* ctx, float id, float t) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return -1.0F;
    if (mq_db_instance_set_time(db->inst, t) != MQ_DB_OK) return -1.0F;
    if (auto* sc = db_comp((Engine*)ctx, id)) sc->state.time = mq_db_instance_time(db->inst);
    return 0.0F;
}

float dragonbones_duration(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return (db && db->inst) ? mq_db_instance_duration(db->inst) : 0.0F;
}

float dragonbones_play_times(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return (db && db->inst) ? (float)mq_db_instance_play_times(db->inst) : 0.0F;
}

float dragonbones_complete(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return (db && db->inst && mq_db_instance_complete(db->inst)) ? 1.0F : 0.0F;
}

const char* dragonbones_armature(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return db ? db->armature_name.c_str() : "";
}

float dragonbones_skin_count(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return db ? (float)db->skin_names.size() : 0.0F;
}

const char* dragonbones_skin_name(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db) return "";
    const int i = (int)index;
    if (i < 0 || i >= (int)db->skin_names.size()) return "";
    return db->skin_names[(size_t)i].c_str();
}

float dragonbones_set_skin(void* ctx, float id, const char* name) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst || !name || !*name) return -1.0F;
    if (mq_db_instance_set_skin(db->inst, name) != MQ_DB_OK) return -1.0F;
    for (size_t i = 0; i < db->skin_names.size(); ++i)
        if (db->skin_names[i] == name) {
            if (auto* sc = db_comp((Engine*)ctx, id)) sc->skin = (uint32_t)i;
            break;
        }
    return 0.0F;
}

const char* dragonbones_current_skin(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return "";
    const char* nm = mq_db_instance_skin_name(db->inst);
    return nm ? nm : "";
}

float dragonbones_bone_count(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return (db && db->inst) ? (float)mq_db_instance_bone_count(db->inst) : 0.0F;
}

const char* dragonbones_bone_name(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return "";
    const int i = (int)index;
    if (i < 0 || i >= mq_db_instance_bone_count(db->inst)) return "";
    return mq_db_instance_bone_name(db->inst, i);
}

float dragonbones_bone_find(void* ctx, float id, const char* name) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst || !name) return -1.0F;
    return (float)mq_db_instance_bone_find(db->inst, name);
}

float dragonbones_bone_x(void* ctx, float id, const char* name) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst || !name) return 0.0F;
    const int i = mq_db_instance_bone_find(db->inst, name);
    float x = 0.0F, y = 0.0F;
    if (i < 0 || mq_db_instance_bone_world_position(db->inst, i, &x, &y) != MQ_DB_OK) return 0.0F;
    return x;
}

float dragonbones_bone_y(void* ctx, float id, const char* name) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst || !name) return 0.0F;
    const int i = mq_db_instance_bone_find(db->inst, name);
    float x = 0.0F, y = 0.0F;
    if (i < 0 || mq_db_instance_bone_world_position(db->inst, i, &x, &y) != MQ_DB_OK) return 0.0F;
    return y;
}

float dragonbones_slot_count(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return (db && db->inst) ? (float)mq_db_instance_slot_count(db->inst) : 0.0F;
}

const char* dragonbones_slot_name(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return "";
    const int i = (int)index;
    if (i < 0 || i >= mq_db_instance_slot_count(db->inst)) return "";
    return mq_db_instance_slot_name(db->inst, i);
}

float dragonbones_slot_index(void* ctx, float id, const char* name) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst || !name) return -1.0F;
    return (float)mq_db_instance_slot_index(db->inst, name);
}

float dragonbones_slot_display(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return -1.0F;
    return (float)mq_db_instance_slot_display(db->inst, (int)index);
}

float dragonbones_set_slot_display(void* ctx, float id, float index, float display) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db || !db->inst) return -1.0F;
    return mq_db_instance_set_slot_display(db->inst, (int)index, (int)display) == MQ_DB_OK ? 0.0F : -1.0F;
}

float dragonbones_pages(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return db ? (float)db->pages.size() : 0.0F;
}

float dragonbones_page_texture(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db) return -1.0F;
    const int i = (int)index;
    if (i < 0 || i >= (int)db->pages.size()) return -1.0F;
    const uint64_t h = db->pages[(size_t)i].handle;
    return h > 0 ? (float)(h - 1u) : -1.0F;          // 句柄 = gfx 纹理 id + 1
}

float dragonbones_page_width(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    const int i = (int)index;
    if (!db || i < 0 || i >= (int)db->pages.size()) return 0.0F;
    return (float)db->pages[(size_t)i].width;
}

float dragonbones_page_height(void* ctx, float id, float index) {
    auto* db = db_at((Engine*)ctx, id);
    const int i = (int)index;
    if (!db || i < 0 || i >= (int)db->pages.size()) return 0.0F;
    return (float)db->pages[(size_t)i].height;
}

float dragonbones_vertex_count(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return db ? (float)db->draw.vertex_count : 0.0F;
}

float dragonbones_batch_count(void* ctx, float id) {
    auto* db = db_at((Engine*)ctx, id);
    return db ? (float)db->draw.batch_count : 0.0F;
}

float dragonbones_bounds(void* ctx, float id, float which) {
    auto* db = db_at((Engine*)ctx, id);
    if (!db) return 0.0F;
    switch ((int)which) {
    case 0: return db->draw.bounds_x;
    case 1: return db->draw.bounds_y;
    case 2: return db->draw.bounds_w;
    case 3: return db->draw.bounds_h;
    default: return 0.0F;
    }
}

}  // namespace host
