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
        auto* na = component<scn::NavAgent>(*e);
        if (!na) continue;
        if (!na->has_target) { na->state = 0; continue; }
        const float tx = na->tx, ty = na->ty, speed = na->speed, arrive = na->arrive_dist;
        const bool repath = na->repath;

        SystemsRuntime::AgentPath& ap = systems.agent_paths[id];
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
    tex_size_cache.clear();
    material_cache.clear();
    mesh_cache.clear();
    mesh_bounds.clear();
    pick_cam_valid = false;
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
    std::vector<std::pair<std::string, bool>> pbr_textures;
    std::vector<std::string> mesh_paths;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        if (const auto* mr = component<render::MeshRenderer>(*e)) {
            if (!mr->mesh_path.empty() && !gltf::is_gltf_path(mr->mesh_path))
                mesh_paths.push_back(mr->mesh_path);
        }
        if (auto* sp = component<scn::Sprite>(*e)) {
            if (!sp->texture.empty()) load_texture_ref(scoped_texture_ref(id, sp->texture));
            if (!sp->material.empty()) {
                scn::SpriteMaterial m{};
                if (load_material(sp->material, m)) {
                    material_cache[sp->material] = m;
                    if (!m.texture.empty()) load_texture_ref(scoped_texture_ref(id, m.texture));
                }
            }
        }
        /* UI 图片（ui.Image）：走独立通道（create_texture2 → 可进 UI 动态合图）；
         * 装配期把贴图尺寸写回 transient 字段，没有 ui.Control 时用它当缺省大小。 */
        if (auto* im = component<ui::Image>(*e)) {
            if (!im->texture.empty()) {
                int w = 0, h = 0;
                if (load_ui_texture(scoped_texture_ref(id, im->texture), &w, &h) >= 0)
                    im->texture_size = math::Vec2{ (float)w, (float)h };
            }
        }
        /* 3D PBR：把材质里所有文件贴图收集起来，装配期并行 decode+mip（GPU 上传仍主线程）。
         * 这些槽与 extract_3d 的读取一一对应；新增槽忘加只会退回 lazy，不影响正确性。 */
        if (const auto* pm = component<asset::PbrMaterial>(*e)) {
            auto add = [&](const std::string& ref, bool srgb) {
                if (ref.empty()) return;
                if (ref.rfind("builtin:", 0) == 0 || ref.rfind("rendertexture://", 0) == 0) return;
                pbr_textures.emplace_back(scoped_texture_ref(id, ref), srgb);
            };
            add(pm->base_color_texture, true);
            add(pm->metallic_roughness_texture, false);
            add(pm->occlusion_texture, false);
            add(pm->normal_texture, false);
            add(pm->emissive_texture, true);
            add(pm->matcap_texture, true);
            add(pm->clearcoat_normal_texture, false);
            add(pm->clearcoat_texture, false);
            add(pm->clearcoat_roughness_texture, false);
            add(pm->sheen_color_texture, true);
            add(pm->sheen_roughness_texture, false);
            add(pm->iridescence_texture, false);
            add(pm->iridescence_thickness_texture, false);
            add(pm->anisotropy_texture, false);
            add(pm->specular_texture, false);
            add(pm->specular_color_texture, true);
            add(pm->transmission_texture, false);
            add(pm->volume_thickness_texture, false);
            add(pm->diffuse_transmission_texture, false);
            add(pm->diffuse_transmission_color_texture, true);
        }
    }
    if (!mesh_paths.empty()) preload_mesh_refs(std::move(mesh_paths));
    if (!pbr_textures.empty()) preload_texture_refs(std::move(pbr_textures), "pbr");
}

}  // namespace maque::rt
