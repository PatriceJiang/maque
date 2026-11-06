/* 渲染目标与相机（RenderTexture / MSAA 解析 / 离屏相机 pass / 环境填充 / 视觉哈希）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S6，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

Engine::RenderTextureState* Engine::render_texture_for(int entity) {
    for (auto& rt : render_textures) if (rt.entity == entity) return &rt;
    return nullptr;
}

const Engine::RenderTextureState* Engine::render_texture_for(int entity) const {
    for (const auto& rt : render_textures) if (rt.entity == entity) return &rt;
    return nullptr;
}

/* MSAA 样本数解析：主目标与每张相机离屏纹理走同一条链，所以"整帧开 4x"与"只有小地图开 4x"
 * 是同一套语义，不存在第二套优先级。
 *   · 更专门的开关赢：RT 自己看 opt_rt_msaa，再退到全局 opt_msaa / MAQUE_MSAA；
 *   · 显式覆盖之下，才是目标自己的字段（render.RenderTexture.msaa_samples）与场景字段；
 *   · 返回 1 或 4；gfx 侧 4x 附件创建失败会自行回退 1x（打印一次，不静默）。 */
int Engine::resolve_msaa(int target_override, bool is_render_texture) {
    if (!msaa_env_loaded) {
        msaa_env_loaded = true;
        if (const char* msaa_env = std::getenv("MAQUE_MSAA")) {
            msaa_env_set = true;
            msaa_env_value = std::atoi(msaa_env);
        }
    }
    int samples = scene ? scene->desc.msaa_samples : 0;
    if (target_override > 0) samples = target_override;
    if (msaa_env_set) samples = msaa_env_value;
    if (opt_msaa > 0) samples = opt_msaa;
    if (is_render_texture && opt_rt_msaa > 0) samples = opt_rt_msaa;
    return samples >= 4 ? 4 : 1;
}

void Engine::destroy_render_textures() {
    for (auto& rt : render_textures) {
        auto erase_key = [&](const std::string& key) {
            auto it = tex_cache.find(key);
            if (it != tex_cache.end() && it->second == rt.handle) tex_cache.erase(it);
        };
        erase_key(rt.uri);
        const int owner = entity_instance.count(rt.entity) ? entity_instance[rt.entity] : 0;
        if (owner == 0) erase_key("rendertexture://" + rt.name);
        if (rt.handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt.handle);
    }
    render_textures.clear();
    camera_captures.clear();
}

void Engine::setup_render_textures() {
    destroy_render_textures();
    sync_render_textures();
}

void Engine::sync_render_textures() {
    if (!scene || !reflect) return;
    const mq_type_desc* rtd = mq_reflect_type_by_name(reflect, "render.RenderTexture");
    if (!rtd) return;
    const bool supported = MQ_RENDERER_HAS(rapi, create_render_texture) &&
                           MQ_RENDERER_HAS(rapi, configure_render_texture) &&
                           MQ_RENDERER_HAS(rapi, render_to_texture) &&
                           MQ_RENDERER_HAS(rapi, read_texture);
    if (!supported) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::print(stderr, "runtime: renderer 不支持 RenderTexture ABI（需要 create/configure/render/read）\n");
        }
    }
    std::vector<int> ids;
    for (const auto& [id, e] : scene->nodes)
        if (e.comps.find(rtd->type_id) != e.comps.end()) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::unordered_set<int> live(ids.begin(), ids.end());

    /* 先清理已销毁节点/已移除组件，且从 tex_cache 擦除别名，避免句柄重复释放。 */
    for (size_t i = 0; i < render_textures.size();) {
        RenderTextureState& rt = render_textures[i];
        if (live.count(rt.entity)) { ++i; continue; }
        auto erase_key = [&](const std::string& key) {
            auto it = tex_cache.find(key);
            if (it != tex_cache.end() && it->second == rt.handle) tex_cache.erase(it);
        };
        erase_key(rt.uri);
        const int owner = entity_instance.count(rt.entity) ? entity_instance[rt.entity] : 0;
        if (owner == 0) erase_key("rendertexture://" + rt.name);
        if (rt.handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt.handle);
        render_textures.erase(render_textures.begin() + (ptrdiff_t)i);
    }

    std::unordered_set<std::string> used;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        ComponentSlot& slot = component_slot(*e, rtd);
        std::vector<uint8_t>& blob = slot.bytes;
        const bool has3d_camera = component<render::Camera3D>(*e) != nullptr;
        const bool has2d_camera = component<render::Camera2D>(*e) != nullptr ||
                                  component<scn::Camera>(*e) != nullptr;
        auto* rtc = as_component_mut<render::RenderTexture>(rtd, blob.data());
        if (!rtc) continue;
        std::string name = rtc->name.empty() ? e->name : rtc->name;
        if (name.empty()) name = "camera-" + std::to_string(id);
        const int owner = entity_instance.count(id) ? entity_instance[id] : 0;
        const std::string uri = "rendertexture://" + std::to_string(owner) + "/" + name;
        const bool valid_target = (has3d_camera || has2d_camera) && supported;
        const bool duplicate = valid_target && !used.insert(uri).second;
        RenderTextureState* rt = render_texture_for(id);

        auto write_transients = [&](const std::string& texture, bool ready, uint64_t rendered) {
            rtc->texture = texture;
            rtc->ready = ready;
            rtc->last_frame = (uint32_t)rendered;
        };
        if (duplicate || (!has3d_camera && !has2d_camera) || !supported) {
            if (duplicate)
                std::print(stderr, "runtime: RenderTexture '{}' 在实例 {} 内重名（entity={} 跳过）\n", name, owner, id);
            else if (!has3d_camera && !has2d_camera)
                std::print(stderr, "runtime: RenderTexture entity={} 必须与 scn.Camera/render.Camera2D/Camera3D 同节点\n", id);
            if (rt) {
                auto erase_key = [&](const std::string& key) {
                    auto it = tex_cache.find(key);
                    if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                };
                erase_key(rt->uri);
                erase_key("rendertexture://" + rt->name);
                if (rt->handle >= 0 && MQ_RENDERER_HAS(rapi, destroy_texture)) rapi->destroy_texture(rself, rt->handle);
                render_textures.erase(std::remove_if(render_textures.begin(), render_textures.end(),
                                                     [&](const RenderTextureState& s) { return s.entity == id; }),
                                      render_textures.end());
            }
            write_transients("", false, 0);
            continue;
        }

        const int32_t requested_w = (int32_t)rtc->width;
        const int32_t requested_h = (int32_t)rtc->height;
        const int32_t width = std::max(1, requested_w > 0 ? requested_w : W);
        const int32_t height = std::max(1, requested_h > 0 ? requested_h : H);
        const int32_t filter = (int32_t)rtc->filter;
        const math::Color cc = rtc->clear_color;
        mq_renderer_render_texture_desc desc{};
        desc.struct_size = sizeof desc;
        desc.width = width; desc.height = height; desc.pixel_format = 0;
        desc.filter = filter; desc.wrap_u = 1; desc.wrap_v = 1;
        desc.clear_color[0] = cc.r; desc.clear_color[1] = cc.g;
        desc.clear_color[2] = cc.b; desc.clear_color[3] = cc.a;

        if (!rt) {
            int32_t handle = -1;
            if (rapi->create_render_texture(rself, &desc, &handle) != 0 || handle < 0) {
                std::print(stderr, "runtime: RenderTexture '{}' 创建失败（{}x{}）\n", uri, width, height);
                write_transients("", false, 0);
                continue;
            }
            RenderTextureState state;
            state.entity = id; state.handle = handle; state.width = width; state.height = height;
            state.filter = filter; state.name = name; state.uri = uri; state.is_3d = has3d_camera;
            render_textures.push_back(std::move(state));
            rt = &render_textures.back();
            std::print("runtime: RenderTexture '{}' {}x{} handle={} camera={}\n",
                       uri, width, height, (long long)handle, has3d_camera ? "3d" : "2d");
        } else {
            if (rt->uri != uri) {
                auto it = tex_cache.find(rt->uri);
                if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                if (owner == 0) {
                    const std::string old_short = "rendertexture://" + rt->name;
                    it = tex_cache.find(old_short);
                    if (it != tex_cache.end() && it->second == rt->handle) tex_cache.erase(it);
                }
                rt->uri = uri; rt->name = name;
            }
            if (rt->width != width || rt->height != height || rt->filter != filter) {
                if (rapi->configure_render_texture(rself, rt->handle, &desc) == 0) {
                    rt->width = width; rt->height = height; rt->filter = filter;
                    rt->dirty = true; rt->ready = false; rt->capture_pixels.clear();
                } else {
                    std::print(stderr, "runtime: RenderTexture '{}' 重配 {}x{} 失败，保留旧目标 {}x{}\n",
                               uri, width, height, rt->width, rt->height);
                }
            }
            if (rt->is_3d != has3d_camera) rt->dirty = true;
            rt->is_3d = has3d_camera;
        }
        rt->update_mode = (int32_t)rtc->update_mode;
        /* 目标自己的 MSAA 变化不改纹理本身（多采样只是 pass 的临时附件），
         * 但要重画一次，否则 OnDemand 目标会留着旧样本数的画面。 */
        const int32_t rt_msaa = (int32_t)rtc->msaa_samples;
        if (rt->msaa_samples != rt_msaa) rt->dirty = true;
        rt->msaa_samples = rt_msaa;
        const bool include_ui = rtc->include_ui;
        const bool enabled = rtc->enabled;
        if (rt->include_ui != include_ui || (!rt->enabled && enabled)) rt->dirty = true;
        rt->include_ui = include_ui;
        rt->enabled = enabled;
        tex_cache[rt->uri] = rt->handle;
        if (owner == 0) tex_cache["rendertexture://" + rt->name] = rt->handle;
        write_transients(rt->uri, rt->ready, rt->last_frame);
    }
    std::sort(render_textures.begin(), render_textures.end(),
              [](const RenderTextureState& a, const RenderTextureState& b) { return a.entity < b.entity; });
}

void Engine::setup_3d() {
    has_3d = false;
    pick_cam_valid = false;      /* 新场景：等第一帧 extract_3d 写入相机后再允许拾取 */
    if (!scene) return;
    int n_cam = 0, n_mesh = 0, n_light = 0;
    for (auto& kv : scene->nodes) {
        SceneNode& node = kv.second;
        if (component<render::Camera3D>(node)) ++n_cam;
        if (component<render::MeshRenderer>(node)) ++n_mesh;
        if (component<render::Light>(node)) ++n_light;
    }
    if (!n_cam && !n_mesh) return;
    has_3d = true;
    if (cube_mesh < 0 && rapi && rself && rapi->create_mesh) {
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        build_cube(verts, indices);
        mq_renderer_mesh_gpu md{};
        md.vertices = verts.data();
        md.vertex_bytes = (uint64_t)verts.size() * sizeof(float);
        md.vertex_stride = 48;
        md.vertex_layout = 0;
        md.indices = indices.data();
        md.index_count = (uint32_t)indices.size();
        md.bounds_min[0] = md.bounds_min[1] = md.bounds_min[2] = -0.6F;
        md.bounds_max[0] = md.bounds_max[1] = md.bounds_max[2] = 0.6F;
        int32_t id = -1;
        if (rapi->create_mesh(rself, &md, &id) == 0) {
            cube_mesh = id;
            mesh_stats[id] = { (uint32_t)(verts.size() / 12), (uint32_t)indices.size() };
        }
    }
    std::print("runtime: 3d scene cams={} meshes={} lights={} cube_mesh={}\n",
               n_cam, n_mesh, n_light, (long long)cube_mesh);
    /* plans/22 §6.1：实体名表 / 骨架 / 片段 / morph 网格预解析（装配期 IO，不在模拟帧内） */
    setup_gltf_assets();
}

bool Engine::make_camera_view2d(int entity, int width, int height, mq_renderer_view& view,
                                uint32_t* layer_mask) {
    SceneNode* e = scene ? scene->at(entity) : nullptr;
    if (!e || !node_active_effective(*this, entity)) return false;
    if (layer_mask) *layer_mask = 0xFFFFFFFFu;
    view = mq_renderer_view{};
    view.vw = (float)width; view.vh = (float)height;
    if (const auto* cam = component<render::Camera2D>(*e)) {
        if (!cam->active) return false;
        const math::Vec2 zoom = cam->zoom;
        const math::Vec2 offset = cam->offset;
        const math::Rect viewport = cam->viewport;
        if (layer_mask) *layer_mask = cam->layer_mask;
        const float zx = std::max(zoom.x, 1e-5F);
        const float zy = std::max(zoom.y, 1e-5F);
        const float nx = std::clamp(viewport.x, 0.0F, 1.0F);
        const float ny = std::clamp(viewport.y, 0.0F, 1.0F);
        const float nw = std::clamp(viewport.width, 0.0F, 1.0F - nx);
        const float nh = std::clamp(viewport.height, 0.0F, 1.0F - ny);
        if (nw <= 0.0F || nh <= 0.0F) return false;
        view.vx = nx * width; view.vy = ny * height;
        view.vw = std::max(1.0F, nw * width); view.vh = std::max(1.0F, nh * height);
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, entity, angle, scale, opacity);
        (void)scale; (void)opacity;
        const float camera_x = e->world_x + offset.x;
        const float camera_y = (float)H - (e->world_y + offset.y);
        view.cx = camera_x;
        view.cy = camera_y;
        view.pan = 0.0F;
        view.eye = 0.0F;
        view.zoom = zx;
        view.zoom_y = zy;
        view.rot = angle * 3.14159265358979323846F / 180.0F;
        return true;
    }
    if (const auto* cam = component<scn::Camera>(*e)) {
        if (!cam->active) return false;
        view.cx = cam->x;
        view.cy = (float)H - cam->y;
        view.zoom = std::max(cam->zoom, 1e-5F);
        view.zoom_y = view.zoom;
        view.rot = cam->rotation;
        return true;
    }
    return false;
}

void Engine::fill_frame_environment(mq_renderer_frame3d& fr) {
    fr.env = mq_renderer_environment{};
    fr.env.sky_texture = -1;
    fr.env.env_texture = -1;
    fr.env.env_irradiance_texture = -1;
    fr.env.env_cubemap = -1;
    fr.env.env_cubemap_irradiance = -1;
    fr.env.sky_mode = 1;
    if (!scene) return;
    float env_col[3] = { scene->desc.env_color.r, scene->desc.env_color.g, scene->desc.env_color.b };
    float sky_col[3] = { scene->desc.env_sky_color.r, scene->desc.env_sky_color.g, scene->desc.env_sky_color.b };
    float ground_col[3] = { scene->desc.env_ground_color.r, scene->desc.env_ground_color.g, scene->desc.env_ground_color.b };
    const bool sky_zero = sky_col[0] == 0.0F && sky_col[1] == 0.0F && sky_col[2] == 0.0F;
    const bool ground_zero = ground_col[0] == 0.0F && ground_col[1] == 0.0F && ground_col[2] == 0.0F;
    for (int i = 0; i < 3; ++i) {
        if (sky_zero) sky_col[i] = env_col[i] * 1.15F;
        if (ground_zero) ground_col[i] = env_col[i] * 0.45F;
        fr.env.env_color[i] = env_col[i];
        fr.env.env_sky_color[i] = sky_col[i];
        fr.env.env_ground_color[i] = ground_col[i];
    }
    fr.env.env_color[3] = fr.env.env_sky_color[3] = fr.env.env_ground_color[3] = 1.0F;
    fr.env.intensity = scene->desc.env_intensity;
    fr.env.env_highlight = scene->desc.env_highlight;
    fr.env.env_rotation = scene->desc.env_rotation * 3.14159265F / 180.0F;

    std::string env_path = scene->desc.env_texture;
    const char* env_force = std::getenv("MAQUE_ENV_IBL");
    if (env_force && env_force[0] == '1') env_path = "builtin:/textures/studio_env";
    if (env_force && env_force[0] == '0') env_path.clear();
    if (!env_path.empty()) load_env_textures(env_path, fr.env.env_texture, fr.env.env_irradiance_texture);
    if (!scene->desc.env_cubemap.empty())
        load_env_cubemap(scene->desc.env_cubemap, fr.env.env_cubemap, fr.env.env_cubemap_irradiance);
    if (fr.env.env_cubemap >= 0) fr.env.sky_mode = scene->desc.env_background ? 2 : 1;
    else if (fr.env.env_texture >= 0 && scene->desc.env_background) fr.env.sky_mode = 3;
}

bool Engine::read_camera_texture(RenderTextureState& rt, bool refresh) {
    if (!rt.ready || rt.handle < 0 || !MQ_RENDERER_HAS(rapi, read_texture)) return false;
    if (!refresh && !rt.capture_pixels.empty()) return true;
    rt.capture_pixels.assign((size_t)rt.width * (size_t)rt.height * 4u, 0);
    const int32_t rc = rapi->read_texture(rself, rt.handle, rt.capture_pixels.data(),
                                          (uint32_t)rt.capture_pixels.size());
    if (rc != 0) { rt.capture_pixels.clear(); return false; }
    rt.capture_frame = rt.last_frame;
    return true;
}

bool Engine::save_camera_texture(int entity, const std::string& path) {
    RenderTextureState* rt = render_texture_for(entity);
    if (!rt || !rt->ready || rt->handle < 0 || path.empty() || !MQ_RENDERER_HAS(rapi, read_texture)) return false;
    std::vector<uint8_t> pixels((size_t)rt->width * (size_t)rt->height * 4u, 0);
    if (rapi->read_texture(rself, rt->handle, pixels.data(), (uint32_t)pixels.size()) != 0) return false;
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    const int rc = png ? mq_image_write_png(path.c_str(), (uint32_t)rt->width, (uint32_t)rt->height,
                                             pixels.data(), (uint32_t)rt->width * 4u)
                       : (write_ppm(path, rt->width, rt->height, pixels) ? 0 : 1);
    if (rc == 0)
        std::print("runtime: camera shot '{}' {}x{} camera={} frame={}\n",
                   path, rt->width, rt->height, entity, (unsigned long long)rt->last_frame);
    else
        std::print(stderr, "runtime: camera shot 写入失败 '{}' camera={}\n", path, entity);
    return rc == 0;
}

uint64_t Engine::compute_scene_visual_hash() const {
    if (!scene) return 0;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    /* 只做“本进程内前后帧比较”，不跨进程/跨版本 → 不需要树序：
     * 直接按 nodes/comps 的 map 顺序混入（省掉 collect_preorder + 每节点排序 + 二次 find）。 */
    for (const auto& [id, e] : scene->nodes) {
        mix(&id, sizeof id);
        mix(&e.parent, sizeof e.parent);
        mix(&e.active, sizeof e.active);
        mix(&e.scale, sizeof e.scale);
        mix(&e.angle, sizeof e.angle);
        mix(&e.opacity, sizeof e.opacity);
        mix(&e.z_index, sizeof e.z_index);
        mix(&e.world_x, sizeof e.world_x);
        mix(&e.world_y, sizeof e.world_y);
        mix(e.world_m, sizeof e.world_m);
        mix(e.name.data(), e.name.size());
        for (const auto& [tid, slot] : e.comps) {
            mix(&tid, sizeof tid);
            mix(slot.data(), slot.size());
        }
    }
    return h;
}

void Engine::render_camera_textures(const std::vector<mq_renderer_item>& draws,
                                    const std::vector<mq_renderer_mesh>& meshes) {
    if (render_textures.empty() || !MQ_RENDERER_HAS(rapi, render_to_texture)) return;
    /* 截图请求会强制 OnDemand 目标在本帧更新。 */
    for (const auto& req : camera_captures)
        if (RenderTextureState* rt = render_texture_for(req.entity)) rt->dirty = true;
    /* 静态场景：所有 RT 都干净时，整帧 RT pass（含各自的 3D 提取/深度/命令缓冲）直接跳过。 */
    if (scene_visual_unchanged) {
        bool any_rt_dirty = false;
        for (const auto& rt : render_textures)
            if (rt.enabled && (rt.dirty || !rt.ready)) { any_rt_dirty = true; break; }
        if (!any_rt_dirty) return;
    }

    /* 依赖排序：A 的可见 2D 输入采样 B，则 B 先于 A。同一环内按实体 id 继续，语义为
     * “较早目标读环中较晚目标的上一帧，较晚目标可读较早目标的本帧”，确定且无同-pass 反馈。 */
    std::unordered_map<int32_t, size_t> by_handle;
    for (size_t i = 0; i < render_textures.size(); ++i) by_handle[render_textures[i].handle] = i;
    std::vector<std::vector<size_t>> edges(render_textures.size());
    std::vector<int> indegree(render_textures.size(), 0);
    for (size_t i = 0; i < render_textures.size(); ++i) {
        std::unordered_set<size_t> deps;
        uint32_t dep_2d_mask = 0xFFFFFFFFu;
        uint32_t dep_3d_mask = 0xFFFFFFFFu;
        SceneNode* te = scene->at(render_textures[i].entity);
        if (!render_textures[i].is_3d) {
            if (const auto* cam = te ? component<render::Camera2D>(*te) : nullptr)
                dep_2d_mask = cam->layer_mask;
        } else {
            if (const auto* cam = te ? component<render::Camera3D>(*te) : nullptr)
                dep_3d_mask = cam->culling_mask;
        }
        auto consider = [&](int32_t texture, int layer) {
            if (texture < 0 || layer < 0 || layer > (render_textures[i].include_ui ? 1 : 0)) return;
            if (!render_textures[i].is_3d && layer < 32 && (dep_2d_mask & (1u << layer)) == 0) return;
            auto it = by_handle.find(texture);
            if (it != by_handle.end() && it->second != i) deps.insert(it->second);
        };
        if (!render_textures[i].is_3d) {
            for (const auto& item : draws) consider(item.texture, item.layer);
            for (const auto& mesh : meshes) consider(mesh.texture, mesh.layer);
        } else {
            if (render_textures[i].include_ui) {
                for (const auto& item : draws) if (item.layer == 1) consider(item.texture, item.layer);
                for (const auto& mesh : meshes) if (mesh.layer == 1) consider(mesh.texture, mesh.layer);
            }
            /* 3D 材质在 per-camera extract_3d 中生成；这里直接扫描 PBR 的字符串贴图槽，
             * 把 rendertexture:// 引用提前加入 pass 依赖。 */
            for (auto& [eid, ent] : scene->nodes) for (const auto& [tid, blob] : ent.comps) {
                const mq_type_desc* d = blob.desc;
                if (!d || std::strcmp(d->name, "asset.PbrMaterial") != 0) continue;
                const auto* mesh = component<render::MeshRenderer>(ent);
                if (!mesh || !node_active_effective(*this, eid) || !mesh->visible) continue;
                const uint32_t mesh_mask = mesh->culling_mask;
                if ((mesh_mask & dep_3d_mask) == 0) continue;
                for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                    const mq_field_desc& f = d->fields[fi];
                    if (f.kind != MQ_TK_STRING || std::string(f.name).find("texture") == std::string::npos) continue;
                    const std::string& ref = *(const std::string*)(blob.data() + f.offset);
                    auto hit = tex_cache.find(scoped_texture_ref(eid, ref));
                    if (hit != tex_cache.end()) consider(hit->second, 0);
                }
            }
        }
        for (size_t dep : deps) { edges[dep].push_back(i); ++indegree[i]; }
    }
    std::vector<size_t> order;
    for (size_t produced = 0; produced < render_textures.size();) {
        size_t pick = render_textures.size();
        for (size_t i = 0; i < render_textures.size(); ++i)
            if (indegree[i] == 0 && std::find(order.begin(), order.end(), i) == order.end() &&
                (pick == render_textures.size() || render_textures[i].entity < render_textures[pick].entity)) pick = i;
        if (pick == render_textures.size()) break;
        order.push_back(pick); ++produced; indegree[pick] = -1;
        for (size_t to : edges[pick]) --indegree[to];
    }
    if (order.size() != render_textures.size()) {
        static bool warned_cycle = false;
        if (!warned_cycle) {
            warned_cycle = true;
            std::print(stderr, "runtime: RenderTexture 依赖含环 → 环内按相机 entity id 使用确定的上一帧/本帧顺序\n");
        }
        std::vector<size_t> rest;
        for (size_t i = 0; i < render_textures.size(); ++i)
            if (std::find(order.begin(), order.end(), i) == order.end()) rest.push_back(i);
        std::sort(rest.begin(), rest.end(), [&](size_t a, size_t b) {
            return render_textures[a].entity < render_textures[b].entity;
        });
        order.insert(order.end(), rest.begin(), rest.end());
    }

    for (size_t oi : order) {
        RenderTextureState& rt = render_textures[oi];
        const bool capture_due = std::any_of(camera_captures.begin(), camera_captures.end(),
                                             [&](const CameraCaptureRequest& r) { return r.entity == rt.entity; });
        if ((!rt.enabled && !capture_due) || !node_active_effective(*this, rt.entity)) continue;
        if (rt.update_mode != 0 && !rt.dirty) continue;
        SceneNode* rt_node = scene->at(rt.entity);
        const auto* rtc = rt_node ? component<render::RenderTexture>(*rt_node) : nullptr;
        if (!rtc) continue;
        mq_renderer_frame3d fr{};
        fr.width = rt.width; fr.height = rt.height;
        fr.env.sky_texture = fr.env.env_texture = fr.env.env_irradiance_texture = -1;
        fr.env.env_cubemap = fr.env.env_cubemap_irradiance = -1;
        /* MSAA 逐目标解析（Engine::resolve_msaa）：F4「RenderTexture MSAA」> F4「MSAA」>
         * MAQUE_MSAA > render.RenderTexture.msaa_samples > scn.Scene.msaa_samples。
         * 2D 分支会退回 1x：多采样只作用于 3D pass，2D 图元层恒单采样（见下）。 */
        const int rt_msaa = resolve_msaa(rt.msaa_samples, true);
        fr.msaa_samples = rt_msaa;
        const math::Color& cc = rtc->clear_color;
        fr.bg[0] = cc.r; fr.bg[1] = cc.g; fr.bg[2] = cc.b; fr.bg[3] = cc.a;

        std::vector<mq_renderer_item> target_items;
        std::vector<mq_renderer_mesh> target_meshes;
        auto keep_layer = [&](int layer) { return layer == 0 || (rt.include_ui && layer == 1); };
        if (!rt.is_3d) {
            if (rt_msaa > 1) {
                /* 别让人以为 2D 目标的 msaa 生效了：2D 图元走 render_impl，管线 sampleCount 恒 1，
                 * 边缘质量由纹理过滤决定，MSAA 对它没有任何作用（只打印一次，不逐帧刷屏）。 */
                static bool warned_2d_msaa = false;
                if (!warned_2d_msaa) {
                    warned_2d_msaa = true;
                    std::print(stderr, "runtime: RenderTexture '{}' 是 2D 相机目标 → MSAA 无效"
                                       "（多采样只作用于 3D pass；本提示只打印一次）\n", rt.uri);
                }
                fr.msaa_samples = 1;
            }
            if (const auto* camera2d = rt_node ? component<render::Camera2D>(*rt_node) : nullptr) {
                const math::Color& camera_clear = camera2d->clear_color;
                fr.bg[0] = camera_clear.r; fr.bg[1] = camera_clear.g;
                fr.bg[2] = camera_clear.b; fr.bg[3] = camera_clear.a;
            }
            mq_renderer_view view{};
            uint32_t camera_mask = 0xFFFFFFFFu;
            if (!make_camera_view2d(rt.entity, rt.width, rt.height, view, &camera_mask)) continue;
            auto keep_2d = [&](int layer) {
                return keep_layer(layer) && layer >= 0 && layer < 32 && (camera_mask & (1u << layer)) != 0;
            };
            for (const auto& item : draws) if (keep_2d(item.layer)) target_items.push_back(item);
            for (const auto& mesh : meshes) if (keep_2d(mesh.layer)) target_meshes.push_back(mesh);
            /* shader 精灵是 2D 图元（layer 0）：2D 相机的 RT 也要捕获它们（docs/13）。
             * 3D RT 不捕获（它只按 include_ui 叠 UI 层；精灵属世界层 2D）。 */
            std::vector<mq_renderer_shader_sprite> target_sprites;
            extract_shader_sprites(target_sprites);
            fr.views2d = &view; fr.view2d_count = 1;
            fr.items = target_items.data(); fr.item_count = (uint32_t)target_items.size();
            fr.meshes = target_meshes.data(); fr.mesh_count = (uint32_t)target_meshes.size();
            fr.shader_sprites = target_sprites.data();
            fr.shader_sprite_count = (uint32_t)target_sprites.size();
            fr.masks = ui_masks.data();
            fr.mask_count = (uint32_t)ui_masks.size();
            const int32_t rc = rapi->render_to_texture(rself, rt.handle, &fr);
            if (rc != 0) { std::print(stderr, "runtime: RenderTexture '{}' 2D render rc={}\n", rt.uri, rc); continue; }
        } else {
            std::vector<mq_renderer_view3d> v3;
            std::vector<mq_renderer_light> l3;
            std::vector<mq_renderer_pbr_material> m3;
            std::vector<mq_renderer_draw3d> d3;
            std::vector<mq_renderer_skin> s3;
            extract_3d(rt.width, rt.height, v3, l3, m3, d3, s3, rt.entity, false);
            if (v3.empty()) continue;
            if (rt.include_ui) {
                for (const auto& item : draws) if (item.layer == 1) target_items.push_back(item);
                for (const auto& mesh : meshes) if (mesh.layer == 1) target_meshes.push_back(mesh);
            }
            std::vector<mq_renderer_shader_pass> passes;
            extract_shader_passes(passes);
            fr.views = v3.data(); fr.view_count = (uint32_t)v3.size();
            fr.lights = l3.data(); fr.light_count = (uint32_t)l3.size();
            fr.materials = m3.data(); fr.material_count = (uint32_t)m3.size();
            fr.draws = d3.data(); fr.draw_count = (uint32_t)d3.size();
            fr.skins = s3.data(); fr.skin_count = (uint32_t)s3.size();
            fr.items = target_items.data(); fr.item_count = (uint32_t)target_items.size();
            fr.meshes = target_meshes.data(); fr.mesh_count = (uint32_t)target_meshes.size();
            fr.masks = ui_masks.data(); fr.mask_count = (uint32_t)ui_masks.size();
            fr.views2d = nullptr; fr.view2d_count = 0;
            fr.passes = passes.data(); fr.pass_count = (uint32_t)passes.size();
            fill_frame_environment(fr);
            const int32_t rc = rapi->render_to_texture(rself, rt.handle, &fr);
            if (rc != 0) { std::print(stderr, "runtime: RenderTexture '{}' 3D render rc={}\n", rt.uri, rc); continue; }
        }
        rt.ready = true;
        rt.dirty = false;
        rt.last_frame = frame;
        if (auto* mut = rt_node ? component<render::RenderTexture>(*rt_node) : nullptr) {
            mut->ready = true;
            mut->last_frame = (uint32_t)frame;
        }
    }
    for (const auto& req : camera_captures) {
        RenderTextureState* rt = render_texture_for(req.entity);
        if (rt && rt->ready && rt->last_frame == frame) {
            (void)save_camera_texture(req.entity, req.path);
        } else {
            std::print(stderr, "runtime: camera shot 跳过 '{}' camera={}（本帧 RenderTexture 更新失败）\n",
                       req.path, req.entity);
        }
    }
    camera_captures.clear();
}

}  // namespace maque::rt
