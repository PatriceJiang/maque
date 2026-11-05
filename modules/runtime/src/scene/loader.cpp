/* 场景 · 反序列化与实例化（.scn → 节点树；load_entity / expand_prefab，docs/12 §5）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

bool Scene::deserialize(Engine& eng, const std::string& text) {
    text_ = text;
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) return false;
    int rc = mq_persist_apply_json(eng.reflect, eng.scene_type, dom, &desc);
    mq_json_free(dom);
    return rc == 0;
}

/* VFS 路径拼接（docs/32 §5.1）：`base` 是相对 VFS 根的目录（"" 或 "dir/"）；
 * `rel` 为绝对路径（/x、http(s)://、builtin:/…）时原样返回，否则相对 base 解析并做
 * "." / ".." 归一化（不允许逃出 VFS 根）。 */
std::string vfs_join(const std::string& base, const std::string& rel) {
    if (rel.empty()) return std::string();
    if (rel[0] == '/' || rel.find("://") != std::string::npos || rel.rfind("builtin:", 0) == 0)
        return rel;
    std::string joined = (base.empty() ? std::string() : base) + rel;
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= joined.size()) {
        size_t slash = joined.find('/', i);
        std::string seg = joined.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "..") { if (!segs.empty()) segs.pop_back(); }
        else if (!seg.empty() && seg != ".") segs.push_back(seg);
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    std::string out = "/";
    for (size_t k = 0; k < segs.size(); ++k) { if (k) out += '/'; out += segs[k]; }
    return out;
}

/* 场景资产所在目录（相对 VFS 根，含结尾 '/'）——用于嵌套实例的资源基准。 */
std::string vfs_dirname(const std::string& vfs_path) {
    const size_t slash = vfs_path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return vfs_path.substr(1, slash == 0 ? 0 : slash - 1) + (slash > 1 ? "/" : "");
}

/* 加载实体树；prefab 就地展开。
 * docs/12 §7：**禁止循环实例化** —— 用 chain（当前展开链上的 prefab 路径）+ 深度上限检测，
 * 命中则报结构化错误并跳过该实例（绝不递归到栈溢出）。 */

/* 载入并展开一个 prefab：把它的根实体挂到 `parent` 下（-1 = 场景根）。
 * 供两处使用：
 *   · `scn.PrefabInstance` 组件 → 展开为**该实体的子实体**（包一层壳，便于整体变换/显隐）；
 *   · 实体的 `source` 简写 → **平级展开**（prefab 的根直接成为兄弟，不套壳）。
 * `name_prefix` 非空时给展开出的**根实体**名加前缀 —— 同一个 prefab 实例多次时，
 * 名字必须能区分，否则场景里会出现一堆同名实体（脚本按名查找就只会命中第一个）。
 * `chain` 为当前展开链（循环引用检测，docs/12 §7）；`depth` 限制递归层数。 */
static void expand_prefab(Engine& eng, Scene& sc, const std::string& src, int parent,
                          std::vector<std::string>* chain, int depth,
                          const std::string& name_prefix = std::string()) {
    /* 相对路径按**当前场景实例的资源基准**解析（嵌套场景内的 prefab 不再按根场景目录解析）。 */
    const std::string src_vfs = vfs_join(eng.building_base, src);
    if (chain) {
        for (const auto& c : *chain)
            if (c == src_vfs) {
                std::print(stderr, "runtime: cyclic prefab reference '{}' — skipped\n", src);
                mq_obs_event(eng.obs, "runtime", 3, "prefab_cycle");
                return;
            }
    }
    void* pd = nullptr; size_t pn = 0;
    if (mq_vfs_read(eng.vfs, src_vfs.c_str(), &pd, &pn) != MQ_VFS_OK) {
        std::print(stderr, "runtime: prefab not found '{}' (resolved '{}')\n", src, src_vfs);
        return;
    }
    std::string ptext((char*)pd, pn);
    mq_vfs_free(pd);
    mq_json* pdom = mq_json_parse(ptext.c_str(), ptext.size());
    if (!pdom) return;
    std::vector<std::string> child_chain = chain ? *chain : std::vector<std::string>{};
    child_chain.push_back(src_vfs);
    const mq_json* pents = mq_json_get(pdom, "entities");
    const uint32_t pcount = mq_json_len(pents);
    for (uint32_t i = 0; i < pcount; ++i) {
        int child = load_entity(eng, sc, mq_json_at(pents, i), parent, &child_chain, depth + 1);
        if (child >= 0 && !name_prefix.empty())
            if (SceneNode* ce = sc.at(child); ce && !ce->name.empty()) ce->name = name_prefix + ce->name;
    }
    mq_json_free(pdom);
    std::print("runtime: prefab '{}' instantiated under entity {}\n", src, parent);
}

int load_entity(Engine& eng, Scene& sc, const mq_json* e, int parent,
                std::vector<std::string>* chain, int depth) {
    if (depth > 64) {
        std::print(stderr, "runtime: prefab nesting too deep (>64), aborted at depth {}\n", depth);
        return -1;
    }
    const char* nm = mq_json_str(mq_json_get(e, "name"), nullptr);
    const mq_json* comps = mq_json_get(e, "components");
    uint32_t cn = mq_json_len(comps);
    const mq_json* ch = mq_json_get(e, "children");
    uint32_t kn = mq_json_len(ch);
    const char* inline_src = mq_json_str(mq_json_get(e, "source"), nullptr);

    /* **平级展开**（`source` 简写，且自身无组件/无子节点）：纯展开指令，**不 spawn 壳实体**。
     * 用于"重复的平级对象"——坦克的 20 个履带印、5 个残骸本来是各自独立的根级实体，
     * 用 PrefabInstance 会多套一层壳（改变层级与 entity_find 语义，还白增 25 个空实体）。
     * 带 components/children 时仍走下面的"壳 + 子实体"路径（那种情况需要一个实体挂它们）。 */
    if (inline_src && *inline_src && cn == 0 && kn == 0) {
        const char* pfx = mq_json_str(mq_json_get(e, "name_prefix"), nullptr);
        expand_prefab(eng, sc, inline_src, parent, chain, depth, pfx ? pfx : "");
        return parent;                                  // 未创建实体 → 返回父（调用方按 <0 判错）
    }

    int idx = sc.spawn();
    if (idx < 0) return -1;
    eng.entity_instance[idx] = eng.building_instance;    // 归属（事件作用域/序列化过滤用）
    sc.attach(idx, parent);                              // 层级真相：父的 children（id 升序）
    SceneNode* ent = sc.at(idx);
    if (nm) ent->name = nm;

    for (uint32_t i = 0; i < cn; ++i) {
        const mq_json* c = mq_json_at(comps, i);
        const char* tname = mq_json_str(mq_json_get(c, "type"), nullptr);
        if (!tname) continue;
        const mq_type_desc* d = mq_reflect_type_by_name(eng.reflect, tname);
        if (!d) { std::print("runtime: unknown component '{}'\n", tname); continue; }
        ComponentSlot& slot = component_slot(*ent, d);
        std::vector<uint8_t>& blob = slot.bytes;
        if (!blob.empty()) destroy_blob(eng, d->type_id, blob);
        construct_blob(eng, d, blob);
        mq_persist_apply_json(eng.reflect, d, c, blob.data());
    }
    /* Prefab 实例（docs/12 §5）：按 source 载入 .prefab.json，就地展开为子实体。
     * detached=true 表示**已展开**（内容已是本场景的一部分），不再展开。
     * ⚠️ 只认 `scn.PrefabInstance`：`scn.SceneInstance` 也有 source/detached 字段，
     * 若按字段名盲扫会被当成 prefab 再展开一次（docs/32 §5 的两类实例必须分开）。 */
    for (auto& [tid, blob] : ent->comps) {
        const mq_type_desc* d = blob.desc;
        if (!d || std::strcmp(d->name, "scn.PrefabInstance") != 0) continue;
        const std::string* src = read_str(d, blob.data(), "source");
        if (!src || src->empty()) continue;
        bool detached = false;
        if (const mq_field_desc* fd = mq_reflect_field(d, "detached")) detached = field_get_bool(fd, blob.data());
        if (detached) continue;
        expand_prefab(eng, sc, *src, idx, chain, depth);
    }

    /* 场景实例（docs/32 §5）：`scn.SceneInstance.source` 指向一个 .scn → 在该实体下实例化
     * 子场景（独立 VM 组/生命周期/事件作用域）。detached=true 表示已烘焙，不再加载源。 */
    {
        const mq_type_desc* sid = mq_reflect_type_by_name(eng.reflect, "scn.SceneInstance");
        if (sid) {
            auto it = ent->comps.find(sid->type_id);
            if (it != ent->comps.end()) {
                std::vector<uint8_t>& blob = it->second.bytes;
                const std::string* src = read_str(sid, blob.data(), "source");
                const mq_field_desc* det = mq_reflect_field(sid, "detached");
                const bool detached = det && field_get_bool(det, blob.data());
                if (!detached && src && !src->empty()) {
                    std::string src_vfs;
                    const int iid = eng.resolve_instance_path(*src, eng.building_base, src_vfs)
                                        ? eng.create_instance(eng.building_instance, idx, src_vfs)
                                        : -1;
                    int state = 0; uint16_t err = 0;
                    if (iid < 0) { state = 3; err = 1; }
                    else {
                        SceneInstanceRuntime* ci = eng.instance_at(iid);
                        /* 静态实例参数（docs/32 §16.2）：构建前写入，子场景 on_start 可见。 */
                        if (ci) {
                            if (const std::string* pp = read_str(sid, blob.data(), "params"))
                                ci->params = *pp;
                        }
                        if (!ci || !eng.build_instance(*ci, (int)eng.scene_chain.size())) {
                            state = 3; err = (uint16_t)(ci ? 2 : 1);
                            if (ci) { ci->state = 3; ci->error = "instance build failed"; }
                        } else {
                            state = 1;
                            if (const mq_field_desc* f = mq_reflect_field(sid, "paused"))
                                ci->paused = field_get_bool(f, blob.data());
                            if (const mq_field_desc* f = mq_reflect_field(sid, "active"))
                                ci->active = field_get_bool(f, blob.data());
                            if (!ci->active) { if (SceneNode* se = eng.scene->at(idx)) se->active = false; }
                        }
                    }
                    if (const mq_field_desc* f = mq_reflect_field(sid, "instance_id"))
                        field_set_i32(f, blob.data(), (int32_t)iid);
                    if (const mq_field_desc* f = mq_reflect_field(sid, "state"))
                        field_set_i32(f, blob.data(), state);
                    if (const mq_field_desc* f = mq_reflect_field(sid, "error"))
                        *(uint16_t*)(blob.data() + f->offset) = err;
                }
            }
        }
    }

    /* 实体的 `source` 简写（平级展开）：prefab 的**根实体直接成为本实体的兄弟**，不套壳。
     * `source` 与自身组件同时出现属于**误用**（组件会挂在一个多余的空壳上）：
     * 这里明确警告，但仍保留壳（否则那些组件无处安放）。 */
    if (inline_src && *inline_src) {
        std::print(stderr, "runtime: entity '{}' 同时给了 source 与 components/children —— "
                           "source 本意是平级展开、不套壳；此处为了容纳组件仍创建了壳实体\n",
                   nm ? nm : "?");
        const char* pfx = mq_json_str(mq_json_get(e, "name_prefix"), nullptr);
        expand_prefab(eng, sc, inline_src, idx, chain, depth, pfx ? pfx : "");
    }
    for (uint32_t i = 0; i < kn; ++i) load_entity(eng, sc, mq_json_at(ch, i), idx, chain, depth);
    return idx;
}

void Scene::instantiate(Engine& eng) {
    this->eng = &eng;
    mq_json* dom = mq_json_parse(text_.c_str(), text_.size());
    if (!dom) return;
    const mq_json* ents_json = mq_json_get(dom, "entities");
    uint32_t n = mq_json_len(ents_json);
    for (uint32_t i = 0; i < n; ++i) load_entity(eng, *this, mq_json_at(ents_json, i), -1);
    mq_json_free(dom);
    std::print("runtime: instantiated {} root entities\n", n);
}

}  // namespace maque::rt
