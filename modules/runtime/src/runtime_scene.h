/* Scene / 场景实例 / 快照写入器：节点树是唯一层级真相（docs/12、docs/32）。
 * 只有 modules/runtime 的 .cpp 包含。 */
#pragma once

#include "runtime_common.h"

namespace maque::rt {

struct Scene {
    scn::Scene desc;
    std::string text_;
    /* 节点 id 采用**单调递增**（不复用槽位）：docs/02 R4/docs/07 §3 要求陈旧句柄不得
     * 静默指向新对象；不复用即从根上消除"销毁后 id 指到别人"的风险。
     * （世代号在等价意义上内化于 id：同一个 id 永恒指向同一个节点或不存在。） */
    int next_id = 0;
    Engine* eng = nullptr;                  // 组件析构需要反射 ctx（remove/重建时）
    std::unordered_map<int, SceneNode> nodes;   // id → 节点（id 只是稳定键；层级看 children）
    std::vector<int> roots;                    // 根节点 id（升序）
    std::vector<std::pair<int, int>> visit_stack_;   // visit 的 DFS 栈（复用容量，不每帧分配）

    Scene() = default;
    ~Scene() { destroy_all(); }
    Scene(const Scene&) = delete; Scene& operator=(const Scene&) = delete;
    Scene(Scene&& o) noexcept
        : desc(std::move(o.desc)), text_(std::move(o.text_)), next_id(o.next_id), eng(o.eng),
          nodes(std::move(o.nodes)), roots(std::move(o.roots)) {
        o.eng = nullptr;             // 所有权转移：源对象不得再析构已移交的资源
    }
    Scene& operator=(Scene&& o) noexcept {
        if (this == &o) return *this;
        destroy_all();
        desc = std::move(o.desc);
        text_ = std::move(o.text_);
        next_id = o.next_id;
        eng = o.eng; o.eng = nullptr;
        nodes = std::move(o.nodes);
        roots = std::move(o.roots);
        return *this;
    }
    /* 析构全部组件 blob（含动态成员）。 */
    void destroy_all() {
        if (!eng) { nodes.clear(); roots.clear(); return; }
        for (auto& [k, ent] : nodes)
            for (auto& [t, b] : ent.comps) destroy_blob(*eng, t, b);
        nodes.clear();
        roots.clear();
    }

    bool deserialize(Engine& eng, const std::string& text);
    void instantiate(Engine& eng);
    int  spawn();
    SceneNode* at(int index);
    const SceneNode* at(int index) const;
    void remove(int index);
    int  count() const { return (int)nodes.size(); }

    /* ---- 场景树结构（唯一层级真相；所有挂接/摘下都经这里，保持 children 有序） ---- */
    void attach(int child, int parent);        // 挂到 parent 下（按 id 升序插入）；parent<0 = 根
    void detach(int child);                    // 从原父（或根表）摘下，parent 置 -1
    void rebuild_tree();                       // 从 parent 字段重建 children/roots（快照/反序列化用）
    int  child_count(int parent) const;
    int  child_at(int parent, int index) const;   // index 按 children 顺序（= id 升序）

    /* 传统场景树访问：前序（父 → 子；兄弟按 id/声明序）。fn(id, node, depth)。
     * 这是引擎唯一的全树遍历方式：渲染提取、世界变换累积、UI 命中、诊断 dump 都走它，
     * 不再每帧重建"id→父"的扁平索引，也不再按 unordered_map 的哈希序扫描。 */
    template <class Fn>
    void visit(Fn&& fn) {
        visit_stack_.clear();
        visit_stack_.reserve(nodes.size());
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) visit_stack_.emplace_back(*it, 0);
        while (!visit_stack_.empty()) {
            const auto [id, depth] = visit_stack_.back();
            visit_stack_.pop_back();
            SceneNode* n = at(id);
            if (!n) continue;
            fn(id, *n, depth);
            for (auto k = n->children.rbegin(); k != n->children.rend(); ++k)
                visit_stack_.emplace_back(*k, depth + 1);
        }
    }

    /* 整棵树 id 序列（前序；兄弟 id 升序）。整场景系统统一按树序处理，
     * 不再各自扫 unordered_map 再排序。 */
    void collect_preorder(std::vector<int>& out) {
        out.clear();
        out.reserve(nodes.size());
        visit([&](int id, SceneNode&, int) { out.push_back(id); });
    }

    /* 每帧一趟树访问：局部 → 世界（2D 位置 + 3D 矩阵 + active/angle/scale/opacity）。 */
    void update_world(Engine& eng);
    void update_world3d(Engine& eng);
    uint64_t checksum(Engine& eng);     // 世界状态校验和（确定性；反射驱动）
    void encode(Engine& eng, SnapshotWriter& w);   // 世界 → 字节（二进制 codec）
    size_t decode(Engine& eng, const uint8_t* in, size_t n);
    void extract(Engine& eng, std::vector<mq_renderer_item>& out);
};

/* ---- 场景实例（docs/32）：`.scn` = 节点树 + 行为（scripts）+ 场景级配置。
 * 每次加载/实例化 = 一个 SceneInstance：独立 VM 组（bus）、独立生命周期与事件作用域。
 * 实例树：根场景 = 0 号；`scn.SceneInstance` 组件在加载期把嵌套 .scn 实例化为子实例。
 * 所有结构变化都发生在帧边界（加载期或动态请求队列），保证确定性。 */
struct SceneInstanceRuntime {
    int id = -1;
    int parent = -1;                 // 父实例（根 = -1）
    int attach_entity = -1;          // 父世界里的挂载壳实体（根 = -1）
    std::string source;              // VFS 路径（"/dir/x.scn"；根 = scn_vfs_path）
    std::string base;                // 资源解析基准（相对 VFS 根；根场景 = ""）
    std::string params;              // 实例参数（JSON 文本；docs/32 §16.2）
    std::vector<int> roots;          // 本实例的顶层实体（parent == attach_entity 且 owner==id）
    std::vector<int> children;       // 子实例（创建/声明顺序）
    mq_script_bus* bus = nullptr;    // 本实例的 VM 组（总线拥有 VM；docs/34 新路径为 null）
    /* docs/34 P0：每语言一个 VM 时，本实例的脚本实例（每脚本一个，声明序）。 */
    std::vector<mq_script_instance*> module_instances;
    std::vector<std::string> module_keys;
    std::vector<std::string> scripts;     // 相对 base 的脚本路径（声明序）
    std::vector<uint64_t> script_hashes;  // 内容哈希（热重载）
    bool paused = false;
    bool active = true;
    bool detached = false;           // 已烘焙：内容属于父场景（保存时写出子树，加载时不再实例化）
    bool owns_shell = false;         // 动态实例自动建的挂载壳（卸载时一并删除）
    int state = 0;                   // 0=pending 1=active 2=destroyed 3=error 4=pending-unload
    std::string error;
};

/* ---- 快照写入器（Scene::encode 的字节汇；无反射依赖） ---- */
struct SnapshotWriter {
    std::vector<uint8_t> buf;
    void bytes(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        buf.insert(buf.end(), b, b + n);
    }
    template <class T> void pod(T v) { bytes(&v, sizeof(T)); }
};

}  // namespace maque::rt
