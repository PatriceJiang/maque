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

/* plans/27 3D 粒子：3D 粒子（CPU 确定性）：固定步长、引擎 RNG；渲染在 extract_3d 里合成
 * billboard 动态网格（一个发射器一次 draw），后续 plans/28 再换 GPU instanced。 */
void Engine::particles3d_step(float dt) {
    if (!scene || dt <= 0.0F) return;
    size_t w = 0;
    for (size_t i = 0; i < systems.particles3d.size(); ++i) {
        SystemsRuntime::Particle3D& p = systems.particles3d[i];
        p.age += dt;
        if (p.age >= p.life) continue;
        p.vx += p.gx * dt; p.vy += p.gy * dt; p.vz += p.gz * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        systems.particles3d[w++] = p;
    }
    systems.particles3d.resize(w);

    std::vector<int> ids;
    scene->collect_preorder(ids);
    constexpr float kTau = 6.28318530718F;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* dp = nullptr;
        auto* pb = comp_by_name(*this, id, "render.ParticleEmitter3D", &dp);
        const auto* pe = (pb && dp) ? as_component<render::ParticleEmitter3D>(dp, pb->data()) : nullptr;
        if (!pe || !pe->emitting || !node_active_effective(*this, id)) continue;
        const float rate = std::max(pe->rate, 0.0F);
        if (rate <= 0.0F) continue;
        const uint32_t max_count = std::clamp(pe->max_count, 1u, 4096u);
        uint32_t alive = 0;
        for (const auto& p : systems.particles3d) if (p.owner == id) ++alive;

        uint32_t& acc = systems.particle3d_accum[id];
        acc += (uint32_t)(rate * dt * 1024.0F);
        const int per_emit = (int)(acc / 1024u);
        acc -= (uint32_t)per_emit * 1024u;

        float base_dir[3] = { pe->direction.x, pe->direction.y, pe->direction.z };
        float bl = std::sqrt(base_dir[0] * base_dir[0] + base_dir[1] * base_dir[1] + base_dir[2] * base_dir[2]);
        if (!(bl > 1e-6F)) { base_dir[0] = 0.0F; base_dir[1] = 1.0F; base_dir[2] = 0.0F; bl = 1.0F; }
        for (int i = 0; i < per_emit; ++i) {
            if (systems.particles3d.size() >= 4096) break;
            if (alive >= max_count) break;
            const float u1 = next_rng() / 4294967296.0F;
            const float u2 = next_rng() / 4294967296.0F;
            const float u3 = next_rng() / 4294967296.0F;
            const float u4 = next_rng() / 4294967296.0F;
            const float z = 2.0F * u1 - 1.0F;
            const float rr = std::sqrt(std::max(0.0F, 1.0F - z * z));
            const float ph = kTau * u2;
            float dir[3] = {
                base_dir[0] / bl * (1.0F - pe->spread) + rr * std::cos(ph) * pe->spread,
                base_dir[1] / bl * (1.0F - pe->spread) + z * pe->spread,
                base_dir[2] / bl * (1.0F - pe->spread) + rr * std::sin(ph) * pe->spread,
            };
            float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (!(dl > 1e-6F)) { dir[0] = 0.0F; dir[1] = 1.0F; dir[2] = 0.0F; dl = 1.0F; }
            const float spd = pe->speed * (0.75F + 0.5F * u3);
            SystemsRuntime::Particle3D p;
            p.owner = id;
            p.x = e->world_m[12]; p.y = e->world_m[13]; p.z = e->world_m[14];
            p.vx = dir[0] / dl * spd;
            p.vy = dir[1] / dl * spd;
            p.vz = dir[2] / dl * spd;
            p.gx = pe->gravity.x; p.gy = pe->gravity.y; p.gz = pe->gravity.z;
            p.life = std::max(pe->lifetime, 0.01F) * (0.75F + 0.5F * u4);
            p.size0 = pe->size_start; p.size1 = pe->size_end;
            p.rot = pe->rotation + (u3 - 0.5F) * 3.14159265F;
            p.rot_speed = pe->rotation_speed;
            p.c0[0] = pe->color_start.r; p.c0[1] = pe->color_start.g;
            p.c0[2] = pe->color_start.b; p.c0[3] = pe->color_start.a;
            p.c1[0] = pe->color_end.r; p.c1[1] = pe->color_end.g;
            p.c1[2] = pe->color_end.b; p.c1[3] = pe->color_end.a;
            systems.particles3d.push_back(p);
            ++alive;
        }
    }
}

}  // namespace maque::rt
