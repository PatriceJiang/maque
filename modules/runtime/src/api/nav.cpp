/* 宿主 API · 导航（docs/15；A* 在帧内确定性执行）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

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
    SceneNode* e = eng->scene->at((int)id);
    if (!e) return 0.0F;
    for (auto& [tid, blob] : e->comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng->reflect, tid);
        if (!d || !(d->role & MQ_ROLE_NAV_AGENT)) continue;
        if (const mq_field_desc* fx = mq_reflect_field(d, "tx")) field_set_f32(fx, blob.data(), x);
        if (const mq_field_desc* fy = mq_reflect_field(d, "ty")) field_set_f32(fy, blob.data(), y);
        if (const mq_field_desc* fh = mq_reflect_field(d, "has_target")) field_set_bool(fh, blob.data(), true);
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

}  // namespace host
