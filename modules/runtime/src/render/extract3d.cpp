/* 3D 提取（组件 → mq_renderer_frame3d；**只读**，不回写模拟，ADR-0005）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S6，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

void Engine::extract_3d(int32_t tw, int32_t th,
                        std::vector<mq_renderer_view3d>& out_views,
                        std::vector<mq_renderer_light>& out_lights,
                        std::vector<mq_renderer_pbr_material>& out_mats,
                        std::vector<mq_renderer_draw3d>& out_draws,
                        std::vector<mq_renderer_skin>& out_skins,
                        int camera_entity, bool allow_orbit) {
    out_views.clear(); out_lights.clear(); out_mats.clear(); out_draws.clear();
    out_skins.clear();
    skin_palettes.clear();                       /* 上一帧调色板已随上一帧渲染用完 */
    if (!gltf_env_read) read_gltf_env();
    if (!scene || tw <= 0 || th <= 0) return;
    /* plans/22 §6.3：每帧先由节点树 + 片段派生姿态算世界矩阵（extract 只读派生数据）。
     * 纯 shader-pass 场景（has_3d=false）没有 3D 组件，不必付这次遍历。 */
    if (has_3d) scene->update_world3d(*this);
    /* 实体 id 排序：unordered_map 迭代顺序不可依赖（确定性） */
    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 材质：M3 从**同节点的 asset.PbrMaterial 组件**读取（M4 换成共享 .material.json 资产）；
     * 无材质组件的节点用默认灰（金属度 0 / 粗糙度 0.6）。 */
    math::Transform3D cam_trs{};
    cam_trs.position = math::Vec3{ 0.0F, 1.6F, 5.0F };
    cam_trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
    float cam_wm[16];                            /* 相机世界矩阵（plans/22：由 world_m 派生 view） */
    m4_identity(cam_wm);
    bool has_cam_wm = false;
    bool has_cam = false;
    bool gltf_lights_added = false;
    bool gltf_light_logged = false;
    float cam_priority = -1.0e9F;
    float fov_y = 60.0F, ortho_height = 2.0F, near_z = 0.1F, far_z = 100.0F, exposure = 1.0F;
    int32_t projection = 0;
    int32_t clear_mode = 1;
    int32_t camera_mask = -1;
    math::Rect camera_viewport{ 0.0F, 0.0F, 1.0F, 1.0F };
    math::Color clear{ 0.06F, 0.06F, 0.09F, 1.0F };

    for (int idx : ids) {
        auto it = scene->nodes.find(idx);
        if (it == scene->nodes.end()) continue;
        const float* wm = it->second.world_m;    /* update_world3d 的派生结果（列主序） */
        const mq_type_desc* dt = nullptr;
        const mq_type_desc* dcam = nullptr;
        const mq_type_desc* dmesh = nullptr;
        const mq_type_desc* dlight = nullptr;
        auto* tb = comp_by_name(*this, idx, "scene.Transform3D", &dt);
        math::Transform3D trs{};
        trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
        if (auto* t3 = tb ? as_component<scene::Transform3D>(dt, tb->data()) : nullptr)
            trs = t3->local;
        auto* cb = comp_by_name(*this, idx, "render.Camera3D", &dcam);
        const bool rt_camera = comp_by_name(*this, idx, "render.RenderTexture", nullptr) != nullptr;
        if (auto* cam = (cb && dcam) ? as_component<render::Camera3D>(dcam, cb->data()) : nullptr;
            cam && (camera_entity < 0 || idx == camera_entity) &&
            !(camera_entity < 0 && rt_camera)) {
            if (cam->active && node_active_effective(*this, idx) &&
                (!has_cam || cam->priority > cam_priority)) {
                has_cam = true;
                cam_priority = cam->priority;
                cam_trs = trs;
                std::memcpy(cam_wm, wm, sizeof cam_wm);   /* 相机用世界矩阵（平级场景 == 旧 trs） */
                has_cam_wm = true;
                projection = (int32_t)cam->projection;
                fov_y = cam->fov_y;
                ortho_height = cam->ortho_height;
                near_z = cam->near;
                far_z = cam->far;
                exposure = cam->exposure;
                clear_mode = (int32_t)cam->clear_mode;
                camera_mask = (int32_t)cam->culling_mask;
                camera_viewport = cam->viewport;
                clear = cam->clear_color;
            }
        }
        auto* lb = comp_by_name(*this, idx, "render.Light", &dlight);
        if (auto* light = (lb && dlight) ? as_component<render::Light>(dlight, lb->data()) : nullptr) {
            /* M5b：Directional / Point / Spot 全类型（后两种用节点世界位置；方向 = 节点 -Z 轴） */
            mq_renderer_light L{};
            L.type = (int)light->type;
            L.color[0] = light->color.r; L.color[1] = light->color.g; L.color[2] = light->color.b;
            L.intensity = light->intensity;
            L.range = light->range;
            constexpr float kDeg = 3.14159265F / 180.0F;
            L.inner_cos = std::cos(light->inner_angle * kDeg);
            L.outer_cos = std::cos(light->outer_angle * kDeg);
            /* plans/22 §6.3：位置/朝向取节点世界矩阵（平级场景与旧 trs 逐字节一致） */
            float az[3];
            m4_axis(wm, 2, az);
            L.pos[0] = wm[12]; L.pos[1] = wm[13]; L.pos[2] = wm[14];
            L.dir[0] = -az[0]; L.dir[1] = -az[1]; L.dir[2] = -az[2];
            out_lights.push_back(L);
        }
        auto* mb = comp_by_name(*this, idx, "render.MeshRenderer", &dmesh);
        const auto* mesh = (mb && dmesh) ? as_component<render::MeshRenderer>(dmesh, mb->data()) : nullptr;
        const std::string* gmp = mesh ? &mesh->mesh_path : nullptr;
        if (mesh && gmp && !gmp->empty() && gltf::is_gltf_path(*gmp)) {
            if (!mesh->visible) continue;
            GltfGpuModel* gm = load_gltf_model(*gmp);
            if (gm && !gm->failed) {
                gltf_state_ensure(idx, *gmp, *gm);
                GltfEntityState& gst = gltf_states[idx];
                /* plans/24 P6：KHR_materials_variants 选择（组件 variant 名称）。 */
                int selected_variant = -1;
                if (!gm->variants.empty()) {
                    const std::string* vn = &mesh->variant;
                    if (vn && !vn->empty()) {
                        for (size_t vi = 0; vi < gm->variants.size(); ++vi)
                            if (gm->variants[vi] == *vn) { selected_variant = (int)vi; break; }
                    }
                }
                /* plans/24 P5：KHR_lights_punctual → frame3d 灯光（按模型尺度归一化）。 */
                for (const GltfGpuModel::Light& gl : gm->lights) {
                    if (gl.node < 0) continue;
                    const size_t nw = (size_t)gl.node * 16u;
                    if (nw + 16u > gst.node_world.size()) continue;
                    float lm[16];
                    m4_mul(wm, &gst.node_world[nw], lm);
                    mq_renderer_light L{};
                    L.type = gl.type;
                    L.pos[0] = lm[12]; L.pos[1] = lm[13]; L.pos[2] = lm[14];
                    float laz[3];
                    m4_axis(lm, 2, laz);
                    L.dir[0] = -laz[0]; L.dir[1] = -laz[1]; L.dir[2] = -laz[2];
                    for (int k = 0; k < 3; ++k) L.color[k] = gl.color[k];
                    if (gl.type == 0) {
                        L.intensity = std::clamp(2.6F * gl.intensity, 0.0F, 20.0F);
                    } else {
                        const float dref = std::max(0.5F * gm->radius, 1e-3F);
                        L.intensity = std::clamp(2.6F * (dref * dref + 1.0F) * gl.intensity,
                                                 0.0F, 2000.0F);
                    }
                    L.range = gl.type == 0 ? 0.0F : gl.range;
                    L.inner_cos = gl.inner_cos;
                    L.outer_cos = gl.outer_cos;
                    out_lights.push_back(L);
                    gltf_lights_added = true;
                    if (render3d_info && !gltf_light_logged) {
                        gltf_light_logged = true;
                        std::print("runtime: gltf light on entity={} node={} type={} intensity={:.3f}\n",
                                   idx, gl.node, gl.type, (double)L.intensity);
                    }
                }
                if (!has_cam && !has_cam_wm && !gm->cameras.empty()) {
                    for (const auto& gc : gm->cameras) {
                        if (!gc.perspective || gc.node < 0) continue;
                        const size_t nw = (size_t)gc.node * 16u;
                        if (nw + 16u > gst.node_world.size()) continue;
                        m4_mul(wm, &gst.node_world[nw], cam_wm);
                        has_cam_wm = true;
                        fov_y = gc.yfov * 57.2957795F;
                        near_z = std::max(gc.znear, 1e-4F);
                        far_z = std::max(gc.zfar, near_z + 1.0F);
                        if (render3d_info)
                            std::print("runtime: gltf camera entity={} node={} fov={:.2f} near={:.3f} far={:.3f}\n",
                                       idx, gc.node, (double)fov_y, (double)near_z, (double)far_z);
                        break;
                    }
                }
                for (size_t pi = 0; pi < gm->prims.size(); ++pi) {
                    const GltfGpuModel::Prim& gp = gm->prims[pi];
                    int mat_index = gp.material;
                    if (selected_variant >= 0 &&
                        (size_t)selected_variant < gp.variant_materials.size() &&
                        gp.variant_materials[(size_t)selected_variant] >= 0)
                        mat_index = gp.variant_materials[(size_t)selected_variant];
                    if (gp.mesh < 0 || mat_index < 0 || (size_t)mat_index >= gm->mats.size())
                        continue;
                    mq_renderer_draw3d dr{};
                    dr.mesh = gp.mesh;
                    dr.material = (int32_t)out_mats.size();
                    out_mats.push_back(gm->mats[(size_t)mat_index]);
                    dr.skin = -1;
                    int skin_index = -1;
                    const bool skinned = gp.skin >= 0 && (size_t)gp.skin < gst.palettes.size() &&
                                         !gst.palettes[(size_t)gp.skin].empty();
                    if (skinned) {
                        const auto& pal = gst.palettes[(size_t)gp.skin];
                        skin_palettes.push_back(pal);
                        mq_renderer_skin rs{};
                        rs.palette = skin_palettes.back().data();
                        rs.joint_count = (uint32_t)(pal.size() / 16);
                        out_skins.push_back(rs);
                        skin_index = (int32_t)out_skins.size() - 1;
                        std::memcpy(dr.model, wm, sizeof dr.model);
                    } else if (gp.node >= 0 && (size_t)gp.node * 16u + 16u <= gst.node_world.size()) {
                        m4_mul(wm, &gst.node_world[(size_t)gp.node * 16u], dr.model);
                    } else {
                        std::memcpy(dr.model, wm, sizeof dr.model);
                    }
                    dr.skin = skin_index;
                    dr.flags |= 0x40000000;            /* draw.layer 是有效 culling mask（旧调用方无此位） */
                    dr.layer = -1;
                    if (const auto* m = as_component<render::MeshRenderer>(dmesh, mb->data()))
                        dr.layer = (int32_t)m->culling_mask;
                    dr.sort_key = (uint64_t)idx;
                    out_draws.push_back(dr);
                }
            }
            continue;
        }
        if (mb && dmesh && cube_mesh >= 0) {
            if (mesh && !mesh->visible) continue;
            /* M3 材质：同节点的 asset.PbrMaterial 组件（M4 起由共享 .material.json 资产引用） */
            mq_renderer_pbr_material mat{};
            for (int ti = 0; ti < 5; ++ti) mat.textures[ti] = -1;   /* -1 = 未给贴图 → 渲染器用 1×1 白 */
            for (int ti = 0; ti < 3; ++ti) mat.textures2[ti] = -1;  /* matcap / gradient / clearcoat-normal */
            for (int ti = 0; ti < 7; ++ti) mat.textures3[ti] = -1;  /* KHR 扩展 7 槽（clearcoat/sheen/iridescence/anisotropy） */
            for (int ti = 0; ti < 2; ++ti) mat.textures4[ti] = -1;  /* KHR_materials_specular 2 槽（factor/color） */
            mat.textures5[0] = -1;                                  /* KHR_materials_transmission 贴图槽（R=透射率掩码） */
            for (int ti = 0; ti < 2; ++ti) mat.textures6[ti] = -1;  /* diffuse transmission（A 乘子 / RGB sRGB 色） */
            mat.textures7[0] = -1;                                  /* KHR_materials_volume thicknessTexture.G */
            for (int ti = 0; ti < 2; ++ti) {
                mat.uv_scale[ti] = 1.0F; mat.uv_offset[ti] = 0.0F;       /* KHR_texture_transform 缺省 */
                mat.normal_uv_scale[ti] = 1.0F; mat.normal_uv_offset[ti] = 0.0F;
            }
            mat.shading_model = 0;                                  /* Standard（与旧 PbrMaterial 语义一致） */
            mat.side = 2;                                           /* Double：兼容旧 cullMode=None */
            mat.opacity = 1.0F;
            mat.emissive_intensity = 1.0F;
            mat.env_intensity = 1.0F;
            mat.normal_scale_y = 1.0F;
            mat.shininess = 30.0F;
            mat.specular_intensity = 1.0F;
            mat.ior = 1.5F;
            mat.specular[0] = mat.specular[1] = mat.specular[2] = 1.0F;
            mat.sheen_roughness = 0.5F;
            mat.iridescence_ior = 1.3F;
            mat.iridescence_thickness_min = 100.0F;
            mat.iridescence_thickness_max = 400.0F;
            mat.clearcoat_normal_scale = 1.0F;
            mat.toon_steps = 3;
            mat.depth_packing = 0;
            mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = mat.base_color[3] = 1.0F;
            mat.metallic = 0.0F;
            mat.roughness = 0.6F;
            mat.normal_scale = 1.0F;
            mat.occlusion_strength = 1.0F;
            const mq_type_desc* dmat = nullptr;
            auto* matb = comp_by_name(*this, idx, "asset.PbrMaterial", &dmat);
            if (const auto* pm = (matb && dmat)
                    ? as_component<asset::PbrMaterial>(dmat, matb->data()) : nullptr) {
                mat.base_color[0] = pm->base_color.r; mat.base_color[1] = pm->base_color.g;
                mat.base_color[2] = pm->base_color.b; mat.base_color[3] = pm->base_color.a;
                mat.emissive[0] = pm->emissive.x; mat.emissive[1] = pm->emissive.y;
                mat.emissive[2] = pm->emissive.z;
                mat.metallic = pm->metallic;
                mat.roughness = pm->roughness;
                mat.normal_scale = pm->normal_scale;
                mat.occlusion_strength = pm->occlusion_strength;
                mat.alpha_cutoff = pm->alpha_cutoff;
                mat.alpha_mode = (int32_t)pm->alpha_mode;
    /* transmission/diffuse 近似把 MASK 改成 BLEND 时保留 alpha 镂空（shader 用 flags2 bit19） */
    if (pm->alpha_mask_in_blend) mat.flags2 |= 0x80000;

                mat.flags = pm->double_sided ? 1 : 0;
                mat.clearcoat = pm->clearcoat;
                mat.clearcoat_roughness = pm->clearcoat_roughness;
                /* plans/19：多着色模型参数（缺省 = Standard 观感，旧场景零变化） */
                mat.shading_model = (int32_t)pm->shading_model;
                mat.side = (int32_t)pm->side;
                mat.opacity = pm->opacity;
                mat.emissive_intensity = pm->emissive_intensity;
                mat.env_intensity = pm->env_intensity;
                mat.normal_scale_y = pm->normal_scale_y;
                mat.specular[0] = pm->specular.x; mat.specular[1] = pm->specular.y;
                mat.specular[2] = pm->specular.z;
                mat.shininess = pm->shininess;
                mat.specular_intensity = pm->specular_intensity;
                mat.ior = pm->ior;
                mat.sheen = pm->sheen;
                mat.sheen_roughness = pm->sheen_roughness;
                mat.sheen_color[0] = pm->sheen_color.x; mat.sheen_color[1] = pm->sheen_color.y;
                mat.sheen_color[2] = pm->sheen_color.z;
                mat.iridescence = pm->iridescence;
                mat.iridescence_ior = pm->iridescence_ior;
                mat.iridescence_thickness_min = pm->iridescence_thickness_min;
                mat.iridescence_thickness_max = pm->iridescence_thickness_max;
                mat.anisotropy = pm->anisotropy;
                mat.anisotropy_rotation = pm->anisotropy_rotation;
                /* KHR_materials_transmission 近似参数：shader 用 Fresnel 视角 alpha、gfx 画两遍（背面→正面）。
                 * Draw 的 anisotropy_rotation 槽 shader 未使用，复用它传 transmission factor（避免扩 uniform）。
                 * ⚠️ 必须写在上面读取材质自身 anisotropy_rotation **之后**：否则会被无条件覆盖为 0，
                 *    shader 收到的 Fresnel 权重恒 0（玻璃退化为均匀半透"幽灵"，CompareDispersion 即此坑）。 */
                const float transmission_factor = pm->transmission_factor;
                if (transmission_factor > 0.001F) {
                    mat.anisotropy_rotation = transmission_factor;
                    mat.flags2 |= 0x100000;             /* bit20 = transmission approx */
                }
                /* KHR_materials_volume / KHR_materials_dispersion：折射厚度与色散近似参数
                 * （gfx 打包进 Draw.clearcoat.z / Draw.alpha.w，材质 uniform 仍是 288B 不扩）。
                 * 缺省 0 = 无体积折射/色散（旧材质零行为变化）。 */
                mat.volume_thickness = pm->volume_thickness;
                mat.dispersion = pm->dispersion;
                /* KHR_materials_volume：thicknessFactor/thicknessTexture 给的是 **mesh 空间** 厚度，
                 * 规范要求随节点变换缩放；attenuationDistance 是**世界空间**。gfx/shader 把它当世界
                 * 空间厚度用（Beer-Lambert 路径长 + 折射偏移），这里先乘节点世界缩放（非均匀用几何均值
                 * 近似路径长）。漏乘时小尺度资产（USDShaderBall 节点 scale=0.01）光学厚度 ×100 → 发黑。 */
                if (mat.volume_thickness > 0.0F) {
                    const float vscale = std::cbrt(std::abs(trs.scale.x * trs.scale.y * trs.scale.z));
                    if (vscale > 1.0e-6F) mat.volume_thickness *= vscale;
                }
                /* KHR_materials_diffuse_transmission：**不透明**薄壁透光（不再 alpha BLEND，
                 * 否则陶瓷/叶片会变幽灵玻璃）。factor → shader 背光透射项；flags2 bit24 标记。 */
                const float diffuse_transmission_factor = pm->diffuse_transmission_factor;
                if (diffuse_transmission_factor > 0.001F) {
                    mat.diffuse_transmission_factor = diffuse_transmission_factor;
                    mat.diffuse_transmission_color[0] = pm->diffuse_transmission_color.x;
                    mat.diffuse_transmission_color[1] = pm->diffuse_transmission_color.y;
                    mat.diffuse_transmission_color[2] = pm->diffuse_transmission_color.z;
                    mat.flags2 |= 0x1000000;            /* bit24 = diffuse transmission approx */
                }
                /* KHR_materials_volume（第 24 轮）：Beer-Lambert 吸收（attenuationColor/Distance）+
                 * thicknessTexture.G（厚度乘子）+ volume_scatter 的 multiscatterColorFactor。 */
                mat.attenuation_color[0] = pm->attenuation_color.x;
                mat.attenuation_color[1] = pm->attenuation_color.y;
                mat.attenuation_color[2] = pm->attenuation_color.z;
                mat.attenuation_distance = pm->attenuation_distance;
                mat.multiscatter_color[0] = pm->multiscatter_color.x;
                mat.multiscatter_color[1] = pm->multiscatter_color.y;
                mat.multiscatter_color[2] = pm->multiscatter_color.z;
                mat.clearcoat_normal_scale = pm->clearcoat_normal_scale;
                mat.toon_steps = pm->toon_steps;
                mat.depth_packing = (int32_t)pm->depth_packing;
                if (pm->flat_shading) mat.flags2 |= 1;
                /* KHR_texture_transform（plans/19）：基础贴图用 uv_scale/uv_offset，法线单独一组 */
                mat.uv_scale[0] = pm->uv_scale.x; mat.uv_scale[1] = pm->uv_scale.y;
                mat.uv_offset[0] = pm->uv_offset.x; mat.uv_offset[1] = pm->uv_offset.y;
                mat.normal_uv_scale[0] = pm->normal_uv_scale.x;
                mat.normal_uv_scale[1] = pm->normal_uv_scale.y;
                mat.normal_uv_offset[0] = pm->normal_uv_offset.x;
                mat.normal_uv_offset[1] = pm->normal_uv_offset.y;
                /* glTF TEXCOORD_1（多 UV 集）：uv1_mask bit0..7 = 8 个**基础**贴图槽是否用第二套 UV；
                 * 折进 flags2 bit4..11（uv1_sel），bit12..18 留给 KHR 扩展贴图 presence（见下），
                 * 故这里只取低 8 位（0xFF）——用 0x1FF 会占用 bit12。
                 * 只有 vertex_layout=1 的网格才有有效 uv1；layout0 的 vs 写 uv1=0，mask=0 时无影响。 */
                {
                    const int32_t uv1_mask = (int32_t)pm->uv1_mask;
                    mat.flags2 |= (uv1_mask & 0xFF) << 4;
                }
                /* M4：PBR 贴图槽（albedo / metallic-roughness / occlusion）；normal 在 M5（需要切线空间） */
                if (const std::string* texref = &pm->base_color_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[0] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr albedo texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->metallic_roughness_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[1] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr mr texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->occlusion_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[2] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr ao texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->normal_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[3] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr normal texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->emissive_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) mat.textures[4] = tid;
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr emissive texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* plans/19：matcap / toon 渐变 / clearcoat 法线三个新贴图槽（flags2 位告知 shader 是否有图） */
                /* KHR_texture_transform.rotation（弧度；glTF T*R*S 顺序，shader 里先缩放再旋转再平移） */
    mat.uv_rotation = pm->uv_rotation;
    mat.normal_uv_rotation = pm->normal_uv_rotation;
    if (const std::string* texref = &pm->matcap_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_srgb(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[0] = tid; mat.flags2 |= 2; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: matcap texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->gradient_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_ref(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[1] = tid; mat.flags2 |= 4; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: toon gradient texture '{}' -> {}\n", *texref, tid);
                    }
                }
                if (const std::string* texref = &pm->clearcoat_normal_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures2[2] = tid; mat.flags2 |= 8; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: clearcoat normal texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* KHR 扩展材质贴图槽（plans/20）：7 槽 → mat.textures3[]，存在项置 flags2 presence bit12..18。
                 * 槽序（与 gfx binding 10..16 / shader 一致）：
                 *   0 clearcoat(R)          1 clearcoat_roughness(G)  2 sheen_color(RGB, sRGB)
                 *   3 sheen_roughness(A)    4 iridescence(R)          5 iridescence_thickness(G)
                 *   6 anisotropy(RG=方向, B=强度)
                 * 除 sheen_color 是颜色贴图（sRGB）外，其余都是线性数据贴图（G/A/R 通道语义由 shader 解释）。
                 * 缺失/空 → textures3 保持 -1、presence=0：shader 走旧路径（旧材质零行为变化）。 */
                {
                    auto load_ext_tex = [&](const std::string& texref, int slot, bool srgb, const char* name) {
                        if (texref.empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, texref)) : load_texture_mips(scoped_texture_ref(idx, texref));
                        if (tid >= 0) {
                            mat.textures3[slot] = tid;
                            mat.flags2 |= 1 << (12 + slot);          /* presence：bit12..18 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr ext texture {} '{}' -> {}\n", name, texref, tid);
                    };
                    load_ext_tex(pm->clearcoat_texture, 0, false, "clearcoat_texture");
                    load_ext_tex(pm->clearcoat_roughness_texture, 1, false, "clearcoat_roughness_texture");
                    load_ext_tex(pm->sheen_color_texture, 2, true, "sheen_color_texture");
                    load_ext_tex(pm->sheen_roughness_texture, 3, false, "sheen_roughness_texture");
                    load_ext_tex(pm->iridescence_texture, 4, false, "iridescence_texture");
                    load_ext_tex(pm->iridescence_thickness_texture, 5, false, "iridescence_thickness_texture");
                    load_ext_tex(pm->anisotropy_texture, 6, false, "anisotropy_texture");
                }
                /* KHR_materials_specular 两槽（flags2 bit21/22）：specularTexture.A=specularFactor（线性）、
                 * specularColorTexture.RGB=specularColorFactor（sRGB）；shader 只作用于电介质 F0。 */
                {
                    auto load_spec_tex = [&](const std::string& texref, int slot, bool srgb, const char* name) {
                        if (texref.empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, texref)) : load_texture_mips(scoped_texture_ref(idx, texref));
                        if (tid >= 0) {
                            mat.textures4[slot] = tid;
                            mat.flags2 |= 1 << (21 + slot);          /* presence：bit21/22 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr specular texture {} '{}' -> {}\n", name, texref, tid);
                    };
                    load_spec_tex(pm->specular_texture, 0, false, "specular_texture");
                    load_spec_tex(pm->specular_color_texture, 1, true, "specular_color_texture");
                }
                /* KHR_materials_transmission 贴图槽（尾部追加）：R = 透射率掩码（0=不透、1=全透）。
                 * 线性数据贴图 → load_texture_mips（不是 sRGB）；句柄放 textures5[0]，
                 * 存在时置 flags2 bit23（presence；gfx 在 group1 新 binding 绑定）。空/失败保持 -1、presence=0。 */
                if (const std::string* texref = &pm->transmission_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures5[0] = tid; mat.flags2 |= 0x800000; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: transmission texture '{}' -> {}\n", *texref, tid);
                    }
                }
                /* KHR_materials_diffuse_transmission 两槽（尾部追加）：
                 * diffuseTransmissionTexture.A = 透射率乘子（线性数据，load_texture_mips）；
                 * diffuseTransmissionColorTexture.RGB = 透射色（sRGB）。presence = flags2 bit25/26，
                 * gfx 绑定在 group1 binding 20/21；空/失败保持 -1、presence=0。 */
                {
                    auto load_dt_tex = [&](const std::string& texref, int slot, bool srgb, const char* name) {
                        if (texref.empty()) return;
                        const int tid = srgb ? load_texture_srgb(scoped_texture_ref(idx, texref)) : load_texture_mips(scoped_texture_ref(idx, texref));
                        if (tid >= 0) {
                            mat.textures6[slot] = tid;
                            mat.flags2 |= 1 << (25 + slot);          /* presence：bit25/26 */
                        }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: pbr diffuse-transmission texture {} '{}' -> {}\n",
                                       name, texref, tid);
                    };
                    load_dt_tex(pm->diffuse_transmission_texture, 0, false, "diffuse_transmission_texture");
                    load_dt_tex(pm->diffuse_transmission_color_texture, 1, true, "diffuse_transmission_color_texture");
                }
                /* KHR_materials_volume thicknessTexture.G（线性数据；flags2 bit27 presence，
                 * gfx 绑定在 group1 binding 22）。 */
                if (const std::string* texref = &pm->volume_thickness_texture) {
                    if (!texref->empty()) {
                        const int tid = load_texture_mips(scoped_texture_ref(idx, *texref));
                        if (tid >= 0) { mat.textures7[0] = tid; mat.flags2 |= 0x8000000; }
                        if (std::getenv("MAQUE_RENDER3D_INFO"))
                            std::print("runtime: volume thickness texture '{}' -> {}\n", *texref, tid);
                    }
                }
            }
            if (std::getenv("MAQUE_RENDER3D_INFO"))
                std::print("runtime: 3d material node={} model={} side={} flags2={} tex2=({},{},{})\n",
                           idx, mat.shading_model, mat.side, mat.flags2,
                           mat.textures2[0], mat.textures2[1], mat.textures2[2]);
            const int32_t mat_index = (int32_t)out_mats.size();
            out_mats.push_back(mat);
            /* M4c：mesh_path 非空时用离线导入网格（glTF → .mesh.json），否则内建 cube */
            int32_t mesh_id = cube_mesh;
            std::string mesh_path_str;
            if (const std::string* mp = mesh ? &mesh->mesh_path : nullptr) {
                if (!mp->empty()) {
                    mesh_path_str = *mp;
                    const int32_t loaded = load_mesh(*mp);
                    if (loaded >= 0) mesh_id = loaded;
                    if (std::getenv("MAQUE_RENDER3D_INFO"))
                        std::print("runtime: mesh renderer node={} path='{}' → mesh={}\n", idx, *mp, loaded);
                }
            }
            /* ---- 蒙皮（plans/22 §6.3）：调色板 palette[j] = world_m(joint) * inverse_bind[j] ----
             * 蒙皮网格忽略节点变换（glTF 规范）→ dr.model = identity、dr.skin = frame3d.skins 下标。
             * 解析失败 → 告警一次并回退静态（dr.model = world_m、skin = -1）；
             * MAQUE_SKIN=0 → 调色板填 identity（bind pose 不变量，用于 e2e A/B 对照）。 */
            int32_t skin_index = -1;
            float model_m[16];
            std::memcpy(model_m, wm, sizeof model_m);
            const std::string* skin_path = mesh ? &mesh->skin_path : nullptr;
            if (skin_path && !skin_path->empty()) {
                auto lay_it = mesh_layout.find(mesh_path_str);
                const bool skinned_ok = !mesh_path_str.empty() && lay_it != mesh_layout.end() &&
                                        lay_it->second >= 2;
                if (!skinned_ok) {
                    if (!gltf_warned.count("skin-layout:" + *skin_path)) {
                        gltf_warned.insert("skin-layout:" + *skin_path);
                        std::print(stderr, "runtime: 蒙皮网格 '{}/{}' 布局缺失或载入失败，回退静态\n",
                                   mesh_path_str, *skin_path);
                    }
                } else {
                    const bool sk_loaded = load_skeleton(*skin_path);
                    auto sit = skeletons.find(*skin_path);
                    const Skeleton* sk = (sk_loaded && sit != skeletons.end() && !sit->second.failed)
                                             ? &sit->second : nullptr;
                    const size_t nj = sk ? sk->joint_names.size() : 0;
                    const bool sk_valid = sk && nj > 0 && sk->inverse_bind.size() == nj * 16;
                    bool built = false;
                    if (sk_valid) {
                        std::vector<float> pal(nj * 16);
                        if (!skin_enabled) {
                            /* MAQUE_SKIN=0：identity 调色板（joint_count 保留）→ bind pose 不变量 */
                            for (size_t j = 0; j < nj; ++j) m4_identity(pal.data() + j * 16);
                            built = true;
                        } else if (!entity_by_name.empty()) {
                            built = true;
                            for (size_t j = 0; j < nj; ++j) {
                                auto jhit = entity_by_name.find(sk->joint_names[j]);
                                const SceneNode* je = jhit == entity_by_name.end()
                                                       ? nullptr : scene->at(jhit->second);
                                if (!je) { built = false; break; }
                                m4_mul(je->world_m, sk->inverse_bind.data() + j * 16,
                                       pal.data() + j * 16);
                            }
                        }
                        if (built) {
                            skin_palettes.push_back(std::move(pal));
                            mq_renderer_skin rs{};
                            rs.palette = skin_palettes.back().data();
                            rs.joint_count = (uint32_t)nj;
                            out_skins.push_back(rs);
                            skin_index = (int32_t)out_skins.size() - 1;
                            m4_identity(model_m);            /* 蒙皮网格忽略节点世界变换 */
                        }
                    }
                    if (!built && !gltf_warned.count("skin-joint:" + *skin_path)) {
                        gltf_warned.insert("skin-joint:" + *skin_path);
                        std::print(stderr, "runtime: 蒙皮 '{}/{}' 关节解析失败，回退静态\n",
                                   mesh_path_str, *skin_path);
                    }
                }
            }
            /* ---- CPU morph（plans/22 §6.4）：权重 = 动画覆盖 > 默认；权变才重算 + update_mesh ---- */
            if (!mesh_path_str.empty()) {
                auto mit = morph_meshes.find(mesh_path_str);
                if (mit != morph_meshes.end() && mit->second.mesh >= 0 && !mit->second.targets.empty()) {
                    MorphMesh& mm = mit->second;
                    auto oit = morph_weight_override.find(idx);
                    const std::vector<float>& mw = (oit != morph_weight_override.end())
                                                       ? oit->second : mm.default_weights;
                    if (mm.owner_entity < 0) mm.owner_entity = idx;
                    if (mm.owner_entity == idx) {
                        if (mm.applied_weights != mw) (void)apply_morph(mesh_path_str, mw);
                    } else if (!mm.warned_shared) {
                        mm.warned_shared = true;
                        std::print(stderr, "runtime: mesh '{}' 被多个实体共享，morph 只取首实体 entity={}\n",
                                   mesh_path_str, mm.owner_entity);
                    }
                }
            }
            mq_renderer_draw3d dr{};
            dr.mesh = mesh_id;
            dr.material = mat_index;
            std::memcpy(dr.model, model_m, sizeof dr.model);
            dr.skin = skin_index;
            dr.flags |= 0x40000000;            /* draw.layer 是有效 culling mask（旧调用方无此位） */
            dr.layer = -1;
            if (const auto* m = as_component<render::MeshRenderer>(dmesh, mb->data()))
                dr.layer = (int32_t)m->culling_mask;
            dr.sort_key = (uint64_t)idx;
            out_draws.push_back(dr);
        }
    }

    /* OrbitControls：仅主相机绕世界原点环绕；RenderTexture 相机保持节点自身变换。 */
    if (allow_orbit) if (const char* oo = std::getenv("MAQUE_ORBIT")) {
        float oy = 0.0F, op = 0.3F, od = 1.0F;
        if (std::sscanf(oo, "%f,%f,%f", &oy, &op, &od) >= 1) {
            orbit_yaw = oy; orbit_pitch = op; orbit_dist = od > 0.0F ? od : 1.0F;
            orbit_enabled = true; orbit_inited = true;
        }
    }
    if (allow_orbit && orbit_enabled) {
        if (!orbit_inited) {
            const float ex = cam_trs.position.x, ey = cam_trs.position.y, ez = cam_trs.position.z;
            float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (d < 1e-4F) d = 1.0F;
            orbit_dist = d;
            orbit_pitch = std::asin(std::max(-1.0F, std::min(1.0F, ey / d)));
            orbit_yaw = std::atan2(ex, ez);
            orbit_inited = true;
        }
        const float cp = std::cos(orbit_pitch), sp = std::sin(orbit_pitch);
        const float sy = std::sin(orbit_yaw), cy = std::cos(orbit_yaw);
        const float px = sy * cp * orbit_dist, py = sp * orbit_dist, pz = cy * cp * orbit_dist;
        cam_trs.position = math::Vec3{ px, py, pz };
        cam_trs.scale = math::Vec3{ 1.0F, 1.0F, 1.0F };
        const float fl = std::sqrt(px * px + py * py + pz * pz);
        if (fl > 1e-6F) {
            const float fx = -px / fl, fy = -py / fl, fz = -pz / fl;
            cam_trs.rotation = math::Vec3{ std::asin(std::max(-1.0F, std::min(1.0F, fy))),
                                           std::atan2(-fx, -fz), 0.0F };
        }
        m4_from_trs(cam_trs, cam_wm);        /* Orbit 覆盖相机：世界矩阵 = 绕世界原点环绕 */
        has_cam_wm = true;
        (void)has_cam;
    }
    if (gltf_lights_added) {
        std::stable_partition(out_lights.begin(), out_lights.end(),
                              [](const mq_renderer_light& L) { return L.type == 0; });
        if (out_lights.size() > 8) out_lights.resize(8);
    }
    opt_last_sun = false;
    if (out_lights.empty()) {                              /* 无方向光：给一盏默认主光，避免全黑 */
        /* F4 引擎选项：默认太阳光可关 / 调强度 / 调方向（仅"场景没有任何光源"这条回退路径）。 */
        if (opt_sun_enabled) {
            mq_renderer_light L{};
            L.type = 0;
            /* 方向全 0 会让 shader 的 normalize 退化 → 给一个合法方向（面板已夹，环境变量入口也兜住） */
            float dx = opt_sun_dir[0], dy = opt_sun_dir[1], dz = opt_sun_dir[2];
            if (std::fabs(dx) + std::fabs(dy) + std::fabs(dz) < 1e-4F) dz = -1.0F;
            L.dir[0] = dx; L.dir[1] = dy; L.dir[2] = dz;
            L.color[0] = L.color[1] = L.color[2] = 1.0F;
            L.intensity = opt_sun_intensity;
            out_lights.push_back(L);
            opt_last_sun = true;
        }
    }

    /* F4 引擎选项：主相机 near/far 覆盖（不写场景组件）。
     * 覆盖关闭时把**场景实际值**回填进选项字段 —— 面板里一勾选"覆盖"就等同当前画面，不跳变。 */
    if (allow_orbit && opt_cam_override) {
        near_z = std::max(opt_cam_near, 1e-4F);
        far_z = std::max(opt_cam_far, near_z + 1e-3F);
    } else if (allow_orbit) {
        opt_cam_near = near_z;
        opt_cam_far = far_z;
    }
    if (allow_orbit) {
        opt_last_near = near_z;
        opt_last_far = far_z;
    }

    mq_renderer_view3d v{};
    if (camera_entity >= 0 && !has_cam) return;       /* RenderTexture 必须由同节点的有效 Camera3D 驱动 */
    /* 纯 shader-pass / 纯 2D 场景（无 Camera3D / MeshRenderer）**不要合成默认相机 view**：
     * gfx 的 3D pass 会按 view 的 clear_color 覆盖自己的 viewport —— 对纯 shader 场景就是把
     * 全屏 `render.ShaderPass` 背景整块擦成背景色（表现为 shader 只剩画布外一圈、或整屏纯背景色）。
     * 不产出 view（view_count=0）时 gfx 直接跳过 3D pass，scene shader pass 就是整幅画面。 */
    if (camera_entity < 0 && !has_3d) return;
    /* 相机 view 由**世界矩阵**派生（plans/22 §6.3）：默认相机 / 平级场景与旧 m4_view(trs) 逐字节一致 */
    if (!has_cam_wm) m4_from_trs(cam_trs, cam_wm);
    float view[16], proj[16];
    m4_view_from_m(cam_wm, view);
    const float vx = std::clamp(camera_viewport.x, 0.0F, 1.0F);
    const float vy = std::clamp(camera_viewport.y, 0.0F, 1.0F);
    const float vw = std::clamp(camera_viewport.width, 0.0F, 1.0F - vx);
    const float vh = std::clamp(camera_viewport.height, 0.0F, 1.0F - vy);
    float root_x = 0.0F, root_y = 0.0F, root_w = (float)tw, root_h = (float)th;
    if (camera_entity < 0 && canvas.window_w > 0.0F && canvas.window_h > 0.0F) {
        const float rx = (float)tw / canvas.window_w;
        const float ry = (float)th / canvas.window_h;
        root_x = canvas.offset_x * rx;
        root_y = canvas.offset_y * ry;
        root_w = canvas.viewport_w * rx;
        root_h = canvas.viewport_h * ry;
    }
    const float pixel_w = std::max(1.0F, vw * root_w);
    const float pixel_h = std::max(1.0F, vh * root_h);
    const float aspect = pixel_w / pixel_h;
    if (projection == 1) m4_orthographic(ortho_height, aspect, near_z, far_z, proj);
    else m4_perspective(fov_y * 3.14159265F / 180.0F, aspect, near_z, far_z, proj);
    std::memcpy(v.view, view, sizeof view);
    std::memcpy(v.proj, proj, sizeof proj);
    v.eye[0] = cam_wm[12]; v.eye[1] = cam_wm[13]; v.eye[2] = cam_wm[14];
    v.viewport[0] = root_x + vx * root_w;
    v.viewport[1] = root_y + vy * root_h;
    v.viewport[2] = pixel_w;
    v.viewport[3] = pixel_h;
    v.exposure = exposure;
    v.clear_mode = clear_mode;
    v.clear_color[0] = clear.r; v.clear_color[1] = clear.g;
    v.clear_color[2] = clear.b; v.clear_color[3] = clear.a;
    v.layer_mask = camera_mask;
    out_views.push_back(v);
    /* 3D 拾取缓存（docs/19 §4）：主相机（camera_entity < 0）的 view/proj/目标像素视口。
     * 拾取在脚本 update / 控制面帧边界调用 → 用**最近一次成帧**的相机，与画面一致。 */
    if (camera_entity < 0) {
        std::memcpy(pick_view, view, sizeof view);
        std::memcpy(pick_proj, proj, sizeof proj);
        std::memcpy(pick_viewport, v.viewport, sizeof pick_viewport);
        pick_tw = tw;
        pick_th = th;
        pick_cam_valid = true;
    }
    if (render3d_info && !gltf_frame_info) {
        gltf_frame_info = true;
        std::print("runtime: 3d frame0 draws={} skins={} morph={} anim_pose={} names={} "
                   "lights={} mats={}\n",
                   (long long)out_draws.size(), (long long)out_skins.size(),
                   (long long)morph_meshes.size(), (long long)anim_pose.size(),
                   (long long)entity_by_name.size(), (long long)out_lights.size(),
                   (long long)out_mats.size());
    }
}

}  // namespace maque::rt
