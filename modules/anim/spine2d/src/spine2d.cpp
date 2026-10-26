/* maque::spine2d —— Spine 2D 官方运行时（spine-cpp）的 C ABI 封装。
 *
 * 上游版本：spine-runtimes **4.2** 分支的 spine-cpp（third_party/spine-cpp）。
 * 为什么是 4.2：4.3 把附件内部结构收成 friend-only（RegionAttachment 不再有
 * `getRegion()/getUVs()`、computeWorldVertices 需要外部传 vertexOffsets）、姿势数值搬到
 * `*Pose` 类、约束收进统一 getConstraints()，与 docs/assets/06 §2 的描述不符；
 * 4.2 的公开面与设计文档一致，能用最少胶水接上。
 *
 * 编译要求：**打开 C++ RTTI**（用 dynamic_cast 判别附件类型；spine 自研 RTTI 只给类名字符串）。
 * 异常仍关闭：spine 用 getError() 返回错误信息，不靠异常。
 *
 * 分层（docs/assets/06 §2、docs/02）：
 *   · 只依赖 spine-cpp + 标准库；**不碰 GPU、不解码纹理**。
 *   · 骨架与图集从**内存**进来（调用方用 VFS 读）；图集页纹理经回调交引擎解码上传后回填句柄。
 *   · 对外只有 C ABI + 不透明句柄；句柄由本模块分配，故由本模块销毁（docs/02 分配者释放）。
 *
 * 确定性（docs/10）：不做内部计时、不开线程、不用系统熵；时间只由 dt / set_time 驱动。
 * 渲染只读：绘制导出只填调用方缓冲，不改任何模拟状态。 */
#include "anim/mq_spine2d.h"
#include <spine/Extension.h>
#include "core/tracy/mq_tracy.h"

#include <spine/spine.h>
#include <spine/ClippingAttachment.h>
#include <spine/Event.h>
#include <spine/PhysicsConstraint.h>
#include <spine/Sequence.h>
#include <spine/SkeletonClipping.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>

using namespace spine;

namespace {

/* 事件记录容量：一个实例环形/上限缓冲（**预分配**，事件路径零分配 —— Batching demo 同场
 * 50 个骨架，任何 per-event new 都会变成每帧上百次分配）。满了就计数丢弃，不静默。 */
constexpr int kMaxEvents = 64;

/* 图集页纹理回调适配：spine 把页名交给我们，我们去问引擎要纹理句柄。 */
struct CallbackTextureLoader : public TextureLoader {
    mq_spine_texture_cb cb = nullptr;
    void *user = nullptr;
    std::vector<std::string> pages;

    /* 页名 → 引擎纹理句柄（另存一份，供 mq_spine_data_page 查询） */
    std::vector<std::pair<std::string, uint64_t>> pageTextures;

    void load(AtlasPage &page, const String &path) override {
        const std::string name(path.buffer(), path.length());
        uint64_t tex = 0;
        int rc = MQ_SPINE_ERR_PAGE;
        if (cb) {
            /* file_data = nullptr：纹理文件由引擎侧按页名自己去 VFS 取（只有它知道挂载点），
             * 回调负责读文件 + 解码 + 上传 + 返回句柄。 */
            rc = cb(user, name.c_str(), nullptr, 0, &tex);
        }
        if (rc != MQ_SPINE_OK || tex == 0) {
            page.texture = nullptr;
            return;
        }
        /* spine 的约定：这里存的是**渲染器自己的贴图对象**（不透明）。本引擎就用纹理句柄
         * 作为该对象 —— 它随后会被带到 region->rendererObject，渲染侧原样取回即可，
         * 不需要（也不能）解释成 AtlasRegion*。 */
        page.texture = reinterpret_cast<void *>(static_cast<uintptr_t>(tex));
        pageTextures.emplace_back(name, tex);
    }

    uint64_t texture_for_name(const std::string &name) const {
        for (const auto &kv : pageTextures)
            if (kv.first == name) return kv.second;
        return 0;
    }

    void unload(void * /*texture*/) override {
        /* 纹理归引擎所有（它创建、它销毁）；这里不释放，避免双重释放（docs/02）。 */
    }
};

int page_index_of(const Vector<AtlasPage *> &pages, AtlasPage *p) {
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i] == p) return static_cast<int>(i);
    return -1;
}

/* 把 spine 的 AnimationState 回调转成 C ABI 回调 + 实例内事件记录。
 * 为什么用 AnimationStateListenerObject 而不是函数指针 typedef：它在
 * SPINE_USE_STD_FUNCTION 开/关两种构建下都存在，签名稳定（同上游 SkeletonRenderer 的用法）。 */
struct InstanceListener : public AnimationStateListenerObject {
    mq_spine_instance *inst = nullptr;
    void callback(AnimationState *state, EventType type, TrackEntry *entry, Event *event) override;
};

} // namespace

/* 皮肤句柄：owned=true 的是 mq_spine_skin_create 出来的（归调用方）；
 * owned=false 的是骨架数据里皮肤的**借用**视图（随 data 存活，destroy 是安全空操作）。 */
struct mq_spine_skin {
    Skin *skin = nullptr;
    bool owned = false;
};

/* 不透明句柄的真实类型（C ABI 只暴露前置声明） */
struct mq_spine_data {
    SkeletonData *skeletonData = nullptr;
    Atlas *atlas = nullptr;                       // 拥有：负责销毁页与区域
    CallbackTextureLoader *loader = nullptr;      // 拥有
    /* 附件加载器必须**活到骨架数据销毁**：附件在解析期留下它的指针（spine 内部按引用持有），
     * 若用栈上局部对象，解析返回后就成了悬空指针 —— ASan 实测在绘制时抓到
     * access-violation（freed 填充 0xf2f2...）。所以堆分配并由本结构持有。 */
    AtlasAttachmentLoader *attachmentLoader = nullptr;
    // 页表不在此缓存指针：atlas 解析过程中页表会增长重分配，缓存指针会悬空（ASan 实测）
    /* 纹理句柄 → 页号：绘制时从 region 只能拿到"贴图对象"（= 句柄），
     * 而批次要按页划分，所以加载期建这张表（页表在解析中会增长，故按值存）。 */
    std::vector<std::pair<uint64_t, int>> handleToPage;
    std::vector<std::string> animNames;           // 稳定序（声明序，docs/10 D4）
    std::vector<std::string> skinNames;
    /* 每个声明皮肤一个**借用**句柄（在 data 存活期内地址稳定 → 调用方拿到就能用，
     * 不必自己管生命周期；见 mq_spine_data_skin_at 的注释）。 */
    std::vector<mq_spine_skin *> skinHandles;
};


struct mq_spine_instance {
    mq_spine_data *owner = nullptr;
    Skeleton *skeleton = nullptr;
    AnimationStateData *stateData = nullptr;
    AnimationState *state = nullptr;
    float x = 0.0F, y = 0.0F, scale = 1.0F;
    bool flipX = false, flipY = false;
    float r = 1.0F, g = 1.0F, b = 1.0F, a = 1.0F;
    Vector<float> worldVerts;                     // 逐附件求值暂存（复用，避免每帧分配）
    Vector<unsigned short> localIdx;              // 逐附件**局部 0 基**索引（clipTriangles 要求）
    /* 裁剪遮罩状态（spine 的 mask）：一个实例一份，逐帧按槽序推进。
     * 官方参考实现 SkeletonRenderer 也持有一个 SkeletonClipping。 */
    SkeletonClipping clipper;

    /* ---- 动画事件（Spineboy demo：start/interrupt/end/complete/dispose + 用户 event）----
     * 记录 = 预分配数组（零分配）；语义：**累加**到 mq_spine_instance_clear_events。
     * runtime 每帧（脚本 update 之前）清一次 → "本帧产生的事件，下一帧脚本能 pull 到"。 */
    InstanceListener listener;
    mq_spine_event_cb eventCb = nullptr;
    void *eventUser = nullptr;
    mq_spine_event events[kMaxEvents];
    int eventCount = 0;
    int eventDropped = 0;

    /* 槽区间（SkeletonRendererSeparator）：只画声明序下标 [slotsFrom, slotsTo] 的槽；
     * slotsFrom < 0 = 不限制，slotsTo < 0 = 到末尾。判定用 getData().getIndex()（稳定）。 */
    int slotsFrom = -1;
    int slotsTo = -1;

    /* mq_spine_attachment_sequence_info 返回的 path 指针指向这里（调用方不拥有）。 */
    std::string seqPathBuf;
};

void InstanceListener::callback(AnimationState * /*state*/, EventType type, TrackEntry *entry,
                                Event *event) {
    mq_spine_instance *inst = this->inst;
    if (!inst) return;
    mq_spine_event ev{};
    ev.type = static_cast<int>(type);
    ev.track = entry ? entry->getTrackIndex() : -1;
    ev.animation = (entry && entry->getAnimation()) ? entry->getAnimation()->getName().buffer() : nullptr;
    if (type == EventType_Event && event) {
        ev.name = event->getData().getName().buffer();
        ev.int_value = event->getIntValue();
        ev.float_value = event->getFloatValue();
        const String &s = event->getStringValue();
        ev.string_value = s.isEmpty() ? nullptr : s.buffer();
    }
    if (inst->eventCount < kMaxEvents) inst->events[inst->eventCount++] = ev;
    else ++inst->eventDropped;
    if (inst->eventCb) inst->eventCb(inst->eventUser, inst, &ev);
}

extern "C" {

uint32_t mq_spine_abi_version(void) { return MQ_SPINE2D_ABI_VERSION; }

mq_spine_data *mq_spine_data_create(const void *skeleton_data, size_t skeleton_size,
                                    int skeleton_is_binary, const char *atlas_data,
                                    const char *atlas_dir_hint, mq_spine_texture_cb load_texture,
                                    void *user) {
    if (!skeleton_data || skeleton_size == 0 || !atlas_data) return nullptr;

    auto *d = new mq_spine_data();

    /* 1) 图集：从**内存**创建（数据可来自 VFS/远程/内存，docs/04）。
     *    页纹理不走它的文件读取，而是走我们的 TextureLoader 回调交给引擎。 */
    d->loader = new CallbackTextureLoader();
    d->loader->cb = load_texture;
    d->loader->user = user;
    const char *dir = atlas_dir_hint ? atlas_dir_hint : "";
    d->atlas = new Atlas(atlas_data, static_cast<int>(std::strlen(atlas_data)), dir, d->loader, true);

    /* 2) 骨架解析。4.2 的加载器收**指针**；错误走 getError()（不用异常）。 */
    d->attachmentLoader = new AtlasAttachmentLoader(d->atlas);
    SkeletonData *sd = nullptr;
    if (skeleton_is_binary) {
        SkeletonBinary binary(d->attachmentLoader, false);
        sd = binary.readSkeletonData(static_cast<const unsigned char *>(skeleton_data),
                                     static_cast<int>(skeleton_size));
        if (!sd) {
            std::print(stderr, "spine2d: 二进制骨架解析失败: {}\n", binary.getError().buffer());
            delete d->attachmentLoader;
            delete d->atlas;
            delete d->loader;
            delete d;
            return nullptr;
        }
    } else {
        SkeletonJson json(d->attachmentLoader, false);
        sd = json.readSkeletonData(static_cast<const char *>(skeleton_data));
        if (!sd) {
            std::print(stderr, "spine2d: JSON 骨架解析失败: {}\n", json.getError().buffer());
            delete d->attachmentLoader;
            delete d->atlas;
            delete d->loader;
            delete d;
            return nullptr;
        }
    }
    d->skeletonData = sd;

    /* 建"句柄 → 页号"表（页号用 AtlasPage::index；句柄从 loader 的页名表拿） */
    if (d->atlas && d->loader) {
        Vector<AtlasPage *> &pg = d->atlas->getPages();
        for (size_t i = 0; i < pg.size(); ++i) {
            if (!pg[i]) continue;
            const uint64_t h = d->loader->texture_for_name(pg[i]->name.buffer());
            if (h != 0) d->handleToPage.emplace_back(h, pg[i]->index);
        }
    }

    /* 动画/皮肤名快照：**声明序**（稳定，docs/10 D4），供 UI/AI 按稳定下标枚举 */
    for (size_t i = 0; i < sd->getAnimations().size(); ++i)
        d->animNames.emplace_back(sd->getAnimations()[i]->getName().buffer());
    for (size_t i = 0; i < sd->getSkins().size(); ++i) {
        Skin *sk = sd->getSkins()[i];
        d->skinNames.emplace_back(sk->getName().buffer());
        /* 借用句柄：地址稳定（vector 只在这里建完，之后不增删）→ 调用方无需管生命周期 */
        auto *h = new mq_spine_skin();
        h->skin = sk;
        h->owned = false;
        d->skinHandles.push_back(h);
    }
    return d;
}

void mq_spine_data_destroy(mq_spine_data *data) {
    if (!data) return;
    delete data->skeletonData;        // 先删骨架数据（其附件引用着 attachmentLoader）
    delete data->attachmentLoader;
    delete data->atlas;               // 拥有页与区域；页纹理归引擎，loader::unload 故意不释放
    delete data->loader;
    for (mq_spine_skin *h : data->skinHandles) delete h;    // 借用句柄的包装（Skin 本体归 skeletonData）
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
int mq_spine_data_skin_find(mq_spine_data *data, const char *name) {
    if (!data || !name) return -1;
    for (size_t i = 0; i < data->skinNames.size(); ++i)
        if (data->skinNames[i] == name) return static_cast<int>(i);
    return -1;
}
mq_spine_skin *mq_spine_data_skin_at(mq_spine_data *data, int index) {
    if (!data || index < 0 || index >= static_cast<int>(data->skinHandles.size())) return nullptr;
    return data->skinHandles[static_cast<size_t>(index)];
}
mq_spine_skin *mq_spine_skin_create(const char *name) {
    if (!name) return nullptr;
    auto *h = new mq_spine_skin();
    h->skin = new Skin(String(name));
    h->owned = true;
    return h;
}
void mq_spine_skin_destroy(mq_spine_skin *skin) {
    if (!skin || !skin->owned) return;      // 借用句柄（data 内部皮肤）不能删
    delete skin->skin;
    delete skin;
}
int mq_spine_skin_add(mq_spine_skin *dst, mq_spine_skin *src) {
    if (!dst || !dst->skin || !src || !src->skin) return MQ_SPINE_ERR_ARG;
    /* 官方 MixAndMatch：skin->addSkin(skeletonData->findSkin("nose/short")) …
     * addSkin 会**复制**附件指针（不拥有），与官方语义一致。 */
    dst->skin->addSkin(src->skin);
    return MQ_SPINE_OK;
}
const char *mq_spine_skin_name(mq_spine_skin *skin) {
    return (skin && skin->skin) ? skin->skin->getName().buffer() : nullptr;
}
int mq_spine_skin_attachment_count(mq_spine_skin *skin) {
    if (!skin || !skin->skin) return 0;
    /* Skin 没有 size()：用 AttachmentMap::Entries 的 hasNext()/next() 数一遍
     * （调用频率低，不值得缓存）。 */
    int n = 0;
    Skin::AttachmentMap::Entries e = skin->skin->getAttachments();
    while (e.hasNext()) { e.next(); ++n; }
    return n;
}
int mq_spine_data_page_count(mq_spine_data *data) {
    return (data && data->atlas) ? static_cast<int>(data->atlas->getPages().size()) : 0;
}
int mq_spine_data_page(mq_spine_data *data, int index, mq_spine_page *out) {
    if (!data || !data->atlas || !out) return MQ_SPINE_ERR_ARG;
    Vector<AtlasPage *> &pages = data->atlas->getPages();
    if (index < 0 || index >= static_cast<int>(pages.size())) return MQ_SPINE_ERR_ARG;
    AtlasPage *p = pages[static_cast<size_t>(index)];
    out->name = p ? p->name.buffer() : nullptr;
    out->width = p ? p->width : 0;
    out->height = p ? p->height : 0;
    /* 纹理句柄存在 loader 的表里（page->texture 是贴图对象槽位，本引擎放的就是句柄，
     * 但按页名查更稳：解析期 page->texture 先被占位、后由 load 覆盖）。 */
    out->texture = (p && data->loader) ? data->loader->texture_for_name(p->name.buffer()) : 0;
    /* pma（图集里的 `pma: true`）：预乘 alpha 的纹理必须配 `src=ONE` 的混合方程，
     * 否则边缘会被乘两次 alpha（发暗/发黑）。渲染侧按这个标志选管线。 */
    out->pma = (p && p->pma) ? 1 : 0;
    return MQ_SPINE_OK;
}
int mq_spine_data_page_texture(mq_spine_data *data, int index, uint64_t texture) {
    if (!data || !data->atlas) return MQ_SPINE_ERR_ARG;
    Vector<AtlasPage *> &pages = data->atlas->getPages();
    if (index < 0 || index >= static_cast<int>(pages.size())) return MQ_SPINE_ERR_ARG;
    AtlasPage *p = pages[static_cast<size_t>(index)];
    if (!p) return MQ_SPINE_ERR_ARG;
    p->texture = reinterpret_cast<void *>(static_cast<uintptr_t>(texture));
    if (data->loader) {
        for (auto &kv : data->loader->pageTextures)
            if (kv.first == p->name.buffer()) { kv.second = texture; break; }
    }
    return MQ_SPINE_OK;
}

mq_spine_instance *mq_spine_instance_create(mq_spine_data *data) {
    if (!data || !data->skeletonData) return nullptr;
    auto *inst = new mq_spine_instance();
    inst->owner = data;
    inst->skeleton = new Skeleton(data->skeletonData);
    inst->stateData = new AnimationStateData(data->skeletonData);
    inst->state = new AnimationState(inst->stateData);
    /* 事件监听：spine 的 AnimationState 是**同步**回调（queue->drain 在 update/setAnimation 内），
     * 无线程、无异步 → 确定性不受影响（docs/10）。listener 是实例成员 → 生命周期与 state 一致。 */
    inst->listener.inst = inst;
    inst->state->setListener(&inst->listener);
    /* **必须显式应用默认皮肤**：Spine 的"setup 姿势附件"并不都在 SkeletonData::slots 里 ——
     * 皮肤的附件（含**裁剪遮罩**）存在 skin 下，不应用皮肤就不会挂到槽上。
     * 实测：不设皮肤时 `getSkin()==null`，spineboy 的 `clipping` 槽**没有附件** →
     * 遮罩永不生效（而且皮肤自带的其它附件也一起缺失）。
     * 官方各平台样例都在创建后 `setSkin(defaultSkin)`；这里照做。 */
    if (Skin *def = data->skeletonData->getDefaultSkin()) inst->skeleton->setSkin(def);
    inst->skeleton->setToSetupPose();

    /* 遮罩验收入口（MAQUE_SPINE_MASK="槽名:皮肤附件名"，默认 "clipping:clipping"）。
     *
     * 为什么需要它：Spine 的裁剪遮罩**不是**在 setup 姿势里自动挂上的 ——
     * `Skeleton::setSkin` 只给"setup 姿势里声明了 attachment 名"的槽挂皮肤附件
     *（读 spine-cpp 源码 Skeleton.cpp:setSkin 确认），而 spineboy 的 `clipping` 槽在
     * setup 里没有附件名，遮罩要靠**动画时间轴或代码显式设置**才生效。
     * 官方 spineboy 的 11 个动画**没有一个**驱动该槽，所以默认播放时遮罩根本不生效 ——
     * 于是"裁剪代码路是否真的正确"无法用现成素材证明。
     * 这个入口让人能显式打开遮罩做验证（e2e 用它做像素级断言），默认关闭、不影响正常渲染。 */
    if (const char *spec = std::getenv("MAQUE_SPINE_MASK")) {
        std::string s(spec);
        const size_t colon = s.find(':');
        const std::string slotName = colon == std::string::npos ? s : s.substr(0, colon);
        const std::string attName = colon == std::string::npos ? s : s.substr(colon + 1);
        Skin *def = inst->owner->skeletonData->getDefaultSkin();
        Slot *slot = slotName.empty() ? nullptr : inst->skeleton->findSlot(String(slotName.c_str()));
        Attachment *att = nullptr;
        if (def && slot) att = def->getAttachment(slot->getData().getIndex(), String(attName.c_str()));
        if (slot && att) {
            slot->setAttachment(att);
            std::print(stderr, "[spine2d] 遮罩已启用: slot={} att={} 类={}\n", slotName, attName,
                       att->getRTTI().getClassName());
        } else {
            std::print(stderr, "[spine2d] 遮罩启用失败: slot={} att={}（findSlot={} getAttachment={}）\n",
                       slotName, attName, slot ? "ok" : "null", att ? "ok" : "null");
        }
    }

    inst->skeleton->updateWorldTransform(Physics_Update);
    if (std::getenv("MAQUE_TRACE_SPINE_DRAW")) {
        Skeleton &sk = *inst->skeleton;
        std::print(stderr, "[spine2d] create: slots={} skin={}\n",
                   sk.getSlots().size(), sk.getSkin() ? sk.getSkin()->getName().buffer() : "(null)");
        /* 逐槽打印附件类型与是否有遮挡（重点看裁剪槽） */
        for (size_t i = 0; i < sk.getDrawOrder().size(); ++i) {
            Slot *s = sk.getDrawOrder()[i];
            if (!s) continue;
            const char *nm = s->getData().getName().buffer();
            Attachment *a = s->getAttachment();
            std::print(stderr, "[spine2d]   slot#{} {:<24} att={}\n", i, nm ? nm : "?",
                       a ? a->getRTTI().getClassName() : "(none)");
        }
    }
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
    if (!inst->skeleton->getData()->findSkin(String(name))) return MQ_SPINE_ERR_NOTFOUND;
    inst->skeleton->setSkin(String(name));
    inst->skeleton->setSlotsToSetupPose();
    return MQ_SPINE_OK;
}

int mq_spine_instance_set_skin_obj(mq_spine_instance *inst, mq_spine_skin *skin) {
    if (!inst || !skin || !skin->skin) return MQ_SPINE_ERR_ARG;
    /* Skin* 版本（官方 MixAndMatch 的 setSkin(skin)）：先 setSkin 挂附件，再按 setup 姿势复位槽。 */
    inst->skeleton->setSkin(skin->skin);
    inst->skeleton->setSlotsToSetupPose();
    return MQ_SPINE_OK;
}

int mq_spine_instance_set_attachment(mq_spine_instance *inst, const char *slot_name,
                                     const char *attachment_name, const char *skin_name) {
    if (!inst || !slot_name) return MQ_SPINE_ERR_ARG;
    Slot *slot = inst->skeleton->findSlot(String(slot_name));
    if (!slot) return MQ_SPINE_ERR_NOTFOUND;
    /* 空附件名 = 卸下该槽附件（官方也用它隐藏槽） */
    if (!attachment_name || !*attachment_name) { slot->setAttachment(nullptr); return MQ_SPINE_OK; }
    Attachment *att = nullptr;
    if (skin_name && *skin_name) {
        Skin *sk = inst->skeleton->getData()->findSkin(String(skin_name));
        /* 运行时**合成**出来的皮肤不在骨架数据的皮肤表里（官方 MixAndMatch 就是这样）→
         * 名字命中"当前皮肤"时用它；两者都没有才算找不到。 */
        if (!sk && inst->skeleton->getSkin() && inst->skeleton->getSkin()->getName() == String(skin_name))
            sk = inst->skeleton->getSkin();
        if (!sk) return MQ_SPINE_ERR_NOTFOUND;
        att = sk->getAttachment(slot->getData().getIndex(), String(attachment_name));
    } else {
        /* 与官方 Skeleton::getAttachment(slotName, attachmentName) 同义：当前皮肤里找 */
        att = inst->skeleton->getAttachment(String(slot_name), String(attachment_name));
    }
    if (!att) return MQ_SPINE_ERR_NOTFOUND;
    slot->setAttachment(att);
    return MQ_SPINE_OK;
}

int mq_spine_instance_play(mq_spine_instance *inst, const char *animation, int loop, int track) {
    if (!inst || !animation || track < 0) return MQ_SPINE_ERR_ARG;
    if (!inst->skeleton->getData()->findAnimation(String(animation))) return MQ_SPINE_ERR_NOTFOUND;
    inst->state->setAnimation(static_cast<size_t>(track), String(animation), loop != 0);
    return MQ_SPINE_OK;
}

int mq_spine_instance_set_animation(mq_spine_instance *inst, int track, const char *animation, int loop) {
    return mq_spine_instance_play(inst, animation, loop, track);
}

int mq_spine_instance_add_animation(mq_spine_instance *inst, int track, const char *animation, int loop,
                                    float delay) {
    if (!inst || !animation || track < 0 || !std::isfinite(delay)) return MQ_SPINE_ERR_ARG;
    if (!inst->skeleton->getData()->findAnimation(String(animation))) return MQ_SPINE_ERR_NOTFOUND;
    /* 官方 Raptor：track1 "gun-grab" false delay=2 —— 排在上一个动画之后 */
    inst->state->addAnimation(static_cast<size_t>(track), String(animation), loop != 0, delay);
    return MQ_SPINE_OK;
}

void mq_spine_instance_stop(mq_spine_instance *inst) {
    if (inst && inst->state) inst->state->clearTracks();
}

int mq_spine_instance_track_count(mq_spine_instance *inst) {
    return (inst && inst->state) ? static_cast<int>(inst->state->getTracks().size()) : 0;
}
const char *mq_spine_instance_track_animation(mq_spine_instance *inst, int track) {
    return mq_spine_instance_animation_name(inst, track);
}
int mq_spine_instance_queue_count(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return 0;
    /* 排队 = 当前条目之后还挂着的链（官方 addAnimation 把新条目接到 last->_next） */
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
    int n = 0;
    while (e && e->getNext()) { e = e->getNext(); ++n; }
    return n;
}
void mq_spine_instance_clear_track(mq_spine_instance *inst, int track) {
    if (inst && inst->state && track >= 0) inst->state->clearTrack(static_cast<size_t>(track));
}
int mq_spine_instance_set_track_time_scale(mq_spine_instance *inst, int track, float scale) {
    if (!inst || !inst->state || track < 0 || !std::isfinite(scale)) return MQ_SPINE_ERR_ARG;
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
    if (!e) return MQ_SPINE_ERR_NOTFOUND;
    e->setTimeScale(scale);
    return MQ_SPINE_OK;
}
float mq_spine_instance_track_time_scale(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return 0.0F;
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
    return e ? e->getTimeScale() : 0.0F;
}
void mq_spine_instance_set_time_scale(mq_spine_instance *inst, float scale) {
    if (inst && inst->state && std::isfinite(scale)) inst->state->setTimeScale(scale);
}
float mq_spine_instance_time_scale(mq_spine_instance *inst) {
    return (inst && inst->state) ? inst->state->getTimeScale() : 0.0F;
}
void mq_spine_instance_set_default_mix(mq_spine_instance *inst, float duration) {
    if (inst && inst->stateData && std::isfinite(duration)) inst->stateData->setDefaultMix(duration);
}
float mq_spine_instance_default_mix(mq_spine_instance *inst) {
    return (inst && inst->stateData) ? inst->stateData->getDefaultMix() : 0.0F;
}
int mq_spine_instance_set_mix(mq_spine_instance *inst, const char *from, const char *to, float duration) {
    if (!inst || !inst->stateData || !from || !to || !std::isfinite(duration)) return MQ_SPINE_ERR_ARG;
    if (!inst->skeleton->getData()->findAnimation(String(from))) return MQ_SPINE_ERR_NOTFOUND;
    if (!inst->skeleton->getData()->findAnimation(String(to))) return MQ_SPINE_ERR_NOTFOUND;
    inst->stateData->setMix(String(from), String(to), duration);
    return MQ_SPINE_OK;
}

int mq_spine_instance_set_time(mq_spine_instance *inst, int track, float time) {
    if (!inst || !inst->state || track < 0) return MQ_SPINE_ERR_ARG;
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
    if (!e) return MQ_SPINE_ERR_NOTFOUND;
    e->setTrackTime(time);
    return MQ_SPINE_OK;
}

int mq_spine_instance_step_ex(mq_spine_instance *inst, float dt, int physics) {
    MQ_TRACY_ZONE_N(anim, "step");
    if (!inst || !std::isfinite(dt)) return MQ_SPINE_ERR_ARG;
    if (physics < 0 || physics > 3) return MQ_SPINE_ERR_ARG;
    /* 固定步语义：dt 由调用方按主循环固定步长给（docs/10 D1）；本模块不自建计时器。
     * ⚠️ `skeleton->update(dt)` 不能省：spine 的 **物理约束**按 `skeleton.getTime()` 的增量
     * 积分（PhysicsConstraint::update: delta = skeleton.time - lastTime）。只推进 state 而不推进
     * skeleton 时间时，celestial-circus 这类骨架的物理永远不累加（实测：物理输出恒定 = 挂件不晃）。 */
    inst->skeleton->update(dt);
    inst->state->update(dt);
    inst->state->apply(*inst->skeleton);
    inst->skeleton->updateWorldTransform(static_cast<Physics>(physics));
    return MQ_SPINE_OK;
}

int mq_spine_instance_step(mq_spine_instance *inst, float dt) {
    /* 默认物理时间轴 = Physics_Update（与官方各平台 SkeletonAnimation 默认一致） */
    return mq_spine_instance_step_ex(inst, dt, static_cast<int>(Physics_Update));
}

int mq_spine_instance_update_world_transform(mq_spine_instance *inst, int physics) {
    if (!inst || !inst->skeleton) return MQ_SPINE_ERR_ARG;
    if (physics < 0 || physics > 3) return MQ_SPINE_ERR_ARG;
    /* 官方 IK demo：postUpdateWorldTransforms 钩子里设完 crosshair 骨骼后**只**重算世界变换。 */
    inst->skeleton->updateWorldTransform(static_cast<Physics>(physics));
    return MQ_SPINE_OK;
}

float mq_spine_instance_time(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return 0.0F;
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
    return e ? e->getTrackTime() : 0.0F;
}

const char *mq_spine_instance_animation_name(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return nullptr;
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
    if (!e) return nullptr;
    return e->getAnimation()->getName().buffer();
}

int mq_spine_instance_animation_complete(mq_spine_instance *inst, int track) {
    if (!inst || !inst->state || track < 0) return 0;
    TrackEntry *e = inst->state->getCurrent(static_cast<size_t>(track));
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
    /* 4.2 的 Skeleton 只有 getColor()（无 setColor 重载）→ 写它返回的引用 */
    Color &sc = inst->skeleton->getColor();
    sc.r = r; sc.g = g; sc.b = b; sc.a = a;
}

void mq_spine_instance_color(mq_spine_instance *inst, float *r, float *g, float *b, float *a) {
    if (!inst) return;
    Color &c = inst->skeleton->getColor();
    if (r) *r = c.r; if (g) *g = c.g; if (b) *b = c.b; if (a) *a = c.a;
}

int mq_spine_instance_bone_position(mq_spine_instance *inst, const char *bone, float *x, float *y) {
    if (!inst || !bone || !x || !y) return MQ_SPINE_ERR_ARG;
    Bone *bn = inst->skeleton->findBone(String(bone));
    if (!bn) return MQ_SPINE_ERR_NOTFOUND;
    *x = bn->getWorldX();
    *y = bn->getWorldY();
    return MQ_SPINE_OK;
}

/* ---- IK / 变换约束 ----------------------------------------------------------------
 * 4.2 的数值直接挂在约束对象上（IkConstraint::setMix 等），不必像 4.3 那样绕 pose。
 * 下标 = 该类型在骨架内的次序（getIkConstraints() 的稳定序，docs/10 D4）。 */
static IkConstraint *ik_at(mq_spine_instance *inst, int index) {
    if (!inst || index < 0) return nullptr;
    Vector<IkConstraint *> &iks = inst->skeleton->getIkConstraints();
    if (index >= static_cast<int>(iks.size())) return nullptr;
    return iks[static_cast<size_t>(index)];
}

int mq_spine_instance_set_float(mq_spine_instance *inst, int target, int index, int field,
                                float value, float /*blend*/) {
    if (!inst || index < 0) return MQ_SPINE_ERR_ARG;
    if (target == 0) {
        IkConstraint *ik = ik_at(inst, index);
        if (!ik) return MQ_SPINE_ERR_ARG;
        switch (field) {
        case 0: ik->setMix(value); break;
        case 1: ik->setSoftness(value); break;
        case 2: ik->setBendDirection(value >= 0.0F ? 1 : -1); break;
        case 3: ik->setCompress(value != 0.0F); break;
        case 4: ik->setStretch(value != 0.0F); break;
        default: return MQ_SPINE_ERR_ARG;
        }
        return MQ_SPINE_OK;
    }
    if (target == 1) {   /* 变换约束 mix 的 6 个分量 */
        Vector<TransformConstraint *> &tcs = inst->skeleton->getTransformConstraints();
        if (index >= static_cast<int>(tcs.size())) return MQ_SPINE_ERR_ARG;
        TransformConstraint *tc = tcs[static_cast<size_t>(index)];
        switch (field) {
        case 0: tc->setMixRotate(value); break;
        case 1: tc->setMixX(value); break;
        case 2: tc->setMixY(value); break;
        case 3: tc->setMixScaleX(value); break;
        case 4: tc->setMixScaleY(value); break;
        case 5: tc->setMixShearY(value); break;
        default: return MQ_SPINE_ERR_ARG;
        }
        return MQ_SPINE_OK;
    }
    return MQ_SPINE_ERR_ARG;
}

int mq_spine_instance_add_float(mq_spine_instance *inst, int target, int index, int field,
                                float value, float blend) {
    const float cur = mq_spine_instance_get_float(inst, target, index, field);
    float next = cur + value;
    if (std::isfinite(blend) && blend >= 0.0F && blend <= 1.0F)
        next = cur + (next - cur) * blend;
    return mq_spine_instance_set_float(inst, target, index, field, next, blend);
}

float mq_spine_instance_get_float(mq_spine_instance *inst, int target, int index, int field) {
    if (!inst || index < 0) return 0.0F;
    if (target == 0) {
        IkConstraint *ik = ik_at(inst, index);
        if (!ik) return 0.0F;
        switch (field) {
        case 0: return ik->getMix();
        case 1: return ik->getSoftness();
        case 2: return static_cast<float>(ik->getBendDirection());
        case 3: return ik->getCompress() ? 1.0F : 0.0F;
        case 4: return ik->getStretch() ? 1.0F : 0.0F;
        default: return 0.0F;
        }
    }
    if (target == 1) {
        Vector<TransformConstraint *> &tcs = inst->skeleton->getTransformConstraints();
        if (index >= static_cast<int>(tcs.size())) return 0.0F;
        TransformConstraint *tc = tcs[static_cast<size_t>(index)];
        switch (field) {
        case 0: return tc->getMixRotate();
        case 1: return tc->getMixX();
        case 2: return tc->getMixY();
        case 3: return tc->getMixScaleX();
        case 4: return tc->getMixScaleY();
        case 5: return tc->getMixShearY();
        default: return 0.0F;
        }
    }
    return 0.0F;
}

int mq_spine_instance_ik_count(mq_spine_instance *inst) {
    return inst ? static_cast<int>(inst->skeleton->getIkConstraints().size()) : 0;
}
const char *mq_spine_instance_ik_name(mq_spine_instance *inst, int index) {
    IkConstraint *ik = ik_at(inst, index);
    return ik ? ik->getData().getName().buffer() : nullptr;
}
int mq_spine_instance_ik_set_mix(mq_spine_instance *inst, int index, float mix) {
    return mq_spine_instance_set_float(inst, 0, index, 0, mix, 1.0F);
}
int mq_spine_instance_transform_count(mq_spine_instance *inst) {
    return inst ? static_cast<int>(inst->skeleton->getTransformConstraints().size()) : 0;
}
const char *mq_spine_instance_transform_name(mq_spine_instance *inst, int index) {
    if (!inst || index < 0 ||
        index >= static_cast<int>(inst->skeleton->getTransformConstraints().size()))
        return nullptr;
    return inst->skeleton->getTransformConstraints()[static_cast<size_t>(index)]
        ->getData().getName().buffer();
}
int mq_spine_instance_transform_position(mq_spine_instance *inst, int index, float *x, float *y) {
    if (!inst || !inst->skeleton || !x || !y || index < 0 ||
        index >= static_cast<int>(inst->skeleton->getTransformConstraints().size()))
        return MQ_SPINE_ERR_ARG;
    TransformConstraint *tc = inst->skeleton->getTransformConstraints()[static_cast<size_t>(index)];
    /* 4.2 的 getTarget() 返回 **BoneData***（声明期的骨骼数据），不是 Bone*。
     * 所以这里无法给"世界坐标"——改为按名字在骨架实例里查同名骨骼，返回其世界位置。 */
    BoneData *targetData = tc->getData().getTarget();
    if (!targetData) return MQ_SPINE_ERR_NOTFOUND;
    Bone *target = inst->skeleton->findBone(targetData->getName());
    if (!target) return MQ_SPINE_ERR_NOTFOUND;
    *x = target->getWorldX();
    *y = target->getWorldY();
    return MQ_SPINE_OK;
}

/* ---- 动画事件 --------------------------------------------------------------------
 * 官方 Spineboy demo 的 5 类轨事件 + 用户 event。事件由 AnimationState 在
 * update/apply/setAnimation 内**同步**回调（spine 内部 EventQueue::drain），无异步。
 * 这些访问器是"pull"面：先 event_count 再按下标取字段；与 set_event_cb 的 push 面互补。 */
void mq_spine_instance_set_event_cb(mq_spine_instance *inst, mq_spine_event_cb cb, void *user) {
    if (!inst) return;
    inst->eventCb = cb;
    inst->eventUser = user;
}
int mq_spine_instance_event_count(mq_spine_instance *inst) { return inst ? inst->eventCount : 0; }
int mq_spine_instance_event(mq_spine_instance *inst, int index, mq_spine_event *out) {
    if (!inst || !out) return MQ_SPINE_ERR_ARG;
    if (index < 0 || index >= inst->eventCount) return MQ_SPINE_ERR_ARG;
    *out = inst->events[static_cast<size_t>(index)];
    return MQ_SPINE_OK;
}
void mq_spine_instance_clear_events(mq_spine_instance *inst) {
    if (!inst) return;
    inst->eventCount = 0;
    inst->eventDropped = 0;
}
int mq_spine_instance_events_dropped(mq_spine_instance *inst) { return inst ? inst->eventDropped : 0; }

/* ---- 骨骼（IK demo：设 crosshair 世界位置 → IK 链跟随）------------------------------
 * 下标 = 骨架内骨骼声明序（getBones() 的稳定序，docs/10 D4）。 */
static Bone *bone_at(mq_spine_instance *inst, int index) {
    if (!inst || !inst->skeleton || index < 0) return nullptr;
    Vector<Bone *> &bones = inst->skeleton->getBones();
    if (index >= static_cast<int>(bones.size())) return nullptr;
    return bones[static_cast<size_t>(index)];
}
int mq_spine_instance_bone_count(mq_spine_instance *inst) {
    return (inst && inst->skeleton) ? static_cast<int>(inst->skeleton->getBones().size()) : 0;
}
const char *mq_spine_instance_bone_name(mq_spine_instance *inst, int index) {
    Bone *b = bone_at(inst, index);
    return b ? b->getData().getName().buffer() : nullptr;
}
int mq_spine_instance_bone_find(mq_spine_instance *inst, const char *name) {
    if (!inst || !inst->skeleton || !name) return -1;
    Vector<Bone *> &bones = inst->skeleton->getBones();
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i] && bones[i]->getData().getName() == String(name)) return static_cast<int>(i);
    return -1;
}
int mq_spine_instance_bone_parent(mq_spine_instance *inst, int index) {
    Bone *b = bone_at(inst, index);
    if (!b) return -1;
    Bone *p = b->getParent();
    if (!p) return -1;
    /* 父骨骼在骨架表中的下标（用**名字**回查：BoneData 的父名是稳定标识） */
    return mq_spine_instance_bone_find(inst, p->getData().getName().buffer());
}
int mq_spine_instance_bone_set_position(mq_spine_instance *inst, int index, float x, float y) {
    Bone *b = bone_at(inst, index);
    if (!b || !std::isfinite(x) || !std::isfinite(y)) return MQ_SPINE_ERR_ARG;
    b->setX(x);
    b->setY(y);
    return MQ_SPINE_OK;
}
int mq_spine_instance_bone_position_at(mq_spine_instance *inst, int index, float *x, float *y) {
    Bone *b = bone_at(inst, index);
    if (!b || !x || !y) return MQ_SPINE_ERR_ARG;
    *x = b->getX();
    *y = b->getY();
    return MQ_SPINE_OK;
}
int mq_spine_instance_bone_world_position(mq_spine_instance *inst, int index, float *x, float *y) {
    Bone *b = bone_at(inst, index);
    if (!b || !x || !y) return MQ_SPINE_ERR_ARG;
    *x = b->getWorldX();
    *y = b->getWorldY();
    return MQ_SPINE_OK;
}
int mq_spine_instance_bone_set_world_position(mq_spine_instance *inst, int index, float x, float y) {
    Bone *b = bone_at(inst, index);
    if (!b || !std::isfinite(x) || !std::isfinite(y)) return MQ_SPINE_ERR_ARG;
    /* 与官方 IK demo 完全同款：位置是**父空间局部**坐标，所以要用**父骨骼**的 worldToLocal
     * （用骨骼自己的 worldToLocal 会把已经包含该骨骼变换的结果再反解一次 → 目标点漂移）。
     * 根骨骼没有父 → 父空间 == 世界空间，直接赋值。 */
    if (Bone *p = b->getParent()) {
        float lx = 0.0F, ly = 0.0F;
        p->worldToLocal(x, y, lx, ly);
        b->setX(lx);
        b->setY(ly);
    } else {
        b->setX(x);
        b->setY(y);
    }
    return MQ_SPINE_OK;
}

/* ---- 槽（SkeletonRendererSeparator：按槽区间分段渲染）--------------------------------
 * 区间用**声明序下标**（SlotData::getIndex）判定，与官方 `findSlot(name)->getData().getIndex()`
 * 同源；不随 draw order timeline 漂移（draw order 位置会漂）。 */
static Slot *slot_at(mq_spine_instance *inst, int index) {
    if (!inst || !inst->skeleton || index < 0) return nullptr;
    Vector<Slot *> &slots = inst->skeleton->getSlots();
    if (index >= static_cast<int>(slots.size())) return nullptr;
    return slots[static_cast<size_t>(index)];
}
int mq_spine_instance_slot_count(mq_spine_instance *inst) {
    return (inst && inst->skeleton) ? static_cast<int>(inst->skeleton->getSlots().size()) : 0;
}
const char *mq_spine_instance_slot_name(mq_spine_instance *inst, int index) {
    Slot *s = slot_at(inst, index);
    return s ? s->getData().getName().buffer() : nullptr;
}
int mq_spine_instance_slot_index(mq_spine_instance *inst, const char *name) {
    if (!inst || !inst->skeleton || !name) return -1;
    Slot *s = inst->skeleton->findSlot(String(name));
    return s ? s->getData().getIndex() : -1;
}
int mq_spine_instance_set_slots_range(mq_spine_instance *inst, int from, int to) {
    if (!inst) return MQ_SPINE_ERR_ARG;
    if (from >= 0 && to >= 0 && to < from) return MQ_SPINE_ERR_ARG;
    inst->slotsFrom = from;
    inst->slotsTo = to;
    return MQ_SPINE_OK;
}
int mq_spine_instance_slots_range(mq_spine_instance *inst, int *from, int *to) {
    if (!inst) return MQ_SPINE_ERR_ARG;
    if (from) *from = inst->slotsFrom;
    if (to) *to = inst->slotsTo;
    return MQ_SPINE_OK;
}
const char *mq_spine_instance_slot_attachment(mq_spine_instance *inst, int index) {
    Slot *s = slot_at(inst, index);
    if (!s) return nullptr;
    Attachment *a = s->getAttachment();
    return a ? a->getName().buffer() : nullptr;
}

/* ---- 物理约束（Physics demo：celestial-circus 的挂件；physicsTranslate 由指针移动驱动）---- */
static PhysicsConstraint *physics_at(mq_spine_instance *inst, int index) {
    if (!inst || !inst->skeleton || index < 0) return nullptr;
    Vector<PhysicsConstraint *> &cs = inst->skeleton->getPhysicsConstraints();
    if (index >= static_cast<int>(cs.size())) return nullptr;
    return cs[static_cast<size_t>(index)];
}
int mq_spine_instance_physics_count(mq_spine_instance *inst) {
    return (inst && inst->skeleton) ? static_cast<int>(inst->skeleton->getPhysicsConstraints().size()) : 0;
}
const char *mq_spine_instance_physics_name(mq_spine_instance *inst, int index) {
    PhysicsConstraint *c = physics_at(inst, index);
    return c ? c->getData().getName().buffer() : nullptr;
}
void mq_spine_instance_physics_translate(mq_spine_instance *inst, float x, float y) {
    if (inst && inst->skeleton && std::isfinite(x) && std::isfinite(y))
        inst->skeleton->physicsTranslate(x, y);
}
int mq_spine_instance_physics_set_float(mq_spine_instance *inst, int index, int field, float value) {
    PhysicsConstraint *c = physics_at(inst, index);
    if (!c || !std::isfinite(value)) return MQ_SPINE_ERR_ARG;
    switch (field) {
    case MQ_SPINE_PHYS_INERTIA: c->setInertia(value); break;
    case MQ_SPINE_PHYS_STRENGTH: c->setStrength(value); break;
    case MQ_SPINE_PHYS_DAMPING: c->setDamping(value); break;
    case MQ_SPINE_PHYS_MASS_INVERSE: c->setMassInverse(value); break;
    case MQ_SPINE_PHYS_WIND: c->setWind(value); break;
    case MQ_SPINE_PHYS_GRAVITY: c->setGravity(value); break;
    case MQ_SPINE_PHYS_MIX: c->setMix(value); break;
    default: return MQ_SPINE_ERR_ARG;
    }
    return MQ_SPINE_OK;
}
float mq_spine_instance_physics_get_float(mq_spine_instance *inst, int index, int field) {
    PhysicsConstraint *c = physics_at(inst, index);
    if (!c) return 0.0F;
    switch (field) {
    case MQ_SPINE_PHYS_INERTIA: return c->getInertia();
    case MQ_SPINE_PHYS_STRENGTH: return c->getStrength();
    case MQ_SPINE_PHYS_DAMPING: return c->getDamping();
    case MQ_SPINE_PHYS_MASS_INVERSE: return c->getMassInverse();
    case MQ_SPINE_PHYS_WIND: return c->getWind();
    case MQ_SPINE_PHYS_GRAVITY: return c->getGravity();
    case MQ_SPINE_PHYS_MIX: return c->getMix();
    default: return 0.0F;
    }
}
void mq_spine_instance_physics_reset(mq_spine_instance *inst) {
    if (!inst || !inst->skeleton) return;
    Vector<PhysicsConstraint *> &cs = inst->skeleton->getPhysicsConstraints();
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i]) cs[i]->reset();
}

/* ---- 序列帧（Sequence demo：dragon-ess 的 flying）------------------------------------
 * 关键事实（读 third_party/spine-cpp 源码确认，别再猜）：
 *   · Sequence 不是独立附件类型 —— 它挂在 RegionAttachment/MeshAttachment 的 `_sequence` 上；
 *   · 换帧发生在 **computeWorldVertices 内部**（`if (_sequence) _sequence->apply(&slot, this)`），
 *     apply 会 setRegion + updateRegion → 当前帧的区域/UV 才生效；
 *   · 帧下标由 SequenceTimeline 写进 **Slot::setSequenceIndex**。
 * 所以"按当前帧取图"= 先求值、后取 region/UV（见 mq_spine_instance_draw 的顺序注释）。 */
static Sequence *sequence_of_slot(mq_spine_instance *inst, int slot_index) {
    Slot *s = slot_at(inst, slot_index);
    if (!s) return nullptr;
    Attachment *a = s->getAttachment();
    if (!a) return nullptr;
    if (auto *ra = dynamic_cast<RegionAttachment *>(a)) return ra->getSequence();
    if (auto *ma = dynamic_cast<MeshAttachment *>(a)) return ma->getSequence();
    return nullptr;
}
int mq_spine_attachment_sequence_info(mq_spine_instance *inst, int slot_index, mq_spine_sequence_info *out) {
    if (!inst || !out) return MQ_SPINE_ERR_ARG;
    Slot *s = slot_at(inst, slot_index);
    if (!s) return MQ_SPINE_ERR_ARG;
    Attachment *a = s->getAttachment();
    if (!a) return MQ_SPINE_ERR_NOTFOUND;
    RegionAttachment *ra = dynamic_cast<RegionAttachment *>(a);
    MeshAttachment *ma = dynamic_cast<MeshAttachment *>(a);
    Sequence *seq = ra ? ra->getSequence() : (ma ? ma->getSequence() : nullptr);
    if (!seq) return MQ_SPINE_ERR_NOTFOUND;
    /* ⚠️ 必须先 apply 再用 region：序列帧附件的区域**不是**加载期设好的
     *（AtlasAttachmentLoader 对 sequence 只填 Sequence::getRegions()，附件的 _region 一直是 NULL，
     * 直到 Sequence::apply 把当前帧的 region 换上）。所以不 apply 就读 getRegion()
     * 会拿到 NULL → 信息全 0，看起来像"帧没变"。apply 与 computeWorldVertices 内部同款、幂等。 */
    seq->apply(s, a);
    TextureRegion *tr = ra ? ra->getRegion() : ma->getRegion();

    const int index = s->getSequenceIndex();
    const int setup = seq->getSetupIndex();
    const int frame = (index < 0) ? setup : index;
    std::memset(out, 0, sizeof *out);
    out->frames = static_cast<int>(seq->getRegions().size());
    out->index = index;
    out->setup_index = setup;
    out->id = seq->getId();
    /* 帧路径名 = Sequence::getPath(basePath, frame)（官方用于按帧名找图；这里给人看/给断言用）。
     * getPath 返回**值**（临时 String）→ 拷进实例缓冲，指针活到下次调用。 */
    const String &base = ra ? ra->getPath() : ma->getPath();
    inst->seqPathBuf = seq->getPath(base, frame).buffer();
    out->path = inst->seqPathBuf.c_str();
    if (tr) {
        out->u = tr->u; out->v = tr->v; out->u2 = tr->u2; out->v2 = tr->v2;
        out->width = tr->width; out->height = tr->height;
    }
    return MQ_SPINE_OK;
}
int mq_spine_instance_set_sequence_index(mq_spine_instance *inst, int slot_index, int frame) {
    Slot *s = slot_at(inst, slot_index);
    if (!s) return MQ_SPINE_ERR_ARG;
    Sequence *seq = sequence_of_slot(inst, slot_index);
    if (!seq) return MQ_SPINE_ERR_NOTFOUND;
    if (frame < -1 || frame >= static_cast<int>(seq->getRegions().size())) return MQ_SPINE_ERR_ARG;
    s->setSequenceIndex(frame);
    return MQ_SPINE_OK;
}

/* ---- 绘制数据导出 ----------------------------------------------------------------
 * 语义（docs/assets/06 §2.4）：**只读**产出"绘制输入"，不写任何模拟状态、不碰 GPU。
 * 顶点已在**骨架空间**求值（与 Spine 编辑器一致：**y 向上**，实测 spineboy 站立包围盒
 * y ∈ [-8, 654]、脚在 y≈0），这里叠加实例的平移/缩放/翻转；坐标空间保持不变（仍是 y 向上）。
 * ⚠️ 引擎渲染目标是 y 向下 → **上层负责翻一次 y**（runtime::spine_step：screen_y = H - world_y - ly）。
 * 本模块不自作主张翻 y：那样"骨架旋转/翻转"的语义会在两处互相抵消，排查代价极高（实测踩过）。
 * 顺序 = 骨架的**绘制顺序**（含 draw order timeline 的效果）；批处理按 (页, 混合模式) 划分，
 * 与官方运行时一致，保证叠加/正片叠底等混合的层次正确。
 * 裁剪附件（mask）产生的是**被裁剪后的几何**（官方 SkeletonClipping 路径，见下方逐槽注释）。
 * 两色着色（TintBlack）的暗色通道写在 dark_colors（可为 NULL）；序列帧（Sequence）按当前帧取区域/UV。
 * 槽区间（slotsFrom/slotsTo）可只导出骨架的一部分（SkeletonRendererSeparator）。 */
int mq_spine_instance_draw(mq_spine_instance *inst, float *vertices, float *uvs, float *colors,
                           float *dark_colors, uint32_t *indices, mq_spine_batch *batches,
                           int max_vertices, int max_indices, int max_batches, mq_spine_draw *out) {
    MQ_TRACY_ZONE_N(anim, "draw");
    if (!inst || !out || !vertices || !uvs || !colors || !indices || !batches)
        return MQ_SPINE_ERR_ARG;
    if (max_vertices <= 0 || max_indices <= 0 || max_batches <= 0) return MQ_SPINE_ERR_ARG;

    Skeleton &sk = *inst->skeleton;
    sk.updateWorldTransform(Physics_Update);
    std::memset(out, 0, sizeof *out);

    if (inst->worldVerts.size() < 4096) inst->worldVerts.setSize(4096, 0.0F);

    int vtx = 0, idx = 0, nbatch = 0;
    int curPage = -2, curBlend = -2;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool hasBounds = false;
    const float sx = inst->flipX ? -inst->scale : inst->scale;
    const float sy = inst->flipY ? -inst->scale : inst->scale;

    Vector<Slot *> &order = sk.getDrawOrder();
    SkeletonClipping &clipper = inst->clipper;
    /* 诊断（MAQUE_TRACE_SPINE_DRAW=1）：裁剪路是否真的被走到、裁掉了多少几何。
     * 为什么需要：遮罩可能"生效但没改变可见像素"，此时截图与不开启**完全一样**，
     * 只看图无法判断代码路有没有跑 —— 所以要有可观测的计数。 */
    const bool traceDraw = std::getenv("MAQUE_TRACE_SPINE_DRAW") != nullptr;
    int clipStarts = 0, clippedAtts = 0, vertsBefore = 0, vertsAfter = 0;
    for (size_t si = 0; si < order.size(); ++si) {
        Slot *slot = order[si];
        if (!slot) continue;
        /* 槽区间（SkeletonRendererSeparator）：按**声明序下标**过滤。
         * 放在最前：被过滤掉的槽不参与裁剪链，也不产几何（与"两次渲染各自只画前半/后半"等价）。 */
        if (inst->slotsFrom >= 0) {
            const int di = slot->getData().getIndex();
            if (di < inst->slotsFrom || (inst->slotsTo >= 0 && di > inst->slotsTo)) {
                clipper.clipEnd(*slot);
                continue;
            }
        }
        Attachment *att = slot->getAttachment();
        if (!att) { clipper.clipEnd(*slot); continue; }
        if (!slot->getData().getName().buffer()) { clipper.clipEnd(*slot); continue; }

        /* 附件类型判别用 C++ RTTI（本项目为 spine 打开 /GR，见 third_party/spine-cpp/CMakeLists.txt）。
         * LinkedMesh 派生自 MeshAttachment，故一个 dynamic_cast 覆盖两者。
         * 包围盒/路径/点不产生填充几何，跳过。 */
        RegionAttachment *ra = dynamic_cast<RegionAttachment *>(att);
        MeshAttachment *ma = dynamic_cast<MeshAttachment *>(att);
        ClippingAttachment *clip = dynamic_cast<ClippingAttachment *>(att);

        /* 裁剪遮罩（mask）：spine 的"裁剪附件"从该槽起生效、直到它的 endSlot。
         * 官方参考实现（SkeletonRenderer::render）：遇到裁剪附件就 clipStart 并 continue；
         * 之后每个附件若 clipper.isClipping() 就用自己的几何去 clipTriangles，
         * 取 getClippedVertices/Triangles/UVs 作为输出；每次处理完调用 clipEnd(slot)
         * （它会在到达 endSlot 时真正结束本次裁剪），循环末尾再 clipEnd() 收尾。 */
        if (clip) { clipper.clipStart(*slot, clip); ++clipStarts; continue; }
        if (!ra && !ma) { clipper.clipEnd(*slot); continue; }

        /* 颜色 = 实例色 × 槽色 × **附件色**（官方参考实现同款三项相乘）。
         * 附件自身颜色由动画/皮肤设置，漏乘会让"变红/变暗"这类受击反馈失效。 */
        const Color &ac = ra ? ra->getColor() : ma->getColor();
        const Color &slotColor = slot->getColor();
        const float cr = inst->r * slotColor.r * ac.r;
        const float cg = inst->g * slotColor.g * ac.g;
        const float cb = inst->b * slotColor.b * ac.b;
        const float ca = inst->a * slotColor.a * ac.a;
        if (ca <= 0.0F) { clipper.clipEnd(*slot); continue; }   /* 全透明：不必产出几何 */

        /* 两色着色（TintBlack）的暗色通道：官方 SkeletonRenderer 的取值是
         *   darkColor = 0xff000000 | slot.darkColor.rgb   （alpha 恒为 1，不乘实例/附件色）
         * 且 slot 没有 dark 时 = 不透明黑（等价于"不启用两色着色"）。
         * 官方源码同样的判定：`if (slot.hasDarkColor()) …` —— 注意**不是**逐附件。 */
        float dr = 0.0F, dg = 0.0F, db = 0.0F;
        if (slot->hasDarkColor()) {
            const Color &dc = slot->getDarkColor();
            dr = dc.r; dg = dc.g; db = dc.b;
        }

        int vcount = 0;
        Vector<float> *uvArr = nullptr;
        /* 局部 0 基索引（**不能**预先加 vtx 偏移）：clipTriangles 需要局部索引，
         * 官方实现也是先裁剪、再按 batch 输出。 */
        unsigned short *localIdx = inst->localIdx.buffer();
        int localIdxCount = 0;
        float *world = inst->worldVerts.buffer();

        if (ra) {
            vcount = 4;
            if (inst->worldVerts.size() < 8) inst->worldVerts.setSize(8, 0.0F);
            world = inst->worldVerts.buffer();
            ra->computeWorldVertices(*slot, world, 0, 2);
            uvArr = &ra->getUVs();
            static const unsigned short kQuad[6] = { 0, 1, 2, 2, 3, 0 };
            if (inst->localIdx.size() < 6) inst->localIdx.setSize(6, 0);
            localIdx = inst->localIdx.buffer();
            for (int k = 0; k < 6; ++k) localIdx[k] = kQuad[k];
            localIdxCount = 6;
        } else {
            vcount = static_cast<int>(ma->getWorldVerticesLength() / 2);
            if (vcount <= 0) { clipper.clipEnd(*slot); continue; }
            /* 扩容要**先做**，做完再取 buffer —— setSize 可能重分配，先取的指针会失效
             *（ASan 在这里抓到过 access-violation，就是这个原因）。 */
            if (inst->worldVerts.size() < ma->getWorldVerticesLength())
                inst->worldVerts.setSize(ma->getWorldVerticesLength(), 0.0F);
            world = inst->worldVerts.buffer();
            /* 4.2：网格走 VertexAttachment 的 6 参版本：
             *   computeWorldVertices(Slot&, start, count, float* worldVertices, offset, stride)
             * （4.2 的这个签名里**没有** Skeleton&，与 4.3 不同） */
            ma->computeWorldVertices(*slot, 0, static_cast<size_t>(vcount) * 2, world, 0, 2);
            /* ⚠️ 必须用 **getUVs()**（"normalized within the entire texture"，即归一化到整张图集页），
             * 不能用 getRegionUVs()（那是**区域局部** 0..1）——用错的表现是：每个网格附件都去采样
             * 图集页的左上角一小块，spineboy 整只角色落进空白区 → **画面里什么都看不见**
             * （踩过：截图只有 HUD/背景，经顶点/UV/颜色 trace 才定位到这一行）。 */
            uvArr = &ma->getUVs();
            Vector<unsigned short> &tri = ma->getTriangles();
            localIdxCount = static_cast<int>(tri.size());
            if (inst->localIdx.size() < tri.size()) inst->localIdx.setSize(tri.size(), 0);
            localIdx = inst->localIdx.buffer();
            for (int k = 0; k < localIdxCount; ++k) localIdx[k] = tri[static_cast<size_t>(k)];
        }

        /* ⚠️ 顺序（Sequence 正确性的关键，**别**把它挪回求值之前）：
         * `computeWorldVertices` 内部会 `_sequence->apply(&slot, this)`（4.2 源码 RegionAttachment.cpp:136
         * / MeshAttachment.cpp:239），把 region 换成**当前序列帧**并 updateRegion（UV/尺寸一起换）。
         * 所以在求值**之前**读 getRegion()/getUVs() 会拿到上一帧的区域 → 表现是
         * "多页序列帧永远画第 0 帧的页/UV"（dragon 有 5 页，页号错会直接串图）。
         * 正确顺序 = 先 computeWorldVertices，再取 region/UV。 */
        TextureRegion *tr = ra ? ra->getRegion() : ma->getRegion();
        if (!tr) { clipper.clipEnd(*slot); continue; }
        /* rendererObject 是"渲染器的贴图对象"（本引擎 = 纹理句柄），**不是** AtlasRegion*，
         * 不要解引用它 —— 只判断"有没有贴图"。UV/尺寸从 tr 自身取（TextureRegion 自带 u/v/u2/v2）。 */
        const uintptr_t texHandle = reinterpret_cast<uintptr_t>(tr->rendererObject);
        if (texHandle == 0) { clipper.clipEnd(*slot); continue; }

        /* 在裁剪区域内 → 用裁剪后的几何替换原几何 */
        if (clipper.isClipping()) {
            const int wBefore = vcount;
            const int iBefore = localIdxCount;
            /* 参数是**索引个数**，不是字节数 —— 传字节数会让它多读一倍索引并越界
             *（ASan 实测：SkeletonClipping.cpp:180 access-violation，就是这里传错了）。 */
            clipper.clipTriangles(world, localIdx, static_cast<size_t>(localIdxCount),
                                  uvArr->buffer(), 2);
            world = clipper.getClippedVertices().buffer();
            uvArr = &clipper.getClippedUVs();
            Vector<unsigned short> &ctri = clipper.getClippedTriangles();
            localIdx = ctri.buffer();
            vcount = static_cast<int>(clipper.getClippedVertices().size() >> 1);
            localIdxCount = static_cast<int>(ctri.size());
            ++clippedAtts;
            vertsBefore += wBefore; vertsAfter += vcount;
            if (traceDraw)
                std::print(stderr, "[spine2d] clip slot={} att={} v {}->{} i {}->{}\n",
                           slot->getData().getName().buffer(),
                           ra ? "region" : "mesh", wBefore, vcount, iBefore, localIdxCount);
            if (vcount <= 0 || localIdxCount <= 0) { clipper.clipEnd(*slot); continue; }
        }

        if (vtx + vcount > max_vertices || idx + localIdxCount > max_indices) {
            clipper.clipEnd(*slot);
            break;
        }

        for (int k = 0; k < vcount; ++k) {
            const float px = inst->x + world[k * 2] * sx;
            const float py = inst->y + world[k * 2 + 1] * sy;
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
            if (dark_colors) {
                /* 两色着色的暗色：alpha 恒 1（官方 0xff000000），不做实例/附件色相乘 */
                dark_colors[(vtx + k) * 4] = dr;
                dark_colors[(vtx + k) * 4 + 1] = dg;
                dark_colors[(vtx + k) * 4 + 2] = db;
                dark_colors[(vtx + k) * 4 + 3] = 1.0F;
            }
            if (!hasBounds) { minX = maxX = px; minY = maxY = py; hasBounds = true; }
            else {
                if (px < minX) minX = px; if (px > maxX) maxX = px;
                if (py < minY) minY = py; if (py > maxY) maxY = py;
            }
        }
        /* 索引在这里才加 vtx 偏移（裁剪路径产出的是局部索引） */
        for (int k = 0; k < localIdxCount; ++k)
            indices[idx + k] = static_cast<uint32_t>(vtx) + localIdx[k];
        idx += localIdxCount;

        /* 页号：直接用 spine 给 region 的页索引（AtlasPage::index），不反查、不缓存指针。 */
        int page = -1;
        for (const auto &kv : inst->owner->handleToPage)
            if (kv.first == static_cast<uint64_t>(texHandle)) { page = kv.second; break; }
        const int blend = static_cast<int>(slot->getData().getBlendMode());
        const int segCount = localIdxCount;
        /* ⚠️ 先累加 vtx 再判批次数上限：本槽的索引**已经**写进 indices 了，
         * 若在 break 之后才 `vtx += vcount`，out.vertex_count 会小于索引用到的最小顶点数
         * （= 导出数据自相矛盾，下游按 vertex_count 建缓冲就会读越界）。 */
        vtx += vcount;
        if (nbatch > 0 && page == curPage && blend == curBlend) {
            batches[nbatch - 1].index_count += static_cast<uint32_t>(segCount);
        } else {
            if (nbatch >= max_batches) { clipper.clipEnd(*slot); break; }
            curPage = page;
            curBlend = blend;
            batches[nbatch].page = page;
            batches[nbatch].blend = blend;
            batches[nbatch].first_index = static_cast<uint32_t>(idx - segCount);
            batches[nbatch].index_count = static_cast<uint32_t>(segCount);
            ++nbatch;
        }
        clipper.clipEnd(*slot);
    }
    clipper.clipEnd();

    if (traceDraw)
        std::print(stderr, "[spine2d] draw: clipStarts={} 被裁剪附件={} 顶点 {}->{} (总 {} 顶点)\n",
                   clipStarts, clippedAtts, vertsBefore, vertsAfter, vtx);

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

/* ---- 宿主扩展点（spine-cpp 4.2 要求宿主提供）----
 * `SpineExtension::getInstance()` 首次使用时调用 `spine::getDefaultExtension()` —— 留给宿主实现
 * （分配器 / 文件读取 / 日志）。官方各平台样例都放在自己的 main 里；引擎侧由本模块提供默认实现
 * （上游 `DefaultSpineExtension`：标准 malloc/free + FILE* 读文件）。
 * 与引擎约定：骨架/图集都从**内存**进来（调用方用 VFS 读），故 `_readFile` 正常路径不会用到；
 * 将来要让 spine 直接走 VFS，可 `SpineExtension::setInstance` 换自定义实现。
 *
 * ⚠️ 为什么和 spine2d.cpp 放在一起：本定义若单独成 TU 放进 .a，GNU ld 单趟扫描（先本归档、
 * 后 spine-cpp）不会回扫 → `undefined reference to spine::getDefaultExtension()`。
 * 放在**必然被拉入**的 TU（消费者都引用 mq_spine_*）里即可，无需链接器 group。 */
namespace spine {
SpineExtension* getDefaultExtension() {
    return new DefaultSpineExtension();
}
} // namespace spine
