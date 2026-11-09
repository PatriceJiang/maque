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

/* 遍历所有可拾取网格的 OBB：fn(entity, model, local_bounds_min, local_bounds_max)。
 * 命中几何与 pick_ray / 视锥剔除同源：glTF primitive bounds / .mesh.json bounds / 内建 ±0.6。 */
template <class Fn>
static void for_each_mesh_obb(Engine& eng, Fn&& fn) {
    if (!eng.scene || !eng.has_3d) return;
    /* 脚本 update 早于 extract_3d → 先刷新派生世界矩阵（与画面同一帧）。 */
    eng.scene->update_world3d(eng);
    std::vector<int>& ids = eng.pick_scratch;
    eng.scene->collect_preorder(ids);
    for (const int idx : ids) {
        SceneNode* e = eng.scene->at(idx);
        if (!e || !e->visible) continue;
        const mq_type_desc* dmesh = nullptr;
        auto* mb = comp_by_name(eng, idx, "render.MeshRenderer", &dmesh);
        const auto* mesh = (mb && dmesh) ? as_component<render::MeshRenderer>(dmesh, mb->data()) : nullptr;
        if (!mesh || !mesh->visible) continue;
        const float* wm = e->world_m;

        if (!mesh->mesh_path.empty() && gltf::is_gltf_path(mesh->mesh_path)) {
            auto git = eng.gltf_models.find(mesh->mesh_path);
            if (git == eng.gltf_models.end() || git->second.failed) continue;
            Engine::GltfGpuModel& gm = git->second;
            eng.gltf_state_ensure(idx, mesh->mesh_path, gm);
            auto sit = eng.gltf_states.find(idx);
            if (sit == eng.gltf_states.end()) continue;
            const Engine::GltfEntityState& st = sit->second;
            for (const Engine::GltfGpuModel::Prim& pr : gm.prims) {
                float model[16];
                const bool skinned = pr.skin >= 0 && (size_t)pr.skin < st.palettes.size() &&
                                     !st.palettes[(size_t)pr.skin].empty();
                const size_t off = (size_t)pr.node * 16u;
                if (!skinned && pr.node >= 0 && off + 16u <= st.node_world.size())
                    m4_mul(wm, &st.node_world[off], model);
                else
                    std::memcpy(model, wm, sizeof model);
                fn(idx, model, pr.bounds_min, pr.bounds_max);
            }
            continue;
        }
        if (!mesh->mesh_path.empty()) {
            auto bit = eng.mesh_bounds.find(mesh->mesh_path);
            if (bit == eng.mesh_bounds.end()) continue;
            fn(idx, wm, bit->second.min, bit->second.max);
            continue;
        }
        /* 无资产路径 → 程序化图元包围盒（缺省 Cube 1.2 = 旧 ±0.6）。 */
        float bmin[3] = { -0.6F, -0.6F, -0.6F };
        float bmax[3] = { 0.6F, 0.6F, 0.6F };
        const float gsize[3] = { mesh->geometry_size.x, mesh->geometry_size.y, mesh->geometry_size.z };
        primitive_bounds((int32_t)mesh->geometry, gsize, bmin, bmax);
        fn(idx, wm, bmin, bmax);
    }
}

/* 模型矩阵三轴缩放的最小值（用于把世界半径换算到本地；非均匀缩放时保守放大）。 */
static float model_min_scale(const float m[16]) {
    const float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    const float s = std::min(sx, std::min(sy, sz));
    return s > 1e-6F ? s : 1e-6F;
}

/* 点（世界）到本地 AABB 的距离（负 = 在内部 → 0）。 */
static float local_point_aabb_dist(const float inv[16], const float p[3],
                                   const float bmin[3], const float bmax[3]) {
    float lp[3];
    if (!m4_project4(inv, p[0], p[1], p[2], 1.0F, lp)) return 1e30F;
    float d2 = 0.0F;
    for (int a = 0; a < 3; ++a) {
        const float d = std::max({ bmin[a] - lp[a], 0.0F, lp[a] - bmax[a] });
        d2 += d * d;
    }
    return std::sqrt(d2);
}

/* OBB-OBB 分离轴测试（15 轴，SAT）。 */
static bool obb_overlap_sat(const float c0[3], const float ax0[3][3], const float he0[3],
                            const float c1[3], const float ax1[3][3], const float he1[3]) {
    float d[3] = { c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2] };
    auto test_axis = [&](const float L[3]) -> bool {
        const float len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (len < 1e-6F) return false;                 /* 退化轴：跳过（不判分离） */
        const float u[3] = { L[0] / len, L[1] / len, L[2] / len };
        float r0 = 0.0F, r1 = 0.0F;
        for (int i = 0; i < 3; ++i) {
            r0 += he0[i] * std::fabs(ax0[i][0] * u[0] + ax0[i][1] * u[1] + ax0[i][2] * u[2]);
            r1 += he1[i] * std::fabs(ax1[i][0] * u[0] + ax1[i][1] * u[1] + ax1[i][2] * u[2]);
        }
        const float dist = std::fabs(d[0] * u[0] + d[1] * u[1] + d[2] * u[2]);
        return dist > r0 + r1;                          /* true = 分离 */
    };
    for (int i = 0; i < 3; ++i) if (test_axis(ax0[i])) return false;
    for (int i = 0; i < 3; ++i) if (test_axis(ax1[i])) return false;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float cr[3] = {
                ax0[i][1] * ax1[j][2] - ax0[i][2] * ax1[j][1],
                ax0[i][2] * ax1[j][0] - ax0[i][0] * ax1[j][2],
                ax0[i][0] * ax1[j][1] - ax0[i][1] * ax1[j][0],
            };
            if (test_axis(cr)) return false;
        }
    }
    return true;
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

    for_each_mesh_obb(*this, [&](int idx, const float* model, const float* bmin, const float* bmax) {
        float t = 0.0F;
        if (ray_local_aabb(origin, d, model, bmin, bmax, t) && t <= max_t)
            set_hit(best, idx, t, origin, d);
    });
    cast_last = best;
    return best;
}

Engine::PickHit Engine::pick_sphere_overlap(const float center[3], float radius) {
    PickHit best;
    if (!scene || !has_3d || !center || !(radius > 0.0F)) return best;
    for_each_mesh_obb(*this, [&](int idx, const float* model, const float* bmin, const float* bmax) {
        float inv[16];
        if (!m4_invert(model, inv)) return;
        const float r_local = radius / model_min_scale(model);
        const float dist = local_point_aabb_dist(inv, center, bmin, bmax);
        if (dist <= r_local) {
            if (best.entity < 0 || dist < best.distance) {
                best.entity = idx;
                best.distance = dist * model_min_scale(model);
                std::memcpy(best.point, center, sizeof best.point);
            }
        }
    });
    cast_last = best;
    return best;
}

Engine::PickHit Engine::pick_sphere_cast(const float origin[3], const float dir[3], float radius,
                                        float max_distance) {
    PickHit best;
    if (!scene || !has_3d || !origin || !dir || !(radius >= 0.0F)) return best;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-8F)) return best;
    const float d[3] = { dir[0] / len, dir[1] / len, dir[2] / len };
    const float max_t = max_distance > 0.0F ? max_distance : 1.0e30F;
    for_each_mesh_obb(*this, [&](int idx, const float* model, const float* bmin, const float* bmax) {
        const float r_local = radius / model_min_scale(model);
        float ebmin[3], ebmax[3];
        for (int a = 0; a < 3; ++a) { ebmin[a] = bmin[a] - r_local; ebmax[a] = bmax[a] + r_local; }
        float t = 0.0F;
        if (ray_local_aabb(origin, d, model, ebmin, ebmax, t) && t <= max_t)
            set_hit(best, idx, t, origin, d);
    });
    cast_last = best;
    return best;
}

Engine::PickHit Engine::pick_obb_overlap(const float center[3], const float half[3], float yaw) {
    PickHit best;
    if (!scene || !has_3d || !center || !half) return best;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    /* 绕 Y 旋转：与 m4_from_trs 的 Ry 列为准（x=(cy,0,-sy)、z=(sy,0,cy)）。 */
    const float ax0[3][3] = { { cy, 0.0F, -sy }, { 0.0F, 1.0F, 0.0F }, { sy, 0.0F, cy } };
    const float he0[3] = { half[0], half[1], half[2] };
    for_each_mesh_obb(*this, [&](int idx, const float* model, const float* bmin, const float* bmax) {
        const float lc[3] = { 0.5F * (bmin[0] + bmax[0]), 0.5F * (bmin[1] + bmax[1]),
                              0.5F * (bmin[2] + bmax[2]) };
        const float c1[3] = {
            model[0] * lc[0] + model[4] * lc[1] + model[8] * lc[2] + model[12],
            model[1] * lc[0] + model[5] * lc[1] + model[9] * lc[2] + model[13],
            model[2] * lc[0] + model[6] * lc[1] + model[10] * lc[2] + model[14],
        };
        const float sx = std::sqrt(model[0] * model[0] + model[1] * model[1] + model[2] * model[2]);
        const float syv = std::sqrt(model[4] * model[4] + model[5] * model[5] + model[6] * model[6]);
        const float sz = std::sqrt(model[8] * model[8] + model[9] * model[9] + model[10] * model[10]);
        if (sx < 1e-6F || syv < 1e-6F || sz < 1e-6F) return;
        const float ax1[3][3] = {
            { model[0] / sx, model[1] / sx, model[2] / sx },
            { model[4] / syv, model[5] / syv, model[6] / syv },
            { model[8] / sz, model[9] / sz, model[10] / sz },
        };
        const float he1[3] = { 0.5F * (bmax[0] - bmin[0]) * sx,
                               0.5F * (bmax[1] - bmin[1]) * syv,
                               0.5F * (bmax[2] - bmin[2]) * sz };
        if (obb_overlap_sat(center, ax0, he0, c1, ax1, he1)) {
            const float dx = c1[0] - center[0], dy = c1[1] - center[1], dz = c1[2] - center[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (best.entity < 0 || dist < best.distance) {
                best.entity = idx;
                best.distance = dist;
                std::memcpy(best.point, center, sizeof best.point);
            }
        }
    });
    cast_last = best;
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
    /* 相机缓存 = 最近一次 3D pass 的 view/proj；RenderFactor 会让 3D target 小于主
     * target，不能再用 pick_tw == TW 判定失效 —— 直接用 pick_tw/pick_th 做像素换算。 */
    if (pick_tw <= 0 || pick_th <= 0)
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
