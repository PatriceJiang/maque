/* 系统 · 粒子（docs/15 §4）：CPU 确定性、池上限、引擎 RNG（参与回放）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S7，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 粒子（docs/15 §4）：CPU 确定性粒子；池上限 + 引擎 RNG（参与回放） ---- */
void Engine::particles_step(float dt) {
    if (!scene) return;
    /* 推进已有粒子（按生成顺序；与实体无关，顺序稳定） */
    size_t w = 0;
    for (size_t i = 0; i < systems.particles.size(); ++i) {
        SystemsRuntime::Particle& p = systems.particles[i];
        p.life += dt;
        if (p.life >= p.max_life) continue;
        p.vy += 0.0F;                                  // 重力在下面按发射器参数施加
        p.x += p.vx * dt; p.y += p.vy * dt;
        systems.particles[w++] = p;
    }
    systems.particles.resize(w);

    /* 发射（按实体 id 升序 → 确定性；RNG 为引擎 RNG） */
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = blob.desc;
            const auto* pe = (d && (d->role & MQ_ROLE_PARTICLE))
                                 ? as_component<scn::ParticleEmitter>(d, blob.data()) : nullptr;
            if (!pe || !pe->enabled) continue;
            const float rate = pe->rate, life = pe->lifetime, speed = pe->speed;
            const float spread = pe->spread, gravity = pe->gravity, size = pe->size;
            const float r = pe->r, g = pe->g, b = pe->b;
            const uint32_t max_count = pe->max_count;
            /* 其它粒子的重力（按各自发射器；用 owner 记录） */
            for (auto& p : systems.particles) if (p.owner == id) p.vy += gravity * dt;
            /* 累加器（每实体一份，存在 systems.emitter_accum；按 id 定位） */
            while ((int)systems.emitter_accum.size() <= id) systems.emitter_accum.push_back(0);
            systems.emitter_accum[id] += (int)(rate * dt * 1024.0F);        // 定点累加（确定性，避免浮点漂移）
            const int per_emit = systems.emitter_accum[id] / 1024;
            systems.emitter_accum[id] -= per_emit * 1024;
            for (int i = 0; i < per_emit; ++i) {
                if (systems.particles.size() >= systems.max_particles) break;
                uint32_t alive_owner = 0;
                for (auto& p : systems.particles) if (p.owner == id) ++alive_owner;
                if (alive_owner >= max_count) break;
                const float ang = (next_rng() / 4294967296.0F) * 6.28318530718F;
                const float mag = speed * (1.0F - spread * (next_rng() / 4294967296.0F));
                SystemsRuntime::Particle p;
                p.owner = id;
                p.x = e->world_x; p.y = e->world_y;
                p.vx = std::cos(ang) * mag;
                p.vy = std::sin(ang) * mag;
                p.max_life = life > 0 ? life : 1.0F;
                p.size = size; p.r = r; p.g = g; p.b = b;
                systems.particles.push_back(p);
            }
        }
    }
}

}  // namespace maque::rt
