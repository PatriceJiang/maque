/* 控制面 · 世界快照/校验和（docs/10 §4/§5）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

extern "C" int mq_control_World_checksum(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap,
                  "{\"trail\":\"%llu\",\"last\":\"%llu\",\"frame\":%llu,"
                  "\"scene\":\"%llu\",\"physics\":\"%llu\",\"particles\":%zu}",
                  (unsigned long long)e->world_checksum(), (unsigned long long)e->last_checksum,
                  (unsigned long long)e->frame,
                  (unsigned long long)(e->scene ? e->scene->checksum(*e) : 0),
                  (unsigned long long)(e->physics ? mq_physics_checksum(e->physics) : 0),
                  e->systems.particles.size());

    return 0;
}

/* World.snapshot：抓当前世界快照（返回字节数与校验和；AI undo/回滚点）。 */
extern "C" int mq_control_World_snapshot(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->snap_manual) e->snap_manual = mq_snapshot_create();     // 独立于周期池（不被 60 帧环覆盖）
    if (!e->world_snapshot(e->snap_manual)) { std::snprintf(out, cap, "{\"ok\":false}"); return 0; }
    size_t n = 0; mq_snapshot_bytes(e->snap_manual, &n);
    std::snprintf(out, cap, "{\"ok\":true,\"bytes\":%zu,\"checksum\":\"%llu\",\"frame\":%llu}",
                  n, (unsigned long long)mq_snapshot_checksum(e->snap_manual),
                  (unsigned long long)e->frame);
    return 0;
}

/* World.restore：回到最近快照（A5 快迭代/undo，docs/10 §4）。 */
extern "C" int mq_control_World_restore(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    mq_snapshot* src = e->snap_manual ? e->snap_manual : e->snap_last;
    if (!src) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no snapshot\"}"); return 0; }
    e->world_restore(src);
    std::snprintf(out, cap, "{\"ok\":true,\"entities\":%d}", e->scene->count());
    return 0;
}

extern "C" int mq_control_World_trail(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "{\"trail\":\"" + std::to_string(e->world_trail()) + "\",\"frames\":[";
    char item[64];
    for (size_t i = 0; i < e->trail.size(); ++i) {
        std::snprintf(item, sizeof item, "%s[%llu,\"%llu\"]", i ? "," : "",
                      (unsigned long long)e->trail[i].first, (unsigned long long)e->trail[i].second);
        o += item;
    }
    o += "]}";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}
