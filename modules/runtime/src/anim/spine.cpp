/* Spine 2D（docs/27）：装配期建实例、帧内步进、提取期出网格图元
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S5，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- Spine 2D（docs/27）：装配期建实例、帧内步进、提取期出网格图元 ----
 *
 * 分层与数据流（每一层只做一件事）：
 *   VFS 读 .skel/.atlas ──► mq_spine_data_create（骨架 + 图集，页纹理走回调）
 *   纹理回调：页名 → VFS 路径 → image 解码 → rapi->create_texture → 回填非零句柄
 *   每帧：mq_spine_instance_step(dt*speed) → mq_spine_instance_draw(CPU 顶点/UV/颜色/索引/批次)
 *         → 顶点做"骨架空间 → 渲染目标像素空间"映射（y 向上翻成 y 向下，见 spine_extract 注释）
 *   提取：每个批次 = 一条 mq_renderer_mesh（同一图集页 + 同一混合模式）
 *
 * ⚠️ 实测事实（决定了下面的变换怎么写，别再凭印象改）：
 *   spine 的绘制顶点在**骨架子空间**里 y **向上**（spineboy 站立姿势包围盒 y ∈ [-8, 654]，
 *   脚在 y≈0、头在正 y；spine 编辑器与大多数 2D 运行时一致）。所以映射到引擎的
 *   "屏幕像素、y 向下"必须**翻一次 y**（H - world_y - local_y），而不是"直接用"。 */

/* 页纹理回调：spine 把页名交给我们，我们按 (atlas 目录 + 页名) 去 VFS 读 → 解码 → 上传。
 * 句柄约定：0 = 未回填（spine 侧语义），而 gfx 的纹理 id 从 0 开始 → 句柄 = id + 1。 */
static int spine_texture_cb(void* user, const char* page_name, const void* /*file_data*/,
                            size_t /*size*/, uint64_t* out_texture) {
    auto* cx = static_cast<SpineTexCtx*>(user);
    if (!cx || !cx->eng || !page_name || !*page_name || !out_texture) return MQ_SPINE_ERR_ARG;
    *out_texture = 0;
    const std::string path = cx->dir + page_name;
    const int tex = cx->eng->load_texture(path);
    if (tex < 0) {
        std::print(stderr, "runtime: spine atlas page read failed '{}'\n", path);
        return MQ_SPINE_ERR_PAGE;
    }
    *out_texture = (uint64_t)tex + 1u;      // 非零（0 = 未回填）
    return MQ_SPINE_OK;
}

/* 路径归一：组件里的资源引用可以是 "/art/x.skel" 或 "art/x.skel" → VFS 一律要前导 '/'。 */
static std::string vfs_path_of(const std::string& ref) {
    return (!ref.empty() && ref[0] == '/') ? ref : ("/" + ref);
}
static std::string dir_of(const std::string& path) {          // "/art/x.atlas" → "/art/"
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string("/") : path.substr(0, slash + 1);
}
static std::string stem_of(const std::string& path) {         // "/art/x.skel" → "/art/x"
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

SpineRuntime::Instance* Engine::spine_of(int entity) {
    for (auto& sp : spine_rt.items) if (sp.entity == entity) return &sp;
    return nullptr;
}

/* 事件回调（spine2d → 引擎）：把 spine 的动画事件**翻译成引擎的脚本事件**。
 *
 * 为什么走这条路（而不是让脚本轮询 ABI）：docs/14 的事件总线是引擎唯一的脚本回调路径，
 * 按**处理器名**路由 → JS/Lua 语言无关、投递顺序确定、错误隔离都在总线里做好了。
 * 同一份事实还留了 pull 面（spine_event_count/…），push 与 pull 是同一次记录（有单测对拍）。
 *
 * 命名约定（与官方 demo 的 5 类事件一一对应，多一个用户事件）：
 *   start→"spine_start" interrupt→"spine_interrupt" end→"spine_end"
 *   complete→"spine_complete" dispose→"spine_dispose" event→"spine_event"
 * 处理器值 = 轨下标；用户事件的细节（名字/int/float/string）用 spine_event_* 取。
 *
 * ⚠️ 单线程、帧内同步调用（spine 的 AnimationState 就是这样），无线程、无异步 → 确定性不变（docs/10）。 */
static void spine_event_cb(void* user, mq_spine_instance* /*inst*/, const mq_spine_event* ev) {
    auto* sp = static_cast<SpineRuntime::Instance*>(user);
    if (!sp || !sp->eng || !ev) return;
    Engine& eng = *sp->eng;
    const char* name = nullptr;
    switch (ev->type) {
    case MQ_SPINE_EVENT_START:     name = "spine_start"; break;
    case MQ_SPINE_EVENT_INTERRUPT: name = "spine_interrupt"; break;
    case MQ_SPINE_EVENT_END:       name = "spine_end"; break;
    case MQ_SPINE_EVENT_COMPLETE:  name = "spine_complete"; break;
    case MQ_SPINE_EVENT_DISPOSE:   name = "spine_dispose"; break;
    case MQ_SPINE_EVENT_EVENT:     name = "spine_event"; break;
    default: return;
    }
    /* 值语义：用户事件带 int 值（官方 demo 的 "%d event: %s, %d, %f, %s" 里那个 int），
     * 其余事件带 track —— 都只是"标量提示"，精确字段由 pull 面给（docs/14 载荷只有标量）。 */
    const float value = (ev->type == MQ_SPINE_EVENT_EVENT) ? (float)ev->int_value : (float)ev->track;
    /* 重入护栏：脚本处理器里再调 spine_play 会同步触发新事件 → 再投递 → 若处理器又 play，
     * 就会无限递归到爆栈。正常链式播放只需要 1~2 层，故限深 8；超限只跳过**推送**
     *（事件本身仍进 pull 记录，数据不丢），并明确报一次。 */
    if (eng.spine_event_depth >= 8) {
        static bool warned = false;
        if (!warned) { warned = true;
            std::print(stderr, "runtime: spine 事件推送重入超过 8 层 → 后续只记录不推送（脚本处理器疑似自递归）\n"); }
        return;
    }
    ++eng.spine_event_depth;
    host::node_emit_number(&eng, (float)sp->entity, name, value);
    --eng.spine_event_depth;
}

void Engine::spine_release() {
    for (auto& sp : spine_rt.items) {
        if (sp.inst) { mq_spine_instance_destroy(sp.inst); sp.inst = nullptr; }
        if (sp.data) { mq_spine_data_destroy(sp.data); sp.data = nullptr; }   // 页纹理归引擎（tex_cache），此处不释放
        /* 运行时合成的皮肤：本引擎是分配者 → 由本引擎释放（ABI 的"分配者释放"约定） */
        for (mq_spine_skin* sk : sp.own_skins) mq_spine_skin_destroy(sk);
        sp.own_skins.clear();
        sp.composed = nullptr;
    }
    spine_rt.items.clear();
}

/* 帧边界：清空所有实例的事件记录。
 * 位置很关键 —— 必须在**脚本 update 之后、spine_step 之前**：
 *   · spine_step 在本帧脚本之后跑 → 它产生的事件留到下一帧脚本，才 pull 得到；
 *   · 脚本自己调 spine_play 产生的 start 也在本帧脚本之后才被清 → 下一帧同样 pull 得到。
 * 若放在脚本 update **之前**清（踩过）：上一帧 step 的事件会在脚本读之前被抹掉，
 * spine_event_count 永远是 0（push 面照常工作，但 pull 面瞎了）。 */
void Engine::spine_clear_events() {
    for (auto& sp : spine_rt.items) {
        if (!sp.inst) continue;
        sp.events_last = mq_spine_instance_event_count(sp.inst);
        mq_spine_instance_clear_events(sp.inst);
    }
}

void Engine::spine_setup() {
    if (!scene || !vfs || !rapi) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        const mq_type_desc* d = nullptr;
        for (auto& [tid, blob] : e->comps) {
            if (blob.desc && (blob.desc->role & MQ_ROLE_SPINE)) { d = blob.desc; break; }
        }
        if (!d) continue;
        auto& blob = component_slot(*e, d).bytes;
        const std::string* skelref = read_str(d, blob.data(), "skeleton");
        if (!skelref || skelref->empty()) {
            std::print(stderr, "runtime: spine entity {} missing 'skeleton' ref\n", id);
            continue;
        }
        SpineRuntime::Instance sp;
        sp.entity = id;
        sp.comp = d->type_id;
        sp.skel_path = vfs_path_of(*skelref);
        /* 图集路径：① 组件的 atlas 字段（显式）；② material 字段（spine 约定：指向 .atlas）；
         * ③ 与骨架同名的 .atlas（约定，最省事） */
        if (const std::string* ap = read_str(d, blob.data(), "atlas"); ap && !ap->empty())
            sp.atlas_path = vfs_path_of(*ap);
        else if (const std::string* mp = read_str(d, blob.data(), "material"); mp && !mp->empty())
            sp.atlas_path = vfs_path_of(*mp);
        else
            sp.atlas_path = stem_of(sp.skel_path) + ".atlas";
        sp.tex_dir = dir_of(sp.atlas_path);

        /* 骨架：从内存进（.skel = 二进制，.json = 文本；docs/27 的实测约定） */
        std::string ext;
        { const size_t dot = sp.skel_path.rfind('.');
          if (dot != std::string::npos) ext = sp.skel_path.substr(dot); }
        const int is_binary = (ext == ".json") ? 0 : 1;
        void* sd = nullptr; size_t sn = 0;
        if (mq_vfs_read(vfs, sp.skel_path.c_str(), &sd, &sn) != MQ_VFS_OK || !sd || sn == 0) {
            std::print(stderr, "runtime: spine skeleton read failed '{}'\n", sp.skel_path);
            continue;
        }
        std::vector<char> skel((char*)sd, (char*)sd + sn);
        mq_vfs_free(sd);
        void* ad = nullptr; size_t an = 0;
        if (mq_vfs_read(vfs, sp.atlas_path.c_str(), &ad, &an) != MQ_VFS_OK || !ad || an == 0) {
            std::print(stderr, "runtime: spine atlas read failed '{}'"
                               " (set the component's material field to the .atlas path)\n", sp.atlas_path);
            continue;
        }
        std::string atlas_text((char*)ad, an);
        mq_vfs_free(ad);
        atlas_text.push_back('\0');                            // spine 内部按 C 字符串解析

        sp.tex_ctx.eng = this;
        sp.tex_ctx.dir = sp.tex_dir;
        sp.data = mq_spine_data_create(skel.data(), sn, is_binary, atlas_text.c_str(), "",
                                       spine_texture_cb, &sp.tex_ctx);
        if (!sp.data) {
            std::print(stderr, "runtime: spine parse failed '{}' (binary={})\n", sp.skel_path, is_binary);
            continue;
        }
        sp.inst = mq_spine_instance_create(sp.data);
        if (!sp.inst) { mq_spine_data_destroy(sp.data); sp.data = nullptr; continue; }

        const int n_anim = mq_spine_data_animation_count(sp.data);
        const int n_skin = mq_spine_data_skin_count(sp.data);
        const int n_page = mq_spine_data_page_count(sp.data);
        for (int i = 0; i < n_anim; ++i) {
            const char* nm = mq_spine_data_animation_name(sp.data, i);
            sp.anim_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_skin; ++i) {
            const char* nm = mq_spine_data_skin_name(sp.data, i);
            sp.skin_names.emplace_back(nm ? nm : "");
        }
        for (int i = 0; i < n_page; ++i) {
            mq_spine_page pg{};
            if (mq_spine_data_page(sp.data, i, &pg) != MQ_SPINE_OK) continue;
            SpineRuntime::Instance::Page p;
            p.name = pg.name ? pg.name : "";
            p.width = pg.width; p.height = pg.height; p.handle = pg.texture; p.pma = pg.pma;
            sp.pages.push_back(std::move(p));
        }
        /* 初值：皮肤 / 动画 / 循环（组件字段是稳定路径，见 schema/world/anim.yaml） */
        int start_anim = (int)read_num(d, blob.data(), "animation", 0.0F);
        if (start_anim < 0 || start_anim >= n_anim) start_anim = 0;
        const int skin_idx = (int)read_num(d, blob.data(), "skin", 0.0F);
        if (skin_idx > 0 && skin_idx < n_skin)
            mq_spine_instance_set_skin(sp.inst, sp.skin_names[(size_t)skin_idx].c_str());
        /* 混合/时间缩放/槽区间：组件字段 → 实例（脚本可随时覆盖；见 host.yaml 的 spine_* 段） */
        mq_spine_instance_set_default_mix(sp.inst, read_num(d, blob.data(), "default_mix", 0.0F));
        mq_spine_instance_set_time_scale(sp.inst, read_num(d, blob.data(), "time_scale", 1.0F));
        const int sf = (int)read_num(d, blob.data(), "slots_from", -1.0F);
        const int st = (int)read_num(d, blob.data(), "slots_to", -1.0F);
        if (sf >= 0) mq_spine_instance_set_slots_range(sp.inst, sf, st);
        const bool autoplay = read_num(d, blob.data(), "autoplay", 1.0F) != 0.0F;
        if (autoplay && n_anim > 0) {
            const bool loop = read_num(d, blob.data(), "play_mode", 1.0F) == 1.0F;   // PlayMode::Loop = 1
            if (mq_spine_instance_play(sp.inst, sp.anim_names[(size_t)start_anim].c_str(), loop ? 1 : 0, 0) == MQ_SPINE_OK)
                sp.last_anim = start_anim;
        }
        /* 绘制缓冲：按"顶点数 ≈ 附件数 × 段数"的规模预留（骨架 4k 顶点足够；不足时下一帧扩容重试） */
        sp.verts.resize(8192); sp.uvs.resize(8192); sp.cols.resize(16384); sp.darks.resize(16384);
        sp.tverts.resize(8192);
        sp.idxs.resize(32768); sp.batches.resize(512);
        sp.z = (int)read_num(d, blob.data(), "z_index", 0.0F) * 100000 + id;
        /* instance 字段（transient）：给自省/脚本一个"实例存在"的证据（下标 + 1，0 = 无） */
        if (const mq_field_desc* f = mq_reflect_field(d, "instance"))
            *(uint64_t*)(blob.data() + f->offset) = (uint64_t)(spine_rt.items.size() + 1);
        std::print("runtime: spine entity {} '{}' skel='{}' atlas='{}' anims={} skins={} pages={}\n",
                   id, e->name, sp.skel_path, sp.atlas_path, n_anim, n_skin, n_page);
        if (!sp.pages.empty()) {
            std::print("runtime: spine pages:");
            for (const auto& p : sp.pages)
                std::print(" [{} {}x{} tex={} pma={}]", p.name, p.width, p.height, (long long)p.handle, p.pma);
            std::print("\n");
        }
        spine_rt.items.push_back(std::move(sp));
    }
    spine_rt.items.shrink_to_fit();          // 之后不再增删 → tex_ctx / eng / 事件回调 user 的地址稳定
    /* 事件回调：**必须在向量定型之后**装（user = &spines[i]，push_back 会让旧地址失效）。
     * 装完即生效：之后任何 play/addAnimation/step 触发的动画事件都会同步投给脚本总线。 */
    for (auto& sp : spine_rt.items) {
        sp.eng = this;
        mq_spine_instance_set_event_cb(sp.inst, spine_event_cb, &sp);
    }
    std::print("runtime: spine instances={}\n", spine_rt.items.size());
}

void Engine::spine_step(float dt) {
    if (!scene) return;
    for (auto& sp : spine_rt.items) {
        if (!sp.inst || !sp.data) continue;
        SceneNode* e = scene->at(sp.entity);
        auto* sc = e ? component<anim::SpineSkeleton>(*e) : nullptr;
        if (!sc) continue;

        /* 组件字段 → 语义（脚本改这些字段即生效；无需额外 API 也能驱动动画）。
         * 类型在编译期已知 → 直接成员读，不再每帧 12 次按名反射。 */
        const int n_anim = (int)sp.anim_names.size();
        int want = (int)sc->animation;
        if (want < 0 || want >= n_anim) want = sp.last_anim;
        const bool loop = sc->play_mode == anim::PlayMode::Loop;
        if (want != sp.last_anim && want >= 0) {                 // 组件里换了动画 → 真的去播
            /* 但若该轨**已经在播**这个动画（例如 add_animation 排队到点自己切过去了），
             * 只同步 last_anim，不要重启（重启会白白打断播放头/触发一次 start+interrupt）。 */
            const char* cur = mq_spine_instance_track_animation(sp.inst, 0);
            if (cur && sp.anim_names[(size_t)want] == cur) {
                sp.last_anim = want;
            } else if (mq_spine_instance_play(sp.inst, sp.anim_names[(size_t)want].c_str(), loop ? 1 : 0, 0) == MQ_SPINE_OK) {
                sp.last_anim = want;
            }
        }
        const float speed = sc->speed;
        /* 物理时间轴：SpinePhysTimestep::NoPhysics(2) → 只应用已有物理姿势（Physics_Pose），
         * 其余（Inherit/Fixed）→ 正常更新（Physics_Update）。忽略这个字段会让"冻结物理"失效。 */
        const int physics = (sc->phys_timestep == anim::SpinePhysTimestep::NoPhysics)
                                ? MQ_SPINE_PHYSICS_POSE : MQ_SPINE_PHYSICS_UPDATE;
        mq_spine_instance_step_ex(sp.inst, dt * speed, physics);  // 固定步长（确定性，docs/10 D1）

        /* 变换：模块负责 缩放/翻转（骨架单位 → 屏幕单位），旋转/平移留给提取期（见 spine_extract） */
        float angle = 0.0F, scale = 1.0F, opacity = 1.0F;
        accumulate_node_attrs(*this, sp.entity, angle, scale, opacity);
        const int flip = (int)sc->flip;       // FlipMode：1=水平 2=垂直 3=两者
        mq_spine_instance_set_transform(sp.inst, 0.0F, 0.0F, scale,
                                        (flip & 1) ? 1 : 0, (flip & 2) ? 1 : 0);
        const math::Color& tint = sc->color;
        mq_spine_instance_set_color(sp.inst, tint.r, tint.g, tint.b, tint.a * opacity);

        /* 绘制数据（CPU 侧；只读导出，不改模拟状态） */
        mq_spine_draw out{};
        const int rc = mq_spine_instance_draw(sp.inst, sp.verts.data(), sp.uvs.data(), sp.cols.data(),
                                              sp.darks.data(), sp.idxs.data(), sp.batches.data(),
                                              (int)(sp.verts.size() / 2), (int)sp.idxs.size(),
                                              (int)sp.batches.size(), &out);
        if (rc != MQ_SPINE_OK) { sp.draw = mq_spine_draw{}; continue; }
        if (out.vertex_count >= (int)(sp.verts.size() / 2) || out.index_count >= (int)sp.idxs.size()) {
            /* 缓冲满 = 几何被截断（会少画附件）→ 翻倍后下一帧重试，并明确报一次 */
            sp.verts.resize(sp.verts.size() * 2); sp.uvs.resize(sp.uvs.size() * 2);
            sp.cols.resize(sp.cols.size() * 2); sp.darks.resize(sp.darks.size() * 2);
            sp.tverts.resize(sp.tverts.size() * 2);
            sp.idxs.resize(sp.idxs.size() * 2); sp.batches.resize(sp.batches.size() * 2);
            static bool warned = false;
            if (!warned) { warned = true;
                std::print(stderr, "runtime: spine draw buffers full -> growing"
                                   " (this frame's geometry may be truncated)\n"); }
        }
        sp.draw = out;
        sp.z = (int)sc->z_index * 100000 + sp.entity;

        /* 骨架空间 → 渲染目标像素空间：
         *   · spine 顶点 y **向上**（实测；见本节顶部注释），引擎渲染目标 y 向下 → 翻一次；
         *   · 累计节点角度绕实体原点旋转，再平移到实体世界位置；
         *   · world 是 y 向上的世界坐标，屏幕 y = H - world_y。 */
        const float rad = angle * 3.14159265358979323846F / 180.0F;
        const float cs = std::cos(rad), sn = std::sin(rad);
        const float ox = e->world_x, oy = (float)H - e->world_y;
        const int vc = out.vertex_count;
        if ((int)(sp.tverts.size() / 2) < vc) sp.tverts.resize((size_t)vc * 2);
        for (int i = 0; i < vc; ++i) {
            const float lx = sp.verts[(size_t)i * 2], ly = sp.verts[(size_t)i * 2 + 1];
            const float rx = lx * cs - ly * sn;
            const float ry = lx * sn + ly * cs;
            sp.tverts[(size_t)i * 2] = ox + rx;
            sp.tverts[(size_t)i * 2 + 1] = oy - ry;
        }
        /* 诊断（MAQUE_TRACE_SPINE=1）：绘制数据规模 / 变换前后包围盒 / UV 统计 —— 排查"看不见"的第一手证据
         * （实测靠它定位过两个坑：mesh.vertex_count 忘填 → 渲染器静默不画；UV 取错 accessor → 全采样到空白） */
        if (std::getenv("MAQUE_TRACE_SPINE") && (frame % 20 == 0)) {
            float minx = 0, maxx = 0, miny = 0, maxy = 0;
            int uv_zero = 0; float u0 = 2, u1 = -1, v0 = 2, v1 = -1;
            for (int i = 0; i < vc; ++i) {
                const float x = sp.tverts[(size_t)i * 2], y = sp.tverts[(size_t)i * 2 + 1];
                if (i == 0) { minx = maxx = x; miny = maxy = y; }
                else { minx = std::min(minx, x); maxx = std::max(maxx, x);
                       miny = std::min(miny, y); maxy = std::max(maxy, y); }
                const float uu = sp.uvs[(size_t)i * 2], vv = sp.uvs[(size_t)i * 2 + 1];
                if (uu == 0.0F && vv == 0.0F) ++uv_zero;
                u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                v0 = std::min(v0, vv); v1 = std::max(v1, vv);
            }
            std::print("[spine] frame={} entity={} verts={} idx={} batches={} local_bbox=({:.1f},{:.1f},{:.1f},{:.1f})"
                       " screen_bbox=({:.1f},{:.1f})-({:.1f},{:.1f}) scale={:.2f} angle={:.1f} origin=({:.1f},{:.1f})"
                       " pages={} uv_zero={}/{} uv_span=u[{:.4f}..{:.4f}]v[{:.4f}..{:.4f}] events={}\n",
                       (long long)frame, sp.entity, out.vertex_count, out.index_count, out.batch_count,
                       out.bounds_x, out.bounds_y, out.bounds_w, out.bounds_h,
                       minx, miny, maxx, maxy, scale, angle, ox, oy, sp.pages.size(),
                       uv_zero, vc, u0, u1, v0, v1, sp.events_last);
        }
        /* transient 回写：自省/脚本能读到"正在播什么、播到哪"（不进快照/校验和） */
        sc->state.time = mq_spine_instance_time(sp.inst, 0);
        sc->state.track = 0;
        sc->state.loop = loop;
    }
}

void Engine::spine_extract(std::vector<mq_renderer_mesh>& out) {
    for (auto& sp : spine_rt.items) {
        if (!sp.inst || sp.draw.vertex_count <= 0 || sp.draw.index_count <= 0) continue;
        out.reserve(out.size() + (size_t)sp.draw.batch_count);        for (int b = 0; b < sp.draw.batch_count; ++b) {
            const mq_spine_batch& sb = sp.batches[(size_t)b];
            mq_renderer_mesh m{};
            m.vertices = sp.tverts.data();
            m.uvs = sp.uvs.data();
            m.colors = sp.cols.data();
            m.dark_colors = sp.darks.data();     // 两色着色（TintBlack）暗色通道；长度同 colors
            m.indices = sp.idxs.data();
            m.vertex_count = (uint32_t)sp.draw.vertex_count;     // 顶点数（少了它 = 渲染器不画，且是静默的）
            m.first_index = sb.first_index;
            m.index_count = sb.index_count;
            m.base_vertex = 0;
            m.blend = sb.blend;
            m.layer = 0;
            m.z = sp.z;
            m.texture = -1;
            if (sb.page >= 0 && sb.page < (int)sp.pages.size()) {
                const SpineRuntime::Instance::Page& p = sp.pages[(size_t)sb.page];
                m.pma = p.pma;
                /* 句柄 = gfx 纹理 id + 1（0 表示未回填）→ 还原成引擎纹理 id 交给渲染器 */
                if (p.handle > 0) {
                    const int tex = (int)(p.handle - 1);
                    if (rapi && rapi->has_texture && rapi->has_texture(rself, tex)) m.texture = tex;
                }
            }
            out.push_back(m);
        }
    }
}

/* 批量截图落地：模板 → 文件名 → 写图；并判断"是不是最后一张"（是则退出）。
 * 模板里的 `{}` 换成帧号；没有 `{}` 就自动插到扩展名前（`out.png` → `out_0042.png`）。
 * 格式仍按扩展名（.png → PNG，其余 → PPM）。 */
void Engine::write_seq_shot(const std::vector<uint8_t>& px, int tw, int th) {
    std::string path = shot_seq;
    char num[32];
    std::snprintf(num, sizeof num, "%d", (int)frame);
    const size_t brace = path.find("{}");
    if (brace != std::string::npos) {
        path.replace(brace, 2, num);
    } else {
        const size_t dot = path.find_last_of('.');
        char pad[32];
        std::snprintf(pad, sizeof pad, "_%04d", (int)frame);
        if (dot == std::string::npos) path += pad;
        else path.insert(dot, pad);
    }
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    const int rc = png ? mq_image_write_png(path.c_str(), (uint32_t)tw, (uint32_t)th,
                                            px.data(), (uint32_t)tw * 4u)
                       : (write_ppm(path, tw, th, px) ? 0 : 1);
    if (rc == 0) std::print("runtime: shot[{}] '{}' {}x{} frame={}\n",
                            shots_taken + 1, path, tw, th, (int)frame);
    else std::print(stderr, "runtime: shot 写入失败 '{}'\n", path);
    ++shots_taken;
    /* 收尾条件：显式帧号列表 → 拍完最大帧即退；every 模式 → 拍够 count（count=0 → 由
     * MAQUE_MAX_FRAMES 控制，不自己退）。 */
    if (!shot_list.empty()) {
        const int last = *std::max_element(shot_list.begin(), shot_list.end());
        if ((int)frame >= last) quit = true;
    } else if (shot_count > 0 && shots_taken >= shot_count) {
        quit = true;
    }
}

}  // namespace maque::rt
