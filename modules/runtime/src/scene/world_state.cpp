/* 世界状态（docs/10 §4/§5）：快照/回滚、校验和、失步轨迹
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

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
    w.pod((uint32_t)systems.emitter_accum.size());
    for (int acc : systems.emitter_accum) w.pod((int32_t)acc);
    w.pod((uint32_t)systems.particles.size());
    for (auto& p : systems.particles) {
        w.pod((int32_t)p.owner);
        w.pod(p.x); w.pod(p.y); w.pod(p.vx); w.pod(p.vy);
        w.pod(p.life); w.pod(p.max_life); w.pod(p.size);
        w.pod(p.r); w.pod(p.g); w.pod(p.b);
    }
    encode_globals(w);                                // 全局状态（docs/32 §16.3；旧快照无此块）
    encode_instances(w);                              // 实例表 + 实体归属（docs/32 §6.3）
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
        systems.particles.clear();
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
            systems.emitter_accum.clear();
            for (uint32_t i = 0; i < acount && off + 4 <= n; ++i) {
                int32_t v = 0;
                std::memcpy(&v, (const uint8_t*)bytes + off, 4);
                off += 4;
                systems.emitter_accum.push_back(v);
            }
        }
        if (off + 4 <= n) {
            uint32_t pcount = 0;
            std::memcpy(&pcount, (const uint8_t*)bytes + off, 4);
            off += 4;
            if (pcount < 100000) {
                systems.particles.reserve(pcount);
                for (uint32_t i = 0; i < pcount; ++i) {
                    if (off + sizeof(SystemsRuntime::Particle) - sizeof(int) > n) break;
                    SystemsRuntime::Particle p{};
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
                    systems.particles.push_back(p);
                }
            }
        }
        /* 全局状态（docs/32 §16.3）：必须在实例 on_start 之前恢复（脚本可能读它）。
         * 旧快照没有该块 → restore_globals 探测 magic 失败、原样跳过。 */
        (void)restore_globals((const uint8_t*)bytes, n, off);
        /* 实例表（docs/32 §6.3）：结构相同时保留 VM 状态；结构变化才重建并 on_start 重注册。 */
        const int inst_mode = restore_instances((const uint8_t*)bytes, n, off);
        if (inst_mode == 1) {
            (void)dispatch_instance_on_start(0);
            if (global_bus) (void)call_global_event("on_start", 0.0, 0.0, 0.0, nullptr);
            if (overlay_bus) (void)call_overlay_event("on_start", 0.0, 0.0, 0.0, nullptr);
        }
    }
    scene->update_world(*this);
}

/* 全局状态校验和（docs/32 §16.3）：空 → 0（老场景的 world_trail 零变化）。
 * 键有序（std::map）→ 遍历确定；数值按位混入（-0.0 与 +0.0 不同，符合快照字节语义）。 */
uint64_t Engine::global_checksum() {
    if (global_nums.empty() && global_texts.empty()) return 0;
    uint64_t h = 1469598103934665603ull;
    auto mix_bytes = [&](const std::string& s) {
        for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ull; }
    };
    for (auto& [k, v] : global_nums) {
        mix_bytes(k);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h ^= bits; h *= 1099511628211ull;
    }
    for (auto& [k, v] : global_texts) {
        h ^= 0x9E3779B97F4A7C15ull; h *= 1099511628211ull;   // 数值/文本段分开
        mix_bytes(k);
        mix_bytes(v);
    }
    return h;
}

/* 逐帧校验和默认关闭：正常游玩/发布不需要每帧遍历全场景做“记账”。
 * 只有需要失步定位/回放的路径才打开：
 *   · MAQUE_REPLAY / MAQUE_RECORD / MAQUE_TRAIL_OUT / MAQUE_CRASH_REPORT（自动）
 *   · MAQUE_CHECKSUM=1（显式；0/off 只影响普通运行，回放/录制仍强制打开）
 * 关闭时 finalize_run 仍算一次“终局指纹”，用于退出日志/测试对比，开销 O(场景) 一次。
 * MAQUE_CHECKSUM_INFO=1 打印实际状态。 */
void Engine::checksum_init() {
    if (checksum_checked) return;
    checksum_checked = true;
    auto env_on = [](const char* name) {
        const char* v = std::getenv(name);
        if (!v || !*v) return false;
        return std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0 &&
               std::strcmp(v, "false") != 0;
    };
    const char* c = std::getenv("MAQUE_CHECKSUM");
    if (c && *c)
        checksum_track = std::strcmp(c, "0") != 0 && std::strcmp(c, "off") != 0 &&
                         std::strcmp(c, "false") != 0;
    /* 这些路径没有逐帧 checksum 就无法判断 replay 是否失步 / 崩溃现场是否可复现。 */
    if (env_on("MAQUE_REPLAY") || env_on("MAQUE_RECORD") || env_on("MAQUE_TRAIL_OUT") ||
        env_on("MAQUE_CRASH_REPORT"))
        checksum_track = true;
    if (env_on("MAQUE_CHECKSUM_INFO")) {
        if (checksum_track) std::print("runtime: checksum per-frame=on\n");
        else std::print("runtime: checksum per-frame=off (final fingerprint at exit)\n");
    }
}

/* 世界校验和：场景（实体/组件标量）+ 物理求解器状态 + 粒子（都是模拟状态，docs/10 §5）。 */
uint64_t Engine::world_checksum() {
    uint64_t h = scene ? scene->checksum(*this) : 1469598103934665603ull;
    h ^= instance_checksum() * 1099511628211ull;      // 实例结构（docs/32 §6.3/§8）
    h ^= global_checksum() * 1099511628211ull;        // 全局状态（空 → 0，零回归）
    if (physics) h ^= mq_physics_checksum(physics) * 1099511628211ull;
    for (auto& p : systems.particles) {
        h ^= (uint64_t)(int64_t)(p.x * 4096.0F); h *= 1099511628211ull;
        h ^= (uint64_t)(int64_t)(p.y * 4096.0F); h *= 1099511628211ull;
        h ^= (uint64_t)(int64_t)(p.life * 4096.0F); h *= 1099511628211ull;
    }
    return h;
}

/* 校验和轨迹（逐帧；用于失步定位与回放对比，docs/10 §5/§7）。 */
uint64_t Engine::world_trail() const {
    /* 默认不逐帧记录：只有终局指纹（finalize_run 计算）或调用方显式要求 checksum 时才有 trail。 */
    if (trail.empty()) return last_checksum;
    uint64_t h = 1469598103934665603ull;
    for (auto& [f, c] : trail) {
        h ^= f; h *= 1099511628211ull;
        h ^= c; h *= 1099511628211ull;
    }
    return h;
}

}  // namespace maque::rt
