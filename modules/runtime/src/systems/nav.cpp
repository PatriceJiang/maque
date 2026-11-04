/* 系统 · 导航（docs/15）：A* 在帧内确定性执行，代理沿路径追踪
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S7，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 导航（docs/15 导航）：A* 在帧内确定性执行，代理沿路径追踪 ---- */
void Engine::nav_step() {
    if (!scene || !nav) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            auto* na = (d && (d->role & MQ_ROLE_NAV_AGENT))
                           ? as_component_mut<scn::NavAgent>(d, blob.data()) : nullptr;
            if (!na) continue;
            if (!na->has_target) { na->state = 0; continue; }
            const float tx = na->tx, ty = na->ty, speed = na->speed, arrive = na->arrive_dist;
            const bool repath = na->repath;

            AgentPath& ap = agent_paths[id];
            /* 路径缺失/目标变化 → 重新寻路（确定性：与实体 id 顺序无关的单点查询） */
            const bool need_path = ap.path.empty() || ap.index >= ap.path.size() / 2;
            if (need_path || (repath && !ap.path.empty())) {
                const uint32_t idx_last = ap.path.empty() ? 0 : (uint32_t)(ap.path.size() / 2 - 1);
                const float goal_x = ap.path.empty() ? tx : ap.path[idx_last * 2];
                const float goal_y = ap.path.empty() ? ty : ap.path[idx_last * 2 + 1];
                if (need_path || (repath && (goal_x != tx || goal_y != ty))) {
                    std::vector<float> buf(2 * 4096);
                    uint32_t n = 0, need = 0;
                    const int rc = mq_nav_find_path(nav, e->world_x, e->world_y, tx, ty, buf.data(),
                                                   (uint32_t)buf.size() / 2, &n, &need);
                    if (rc == MQ_NAV_ERR_CAPACITY) {
                        buf.resize(need);
                        mq_nav_find_path(nav, e->world_x, e->world_y, tx, ty, buf.data(), need, &n, &need);
                    }
                    if (rc == MQ_NAV_OK) {
                        n = mq_nav_smooth(nav, buf.data(), n);
                        ap.path.assign(buf.begin(), buf.begin() + (size_t)n * 2);
                        ap.index = 0;
                        na->state = 1;   // Moving
                    } else {
                        ap.path.clear();
                        ap.index = 0;
                        na->state = 3;   // NoPath
                        continue;
                    }
                }
            }
            if (ap.path.empty()) continue;
            float x = e->world_x, y = e->world_y;
            const float dt = 1.0F / 60.0F;
            const int rc = mq_nav_step_towards(nav, &x, &y, ap.path.data(),
                                               (uint32_t)(ap.path.size() / 2), &ap.index,
                                               speed, dt, arrive);
            host::entity_set_pos(this, (float)id, x, y);
            e->world_x = x; e->world_y = y;
            if (rc == 1) na->state = 2;    // Arrived
        }
    }
}

/* 装配期预解析（docs/13 §8：渲染/提取只读）：把所有实体的纹理与材质引用解析进缓存。
 * 引擎在装配阶段做 IO（合法：不在模拟帧内，docs/10 D7）。 */
/* 切换场景时**必须**清空资源缓存（docs/32 §5.1 资源基准）：缓存键是组件里的**相对路径**
 *（如 khronos 每个模型的 "textures/image0.png"），而 VFS 根挂载点已换成新场景目录 ——
 * 不清会直接命中上一个场景的纹理/网格，表现为“换了场景但贴图还是上一个模型”。 */
void Engine::clear_asset_caches() {
    destroy_render_textures();   /* 先擦除 tex_cache 中的虚拟 URI/别名并释放一次，避免下方重复销毁 */
    const size_t nt = tex_cache.size(), nm = mesh_cache.size(), ne = env_tex_cache.size();
    const size_t nec = env_cube_cache.size();
    const size_t ng = gclips.size(), ns = skeletons.size(), nmo = morph_meshes.size();
    const size_t ngl = gltf_models.size();
    if (rapi && rself) {
        if (rapi->destroy_texture) {
            for (auto& [k, id] : tex_cache) if (id >= 0) rapi->destroy_texture(rself, id);
            for (auto& [k, pair] : env_tex_cache) {
                if (pair.first >= 0) rapi->destroy_texture(rself, pair.first);
                if (pair.second >= 0) rapi->destroy_texture(rself, pair.second);
            }
            for (auto& [k, pair] : env_cube_cache) {
                if (pair.first >= 0) rapi->destroy_texture(rself, pair.first);
                if (pair.second >= 0) rapi->destroy_texture(rself, pair.second);
            }
            for (auto& [k, gm] : gltf_models)
                for (int32_t t : gm.textures)
                    if (t >= 0) rapi->destroy_texture(rself, t);
        }
        if (rapi->destroy_mesh) {
            for (auto& [k, id] : mesh_cache) if (id >= 0) rapi->destroy_mesh(rself, id);
            for (auto& [k, gm] : gltf_models)
                for (const auto& pr : gm.prims)
                    if (pr.mesh >= 0) rapi->destroy_mesh(rself, pr.mesh);
        }
    }
    for (const auto& [k, id] : mesh_cache) mesh_stats.erase(id);
    for (const auto& [k, gm] : gltf_models)
        for (const auto& pr : gm.prims) mesh_stats.erase(pr.mesh);
    /* cube_mesh 跨场景保留，其 stats 也保留；其余句柄随缓存销毁同步失效。 */
    tex_cache.clear();
    material_cache.clear();
    mesh_cache.clear();
    env_tex_cache.clear();
    env_cube_cache.clear();
    gltf_models.clear();
    gltf_states.clear();
    /* plans/22 §6.5：glTF 动画/蒙皮/morph 缓存与名字表也按**相对路径/场景**失效 */
    gclips.clear();
    skeletons.clear();
    morph_meshes.clear();
    mesh_layout.clear();
    anim_pose.clear();
    morph_weight_override.clear();
    entity_morph_mesh.clear();
    entity_by_name.clear();
    skin_palettes.clear();
    gltf_warned.clear();
    gltf_frame_info = false;
    gltf_anim_info = false;
    if (nt + nm + ne + nec + ng + ns + nmo + ngl > 0)
        std::print("runtime: asset caches cleared on scene switch ({} textures, {} meshes, {} env, {} env_cube, "
                   "{} clips, {} skeletons, {} morph, {} gltf)\n",
                   nt, nm, ne, nec, ng, ns, nmo, ngl);
}

void Engine::preload_scene_assets() {
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (!d) continue;
            if (d->role & MQ_ROLE_SPRITE) {
                if (const std::string* tp = read_str(d, blob.data(), "texture"); tp && !tp->empty())
                    load_texture_ref(scoped_texture_ref(id, *tp));
                if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty()) {
                    scn::SpriteMaterial m{};
                    if (load_material(*mp, m)) {
                        material_cache[*mp] = m;
                        if (!m.texture.empty()) load_texture_ref(scoped_texture_ref(id, m.texture));
                    }
                }
            }
        }
    }
}

}  // namespace maque::rt
