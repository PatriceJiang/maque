/* 宿主 API · 3D 粒子场（plans/27 3D 粒子 / plans/28）：
 * 脚本每帧把外部模拟结果（Boids 等）以 48B/粒子 的字节块 upload 给 render.ParticleField3D。
 *
 * 数据布局（12×float32）：pos.xyz, size, color.rgba, rotation, pad×3。
 * 空数据 = 清空；超出 max_count 的部分截断。渲染在 extract_3d 第二 pass 合成 billboard 网格。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

mq_script_bytes_result particles3d_upload(void* ctx, float id, mq_script_bytes_view data) {
    mq_script_bytes_result out{ nullptr, 0, nullptr, nullptr };
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->scene) return out;
    const int entity = (int)id;
    if (!eng->scene->at(entity) || !node_active_effective(*eng, entity)) return out;

    const mq_type_desc* d = nullptr;
    auto* b = comp_by_name(*eng, entity, "render.ParticleField3D", &d);
    auto* pf = (b && d)
                    ? const_cast<render::ParticleField3D*>(
                          as_component<render::ParticleField3D>(d, b->data()))
                    : nullptr;
    if (!pf || !pf->enabled) return out;

    auto& field = eng->particle_fields[entity];
    const size_t stride = 12u * sizeof(float);
    if (!data.data || data.size == 0 || (data.size % stride) != 0) {
        field.data.clear();
        field.count = 0;
        field.dirty = true;
        pf->count = 0;
        return out;
    }
    uint32_t count = (uint32_t)(data.size / stride);
    count = std::min<uint32_t>(count, std::clamp<uint32_t>(pf->max_count, 1u, 8192u));
    field.data.resize((size_t)count * 12u);
    std::memcpy(field.data.data(), data.data, (size_t)count * stride);
    field.count = count;
    field.dirty = true;
    pf->count = count;
    return out;
}

float particles3d_count(void* ctx, float id) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->scene) return -1.0F;
    auto it = eng->particle_fields.find((int)id);
    return it == eng->particle_fields.end() ? -1.0F : (float)it->second.count;
}

}  // namespace host
