/* 资源 · glTF 蒙皮/动画/morph 采样（plans/22 §6；全部是派生缓存，不进快照）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S4，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

bool Engine::load_skeleton(const std::string& path) {
    auto it = skeletons.find(path);
    if (it != skeletons.end()) return !it->second.failed;
    Skeleton sk;
    sk.name = path;
    sk.failed = true;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data) {
        std::print(stderr, "runtime: skeleton 未找到 '{}'\n", path);
        skeletons[path] = std::move(sk);
        return false;
    }
    mq_json* dom = mq_json_parse((const char*)data, n);
    mq_vfs_free(data);
    if (!dom) {
        std::print(stderr, "runtime: skeleton json 解析失败 '{}'\n", path);
        skeletons[path] = std::move(sk);
        return false;
    }
    const mq_json* jn = mq_json_get(dom, "joint_names");
    const mq_json* jb = mq_json_get(dom, "inverse_bind");
    const uint32_t nj = mq_json_len(jn);
    const uint32_t nb = mq_json_len(jb);
    bool ok = nj > 0 && nj == nb;
    if (!ok)
        std::print(stderr, "runtime: skeleton '{}' 无效（joint_names={} inverse_bind={}）\n", path, nj, nb);
    if (ok) {
        sk.joint_names.reserve(nj);
        for (uint32_t i = 0; i < nj; ++i) {
            const char* nm = mq_json_str(mq_json_at(jn, i), "");
            sk.joint_names.emplace_back(nm ? nm : "");
        }
        sk.inverse_bind.resize((size_t)nj * 16);
        for (uint32_t i = 0; i < nj && ok; ++i) {
            const mq_json* row = mq_json_at(jb, i);
            if (mq_json_len(row) != 16) { ok = false; break; }
            for (uint32_t k = 0; k < 16; ++k)
                sk.inverse_bind[(size_t)i * 16 + k] = (float)mq_json_num(mq_json_at(row, k), 0.0);
        }
        if (!ok) sk.inverse_bind.clear();
    }
    const char* nm = mq_json_str(mq_json_get(dom, "name"), nullptr);
    if (nm && *nm) sk.name = nm;
    mq_json_free(dom);
    if (!ok) {
        skeletons[path] = std::move(sk);
        return false;
    }
    sk.failed = false;
    const size_t joints = sk.joint_names.size();
    skeletons[path] = std::move(sk);
    if (render3d_info)
        std::print("runtime: skeleton '{}' joints={}\n", path, (long long)joints);
    return true;
}

bool Engine::load_gclip(const std::string& path) {
    auto it = gclips.find(path);
    if (it != gclips.end()) return !it->second.failed;
    GClip clip;
    clip.name = path;
    clip.failed = true;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data) {
        std::print(stderr, "runtime: clip 未找到 '{}'\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    mq_json* dom = mq_json_parse((const char*)data, n);
    mq_vfs_free(data);
    if (!dom) {
        std::print(stderr, "runtime: clip json 解析失败 '{}'\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    clip.duration = (float)mq_json_num(mq_json_get(dom, "duration"), 0.0);
    const mq_json* jtracks = mq_json_get(dom, "tracks");
    const uint32_t nt = mq_json_len(jtracks);
    float max_time = 0.0F;
    for (uint32_t ti = 0; ti < nt; ++ti) {
        const mq_json* jt = mq_json_at(jtracks, ti);
        const char* node = mq_json_str(mq_json_get(jt, "node"), "");
        const char* pathname = mq_json_str(mq_json_get(jt, "path"), "");
        if (!node || !*node || !pathname) continue;
        int tp = -1;
        if (std::strcmp(pathname, "translation") == 0) tp = 0;
        else if (std::strcmp(pathname, "rotation") == 0) tp = 1;
        else if (std::strcmp(pathname, "scale") == 0) tp = 2;
        else if (std::strcmp(pathname, "weights") == 0) tp = 3;
        if (tp < 0) continue;                          /* 未知通道（pointer 等）跳过 */
        GClipTrack tr;
        tr.node = node;
        tr.path = tp;
        const char* interp = mq_json_str(mq_json_get(jt, "interpolation"), "LINEAR");
        tr.step = interp && std::strcmp(interp, "STEP") == 0;
        const mq_json* jtimes = mq_json_get(jt, "times");
        const mq_json* jvals = mq_json_get(jt, "values");
        const uint32_t nk = mq_json_len(jtimes);
        const uint32_t nv = mq_json_len(jvals);
        if (nk == 0 || nk != nv) {                     /* 关键帧/值数量不一致：跳过该轨道 */
            std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 关键帧数不符（{} vs {}），跳过\n",
                       path, tr.node, pathname, nk, nv);
            continue;
        }
        const uint32_t comps = mq_json_len(mq_json_at(jvals, 0));
        if (comps == 0 || (tp == 1 && comps != 4) ||
            ((tp == 0 || tp == 2) && comps != 3)) {
            std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 分量数 {} 不符，跳过\n",
                       path, tr.node, pathname, comps);
            continue;
        }
        tr.comps = comps;
        tr.times.resize(nk);
        tr.values.resize((size_t)nk * comps);
        bool ok = true;
        for (uint32_t k = 0; k < nk && ok; ++k) {
            tr.times[k] = (float)mq_json_num(mq_json_at(jtimes, k), 0.0);
            const mq_json* row = mq_json_at(jvals, k);
            if (mq_json_len(row) != comps) { ok = false; break; }
            for (uint32_t c = 0; c < comps; ++c)
                tr.values[(size_t)k * comps + c] = (float)mq_json_num(mq_json_at(row, c), 0.0);
        }
        if (!ok) { std::print(stderr, "runtime: clip '{}' 轨道 {}/{} 值行宽不符，跳过\n", path, tr.node, pathname); continue; }
        if (!tr.times.empty() && tr.times.back() > max_time) max_time = tr.times.back();
        clip.tracks.push_back(std::move(tr));
    }
    const char* nm = mq_json_str(mq_json_get(dom, "name"), nullptr);
    if (nm && *nm) clip.name = nm;
    mq_json_free(dom);
    if (clip.tracks.empty()) {
        std::print(stderr, "runtime: clip '{}' 无可采样轨道\n", path);
        gclips[path] = std::move(clip);
        return false;
    }
    if (clip.duration <= 0.0F) clip.duration = max_time;   /* 缺省由关键帧推导 */
    clip.failed = false;
    const int ntracks = (int)clip.tracks.size();
    const float dur = clip.duration;
    gclips[path] = std::move(clip);
    if (render3d_info)
        std::print("runtime: clip '{}' tracks={} duration={:.3f}\n", path, ntracks, (double)dur);
    return true;
}

/* 采样一条轨道：STEP 取前关键帧；LINEAR 线性（rotation 用四元数 slerp）。 */
static void sample_gclip_track(const GClipTrack& tr, float time, std::vector<float>& out) {
    out.assign(tr.comps, 0.0F);
    const uint32_t nk = (uint32_t)tr.times.size();
    if (nk == 0 || tr.comps == 0) return;
    if (time <= tr.times[0]) {
        std::memcpy(out.data(), tr.values.data(), sizeof(float) * tr.comps);
        return;
    }
    if (time >= tr.times[nk - 1]) {
        std::memcpy(out.data(), tr.values.data() + (size_t)(nk - 1) * tr.comps,
                    sizeof(float) * tr.comps);
        return;
    }
    uint32_t k = 0;
    while (k + 1 < nk && tr.times[k + 1] <= time) ++k;
    const float* a = tr.values.data() + (size_t)k * tr.comps;
    if (tr.step) {
        std::memcpy(out.data(), a, sizeof(float) * tr.comps);
        return;
    }
    const float* b = tr.values.data() + (size_t)(k + 1) * tr.comps;
    const float span = tr.times[k + 1] - tr.times[k];
    float alpha = span > 0.0F ? (time - tr.times[k]) / span : 0.0F;
    if (alpha < 0.0F) alpha = 0.0F;
    if (alpha > 1.0F) alpha = 1.0F;
    if (tr.path == 1 && tr.comps == 4) {
        /* quat：dot<0 先翻转；dot>0.9995 用 nlerp（近似共线避免除零）；否则 slerp */
        float qb[4] = { b[0], b[1], b[2], b[3] };
        float dot = a[0] * qb[0] + a[1] * qb[1] + a[2] * qb[2] + a[3] * qb[3];
        if (dot < 0.0F) { dot = -dot; for (int i = 0; i < 4; ++i) qb[i] = -qb[i]; }
        float q[4];
        if (dot > 0.9995F) {
            for (int i = 0; i < 4; ++i) q[i] = a[i] + (qb[i] - a[i]) * alpha;
        } else {
            const float theta = std::acos(std::min(1.0F, dot));
            const float s = std::sin(theta);
            const float wa = std::sin((1.0F - alpha) * theta) / s;
            const float wb = std::sin(alpha * theta) / s;
            for (int i = 0; i < 4; ++i) q[i] = a[i] * wa + qb[i] * wb;
        }
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (ql > 0.0F) { const float inv = 1.0F / ql; for (int i = 0; i < 4; ++i) q[i] *= inv; }
        std::memcpy(out.data(), q, sizeof q);
        return;
    }
    for (uint32_t c = 0; c < tr.comps; ++c) out[c] = a[c] + (b[c] - a[c]) * alpha;
}

/* 按权重 CPU 重算 morph 顶点并上传（rapi->update_mesh）。权重没变不应调用（见 extract_3d）。 */
bool Engine::apply_morph(const std::string& path, const std::vector<float>& weights) {
    auto it = morph_meshes.find(path);
    if (it == morph_meshes.end()) return false;
    MorphMesh& mm = it->second;
    if (mm.mesh < 0 || mm.targets.empty() || mm.base.empty()) return false;
    if (!rapi || !rself || !rapi->update_mesh) {
        if (!mm.warned_no_update) {
            mm.warned_no_update = true;
            std::print(stderr, "runtime: 渲染器不支持 update_mesh，morph 跳过 '{}'\n", path);
        }
        return false;
    }
    std::vector<uint8_t> out = mm.base;
    for (uint32_t vi = 0; vi < mm.vertex_count; ++vi) {
        float* pos = (float*)(out.data() + (size_t)vi * mm.stride);
        float* nrm = pos + 3;
        bool normal_touched = false;
        for (size_t ti = 0; ti < mm.targets.size(); ++ti) {
            const float w = ti < weights.size() ? weights[ti] : 0.0F;
            if (w == 0.0F) continue;
            const MorphTarget& tgt = mm.targets[ti];
            const size_t o = (size_t)vi * 3;
            if (o + 2 < tgt.positions.size()) {
                pos[0] += w * tgt.positions[o];
                pos[1] += w * tgt.positions[o + 1];
                pos[2] += w * tgt.positions[o + 2];
            }
            if (o + 2 < tgt.normals.size()) {
                nrm[0] += w * tgt.normals[o];
                nrm[1] += w * tgt.normals[o + 1];
                nrm[2] += w * tgt.normals[o + 2];
                normal_touched = true;
            }
        }
        if (normal_touched) {
            const float len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            if (len2 > 1.0e-12F) {
                const float inv = 1.0F / std::sqrt(len2);
                nrm[0] *= inv; nrm[1] *= inv; nrm[2] *= inv;
            }
        }
    }
    if (rapi->update_mesh(rself, mm.mesh, out.data(), (uint64_t)out.size()) != 0) {
        if (!mm.warned_no_update) {
            mm.warned_no_update = true;
            std::print(stderr, "runtime: update_mesh 失败，morph 跳过 '{}'\n", path);
        }
        return false;
    }
    mm.applied_weights = weights;
    return true;
}

/* 装配期预解析 glTF 运行时资源（plans/22 §6.1）：实体名表 → 骨架/片段/morph 网格。
 * 装配期做 IO 合法（不在模拟帧内，docs/10 D7）；失败只告警一次（缓存 failed）。 */
void Engine::setup_gltf_assets() {
    read_gltf_env();
    entity_by_name.clear();
    entity_morph_mesh.clear();
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    /* 名字表：唯一名 → 最小 id（重名只告警一次；动画轨道/骨架关节按名字引用实体） */
    int dup = 0;
    std::string dup_example;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e || e->name.empty()) continue;
        auto [nit, inserted] = entity_by_name.emplace(e->name, id);
        (void)nit;
        if (!inserted) { ++dup; if (dup_example.empty()) dup_example = e->name; }
    }
    if (dup > 0)
        std::print(stderr, "runtime: 实体重名 {} 处（例如 '{}'）；按名字解析取最小 id\n",
                   dup, dup_example);
    int n_skin = 0, n_clip = 0, n_morph = 0, n_gltf = 0;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* dmesh = nullptr;
        if (auto* mb = comp_by_name(*this, id, "render.MeshRenderer", &dmesh)) {
            const std::string* mp = read_str(dmesh, mb->data(), "mesh_path");
            if (mp && !mp->empty()) {
                if (gltf::is_gltf_path(*mp)) {
                    if (auto* gm = load_gltf_model(*mp); gm && !gm->failed) {
                        gltf_state_ensure(id, *mp, *gm);
                        ++n_gltf;
                    }
                } else {
                    (void)load_mesh(*mp);                     /* 缓存；失败只告警一次 */
                    auto mit = morph_meshes.find(*mp);
                    if (mit != morph_meshes.end() && mit->second.mesh >= 0 && !mit->second.targets.empty()) {
                        entity_morph_mesh[id] = *mp;
                        ++n_morph;
                    }
                }
            }
            const std::string* sp = read_str(dmesh, mb->data(), "skin_path");
            if (sp && !sp->empty()) {
                ++n_skin;
                (void)load_skeleton(*sp);
            }
        }
        for (auto& [tid, blob] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            const auto* ap = (d && (d->role & MQ_ROLE_ANIM_PLAYER))
                                 ? as_component<scn::AnimationPlayer>(d, blob.data()) : nullptr;
            if (!ap) continue;
            for (const std::string& cp : ap->clips) {
                ++n_clip;
                (void)load_gclip(cp);
            }
        }
    }
    gltf_dynamic = false;
    for (const auto& [gp, gm] : gltf_models) {
        (void)gp;
        if (gm.failed) continue;
        if (!gm.anims.empty() || !gm.skins.empty()) gltf_dynamic = true;
        for (const auto& pr : gm.prims)
            if (!pr.morph_targets.empty()) gltf_dynamic = true;
    }
    if (!gltf_states.empty()) gltf_anim_step(0.0F);
    if (render3d_info || n_skin > 0 || n_clip > 0 || n_morph > 0 || n_gltf > 0)
        std::print("runtime: gltf assets skins={} clips={} morph_meshes={} runtime_gltf={} names={}\n",
                   n_skin, n_clip, n_morph, n_gltf, (long long)entity_by_name.size());
}

void Engine::read_gltf_env() {
    if (gltf_env_read) return;
    gltf_env_read = true;
    auto flag = [](const char* name, bool def) {
        const char* v = std::getenv(name);
        if (!v || !*v) return def;
        return !(v[0] == '0' || v[0] == 'o' || v[0] == 'O' ||
                 v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
    };
    anim_enabled = flag("MAQUE_ANIM", true);
    skin_enabled = flag("MAQUE_SKIN", true);
    morph_enabled = flag("MAQUE_MORPH", true);
    render3d_info = std::getenv("MAQUE_RENDER3D_INFO") != nullptr;
}

/* glTF 片段采样（plans/22 §6.2）：仅处理 clips 非空的 scn.AnimationPlayer（旧 clip 通路不变）。
 * 推进播放头 → 采样（纯函数）→ 写 Engine::anim_pose（pos/rot/scale）与 morph 权重覆盖。 */
void Engine::anim_step_gclip(int entity, const mq_type_desc* d, std::vector<uint8_t>& blob, float dt) {
    auto* ap = as_component_mut<scn::AnimationPlayer>(d, blob.data());
    if (!ap || ap->clips.empty()) return;
    const int32_t ci = ap->clip_index;
    const std::string& clip_path = ap->clips[(ci >= 0 && (size_t)ci < ap->clips.size()) ? (size_t)ci : 0];
    if (!load_gclip(clip_path)) return;
    auto cit = gclips.find(clip_path);
    if (cit == gclips.end() || cit->second.failed || cit->second.tracks.empty()) return;
    const GClip& clip = cit->second;
    const bool playing = ap->playing;
    const bool loop = ap->loop;
    const float speed = ap->speed;
    float time = ap->time;
    if (playing) time += dt * speed;
    if (clip.duration > 0.0F) {
        if (loop) {
            /* 取模（fmod 而非反复相减：退化片段 duration 极小时不会卡住；确定性纯函数） */
            time = std::fmod(time, clip.duration);
            if (time < 0.0F) time += clip.duration;
        } else if (time > clip.duration) {
            time = clip.duration;
        } else if (time < 0.0F) {
            time = 0.0F;
        }
    }
    ap->time = time;
    ap->duration = clip.duration;
    if (!anim_enabled) return;                    /* MAQUE_ANIM=0：保持 bind pose（不写派生姿态） */
    std::vector<float> v;
    for (const GClipTrack& tr : clip.tracks) {
        auto hit = entity_by_name.find(tr.node);
        if (hit == entity_by_name.end()) continue;
        const int te = hit->second;
        sample_gclip_track(tr, time, v);
        if (tr.path == 0 && v.size() >= 3) {
            AnimPose& p = anim_pose[te];
            p.has_pos = true;
            p.pos = math::Vec3{ v[0], v[1], v[2] };
        } else if (tr.path == 1 && v.size() >= 4) {
            AnimPose& p = anim_pose[te];
            p.has_rot = true;
            std::memcpy(p.rot, v.data(), sizeof p.rot);
        } else if (tr.path == 2 && v.size() >= 3) {
            AnimPose& p = anim_pose[te];
            p.has_scale = true;
            p.scale = math::Vec3{ v[0], v[1], v[2] };
        } else if (tr.path == 3) {
            /* weights：写到该节点（含整棵子树）下所有 morph mesh 实体的权重覆盖 */
            std::vector<int> stack2{ te };
            while (!stack2.empty()) {
                const int cur = stack2.back();
                stack2.pop_back();
                if (entity_morph_mesh.find(cur) != entity_morph_mesh.end())
                    morph_weight_override[cur] = v;
                if (SceneNode* cn = scene->at(cur))
                    for (int k2 : cn->children) stack2.push_back(k2);
            }
        }
    }
    if (render3d_info && !gltf_anim_info) {
        gltf_anim_info = true;
        std::print("runtime: anim entity={} clip='{}' tracks={} duration={:.3f} time={:.3f} pose={}\n",
                   entity, clip_path, (int)clip.tracks.size(), (double)clip.duration,
                   (double)time, (int)anim_pose.size());
    }
}

}  // namespace maque::rt
