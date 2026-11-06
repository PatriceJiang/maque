/* 3D 拾取（docs/19 §4）：相机屏幕射线 / 显式世界射线 → 最近命中的 render.MeshRenderer 实体。
 *
 * 命中几何 = 网格**本地包围盒**经节点世界矩阵变换后的 OBB（射线先逆变换到本地空间再做
 * slab 测试 → 对旋转/非均匀缩放都正确，且 t 仍是世界射线参数）。bounds 来源：
 *   · glTF/GLB：GltfGpuModel::Prim.bounds_min/max（模型本地，叠加 glTF 节点世界矩阵）
 *   · .mesh.json：Engine::mesh_bounds（解析期写入；缺失时由顶点计算）
 *   · 内建 cube：±0.6（与 build_cube 几何一致）
 *
 * 不做三角形级求交（v1）：包围盒对"点选物体"足够，且不依赖 CPU 顶点常驻；
 * 蒙皮/morph 网格用**绑定姿态** bounds（变形后的精确轮廓不参与）。
 *
 * 相机射线用 extract_3d 缓存的上一帧 view/proj（与画面一致）；脚本在 update 中调用时
 * 相机若本帧刚移动，会有一帧延迟 —— 与"所见即所点"一致，不需要额外预测。 */

#include "../runtime_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace maque::rt {

namespace {

/* 射线 vs 本地 AABB：先把射线变换到本地空间（dir 不归一化 → t 保持世界参数）。
 * 命中返回 t（>= 0，起点在内 → 0）。 */
bool ray_local_aabb(const float o[3], const float d[3], const float m[16], const float bmin[3],
                    const float bmax[3], float& t) {
    float inv[16];
    if (!m4_invert(m, inv))
        return false;
    float lo[3];
    if (!m4_project4(inv, o[0], o[1], o[2], 1.0F, lo))
        return false;
    const float ld[3] = {
        inv[0] * d[0] + inv[4] * d[1] + inv[8] * d[2],
        inv[1] * d[0] + inv[5] * d[1] + inv[9] * d[2],
        inv[2] * d[0] + inv[6] * d[1] + inv[10] * d[2],
    };
    float tmin = 0.0F, tmax = 1.0e30F;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(ld[a]) < 1.0e-12F) {
            if (lo[a] < bmin[a] || lo[a] > bmax[a])
                return false;
            continue;
        }
        float t1 = (bmin[a] - lo[a]) / ld[a];
        float t2 = (bmax[a] - lo[a]) / ld[a];
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax)
            return false;
    }
    if (tmax < 0.0F)
        return false;
    t = tmin;
    return true;
}

void set_hit(Engine::PickHit& best, int entity, float t, const float o[3], const float d[3]) {
    if (best.entity >= 0 && t >= best.distance)
        return;                                    /* 严格更近才替换（等距取先遍历者） */
    best.entity = entity;
    best.distance = t;
    for (int k = 0; k < 3; ++k) best.point[k] = o[k] + d[k] * t;
}

} // namespace

Engine::PickHit Engine::pick_ray(const float origin[3], const float dir[3], float max_distance) {
    PickHit best;
    if (!scene || !has_3d || !origin || !dir)
        return best;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1.0e-8F))
        return best;
    const float d[3] = { dir[0] / len, dir[1] / len, dir[2] / len };
    const float max_t = max_distance > 0.0F ? max_distance : 1.0e30F;

    /* 脚本 update 早于 extract_3d → 这里先刷新派生世界矩阵，保证拾取与"当前帧"一致。 */
    scene->update_world3d(*this);

    std::vector<int>& ids = pick_scratch;
    scene->collect_preorder(ids);
    for (const int idx : ids) {
        SceneNode* e = scene->at(idx);
        if (!e || !e->visible)
            continue;
        const mq_type_desc* dmesh = nullptr;
        auto* mb = comp_by_name(*this, idx, "render.MeshRenderer", &dmesh);
        const auto* mesh =
            (mb && dmesh) ? as_component<render::MeshRenderer>(dmesh, mb->data()) : nullptr;
        if (!mesh || !mesh->visible)
            continue;
        const float* wm = e->world_m;

        if (!mesh->mesh_path.empty() && gltf::is_gltf_path(mesh->mesh_path)) {
            auto git = gltf_models.find(mesh->mesh_path);
            if (git == gltf_models.end() || git->second.failed)
                continue;
            GltfGpuModel& gm = git->second;
            gltf_state_ensure(idx, mesh->mesh_path, gm);
            auto sit = gltf_states.find(idx);
            if (sit == gltf_states.end())
                continue;
            const GltfEntityState& st = sit->second;
            for (const GltfGpuModel::Prim& pr : gm.prims) {
                /* 与 extract_3d 的 draw.model 同口径：蒙皮网格忽略节点世界矩阵；
                 * 普通 primitive = 实体世界 × glTF 节点世界；节点无效则退回实体世界。 */
                float model[16];
                const bool skinned = pr.skin >= 0 && (size_t)pr.skin < st.palettes.size() &&
                                     !st.palettes[(size_t)pr.skin].empty();
                const size_t off = (size_t)pr.node * 16u;
                if (!skinned && pr.node >= 0 && off + 16u <= st.node_world.size())
                    m4_mul(wm, &st.node_world[off], model);
                else
                    std::memcpy(model, wm, sizeof model);
                float t = 0.0F;
                if (!ray_local_aabb(origin, d, model, pr.bounds_min, pr.bounds_max, t) || t > max_t)
                    continue;
                set_hit(best, idx, t, origin, d);
            }
            continue;
        }

        if (!mesh->mesh_path.empty()) {
            auto bit = mesh_bounds.find(mesh->mesh_path);
            if (bit == mesh_bounds.end())
                continue;
            float t = 0.0F;
            if (!ray_local_aabb(origin, d, wm, bit->second.min, bit->second.max, t) || t > max_t)
                continue;
            set_hit(best, idx, t, origin, d);
            continue;
        }

        static const float kCubeMin[3] = { -0.6F, -0.6F, -0.6F };
        static const float kCubeMax[3] = { 0.6F, 0.6F, 0.6F };
        float t = 0.0F;
        if (!ray_local_aabb(origin, d, wm, kCubeMin, kCubeMax, t) || t > max_t)
            continue;
        set_hit(best, idx, t, origin, d);
    }
    return best;
}

Engine::PickHit Engine::pick_screen(float x, float y) {
    PickHit miss;
    if (std::getenv("MAQUE_PICK_INFO")) {
        std::print(stderr,
                   "[pick] ask=({:.1f},{:.1f}) valid={} has3d={} cam_tw/th={}x{} cur_tw/th={}x{} "
                   "canvas={:.0f}x{:.0f} s={:.3f},{:.3f}\n",
                   (double)x, (double)y, pick_cam_valid ? 1 : 0, has_3d ? 1 : 0, pick_tw, pick_th,
                   TW, TH, (double)canvas.window_w, (double)canvas.window_h,
                   (double)canvas.scale_x, (double)canvas.scale_y);
    }
    if (!pick_cam_valid || !scene || !has_3d)
        return miss;
    /* 相机缓存 = 最近一次成帧的 view/proj；渲染目标变化（resize）后等下一帧再拾取。 */
    if (pick_tw != TW || pick_th != TH || pick_tw <= 0 || pick_th <= 0)
        return miss;
    if (!(canvas.window_w > 0.0F) || !(canvas.window_h > 0.0F) ||
        !(canvas.scale_x > 0.0F) || !(canvas.scale_y > 0.0F))
        return miss;

    /* 世界坐标（y 向上，input_x/input_y 同一套）→ 设计坐标（y 向下）→ 渲染目标像素。 */
    const float design_h = (float)(H > 0 ? H : (scene->desc.height > 0 ? scene->desc.height : 1));
    float design_x = x;
    const int views = nviews > 0 ? nviews : 1;
    if (views > 1 && W > 0) {
        const int view = std::clamp((int)std::floor(x / (float)W), 0, views - 1);
        design_x = x + (float)view * (float)W;    /* 输入坐标已按单视图折算 → 还原整行设计坐标 */
    }
    const float design_y = design_h - y;
    const float rx = (float)pick_tw / canvas.window_w;
    const float ry = (float)pick_th / canvas.window_h;
    const float tx = (canvas.offset_x + design_x * canvas.scale_x) * rx;
    const float ty = (canvas.offset_y + design_y * canvas.scale_y) * ry;

    const float vp_x = pick_viewport[0], vp_y = pick_viewport[1];
    const float vp_w = pick_viewport[2], vp_h = pick_viewport[3];
    if (!(vp_w > 0.0F) || !(vp_h > 0.0F))
        return miss;
    if (tx < vp_x || tx > vp_x + vp_w || ty < vp_y || ty > vp_y + vp_h)
        return miss;

    const float ndc_x = (tx - vp_x) / vp_w * 2.0F - 1.0F;
    const float ndc_y = 1.0F - (ty - vp_y) / vp_h * 2.0F;   /* 目标像素 y 向下，NDC y 向上 */

    float vp[16], inv[16];
    m4_mul(pick_proj, pick_view, vp);   /* clip = P · V · world（列主序矩阵乘） */
    if (!m4_invert(vp, inv))
        return miss;
    float near_p[3], far_p[3];
    if (!m4_project4(inv, ndc_x, ndc_y, 0.0F, 1.0F, near_p))   /* WebGPU NDC z ∈ [0,1] */
        return miss;
    if (!m4_project4(inv, ndc_x, ndc_y, 1.0F, 1.0F, far_p))
        return miss;
    if (std::getenv("MAQUE_PICK_INFO")) {
        std::print(stderr,
                   "[pick] in=({:.1f},{:.1f}) tw/th={}x{} canvas={:.0f}x{:.0f} s={:.3f},{:.3f} "
                   "off={:.1f},{:.1f} tx/ty={:.1f},{:.1f} vp={:.1f},{:.1f},{:.1f},{:.1f} "
                   "ndc={:.3f},{:.3f} near=({:.2f},{:.2f},{:.2f}) far=({:.2f},{:.2f},{:.2f})\n",
                   (double)x, (double)y, pick_tw, pick_th, (double)canvas.window_w,
                   (double)canvas.window_h, (double)canvas.scale_x, (double)canvas.scale_y,
                   (double)canvas.offset_x, (double)canvas.offset_y, (double)tx, (double)ty,
                   (double)vp_x, (double)vp_y, (double)vp_w, (double)vp_h, (double)ndc_x,
                   (double)ndc_y, (double)near_p[0], (double)near_p[1], (double)near_p[2],
                   (double)far_p[0], (double)far_p[1], (double)far_p[2]);
    }
    const float dir[3] = { far_p[0] - near_p[0], far_p[1] - near_p[1], far_p[2] - near_p[2] };
    return pick_ray(near_p, dir, 0.0F);
}

} // namespace maque::rt
