/* 世界快照 codec（docs/10 §4）：实体/组件按反射逐字段二进制序列化
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 世界快照（docs/10 §4）：实体/组件用二进制 codec 逐字段序列化。
 * 布局：u32 实体数；每实体 {i32 index, i32 parent, u32 name_len+name,
 *        u32 组件数, 每组件 {u32 type_id, u32 bytes, payload}}。
 * 转换按 index 排序 → 不依赖哈希迭代顺序（docs/10 D4）。 ---- */
void Scene::encode(Engine& eng, SnapshotWriter& w) {
    std::vector<int> idx;
    for (auto& [k, v] : nodes) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    w.pod((uint32_t)idx.size());
    for (int k : idx) {
        SceneNode& e = nodes[k];
        w.pod((int32_t)k);
        w.pod((int32_t)e.parent);
        w.pod((uint8_t)(e.active ? 1 : 0));
        w.pod(e.scale);
        w.pod(e.angle);
        w.pod(e.opacity);
        w.pod((int32_t)e.z_index);
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
    this->eng = &eng;
    const int id_floor = next_id;      // 回滚也不回收已发放的 id（单调契约）
    size_t pos = 0;
    auto take = [&](void* dst, size_t bytes) -> bool {
        if (pos + bytes > n) return false;
        std::memcpy(dst, in + pos, bytes);
        pos += bytes;
        return true;
    };
    uint32_t count = 0;
    if (!take(&count, 4) || count > (1u << 20)) return 0;
    for (auto& [k, ent] : nodes)
        for (auto& [t, b] : ent.comps) destroy_blob(eng, t, b);
    nodes.clear();
    roots.clear();
    for (uint32_t i = 0; i < count; ++i) {
        int32_t k = 0, parent = -1;
        uint32_t name_len = 0;
        uint8_t active = 1;
        float scale = 1.0F, angle = 0.0F, opacity = 1.0F;
        int32_t z_index = 0;
        if (!take(&k, 4) || !take(&parent, 4) || !take(&active, 1)
            || !take(&scale, 4) || !take(&angle, 4) || !take(&opacity, 4) || !take(&z_index, 4)
            || !take(&name_len, 4)) return 0;
        if (name_len > 4096 || pos + name_len > n) return 0;
        std::string nm((const char*)in + pos, name_len);
        pos += name_len;
        uint32_t comps = 0;
        if (!take(&comps, 4) || comps > 4096) return 0;
        SceneNode e;
        e.name = std::move(nm);
        e.parent = parent;
        e.active = active != 0;
        e.scale = scale;
        e.angle = angle;
        e.opacity = opacity;
        e.z_index = (int)z_index;
        if (k < 0) return 0;              // 实体索引按快照重建（脚本 id 稳定，docs/10 D4）
        for (uint32_t c = 0; c < comps; ++c) {
            uint32_t tid = 0, len = 0;
            if (!take(&tid, 4) || !take(&len, 4) || pos + len > n) return 0;
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            std::vector<uint8_t> blob;
            if (d) {
                construct_blob(eng, d, blob);          // 先构造（析构由 destroy_blob 负责）
                if (len && mq_persist_decode(eng.reflect, d, in + pos, len, blob.data()) < 0) {
                    destroy_blob(eng, tid, blob);
                    return 0;
                }
                e.comps[tid] = std::move(blob);
            }
            pos += len;
        }
        nodes[k] = std::move(e);
        if (k + 1 > next_id) next_id = k + 1;
    }
    if (id_floor > next_id) next_id = id_floor;
    rebuild_tree();                 // children/roots 是派生结构：快照只存 parent，恢复后重建
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
    for (auto& [k, v] : nodes) idx.push_back(k);
    std::sort(idx.begin(), idx.end());
    for (int k : idx) {
        SceneNode& e = nodes[k];
        mix(&k, sizeof k); mix(&e.parent, sizeof e.parent); mix(&e.world_x, sizeof e.world_x); mix(&e.world_y, sizeof e.world_y);
        mix(&e.active, sizeof e.active); mix(&e.scale, sizeof e.scale); mix(&e.angle, sizeof e.angle);
        mix(&e.opacity, sizeof e.opacity); mix(&e.z_index, sizeof e.z_index);
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
                /* transient 字段是派生/装饰状态（排版度量、控件 phase、rect…）：
                 * 不进快照（docs/10 §4）→ 也不进校验和，否则回滚后必然不等 */
                if (f->attrs_flags & MQ_ATTR_TRANSIENT) continue;
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

}  // namespace maque::rt
