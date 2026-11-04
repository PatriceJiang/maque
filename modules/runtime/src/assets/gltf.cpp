/* 资源 · glTF/GLB 运行时装配（plans/23/24；GPU 侧上传，CPU 解析在 gltf_model.cpp）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S4，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

Engine::GltfGpuModel* Engine::load_gltf_model(const std::string& path) {
    auto cached = gltf_models.find(path);
    if (cached != gltf_models.end()) return &cached->second;
    GltfGpuModel gpu;
    auto fail = [&](const char* why) -> GltfGpuModel* {
        std::print(stderr, "runtime: gltf 加载失败 '{}': {}\n", path, why);
        gpu.failed = true;
        gltf_models[path] = std::move(gpu);
        return &gltf_models[path];
    };
    std::vector<uint8_t> bytes;
    if (!read_vfs_file(path, bytes))
        return fail("读不到文件（mesh_path 相对场景目录 / VFS 根）");
    const auto start = std::chrono::steady_clock::now();
    auto readfn = [this](const std::string& p, std::vector<uint8_t>& out) {
        return read_vfs_file(p, out);
    };
    gltf::Model cpu = gltf::parse(path, bytes, readfn);
    if (cpu.failed || (cpu.primitives.empty() && cpu.lights.empty() && cpu.cameras.empty()))
        return fail(cpu.error.empty() ? "没有可导入的三角网格/灯光/相机" : cpu.error.c_str());

    /* ---- 图像解码（同一 image 只解码一次）---- */
    struct Decoded {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
    };
    std::vector<Decoded> decoded(cpu.images.size());
    auto decode_image = [&](int idx) -> Decoded* {
        if (idx < 0 || (size_t)idx >= cpu.images.size()) return nullptr;
        Decoded& d = decoded[(size_t)idx];
        if (d.ok) return &d;
        if (d.w < 0) return nullptr;
        const gltf::Image& im = cpu.images[(size_t)idx];
        if (!im.valid || im.bytes.empty()) {
            d.w = -1;
            return nullptr;
        }
        uint8_t* rgba = nullptr;
        uint32_t w = 0, h = 0;
        char err[128] = { 0 };
        if (mq_image_decode(im.bytes.data(), im.bytes.size(), &rgba, &w, &h, err, sizeof err) != 0) {
            std::print(stderr, "runtime: gltf '{}' image[{}] 解码失败（{}）\n", path, idx, err);
            d.w = -1;
            return nullptr;
        }
        d.rgba.assign(rgba, rgba + (size_t)w * (size_t)h * 4u);
        d.w = (int)w;
        d.h = (int)h;
        d.ok = true;
        mq_image_free(rgba);
        return &d;
    };

    /* ---- 纹理上传（同一 image + sampler + sRGB 组合只上传一次）---- */
    std::unordered_map<uint64_t, int32_t> tex_map;
    auto tex_for = [&](const gltf::TextureRef& ref, bool srgb) -> int32_t {
        if (!ref.present) return -1;
        Decoded* d = decode_image(ref.image);
        if (!d) return -1;
        const uint64_t key = ((uint64_t)(uint32_t)ref.image << 32) |
                             ((uint64_t)(srgb ? 1u : 0u) << 20) |
                             ((uint64_t)(ref.sampler.filter & 3) << 16) |
                             ((uint64_t)(ref.sampler.wrap_u & 3) << 8) |
                             (uint64_t)(ref.sampler.wrap_v & 3);
        auto hit = tex_map.find(key);
        if (hit != tex_map.end()) return hit->second;
        const int32_t id = upload_texture_rgba(d->rgba.data(), d->w, d->h, srgb,
                                               ref.sampler.filter, ref.sampler.wrap_u,
                                               ref.sampler.wrap_v);
        tex_map[key] = id;
        if (id >= 0) gpu.textures.push_back(id);
        if (render3d_info) {
            const std::string& label = cpu.images[(size_t)ref.image].uri;
            std::print("runtime: gltf texture image={} '{}' srgb={} -> {}\n",
                       ref.image, label, srgb ? 1 : 0, id);
        }
        return id;
    };

    /* ---- 材质（核心 PBR + KHR 扩展；默认值与 extract_3d 组件路径一致）---- */
    auto defaults = [&]() {
        mq_renderer_pbr_material mat{};
        for (int ti = 0; ti < 5; ++ti) mat.textures[ti] = -1;
        for (int ti = 0; ti < 3; ++ti) mat.textures2[ti] = -1;
        for (int ti = 0; ti < 7; ++ti) mat.textures3[ti] = -1;
        for (int ti = 0; ti < 2; ++ti) {
            mat.textures4[ti] = -1;
            mat.textures6[ti] = -1;
        }
        mat.textures5[0] = -1;
        mat.textures7[0] = -1;
        for (int ti = 0; ti < 2; ++ti) {
            mat.uv_scale[ti] = 1.0F;
            mat.uv_offset[ti] = 0.0F;
            mat.normal_uv_scale[ti] = 1.0F;
            mat.normal_uv_offset[ti] = 0.0F;
        }
        mat.shading_model = 0;
        mat.side = 2;
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
        mat.clearcoat_roughness = 0.03F;
        mat.toon_steps = 3;
        mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = mat.base_color[3] = 1.0F;
        mat.metallic = 0.0F;
        mat.roughness = 0.6F;
        mat.normal_scale = 1.0F;
        mat.occlusion_strength = 1.0F;
        mat.alpha_cutoff = 0.5F;
        return mat;
    };

    gpu.mats.reserve(cpu.materials.size());
    for (const gltf::Material& m : cpu.materials) {
        mq_renderer_pbr_material mat = defaults();
        for (int k = 0; k < 4; ++k) mat.base_color[k] = m.base_color[k];
        for (int k = 0; k < 3; ++k) mat.emissive[k] = m.emissive[k];
        mat.metallic = m.metallic;
        mat.roughness = m.roughness;
        mat.normal_scale = m.normal_scale;
        mat.occlusion_strength = m.occlusion_strength;
        mat.alpha_cutoff = m.alpha_cutoff;
        mat.alpha_mode = m.alpha_mode;
        mat.flags = m.double_sided ? 1 : 0;
        mat.side = m.double_sided ? 2 : 0;
        mat.shading_model = m.unlit ? 2 : 0;
        mat.emissive_intensity = m.emissive_strength;
        int32_t uv1_mask = 0;
        const int32_t t_base = tex_for(m.base_color_tex, true);
        const int32_t t_mr = tex_for(m.metallic_roughness_tex, false);
        const int32_t t_ao = tex_for(m.occlusion_tex, false);
        const int32_t t_nrm = tex_for(m.normal_tex, false);
        const int32_t t_em = tex_for(m.emissive_tex, true);
        if (t_base >= 0) mat.textures[0] = t_base;
        if (t_mr >= 0) mat.textures[1] = t_mr;
        if (t_ao >= 0) mat.textures[2] = t_ao;
        if (t_nrm >= 0) mat.textures[3] = t_nrm;
        if (t_em >= 0) mat.textures[4] = t_em;
        if (m.base_color_tex.present && m.base_color_tex.texcoord > 0) uv1_mask |= 1 << 0;
        if (m.metallic_roughness_tex.present && m.metallic_roughness_tex.texcoord > 0) uv1_mask |= 1 << 1;
        if (m.occlusion_tex.present && m.occlusion_tex.texcoord > 0) uv1_mask |= 1 << 2;
        if (m.normal_tex.present && m.normal_tex.texcoord > 0) uv1_mask |= 1 << 3;
        if (m.emissive_tex.present && m.emissive_tex.texcoord > 0) uv1_mask |= 1 << 4;
        mat.flags2 |= (uv1_mask & 0xFF) << 4;
        if (m.base_color_tex.has_transform) {
            for (int k = 0; k < 2; ++k) {
                mat.uv_scale[k] = m.base_color_tex.scale[k];
                mat.uv_offset[k] = m.base_color_tex.offset[k];
            }
            mat.uv_rotation = m.base_color_tex.rotation;
        }
        if (m.normal_tex.has_transform) {
            for (int k = 0; k < 2; ++k) {
                mat.normal_uv_scale[k] = m.normal_tex.scale[k];
                mat.normal_uv_offset[k] = m.normal_tex.offset[k];
            }
            mat.normal_uv_rotation = m.normal_tex.rotation;
        }
        /* P4 KHR 材质扩展。 */
        mat.shading_model = m.unlit ? 2 : (m.physical ? 1 : 0);
        mat.opacity = m.opacity;
        mat.clearcoat = m.clearcoat;
        mat.clearcoat_roughness = m.clearcoat_roughness > 0.0F ? m.clearcoat_roughness : 0.03F;
        mat.sheen = m.sheen;
        mat.sheen_roughness = m.sheen > 0.0F ? m.sheen_roughness : 0.5F;
        for (int k = 0; k < 3; ++k) mat.sheen_color[k] = m.sheen_color[k];
        mat.iridescence = m.iridescence;
        mat.iridescence_ior = m.iridescence_ior;
        mat.iridescence_thickness_min = m.iridescence_thickness_min;
        mat.iridescence_thickness_max = m.iridescence_thickness_max;
        mat.specular_intensity = m.specular_intensity;
        for (int k = 0; k < 3; ++k) mat.specular[k] = m.specular_color[k];
        mat.ior = m.ior;
        mat.anisotropy = m.anisotropy;
        mat.anisotropy_rotation = m.anisotropy_rotation;
        mat.volume_thickness = m.volume_thickness;
        mat.attenuation_distance = m.attenuation_distance;
        for (int k = 0; k < 3; ++k) {
            mat.attenuation_color[k] = m.attenuation_color[k];
            mat.diffuse_transmission_color[k] = m.diffuse_transmission_color[k];
            mat.multiscatter_color[k] = m.multiscatter_color[k];
        }
        mat.dispersion = m.dispersion;
        if (m.alpha_mask_in_blend) mat.flags2 |= 0x80000;
        if (m.transmission > 0.001F) {
            mat.anisotropy_rotation = m.transmission;
            mat.flags2 |= 0x100000;
        }
        if (m.diffuse_transmission > 0.001F) {
            mat.diffuse_transmission_factor = m.diffuse_transmission;
            mat.flags2 |= 0x1000000;
        }
        auto tex3 = [&](const gltf::TextureRef& ref, int slot, bool srgb) {
            const int tid = tex_for(ref, srgb);
            if (tid >= 0) {
                mat.textures3[slot] = tid;
                mat.flags2 |= 1 << (12 + slot);
            }
        };
        tex3(m.clearcoat_tex, 0, false);
        tex3(m.clearcoat_roughness_tex, 1, false);
        tex3(m.sheen_color_tex, 2, true);
        tex3(m.sheen_roughness_tex, 3, false);
        tex3(m.iridescence_tex, 4, false);
        tex3(m.iridescence_thickness_tex, 5, false);
        tex3(m.anisotropy_tex, 6, false);
        {
            const int tid = tex_for(m.clearcoat_normal_tex, false);
            if (tid >= 0) { mat.textures2[2] = tid; mat.flags2 |= 8; }
        }
        {
            const int tid = tex_for(m.specular_tex, false);
            if (tid >= 0) { mat.textures4[0] = tid; mat.flags2 |= 1 << 21; }
            const int tid2 = tex_for(m.specular_color_tex, true);
            if (tid2 >= 0) { mat.textures4[1] = tid2; mat.flags2 |= 1 << 22; }
        }
        {
            const int tid = tex_for(m.transmission_tex, false);
            if (tid >= 0) { mat.textures5[0] = tid; mat.flags2 |= 0x800000; }
        }
        {
            const int tid = tex_for(m.diffuse_transmission_tex, false);
            if (tid >= 0) { mat.textures6[0] = tid; mat.flags2 |= 1 << 25; }
            const int tid2 = tex_for(m.diffuse_transmission_color_tex, true);
            if (tid2 >= 0) { mat.textures6[1] = tid2; mat.flags2 |= 1 << 26; }
        }
        {
            const int tid = tex_for(m.volume_thickness_tex, false);
            if (tid >= 0) { mat.textures7[0] = tid; mat.flags2 |= 0x8000000; }
        }
        if (render3d_info)
            std::print("runtime: gltf material[{}] '{}' shading={} clearcoat={:.3f} sheen={:.3f} "
                       "iridescence={:.3f} specular={:.3f} ior={:.3f} anisotropy={:.3f} "
                       "transmission={:.3f} volume={:.3f} diffuse_transmission={:.3f} tex3={} "
                       "tex4={}/{} tex5={} tex6={}/{} tex7={}\n",
                       gpu.mats.size(), m.name, mat.shading_model, (double)mat.clearcoat,
                       (double)mat.sheen, (double)mat.iridescence, (double)mat.specular_intensity,
                       (double)mat.ior, (double)mat.anisotropy, (double)m.transmission,
                       (double)mat.volume_thickness, (double)mat.diffuse_transmission_factor,
                       mat.textures3[0] >= 0 || mat.textures3[1] >= 0 || mat.textures3[2] >= 0 ||
                               mat.textures3[3] >= 0 || mat.textures3[4] >= 0 ||
                               mat.textures3[5] >= 0 || mat.textures3[6] >= 0,
                       mat.textures4[0], mat.textures4[1], mat.textures5[0], mat.textures6[0],
                       mat.textures6[1], mat.textures7[0]);
        gpu.mats.push_back(mat);
    }

    /* ---- 节点 / 蒙皮 / 动画 / 灯光 / 相机 ---- */
    gpu.nodes.reserve(cpu.nodes.size());
    for (const gltf::Node& n : cpu.nodes) {
        GltfGpuModel::Node out;
        out.parent = n.parent;
        for (int k = 0; k < 3; ++k) { out.t[k] = n.translation[k]; out.s[k] = n.scale[k]; }
        for (int k = 0; k < 4; ++k) out.r[k] = n.rotation[k];
        out.has_matrix = n.has_matrix;
        std::memcpy(out.m, n.matrix, sizeof out.m);
        gpu.nodes.push_back(out);
    }
    gpu.order = cpu.node_order;
    gpu.skins.reserve(cpu.skins.size());
    for (const gltf::Skin& s : cpu.skins) {
        GltfGpuModel::Skin out;
        out.joints = s.joints;
        out.inverse_bind = s.inverse_bind;
        gpu.skins.push_back(std::move(out));
    }
    gpu.anims.reserve(cpu.animations.size());
    for (const gltf::Animation& a : cpu.animations) {
        GltfGpuModel::Anim out;
        out.name = a.name;
        out.duration = a.duration;
        out.channels.reserve(a.channels.size());
        for (const gltf::AnimationChannel& c : a.channels) {
            GltfGpuModel::AnimChannel oc;
            oc.node = c.node;
            oc.path = c.path;
            oc.interp = c.interp;
            oc.comps = c.comps;
            oc.pointer_material = c.pointer_material;
            oc.pointer_field = c.pointer_field;
            oc.times = c.times;
            oc.values = c.values;
            out.channels.push_back(std::move(oc));
        }
        gpu.anims.push_back(std::move(out));
    }
    gpu.lights.reserve(cpu.lights.size());
    for (const gltf::Light& L : cpu.lights) {
        GltfGpuModel::Light out;
        out.node = L.node;
        out.type = L.type;
        for (int k = 0; k < 3; ++k) out.color[k] = L.color[k];
        out.intensity = L.intensity;
        out.range = L.range;
        const float inner = std::clamp(L.inner_angle, 0.0F, 1.5707963F);
        const float outer = std::clamp(L.outer_angle, inner, 1.5707963F);
        out.inner_cos = std::cos(inner);
        out.outer_cos = std::cos(outer);
        gpu.lights.push_back(out);
    }
    gpu.cameras.reserve(cpu.cameras.size());
    for (const gltf::Camera& C : cpu.cameras) {
        GltfGpuModel::Camera out;
        out.node = C.node;
        out.perspective = C.perspective;
        out.yfov = C.yfov;
        out.znear = C.znear;
        out.zfar = C.zfar;
        gpu.cameras.push_back(out);
    }
    gpu.variants = cpu.variants;
    {
        float radius = 0.0F;
        for (const gltf::Primitive& p : cpu.primitives) {
            for (int ci = 0; ci < 8; ++ci) {
                const float x = (ci & 1) ? p.bounds_max[0] : p.bounds_min[0];
                const float y = (ci & 2) ? p.bounds_max[1] : p.bounds_min[1];
                const float z = (ci & 4) ? p.bounds_max[2] : p.bounds_min[2];
                radius = std::max(radius, std::sqrt(x * x + y * y + z * z));
            }
        }
        gpu.radius = std::max(radius, 1e-3F);
    }
    if (render3d_info) {
        for (size_t i = 0; i < gpu.skins.size(); ++i)
            std::print("runtime: gltf skin '{}'[{}] joints={}\n",
                       path, i, (long long)gpu.skins[i].joints.size());
        for (const auto& a : gpu.anims)
            std::print("runtime: gltf anim '{}' channels={} duration={:.3f}\n",
                       a.name, (long long)a.channels.size(), (double)a.duration);
        for (size_t i = 0; i < gpu.lights.size(); ++i)
            std::print("runtime: gltf light[{}] node={} type={} intensity={:.3f} range={:.3f} "
                       "spot_cos={:.3f}/{:.3f}\n",
                       i, gpu.lights[i].node, gpu.lights[i].type,
                       (double)gpu.lights[i].intensity, (double)gpu.lights[i].range,
                       (double)gpu.lights[i].inner_cos, (double)gpu.lights[i].outer_cos);
        for (size_t i = 0; i < gpu.cameras.size(); ++i)
            std::print("runtime: gltf camera[{}] node={} perspective={} yfov={:.3f} z=[{:.3f},{:.3f}]\n",
                       i, gpu.cameras[i].node, gpu.cameras[i].perspective ? 1 : 0,
                       (double)gpu.cameras[i].yfov, (double)gpu.cameras[i].znear,
                       (double)gpu.cameras[i].zfar);
        std::print("runtime: gltf lights={} cameras={} radius={:.3f} variants={}\n",
                   (long long)gpu.lights.size(), (long long)gpu.cameras.size(),
                   (double)gpu.radius, (long long)gpu.variants.size());
    }

    /* ---- 网格上传：无蒙皮 48/56B；蒙皮再追加 joints u16x4 + weights f32x4（stride 72/80）---- */
    struct PendingMorph { size_t prim = 0; };
    std::vector<PendingMorph> pending_morph;
    gpu.prims.reserve(cpu.primitives.size());
    for (const gltf::Primitive& p : cpu.primitives) {
        if (!rapi || !rself || !rapi->create_mesh) break;
        if (p.vertices.empty() || p.indices.empty()) continue;
        const bool skinned = p.skin >= 0 && !p.joints.empty() && !p.weights.empty();
        const uint32_t row_floats = (p.vertex_layout == 1) ? 14u : 12u;
        const uint32_t base_stride = (p.vertex_layout == 1) ? 56u : 48u;
        if (p.vertices.size() % row_floats != 0) {
            std::print(stderr, "runtime: gltf '{}' primitive 顶点行宽不符（layout={}）→ 跳过\n",
                       path, p.vertex_layout);
            continue;
        }
        const uint32_t vn = (uint32_t)(p.vertices.size() / row_floats);
        const uint32_t stride = base_stride + (skinned ? 24u : 0u);
        const int layout = p.vertex_layout + (skinned ? 2 : 0);
        std::vector<uint8_t> packed;
        const void* vptr = p.vertices.data();
        uint64_t vbytes = (uint64_t)p.vertices.size() * sizeof(float);
        if (skinned) {
            packed.resize((size_t)vn * stride);
            for (uint32_t i = 0; i < vn; ++i) {
                uint8_t* dst = packed.data() + (size_t)i * stride;
                std::memcpy(dst, p.vertices.data() + (size_t)i * row_floats, base_stride);
                uint16_t* jdst = (uint16_t*)(dst + base_stride);
                const float* wdst = (const float*)(dst + base_stride + 8);
                for (int k = 0; k < 4; ++k)
                    jdst[k] = (uint16_t)p.joints[(size_t)i * 4 + (size_t)k];
                std::memcpy((void*)wdst, p.weights.data() + (size_t)i * 4, 16);
            }
            vptr = packed.data();
            vbytes = packed.size();
        }
        mq_renderer_mesh_gpu md{};
        md.vertices = vptr;
        md.vertex_bytes = vbytes;
        md.vertex_stride = stride;
        md.vertex_layout = layout;
        md.indices = p.indices.data();
        md.index_count = (uint32_t)p.indices.size();
        for (int k = 0; k < 3; ++k) {
            md.bounds_min[k] = p.bounds_min[k];
            md.bounds_max[k] = p.bounds_max[k];
        }
        int32_t mesh_id = -1;
        if (rapi->create_mesh(rself, &md, &mesh_id) != 0 || mesh_id < 0) {
            std::print(stderr, "runtime: gltf '{}' primitive 上传失败（node={}）\n", path, p.node);
            continue;
        }
        mesh_stats[mesh_id] = { vn, (uint32_t)p.indices.size() };
        GltfGpuModel::Prim pr;
        pr.mesh = mesh_id;
        pr.material = p.material;
        pr.node = p.node;
        pr.skin = skinned ? p.skin : -1;
        pr.layout = layout;
        pr.vertex_count = vn;
        pr.stride = stride;
        pr.default_weights = p.morph_weights;
        pr.applied_weights = p.morph_weights;
        pr.variant_materials = p.variant_materials;
        pr.morph_targets.reserve(p.morph_targets.size());
        for (const gltf::MorphTarget& mt : p.morph_targets) {
            GltfGpuModel::MorphTarget out;
            out.positions = mt.positions;
            out.normals = mt.normals;
            pr.morph_targets.push_back(std::move(out));
        }
        if (!pr.morph_targets.empty()) {
            pr.base.resize((size_t)vn * stride);
            std::memcpy(pr.base.data(), vptr, pr.base.size());
            pending_morph.push_back(PendingMorph{ gpu.prims.size() });
        }
        gpu.prims.push_back(std::move(pr));
    }
    if (gpu.prims.empty() && gpu.lights.empty() && gpu.cameras.empty())
        return fail("没有可上传的网格/灯光/相机");

    bool need_default = false;
    for (const auto& pr : gpu.prims)
        if (pr.material < 0 || (size_t)pr.material >= gpu.mats.size()) need_default = true;
    if (need_default) {
        gpu.mats.push_back(defaults());
        const int32_t def = (int32_t)gpu.mats.size() - 1;
        for (auto& pr : gpu.prims)
            if (pr.material < 0 || (size_t)pr.material >= gpu.mats.size()) pr.material = def;
    }
    if (morph_enabled) {
        for (const PendingMorph& pm : pending_morph) {
            GltfGpuModel::Prim& pr = gpu.prims[pm.prim];
            bool any = false;
            for (float w : pr.default_weights) if (w != 0.0F) { any = true; break; }
            if (any) (void)apply_gltf_morph(-1, gpu, pm.prim, pr.default_weights);
        }
    }

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start).count();
    if (render3d_info) {
        size_t n_morph = 0, n_targets = 0;
        for (const auto& pr : gpu.prims) {
            if (!pr.morph_targets.empty()) { ++n_morph; n_targets += pr.morph_targets.size(); }
        }
        std::print("runtime: gltf model '{}' primitives={} materials={} textures={} nodes={} "
                   "skins={} anims={} morph_prims={} morph_targets={} ({:.1f} ms)\n",
                   path, (long long)gpu.prims.size(), (long long)gpu.mats.size(),
                   (long long)gpu.textures.size(), (long long)gpu.nodes.size(),
                   (long long)gpu.skins.size(), (long long)gpu.anims.size(),
                   (long long)n_morph, (long long)n_targets, ms);
        if (n_morph > 0)
            std::print("runtime: gltf morph '{}' primitives={} targets={}\n",
                       path, (long long)n_morph, (long long)n_targets);
    }
    gltf_models[path] = std::move(gpu);
    return &gltf_models[path];
}

/* ---- glTF 片段 / 蒙皮 / morph 解析与采样（plans/22 §6）----
 * 资产是派生文本 JSON（.skel.json / .clip.json），按 VFS 路径缓存；失败也缓存（failed=true），
 * 成功/失败都只告警一次。装配期预解析（setup_gltf_assets）→ 模拟帧内只读缓存。 */

/* 读组件里的 string[] 字段（反射 container 算子；用于 scn.AnimationPlayer.clips）。 */
static std::vector<std::string> read_string_array(const mq_field_desc* f, void* field) {
    std::vector<std::string> out;
    if (!f || f->kind != MQ_TK_ARRAY_DYN || !f->container_size || !f->container_elem) return out;
    const uint32_t n = f->container_size(field);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const std::string* s = (const std::string*)f->container_elem(field, i);
        if (s && !s->empty()) out.push_back(*s);
    }
    return out;
}

}  // namespace maque::rt
