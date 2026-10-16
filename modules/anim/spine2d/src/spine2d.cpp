/* maque::spine2d —— Spine 2D 官方运行时（spine-cpp）的 C ABI 封装。
 *
 * 分层（docs/assets/06 §2、docs/02）：
 *   · 本模块只依赖 spine-cpp + 标准库；**不碰 GPU、不解码纹理**。
 *   · 骨架与图集从**内存**进来（调用方用 VFS 读），图集页纹理通过回调交给引擎侧解码上传，
 *     回填句柄后再查询。
 *   · 对外只有 C ABI + 不透明句柄，分配者释放（句柄由本模块分配，故由本模块销毁）。
 *
 * 确定性（docs/10）：不做任何内部计时、不开线程、不用系统熵；时间只由 dt / set_time 驱动。
 * 渲染只读：mq_spine_draw 只填调用方缓冲，不改任何模拟状态。 */
#include "anim/mq_spine2d.h"

#include <spine/spine.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <vector>

using namespace spine;

namespace {

/* ---- 图集页纹理回调适配：spine 的 TextureLoader 把页名交给我们，我们去问引擎 ---- */
struct CallbackTextureLoader : public TextureLoader {
    mq_spine_texture_cb cb = nullptr;
    void *user = nullptr;
    /* 跨页目录前缀（图集里写的是相对文件名，实际读取由引擎侧按页名再查 VFS） */
    std::string dirHint;
    /* spine 会为每页调用一次 load()；我们记下页名 → 纹理句柄，供 mq_spine_data_page 用 */
    std::vector<std::pair<std::string, uint64_t>> pages;
    std::vector<std::string> failed;

    void load(AtlasPage &page, const String &path) override {
        const std::string name(path.buffer(), path.length());
        uint64_t tex = 0;
        int rc = MQ_SPINE_ERR_PAGE;
        if (cb) {
            /* file_data = nullptr：纹理文件由引擎侧自己按页名去 VFS 取（它才知道挂载点）。
             * 这里只传页名与目录提示，回调负责读文件 + 解码 + 上传 + 返回句柄。 */
            rc = cb(user, name.c_str(), nullptr, 0, &tex);
        }
        if (rc != MQ_SPINE_OK || tex == 0) {
            failed.push_back(name);
            page.texture = nullptr;
            return;
        }
        page.texture = reinterpret_cast<void *>(static_cast<uintptr_t>(tex));
        pages.emplace_back(name, tex);
    }

    void unload(void * /*texture*/) override {
        /* 纹理归引擎所有（它创建、它销毁）；这里不释放，避免双重释放（docs/02 分配者释放）。 */
    }
};

/* 页名 → 下标（spine 的 AtlasPage 指针 → 我们导出的下标） */
int page_index_of(const std::vector<AtlasPage *> &pages, AtlasPage *p) {
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i] == p) return static_cast<int>(i);
    return -1;
}

} // namespace

/* 不透明句柄的真实类型（C ABI 只暴露前置声明） */
struct mq_spine_data {
    SkeletonData *skeletonData = nullptr;
    Atlas *atlas = nullptr;                       // 拥有，负责销毁页与区域
    CallbackTextureLoader *loader = nullptr;      // 拥有
    std::vector<AtlasPage *> pages;               // atlas 拥有；这里只存指针做下标映射
    std::vector<std::string> animNames;           // 稳定序（声明序）
    std::vector<std::string> skinNames;
};

struct mq_spine_instance {
    mq_spine_data *owner = nullptr;
    Skeleton *skeleton = nullptr;
    AnimationStateData *stateData = nullptr;
    AnimationState *state = nullptr;
    float x = 0.0F, y = 0.0F, scale = 1.0F;
    bool flipX = false, flipY = false;
    float r = 1.0F, g = 1.0F, b = 1.0F, a = 1.0F;
    /* 复用的临时缓冲：避免每帧分配（绘制数据是逐帧派生的） */
    std::vector<float> worldVerts;
    std::vector<float> uvBuf;
    std::vector<float> colBuf;
    std::vector<uint32_t> idxBuf;
    std::vector<mq_spine_batch> batchBuf;
};

extern "C" {

uint32_t mq_spine_abi_version(void) { return MQ_SPINE2D_ABI_VERSION; }

mq_spine_data *mq_spine_data_create(const void *skeleton_data, size_t skeleton_size,
                                    int skeleton_is_binary, const char *atlas_data,
                                    const char *atlas_dir_hint, mq_spine_texture_cb load_texture,
                                    void *user) {
    if (!skeleton_data || skeleton_size == 0 || !atlas_data) return nullptr;

    auto *d = new mq_spine_data();

    /* 1) 图集：spine 的 Atlas 支持"从内存创建"（docs/assets/06 §2 要求数据可来自 VFS/远程）。
     *    页纹理不走它的文件读取，而是走我们的 TextureLoader 回调交给引擎。 */
    d->loader = new CallbackTextureLoader();
    d->loader->cb = load_texture;
    d->loader->user = user;
    if (atlas_dir_hint) d->loader->dirHint = atlas_dir_hint;

    const char *dir = atlas_dir_hint ? atlas_dir_hint : "";
    d->atlas = new Atlas(atlas_data, static_cast<int>(std::strlen(atlas_data)), dir, d->loader, true);
    d->pages = d->atlas->getPages();

    /* 骨架解析：纹理已由 loader 交给引擎，故不创建纹理（createTexture 参数只作用于图集）。 */
    AtlasAttachmentLoader attachmentLoader(*d->atlas);
    SkeletonData *sd = nullptr;
    if (skeleton_is_binary) {
        SkeletonBinary binary(attachmentLoader);
        binary.setScale(1.0F);
        sd = binary.readSkeletonData(static_cast<const unsigned char *>(skeleton_data),
                                     static_cast<int>(skeleton_size));
        if (!sd) {
            std::print(stderr, "spine2d: 二进制骨架解析失败: {}\n", binary.getError().buffer());
            delete d->atlas;
            delete d->loader;
            delete d;
            return nullptr;
        }
    } else {
        SkeletonJson json(attachmentLoader);
        json.setScale(1.0F);
        sd = json.readSkeletonData(static_cast<const char *>(skeleton_data));
        if (!sd) {
            std::print(stderr, "spine2d: JSON 骨架解析失败: {}\n", json.getError().buffer());
            delete d->atlas;
            delete d->loader;
            delete d;
            return nullptr;
        }
    }
    d->skeletonData = sd;

    /* 动画/皮肤名快照：**声明序**（稳定，docs/10 D4），供 UI/AI 按稳定下标枚举 */
    for (size_t i = 0; i < sd->getAnimations().size(); ++i)
        d->animNames.emplace_back(sd->getAnimations()[i]->getName().buffer());
    for (size_t i = 0; i < sd->getSkins().size(); ++i)
        d->skinNames.emplace_back(sd->getSkins()[i]->getName().buffer());
    return d;
}

void mq_spine_data_destroy(mq_spine_data *data) {
    if (!data) return;
    delete data->skeletonData;
    delete data->atlas;   // 拥有页与区域；页纹理归引擎，loader::unload 故意不释放
    delete data->loader;
    delete data;
}

int mq_spine_data_animation_count(mq_spine_data *data) {
    return data ? static_cast<int>(data->animNames.size()) : 0;
}
const char *mq_spine_data_animation_name(mq_spine_data *data, int index) {
    if (!data || index < 0 || index >= static_cast<int>(data->animNames.size())) return nullptr;
    return data->animNames[static_cast<size_t>(index)].c_str();
}
int mq_spine_data_animation_index(mq_spine_data *data, const char *name) {
    if (!data || !name) return -1;
    for (size_t i = 0; i < data->animNames.size(); ++i)
        if (data->animNames[i] == name) return static_cast<int>(i);
    return -1;
}
int mq_spine_data_skin_count(mq_spine_data *data) {
    return data ? static_cast<int>(data->skinNames.size()) : 0;
}
const char *mq_spine_data_skin_name(mq_spine_data *data, int index) {
    if (!data || index < 0 || index >= static_cast<int>(data->skinNames.size())) return nullptr;
    return data->skinNames[static_cast<size_t>(index)].c_str();
}
int mq_spine_data_page_count(mq_spine_data *data) {
    return data ? static_cast<int>(data->pages.size()) : 0;
}
int mq_spine_data_page(mq_spine_data *data, int index, mq_spine_page *out) {
    if (!data || !out || index < 0 || index >= static_cast<int>(data->pages.size()))
        return MQ_SPINE_ERR_ARG;
    AtlasPage *p = data->pages[static_cast<size_t>(index)];
    out->name = p ? p->name.buffer() : nullptr;
    out->width = p ? p->width : 0;
    out->height = p ? p->height : 0;
    out->texture = p ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p->texture)) : 0;
    return MQ_SPINE_OK;
}
int mq_spine_data_page_texture(mq_spine_data *data, int index, uint64_t texture) {
    if (!data || index < 0 || index >= static_cast<int>(data->pages.size()))
        return MQ_SPINE_ERR_ARG;
    AtlasPage *p = data->pages[static_cast<size_t>(index)];
    if (!p) return MQ_SPINE_ERR_ARG;
    p->texture = reinterpret_cast<void *>(static_cast<uintptr_t>(texture));
    return MQ_SPINE_OK;
}

mq_spine_instance *mq_spine_instance_create(mq_spine_data *data) {
    if (!data || !data->skeletonData) return nullptr;
    auto *inst = new mq_spine_instance();
    if (!inst) return nullptr;
    inst->owner = data;
    inst->skeleton = new Skeleton(*data->skeletonData);
    inst->stateData = new AnimationStateData(*data->skeletonData);
    if (!inst->skeleton || !inst->stateData) {
        delete inst->skeleton;
        delete inst->stateData;
        delete inst;
        return nullptr;
    }
    inst->state = new AnimationState(*inst->stateData);
    if (!inst->state) {
        delete inst->skeleton;
        delete inst->stateData;
        delete inst;
        return nullptr;
    }
    /* setup 姿势 + 世界变换，保证未播放动画时也有正确姿势 */
    inst->skeleton->setupPose();
    inst->skeleton->updateWorldTransform(Physics_Update);
    return inst;
}

void mq_spine_instance_destroy(mq_spine_instance *inst) {
    if (!inst) return;
    delete inst->state;
    delete inst->stateData;
    delete inst->skeleton;
    delete inst;
}

int mq_spine_instance_set_skin(mq_spine_instance *inst, const char *name) {
    if (!inst || !name) return MQ_SPINE_ERR_ARG;
    if (!inst->skeleton->getData().findSkin(String(name))) return MQ_SPINE_ERR_NOTFOUND;
    inst->skeleton->setSkin(String(name));
    inst->skeleton->setupPoseSlots();
    return MQ_SPINE_OK;
}

int mq_spine_instance_play(mq_spine_instance *inst, const char *animation, int loop, int track) {
    if (!inst || !animation) return MQ_SPINE_ERR_ARG;
    if (track < 0) return MQ_SPINE_ERR_ARG;
    if (!inst->skeleton->getData().findAnimation(String(animation))) return MQ_SPINE_ERR_NOTFOUND;
    inst->state->setAnimation(static_cast<size_t>(track), String(animation), loop != 0);
    return MQ_SPINE_OK;
}

void mq_spine_instance_stop(mq_spine_instance *inst) {
    if (inst && inst->state) inst->state->clearTracks();
}

int mq_spine_instance_set_time(mq_spine_instance *inst, int track, float time) {
    if (!inst || !inst->state || track < 0) return MQ_SPINE_ERR_ARG;
    TrackEntry *e = inst->state->getTrack(static_cast<size_t>(track));
    if (!e) return MQ_SPINE_ERR_NOTFOUND;
    e->setTrackTime(time);
    return MQ_SPINE_OK;
}

int mq_spine_instance_step(mq_spine_instance *inst, float dt) {
    if (!inst || !std::isfinite(dt)) return MQ_SPINE_ERR_ARG;
    /* 固定步语义：由调用方传主循环的固定 dt（docs/10 D1）；本模块不自建计时器 */
    inst->state->update(dt);
    inst->state->apply(*inst->skeleton);
    inst->skeleton->updateWorldTransform(Physics_Update);
    return MQ_SPINE_OK;
}

float mq_spine_instance_time(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return 0.0F;
    TrackEntry *e = inst->state->getTrack(static_cast<size_t>(track));
    return e ? e->getTrackTime() : 0.0F;
}

const char *mq_spine_instance_animation_name(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return nullptr;
    TrackEntry *e = inst->state->getTrack(static_cast<size_t>(track));
    if (!e) return nullptr;
    return e->getAnimation().getName().buffer();
}

int mq_spine_instance_animation_complete(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return 0;
    TrackEntry *e = inst->state->getTrack(static_cast<size_t>(track));
    return (e && e->isComplete()) ? 1 : 0;
}

void mq_spine_instance_set_transform(mq_spine_instance *inst, float x, float y, float scale,
                                     int flip_x, int flip_y) {
    if (!inst) return;
    inst->x = x; inst->y = y;
    inst->scale = (std::isfinite(scale) && scale != 0.0F) ? scale : 1.0F;
    inst->flipX = flip_x != 0;
    inst->flipY = flip_y != 0;
}

void mq_spine_instance_set_color(mq_spine_instance *inst, float r, float g, float b, float a) {
    if (!inst) return;
    inst->r = r; inst->g = g; inst->b = b; inst->a = a;
    inst->skeleton->setColor(Color(r, g, b, a));
}

void mq_spine_instance_color(mq_spine_instance *inst, float *r, float *g, float *b, float *a) {
    if (!inst) return;
    Color c = inst->skeleton->getColor();
    if (r) *r = c.r; if (g) *g = c.g; if (b) *b = c.b; if (a) *a = c.a;
}

int mq_spine_instance_bone_position(mq_spine_instance *inst, const char *bone, float *x, float *y) {
    if (!inst || !bone || !x || !y) return MQ_SPINE_ERR_ARG;
    Bone *bn = inst->skeleton->findBone(String(bone));
    if (!bn) return MQ_SPINE_ERR_NOTFOUND;
    /* 4.3：世界坐标在 pose 上（getAppliedPose 是"渲染用最终姿势"） */
    *x = bn->getAppliedPose().getWorldX();
    *y = bn->getAppliedPose().getWorldY();
    return MQ_SPINE_OK;
}

/* ---- IK 约束访问（spine-cpp 4.3）--------------------------------------------------
 * 4.3 的姿势数值在各自的 Pose 类上（IkConstraintPose），约束对象则由
 * Skeleton::getConstraints() 统一持有（Constraint*，需按类型判别），
 * 所以这里按 IkConstraint 过滤后暴露一个稳定的**下标序列**（= 该数组中的 IK 次序，
 * 确定性，docs/10 D4），而不是把 spine 的指针暴露出去。
 * field: 0=mix 1=softness 2=bend_direction 3=compress 4=stretch */
static IkConstraint *ik_at(mq_spine_instance *inst, int index) {
    if (!inst || index < 0) return nullptr;
    auto &cs = inst->skeleton->getConstraints();
    int k = 0;
    for (size_t i = 0; i < cs.size(); ++i) {
        IkConstraint *ik = dynamic_cast<IkConstraint *>(cs[i]);
        if (!ik) continue;
        if (k == index) return ik;
        ++k;
    }
    return nullptr;
}

int mq_spine_instance_set_float(mq_spine_instance *inst, int /*target*/, int index, int field,
                                float value, float /*blend*/) {
    IkConstraint *ik = ik_at(inst, index);
    if (!ik) return MQ_SPINE_ERR_ARG;
    IkConstraintPose &pose = ik->getAppliedPose();
    switch (field) {
    case 0: pose.setMix(value); return MQ_SPINE_OK;
    case 1: pose.setSoftness(value); return MQ_SPINE_OK;
    case 2: pose.setBendDirection(value >= 0.0F ? 1 : -1); return MQ_SPINE_OK;
    case 3: pose.setCompress(value != 0.0F); return MQ_SPINE_OK;
    case 4: pose.setStretch(value != 0.0F); return MQ_SPINE_OK;
    default: return MQ_SPINE_ERR_ARG;
    }
}

int mq_spine_instance_add_float(mq_spine_instance *inst, int target, int index, int field,
                                float value, float blend) {
    const float cur = mq_spine_instance_get_float(inst, target, index, field);
    float next = cur + value;
    if (std::isfinite(blend) && blend >= 0.0F && blend <= 1.0F)
        next = cur + (next - cur) * blend;
    return mq_spine_instance_set_float(inst, target, index, field, next, blend);
}

float mq_spine_instance_get_float(mq_spine_instance *inst, int /*target*/, int index, int field) {
    IkConstraint *ik = ik_at(inst, index);
    if (!ik) return 0.0F;
    IkConstraintPose &pose = ik->getAppliedPose();
    switch (field) {
    case 0: return pose.getMix();
    case 1: return pose.getSoftness();
    case 2: return static_cast<float>(pose.getBendDirection());
    case 3: return pose.getCompress() ? 1.0F : 0.0F;
    case 4: return pose.getStretch() ? 1.0F : 0.0F;
    default: return 0.0F;
    }
}

int mq_spine_instance_ik_count(mq_spine_instance *inst) {
    if (!inst) return 0;
    auto &cs = inst->skeleton->getConstraints();
    int k = 0;
    for (size_t i = 0; i < cs.size(); ++i)
        if (dynamic_cast<IkConstraint *>(cs[i])) ++k;
    return k;
}
const char *mq_spine_instance_ik_name(mq_spine_instance *inst, int index) {
    IkConstraint *ik = ik_at(inst, index);
    return ik ? ik->getData().getName().buffer() : nullptr;
}
int mq_spine_instance_ik_set_mix(mq_spine_instance *inst, int index, float mix) {
    return mq_spine_instance_set_float(inst, 0, index, 0, mix, 1.0F);
}

/* 变换约束：同样按类型过滤出稳定下标序列（4.3 无 getTransformConstraints()） */
static TransformConstraint *transform_at(mq_spine_instance *inst, int index) {
    if (!inst || index < 0) return nullptr;
    auto &cs = inst->skeleton->getConstraints();
    int k = 0;
    for (size_t i = 0; i < cs.size(); ++i) {
        TransformConstraint *tc = dynamic_cast<TransformConstraint *>(cs[i]);
        if (!tc) continue;
        if (k == index) return tc;
        ++k;
    }
    return nullptr;
}
int mq_spine_instance_transform_count(mq_spine_instance *inst) {
    if (!inst) return 0;
    auto &cs = inst->skeleton->getConstraints();
    int k = 0;
    for (size_t i = 0; i < cs.size(); ++i)
        if (dynamic_cast<TransformConstraint *>(cs[i])) ++k;
    return k;
}
const char *mq_spine_instance_transform_name(mq_spine_instance *inst, int index) {
    TransformConstraint *tc = transform_at(inst, index);
    return tc ? tc->getData().getName().buffer() : nullptr;
}
int mq_spine_instance_transform_position(mq_spine_instance *inst, int index, float *x, float *y) {
    if (!x || !y) return MQ_SPINE_ERR_ARG;
    TransformConstraint *tc = transform_at(inst, index);
    if (!tc) return MQ_SPINE_ERR_ARG;
    Bone *target = tc->getData().getTarget();
    if (!target) return MQ_SPINE_ERR_NOTFOUND;
    /* 变换约束没有"世界位置"；返回其目标骨骼的世界位置（调试/跟随用） */
    *x = target->getAppliedPose().getWorldX();
    *y = target->getAppliedPose().getWorldY();
    return MQ_SPINE_OK;
}

/* ---- 绘制数据导出 ----------------------------------------------------------------
 * 语义（docs/assets/06 §2.4）：**只读**产出"绘制输入"，不写任何模拟状态、不碰 GPU。
 * 顶点已在骨架空间求值好，这里再叠加实例的平移/缩放/翻转，得到"绘制数据空间"坐标
 * （y 向下，与引擎的屏幕像素一致：引擎侧只需再乘画布缩放即可）。
 * 顺序 = 骨架的**绘制顺序**（含 draw order timeline 的效果），批处理按 (页, 混合模式) 划分 —— 
 * 与 spine 官方运行时的分组一致，保证叠加/正片叠底等混合模式的层次正确。 */
int mq_spine_instance_draw(mq_spine_instance *inst, float *vertices, float *uvs, float *colors,
                  uint32_t *indices, mq_spine_batch *batches, int max_vertices, int max_indices,
                  int max_batches, mq_spine_draw *out) {
    if (!inst || !out || !vertices || !uvs || !colors || !indices || !batches)
        return MQ_SPINE_ERR_ARG;
    if (max_vertices <= 0 || max_indices <= 0 || max_batches <= 0) return MQ_SPINE_ERR_ARG;

    Skeleton &sk = *inst->skeleton;
    sk.updateWorldTransform(Physics_Update);

    std::memset(out, 0, sizeof *out);

    /* 世界顶点暂存：按需增长（每帧复用，避免逐附件分配） */
    if (inst->worldVerts.size() < 4096) inst->worldVerts.resize(4096);

    int vtx = 0, idx = 0, nbatch = 0;
    int curPage = -2;
    int curBlend = -2;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool hasBounds = false;
    const float sx = inst->flipX ? -inst->scale : inst->scale;
    const float sy = inst->flipY ? -inst->scale : inst->scale;

    Array<Slot *> &order = sk.getDrawOrder().getAppliedPose();
    for (size_t si = 0; si < order.size(); ++si) {
        Slot *slot = order[si];
        if (!slot) continue;
        Attachment *att = slot->getAppliedPose().getAttachment();
        if (!att) continue;

        /* 只处理会"画出来"的附件。4.3 的 RTTI 没有类型枚举（只有 getClassName()），
         * 所以用 dynamic_cast 判别 —— 比字符串比较稳，也和"不依赖名字"的约定一致。
         * 裁剪/包围盒/路径/点不产生填充几何；裁剪遮罩暂不支持（README 记为已知限制，
         * 不静默假装支持）。 */
        RegionAttachment *ra = dynamic_cast<RegionAttachment *>(att);
        MeshAttachment *ma = dynamic_cast<MeshAttachment *>(att);
        if (!ra && !ma) continue;

        Array<float> *uvArr = nullptr;
        Array<unsigned short> *triArr = nullptr;
        int vcount = 0;
        int segStart = idx;          /* 本附件索引段的起点 */
        float *world = inst->worldVerts.data();
        /* 图集区域（两个分支都要用；无区域 = 无纹理 → 跳过） */
        AtlasRegion *ar = nullptr;
        if (ra && ra->getRegion())
            ar = static_cast<AtlasRegion *>(ra->getRegion()->getRendererObject());
        else if (ma)
            ar = ma->getRegion();
        if (!ar) continue;

        const Color &slotColor = slot->getAppliedPose().getColor();
        float cr = inst->r * slotColor.r;
        float cg = inst->g * slotColor.g;
        float cb = inst->b * slotColor.b;
        float ca = inst->a * slotColor.a;

        if (ra) {
            vcount = 4;
            if (vtx + vcount > max_vertices) break;
            ra->computeWorldVertices(*slot, inst->worldVerts, 0, 2);
            uvArr = &ra->getUVs();
            static const unsigned short kQuad[6] = { 0, 1, 2, 2, 3, 0 };
            if (idx + 6 > max_indices) break;
            for (int k = 0; k < 6; ++k) indices[idx + k] = static_cast<uint32_t>(vtx) + kQuad[k];
            idx += 6;
        } else {
            vcount = static_cast<int>(ma->getWorldVerticesLength() / 2);
            if (vcount <= 0) continue;
            if (vtx + vcount > max_vertices) break;
            if (inst->worldVerts.size() < ma->getWorldVerticesLength())
                inst->worldVerts.resize(ma->getWorldVerticesLength());
            world = inst->worldVerts.data();
            ma->computeWorldVertices(sk, *slot, 0, static_cast<size_t>(vcount) * 2, world, 0, 2);
            uvArr = &ma->getRegionUVs();
            triArr = &ma->getTriangles();
            if (idx + static_cast<int>(triArr->size()) > max_indices) break;
            for (size_t k = 0; k < triArr->size(); ++k)
                indices[idx + k] = static_cast<uint32_t>(vtx) + (*triArr)[k];
            idx += static_cast<int>(triArr->size());
        }
        (void)world;

        /* 写入顶点/UV/颜色（world 可能是共享缓冲，两个分支都已就位） */
        for (int k = 0; k < vcount; ++k) {
            const float wx = world[k * 2];
            const float wy = world[k * 2 + 1];
            const float px = inst->x + wx * sx;
            const float py = inst->y + wy * sy;
            vertices[(vtx + k) * 2] = px;
            vertices[(vtx + k) * 2 + 1] = py;
            if (uvArr && static_cast<size_t>(k) * 2 + 1 < uvArr->size()) {
                uvs[(vtx + k) * 2] = (*uvArr)[static_cast<size_t>(k) * 2];
                uvs[(vtx + k) * 2 + 1] = (*uvArr)[static_cast<size_t>(k) * 2 + 1];
            } else {
                uvs[(vtx + k) * 2] = 0.0F;
                uvs[(vtx + k) * 2 + 1] = 0.0F;
            }
            colors[(vtx + k) * 4] = cr;
            colors[(vtx + k) * 4 + 1] = cg;
            colors[(vtx + k) * 4 + 2] = cb;
            colors[(vtx + k) * 4 + 3] = ca;
            if (!hasBounds) {
                minX = maxX = px; minY = maxY = py; hasBounds = true;
            } else {
                if (px < minX) minX = px; if (px > maxX) maxX = px;
                if (py < minY) minY = py; if (py > maxY) maxY = py;
            }
        }

        const int page = page_index_of(inst->owner->pages, ar->getPage());
        const int blend = static_cast<int>(slot->getData()->getBlendMode());
        const int segCount = idx - segStart;
        /* 同 (页, 混合) → 归入当前批次（索引连续）；否则开新批次 */
        if (nbatch > 0 && page == curPage && blend == curBlend) {
            batches[nbatch - 1].index_count += static_cast<uint32_t>(segCount);
        } else {
            if (nbatch >= max_batches) break;
            curPage = page;
            curBlend = blend;
            batches[nbatch].page = page;
            batches[nbatch].blend = blend;
            batches[nbatch].first_index = static_cast<uint32_t>(segStart);
            batches[nbatch].index_count = static_cast<uint32_t>(segCount);
            ++nbatch;
        }
        vtx += vcount;
    }

    out->vertex_count = vtx;
    out->index_count = idx;
    out->batch_count = nbatch;
    out->bounds_x = hasBounds ? minX : 0.0F;
    out->bounds_y = hasBounds ? minY : 0.0F;
    out->bounds_w = hasBounds ? (maxX - minX) : 0.0F;
    out->bounds_h = hasBounds ? (maxY - minY) : 0.0F;
    return MQ_SPINE_OK;
}

} // extern "C"
