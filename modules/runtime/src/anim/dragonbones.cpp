/* DragonBones 2D（docs/36）：装配期建实例、帧内步进、提取期出网格图元
 *
 * 分层与数据流（与 spine.cpp 同构，docs/27 §架构）：
 *   VFS 读 *_ske.json + *_tex.json ──► mq_db_data_create（骨架/图集，页纹理走回调）
 *   纹理回调：页名（imagePath）→ VFS 路径 → image 解码 → gfx 上传 → 回填非零句柄 + 像素尺寸
 *   每帧：mq_db_instance_step(dt*speed) → mq_db_instance_draw（CPU 顶点/UV/颜色/索引/批次）
 *         → 顶点做"armature 空间 → 渲染目标像素空间"映射
 *   提取：每个批次 = 一条 mq_renderer_mesh（同图集页 + 同混合模式）
 *
 * ⚠️ 与 Spine 的关键差异（实测/官方源码确认，别再统一处理）：
 *   DragonBones 是 flash 系 **y 向下**坐标系 —— 导出顶点已经是"屏幕向下为正"，
 *   所以这里直接 `screen_y = origin_y + local_y`，**不翻 y**（spine 必须翻）。
 */
#include "../runtime_engine.h"

namespace maque::rt {

/* 页纹理回调：图集 imagePath → (atlas 目录 + 页名) → VFS 读 → 解码 → 上传。
 * 句柄约定：0 = 未回填；gfx 纹理 id 从 0 开始 → 句柄 = id + 1。
 * 图集页**不参与 2D 自动合图**（走 load_texture_raw：骨骼用 mesh 图元整页采样）。 */
static int dragonbones_texture_cb(void* user, const char* page_name, uint64_t* out_texture,
                                 int* out_width, int* out_height) {
    auto* cx = static_cast<DbTexCtx*>(user);
    if (!cx || !cx->eng || !page_name || !*page_name || !out_texture) return MQ_DB_ERR_ARG;
    *out_texture = 0;
    const std::string path = cx->dir + page_name;
    int w = 0, h = 0;
    const int tex = cx->eng->load_texture_raw(path, &w, &h);
    if (tex < 0) {
        std::print(stderr, "runtime: dragonbones atlas page read failed '{}'\n", path);
        return MQ_DB_ERR_PAGE;
    }
    *out_texture = (uint64_t)tex + 1u;
    /* 像素尺寸回填：官方部分图集 json 不写 width/height（如 weapon_1000），
     * 必须用解码后的真实尺寸，否则 UV 归一化会按错误的页尺寸缩放（贴图错位）。 */
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    return MQ_DB_OK;
}

/* 路径归一：组件里的资源引用可写 "art/x.json" 或 "/art/x.json" → VFS 一律前导 '/'。 */
static std::string db_vfs_path_of(const std::string& ref) {
    return (!ref.empty() && ref[0] == '/') ? ref : ("/" + ref);
}
static std::string db_dir_of(const std::string& path) {          // "/art/x.json" → "/art/"
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string("/") : path.substr(0, slash + 1);
}
static std::string db_stem_of(const std::string& path) {         // "/art/x_ske.json" → "/art/x_ske"
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

DragonBonesRuntime::Instance* Engine::dragonbones_of(int entity) {
    for (auto& db : dragonbones_rt.items) if (db.entity == entity) return &db;
    return nullptr;
}

void Engine::dragonbones_release() {
    for (auto& db : dragonbones_rt.items) {
        if (db.inst) { mq_db_instance_destroy(db.inst); db.inst = nullptr; }
        if (db.data) { mq_db_data_destroy(db.data); db.data = nullptr; }   // 页纹理归 tex_cache
    }
    dragonbones_rt.items.clear();
}

void Engine::dragonbones_setup() {
    if (!scene || !vfs || !rapi) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        auto* sc = e ? component<anim::DragonBonesSkeleton>(*e) : nullptr;
        if (!sc) continue;
        if (sc->skeleton.empty()) {
            std::print(stderr, "runtime: dragonbones entity {} missing 'skeleton' ref\n", id);
            continue;
        }
        DragonBonesRuntime::Instance db;
        db.entity = id;
        db.skel_path = db_vfs_path_of(sc->skeleton);
        /* 图集路径：① 组件 atlas；② 与骨架同名但把 `_ske` 换成 `_tex`（DragonBones 导出约定）。 */
        if (!sc->atlas.empty()) {
            db.atlas_path = db_vfs_path_of(sc->atlas);
        } else {
            const std::string stem = db_stem_of(db.skel_path);
            db.atlas_path = (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_ske") == 0)
                                ? stem.substr(0, stem.size() - 4) + "_tex.json"
                                : stem + "_tex.json";
        }
        db.tex_dir = db_dir_of(db.atlas_path);

        void* sd = nullptr; size_t sn = 0;
        if (mq_vfs_read(vfs, db.skel_path.c_str(), &sd, &sn) != MQ_VFS_OK || !sd || sn == 0) {
            std::print(stderr, "runtime: dragonbones skeleton read failed '{}'\n", db.skel_path);
            continue;
        }
        std::vector<char> skel((char*)sd, (char*)sd + sn);
        mq_vfs_free(sd);
        void* ad = nullptr; size_t an = 0;
        std::string atlas_text;
        if (mq_vfs_read(vfs, db.atlas_path.c_str(), &ad, &an) == MQ_VFS_OK && ad && an > 0) {
            atlas_text.assign((char*)ad, an);
            mq_vfs_free(ad);
        } else {
            std::print(stderr, "runtime: dragonbones atlas read failed '{}'"
                               " (set the component's atlas field to the *_tex.json path)\n", db.atlas_path);
            continue;
        }

        db.tex_ctx.eng = this;
        db.tex_ctx.dir = db.tex_dir;
        db.data = mq_db_data_create(skel.data(), sn, atlas_text.c_str(), "", dragonbones_texture_cb, &db.tex_ctx);
        if (!db.data) {
            std::print(stderr, "runtime: dragonbones parse failed '{}' (binary .dbbin? P0 只支持 JSON)\n",
                       db.skel_path);
            continue;
        }
        /* 选骨架：组件 armature 名（空 → 第 0 个） */
        int arm_idx = 0;
        if (!sc->armature.empty()) {
            arm_idx = mq_db_data_armature_index(db.data, sc->armature.c_str());
            if (arm_idx < 0) {
                std::print(stderr, "runtime: dragonbones armature '{}' not found in '{}' → use [0]\n",
                           sc->armature, db.skel_path);
                arm_idx = 0;
            }
        }
        db.armature_name = mq_db_data_armature_name(db.data, arm_idx);
        db.inst = mq_db_instance_create(db.data, arm_idx);
        if (!db.inst) { mq_db_data_destroy(db.data); db.data = nullptr; continue; }

        const int n_anim = mq_db_data_animation_count(db.data, arm_idx);
        const int n_skin = mq_db_data_skin_count(db.data, arm_idx);
        const int n_page = mq_db_data_page_count(db.data);
        for (int i = 0; i < n_anim; ++i) {
            mq_db_anim ai{};
            db.anim_names.emplace_back(mq_db_data_animation(db.data, arm_idx, i, &ai) == MQ_DB_OK && ai.name
                                           ? ai.name : "");
        }
        for (int i = 0; i < n_skin; ++i) {
            const char* nm = mq_db_data_skin_name(db.data, arm_idx, i);
            db.skin_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_page; ++i) {
            mq_db_page pg{};
            if (mq_db_data_page(db.data, i, &pg) != MQ_DB_OK) continue;
            DragonBonesRuntime::Instance::Page p;
            p.name = pg.name ? pg.name : "";
            p.width = pg.width; p.height = pg.height; p.handle = pg.texture;
            db.pages.push_back(std::move(p));
        }
        /* 皮肤：组件下标（声明序）；越界忽略 */
        if (sc->skin < (uint32_t)n_skin) {
            const std::string& skin_name = db.skin_names[sc->skin];
            if (!skin_name.empty()) mq_db_instance_set_skin(db.inst, skin_name.c_str());
        }
        int start_anim = (int)sc->animation;
        if (start_anim < 0 || start_anim >= n_anim) start_anim = 0;
        if (sc->autoplay && n_anim > 0) {
            const bool loop = sc->play_mode == anim::PlayMode::Loop;
            if (mq_db_instance_play(db.inst, db.anim_names[(size_t)start_anim].c_str(), loop ? 1 : 0) == MQ_DB_OK)
                db.last_anim = start_anim;
        } else if (n_anim > 0) {
            /* 不自动播：也要让姿势停在动画 0 的第 0 帧（与 DragonBones 编辑器一致） */
            mq_db_instance_play(db.inst, db.anim_names[(size_t)start_anim].c_str(), 0);
            mq_db_instance_set_playing(db.inst, 0);
            db.last_anim = start_anim;
        }
        /* 绘制缓冲：按"槽数 × 4 顶点"起步（网格附件更大；不够时下一帧扩容重试） */
        const int slots = mq_db_instance_slot_count(db.inst);
        const size_t vcap = (size_t)std::max(slots * 4, 64) * 2;
        db.verts.resize(vcap); db.uvs.resize(vcap); db.cols.resize(vcap * 2); db.tverts.resize(vcap);
        db.idxs.resize(vcap * 3); db.batches.resize(256);
        db.z = (int)sc->z_index * 100000 + id;
        sc->instance = (uint64_t)(dragonbones_rt.items.size() + 1);

        const int mesh = mq_db_data_mesh_count(db.data);
        const int weighted = mq_db_data_weighted_count(db.data);
        const int nested = mq_db_data_nested_count(db.data);
        const int missing = mq_db_data_missing_texture_count(db.data);
        std::print("runtime: dragonbones entity {} '{}' skel='{}' atlas='{}' armature='{}'"
                   " anims={} skins={} bones={} slots={} pages={} textures={} mesh={} weighted={} nested={} missing_tex={}\n",
                   id, e->name, db.skel_path, db.atlas_path, db.armature_name,
                   n_anim, n_skin, mq_db_instance_bone_count(db.inst), slots, n_page,
                   mq_db_data_texture_count(db.data), mesh, weighted, nested, missing);
        if (weighted > 0)
            std::print(stderr, "runtime: dragonbones '{}' 有 {} 个带权重网格 —— P0 未支持，这些附件会被跳过"
                               "（docs/36 未支持清单）\n", e->name, weighted);
        if (nested > 0)
            std::print(stderr, "runtime: dragonbones '{}' 有 {} 个嵌套子骨架显示 —— P0 未支持（跳过）\n",
                       e->name, nested);
        if (missing > 0)
            std::print(stderr, "runtime: dragonbones '{}' 有 {} 个 image 显示在图集里找不到子纹理"
                               "（骨架/图集不配套？）\n", e->name, missing);
        if (!db.pages.empty()) {
            std::print("runtime: dragonbones pages:");
            for (const auto& p : db.pages)
                std::print(" [{} {}x{} tex={}]", p.name, p.width, p.height, (long long)p.handle);
            std::print("\n");
        }
        dragonbones_rt.items.push_back(std::move(db));
    }
    dragonbones_rt.items.shrink_to_fit();      // 之后不再增删 → tex_ctx 地址稳定
    std::print("runtime: dragonbones instances={}\n", dragonbones_rt.items.size());
}

void Engine::dragonbones_step(float dt) {
    if (!scene) return;
    for (auto& db : dragonbones_rt.items) {
        if (!db.inst || !db.data) continue;
        SceneNode* e = scene->at(db.entity);
        auto* sc = e ? component<anim::DragonBonesSkeleton>(*e) : nullptr;
        if (!sc) continue;

        /* 组件字段 → 语义（组件是唯一真相：脚本改 animation 即生效） */
        const int n_anim = (int)db.anim_names.size();
        int want = (int)sc->animation;
        if (want < 0 || want >= n_anim) want = db.last_anim;
        const bool loop = sc->play_mode == anim::PlayMode::Loop;
        if (want != db.last_anim && want >= 0) {
            const char* cur = mq_db_instance_animation_name(db.inst);
            if (cur && db.anim_names[(size_t)want] == cur) {
                db.last_anim = want;
            } else if (mq_db_instance_play(db.inst, db.anim_names[(size_t)want].c_str(), loop ? 1 : 0) == MQ_DB_OK) {
                db.last_anim = want;
            }
        }
        mq_db_instance_step(db.inst, dt * sc->speed);

        /* 变换：模块负责缩放/翻转（armature 单位 → 屏幕单位），旋转/平移在提取期做 */
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, db.entity, angle, scale, opacity);
        const int flip = (int)sc->flip;
        mq_db_instance_set_transform(db.inst, scale, (flip & 1) ? 1 : 0, (flip & 2) ? 1 : 0);
        mq_db_instance_set_color(db.inst, sc->color.r, sc->color.g, sc->color.b, sc->color.a * opacity);

        mq_db_draw out{};
        const int rc = mq_db_instance_draw(db.inst, db.verts.data(), db.uvs.data(), db.cols.data(),
                                           db.idxs.data(), db.batches.data(),
                                           (int)(db.verts.size() / 2), (int)db.idxs.size(),
                                           (int)db.batches.size(), &out);
        if (rc == MQ_DB_ERR_NOMEM) {
            /* 缓冲满：按报告的实际规模扩容，下一帧重试（本帧几何不完整，宁可不画也不越界） */
            const size_t vcap = (size_t)std::max(out.vertex_count, 1) * 2;
            db.verts.resize(vcap); db.uvs.resize(vcap); db.cols.resize(vcap * 2); db.tverts.resize(vcap);
            db.idxs.resize((size_t)std::max(out.index_count, 1) * 2);
            db.batches.resize((size_t)std::max(out.batch_count, 1) * 2);
            static bool warned = false;
            if (!warned) { warned = true;
                std::print(stderr, "runtime: dragonbones draw buffers full -> growing\n"); }
            db.draw = mq_db_draw{};
            continue;
        }
        if (rc != MQ_DB_OK) { db.draw = mq_db_draw{}; continue; }
        db.draw = out;
        db.z = (int)sc->z_index * 100000 + db.entity;

        /* armature 空间（y 向下）→ 渲染目标像素空间：
         *   · local y 已经是"屏幕向下" → 不翻 y（与 spine 的唯一区别）；
         *   · 累计节点角度绕实体原点旋转（与引擎 2D 图元的 rotation 同式），再平移到世界位置；
         *   · world 是 y 向上的世界坐标 → 屏幕 y = H - world_y。 */
        const float rad = angle * 3.14159265358979323846F / 180.0F;
        const float cs = std::cos(rad), sn = std::sin(rad);
        const float ox = e->world_x, oy = (float)H - e->world_y;
        const int vc = out.vertex_count;
        if ((int)(db.tverts.size() / 2) < vc) db.tverts.resize((size_t)vc * 2);
        for (int i = 0; i < vc; ++i) {
            const float lx = db.verts[(size_t)i * 2], ly = db.verts[(size_t)i * 2 + 1];
            const float rx = lx * cs - ly * sn;
            const float ry = lx * sn + ly * cs;
            db.tverts[(size_t)i * 2] = ox + rx;
            db.tverts[(size_t)i * 2 + 1] = oy + ry;
        }
        if (std::getenv("MAQUE_TRACE_DRAGONBONES") && (frame % 20 == 0)) {
            float minx = 0, maxx = 0, miny = 0, maxy = 0;
            int uv_zero = 0; float u0 = 2, u1 = -1, v0 = 2, v1 = -1;
            for (int i = 0; i < vc; ++i) {
                const float x = db.tverts[(size_t)i * 2], y = db.tverts[(size_t)i * 2 + 1];
                if (i == 0) { minx = maxx = x; miny = maxy = y; }
                else { minx = std::min(minx, x); maxx = std::max(maxx, x);
                       miny = std::min(miny, y); maxy = std::max(maxy, y); }
                const float uu = db.uvs[(size_t)i * 2], vv = db.uvs[(size_t)i * 2 + 1];
                if (uu == 0.0F && vv == 0.0F) ++uv_zero;
                u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                v0 = std::min(v0, vv); v1 = std::max(v1, vv);
            }
            std::print("[dragonbones] frame={} entity={} verts={} idx={} batches={} slots={}"
                       " local_bbox=({:.1f},{:.1f},{:.1f},{:.1f}) screen_bbox=({:.1f},{:.1f})-({:.1f},{:.1f})"
                       " scale={:.2f} angle={:.1f} origin=({:.1f},{:.1f}) uv_zero={}/{} uv_span=u[{:.4f}..{:.4f}]v[{:.4f}..{:.4f}]\n",
                       (long long)frame, db.entity, out.vertex_count, out.index_count, out.batch_count,
                       out.slot_count, out.bounds_x, out.bounds_y, out.bounds_w, out.bounds_h,
                       minx, miny, maxx, maxy, scale, angle, ox, oy, uv_zero, vc, u0, u1, v0, v1);
        }
        /* transient 回写：自省/脚本读得到"在播什么、播到哪"（不进快照/校验和） */
        sc->state.time = mq_db_instance_time(db.inst);
        sc->state.duration = mq_db_instance_duration(db.inst);
        sc->state.play_times = mq_db_instance_play_times(db.inst);
        sc->state.loop = loop;
        sc->state.complete = mq_db_instance_complete(db.inst) != 0;
    }
}

void Engine::dragonbones_extract(std::vector<mq_renderer_mesh>& out) {
    for (auto& db : dragonbones_rt.items) {
        if (!db.inst || db.draw.vertex_count <= 0 || db.draw.index_count <= 0) continue;
        out.reserve(out.size() + (size_t)db.draw.batch_count);
        for (int b = 0; b < db.draw.batch_count; ++b) {
            const mq_db_batch& sb = db.batches[(size_t)b];
            mq_renderer_mesh m{};
            m.vertices = db.tverts.data();
            m.uvs = db.uvs.data();
            m.colors = db.cols.data();
            m.dark_colors = nullptr;             // DragonBones 无两色着色通道
            m.indices = db.idxs.data();
            m.vertex_count = (uint32_t)db.draw.vertex_count;   // 少了它 = 渲染器静默不画
            m.first_index = sb.first_index;
            m.index_count = sb.index_count;
            m.base_vertex = 0;
            m.blend = sb.blend;
            m.layer = 0;
            m.z = db.z;
            m.texture = -1;
            if (sb.page >= 0 && sb.page < (int)db.pages.size()) {
                const auto& p = db.pages[(size_t)sb.page];
                m.pma = 0;
                if (p.handle > 0) {
                    const int tex = (int)(p.handle - 1);
                    if (rapi && rapi->has_texture && rapi->has_texture(rself, tex)) m.texture = tex;
                }
            }
            out.push_back(m);
        }
    }
}

}  // namespace maque::rt
