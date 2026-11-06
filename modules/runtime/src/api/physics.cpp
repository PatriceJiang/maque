/* 宿主 API · 物理/弹球（docs/15 §3；接触查询是帧内派生缓存）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

#include <cmath>

using namespace maque::rt;

namespace host {

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

/* 2D 射线查询（docs/15 §3）：世界坐标 + 方向 → 最近命中的物理实体 id（-1 = 未命中）。
 * 命中句柄经两张表映射回实体：组件体（physics_role_bodies）与脚本球（balls）。 */
float physics_raycast(void* ctx, float x, float y, float dx, float dy, float max_distance) {
    auto* eng = (Engine*)ctx;
    if (!eng->physics) return -1.0F;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 1.0e-6F)) return -1.0F;
    mq_physics_ray ray{};
    ray.ox = x;
    ray.oy = y;
    ray.dx = dx / len;
    ray.dy = dy / len;
    ray.max_toi = max_distance > 0.0F ? max_distance : 1.0e30F;
    mq_physics_ray_hit hit{};
    if (mq_physics_raycast(eng->physics, &ray, &hit) != MQ_PHYSICS_OK || hit.has_hit == 0u)
        return -1.0F;
    for (const auto& [id, body] : eng->physics_role_bodies) {
        if (body.index == hit.body.index && body.generation == hit.body.generation)
            return (float)id;
    }
    for (const auto& b : eng->balls) {
        if (b.body.index == hit.body.index && b.body.generation == hit.body.generation)
            return (float)b.id;
    }
    return -1.0F;
}

float spawn_ball(void* ctx, float x, float y, float vx, float vy, float radius) {
    auto* eng = (Engine*)ctx;
    float id = host::entity_spawn(eng);
    if (id < 0) return -1.0F;
    attach_component(*eng, (int)id, "scn.Transform2D");
    attach_component(*eng, (int)id, "scn.Sprite");
    SceneNode* be = eng->scene->at((int)id);
    if (be) {
        for (auto& [tid, blob] : be->comps) {
            const mq_type_desc* d = blob.desc;
            auto* spr = d ? as_component_mut<scn::Sprite>(d, blob.data()) : nullptr;
            if (!spr) continue;
            spr->radius = radius > 0 ? radius : 8.0F;
            spr->r = 0.4F + eng->next_rng() / 4294967296.0F * 0.6F;
            spr->g = 0.5F;
            spr->b = 1.0F;
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

void ball_clear(void* ctx) {
    auto* eng = (Engine*)ctx;
    for (auto& b : eng->balls) {
        if (eng->physics && b.body.index) mq_physics_remove_body(eng->physics, b.body);
        host::entity_destroy(eng, (float)b.id);
    }
    eng->balls.clear();
}

}  // namespace host
