/* 世界变换（docs/12）：每帧一趟树访问把局部累积成世界坐标/矩阵 + 有效属性
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* 组件局部位置：直接读生成的 Transform2D struct（兼容三种声明形态）。 */
static void read_local_pos(Engine& eng, const SceneNode& e, float* lx, float* ly) {
    *lx = e.local_x; *ly = e.local_y;                      /* 无 Transform2D：显式局部兜底（真值在节点上） */
    for (auto& [tid, blob] : e.comps) {
        const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
        const uint8_t* p = blob.data();
        if (const auto* t = as_component<scn::Transform2D>(d, p)) {
            *lx = t->x; *ly = t->y; return;
        }
        if (const auto* t = as_component<scene::Transform2D>(d, p)) {
            *lx = t->local.position.x; *ly = t->local.position.y; return;
        }
        if (const auto* t = as_component<math::Transform2D>(d, p)) {
            *lx = t->position.x; *ly = t->position.y; return;
        }
    }
}

/* 每帧一趟树访问（Scene::visit，前序）：把局部变换累积成世界坐标，同时算出
 * active/angle/scale/opacity 的有效值。派生值写回节点，后续提取/命中直接 O(1) 读取，
 * 不再对每个节点沿父链重复上溯，也不再重建 "id→父" 的扁平索引。
 * 顺序确定：根 id 升序、兄弟 id 升序（children 有序）→ 与快照/校验和一致（docs/10 D4）。 */
void Scene::update_world(Engine& eng) {
    static const bool tv_on = [] { const char* e = std::getenv("MAQUE_TRACE_FLATTEN"); return e && *e && *e != '0'; }();
    const auto t0 = tv_on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    visit([&](int idx, SceneNode& e, int depth) {
        (void)idx;
        float lx = 0, ly = 0;
        read_local_pos(eng, e, &lx, &ly);
        float px = 0, py = 0, pang = 0, psc = 1, pop = 1;
        bool pvis = true;
        if (SceneNode* pe = e.parent >= 0 ? at(e.parent) : nullptr) {
            px = pe->world_x; py = pe->world_y;
            pang = pe->world_angle; psc = pe->world_scale; pop = pe->world_opacity;
            pvis = pe->visible;
        }
        e.world_x = px + lx;
        e.world_y = py + ly;
        e.world_angle = pang + e.angle;
        e.world_scale = psc * e.scale;
        e.world_opacity = pop * e.opacity;
        if (e.world_opacity < 0.0F) e.world_opacity = 0.0F;
        if (e.world_opacity > 1.0F) e.world_opacity = 1.0F;
        e.visible = pvis && e.active;
        e.depth = depth;
    });
    if (tv_on) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::print("[visit] {:.3f}ms nodes={} roots={}\n", ms, nodes.size(), roots.size());
    }
}

/* 3D 世界矩阵（plans/22 §6.3）：与 update_world 同一趟树序（父先于子）。
 * local = scene.Transform3D.local 叠加 Engine::anim_pose（clips 采样结果）；
 * 有 rotation 轨道用四元数路径（m4_from_quat_trs，无万向锁），否则走 Euler TRS（m4_from_trs）；
 * world_m = parent.world_m * local。平级场景 world_m == m4_from_trs(local)，旧场景零漂移。 */
void Scene::update_world3d(Engine& eng) {
    visit([&](int idx, SceneNode& e, int) {
        math::Transform3D trs{};
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (const auto* t3 = as_component<scene::Transform3D>(d, blob.data())) {
                trs = t3->local;
                break;
            }
        }
        AnimPose* pose = nullptr;
        auto pit = eng.anim_pose.find(idx);
        if (pit != eng.anim_pose.end()) pose = &pit->second;
        if (pose && pose->has_pos) trs.position = pose->pos;
        if (pose && pose->has_scale) trs.scale = pose->scale;
        float local[16];
        if (pose && pose->has_rot) m4_from_quat_trs(trs.position, pose->rot, trs.scale, local);
        else m4_from_trs(trs, local);
        if (SceneNode* pe = e.parent >= 0 ? at(e.parent) : nullptr) m4_mul(pe->world_m, local, e.world_m);
        else std::memcpy(e.world_m, local, sizeof local);
    });
}

/* 节点有效属性：update_world 已沿树累积到节点上（O(1) 读取）。
 * 语义与 2D 直觉一致：角度相加、缩放相乘、透明度相乘并夹 [0,1]、active 沿祖先与。 */
void accumulate_node_attrs(Engine& eng, int idx, float& angle_deg, float& scale, float& opacity) {
    angle_deg = 0.0F; scale = 1.0F; opacity = 1.0F;
    if (!eng.scene) return;
    if (SceneNode* e = eng.scene->at(idx)) {
        angle_deg = e->world_angle;
        scale = e->world_scale;
        opacity = e->world_opacity;
    }
}

/* active 沿祖先与（docs/18：node_set_active 影响自身与子树）—— 有效值由 update_world 缓存。 */
bool node_active_effective(Engine& eng, int idx) {
    if (!eng.scene) return true;
    SceneNode* e = eng.scene->at(idx);
    return e ? e->visible : true;
}

}  // namespace maque::rt
