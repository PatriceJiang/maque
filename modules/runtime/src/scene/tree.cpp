/* 场景 · 节点树（唯一层级真相；children 有序 = 声明/id 序，docs/12）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

int Scene::spawn() {
    const int idx = next_id++;                   // 单调：永不复用（陈旧 id 不会别名新节点）
    nodes.emplace(idx, SceneNode{});
    roots.insert(std::lower_bound(roots.begin(), roots.end(), idx), idx);
    return idx;
}
SceneNode* Scene::at(int index) {
    if (index < 0) return nullptr;
    auto it = nodes.find(index);
    return it == nodes.end() ? nullptr : &it->second;
}
const SceneNode* Scene::at(int index) const {
    if (index < 0) return nullptr;
    auto it = nodes.find(index);
    return it == nodes.end() ? nullptr : &it->second;
}

/* 挂到 parent 下（parent<0 = 场景根）。children 保持 id 升序：
 * 加载/动态创建天然递增 → 顺序 == 声明序；重挂时也维持旧 childAt 的确定性语义。 */
void Scene::attach(int child, int parent) {
    if (child < 0) return;
    auto cit = nodes.find(child);
    if (cit == nodes.end()) return;
    if (cit->second.parent == parent) return;
    detach(child);
    cit = nodes.find(child);                     // detach 只改容器内容，不 erase 节点
    if (parent >= 0 && parent != child) {
        auto pit = nodes.find(parent);
        if (pit != nodes.end()) {
            auto& kids = pit->second.children;
            kids.insert(std::lower_bound(kids.begin(), kids.end(), child), child);
            cit->second.parent = parent;
            return;
        }
    }
    roots.insert(std::lower_bound(roots.begin(), roots.end(), child), child);
    cit->second.parent = -1;
}

void Scene::detach(int child) {
    auto it = nodes.find(child);
    if (it == nodes.end()) return;
    const int p = it->second.parent;
    if (p >= 0) {
        auto pit = nodes.find(p);
        if (pit != nodes.end()) {
            auto& kids = pit->second.children;
            auto k = std::lower_bound(kids.begin(), kids.end(), child);
            if (k != kids.end() && *k == child) kids.erase(k);
        }
    }
    auto r = std::lower_bound(roots.begin(), roots.end(), child);
    if (r != roots.end() && *r == child) roots.erase(r);
    it->second.parent = -1;
}

/* 从 parent 字段重建整棵树（快照 decode / 反序列化后调用）。
 * 脏数据保护：父不存在、自环、祖先环 → 该节点降为根（绝不产生环或悬垂 children）。 */
void Scene::rebuild_tree() {
    roots.clear();
    std::vector<int> ids;
    ids.reserve(nodes.size());
    for (auto& [id, n] : nodes) { n.children.clear(); ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        SceneNode& n = nodes[id];
        const int p = n.parent;
        if (p == id || p < 0 || nodes.find(p) == nodes.end()) { n.parent = -1; roots.push_back(id); continue; }
        bool cycle = false;
        for (int cur = p, guard = 0; cur >= 0 && guard < 4096; ++guard) {
            if (cur == id) { cycle = true; break; }
            auto ci = nodes.find(cur);
            if (ci == nodes.end()) break;
            cur = ci->second.parent;
        }
        if (cycle) { n.parent = -1; roots.push_back(id); continue; }
        nodes[p].children.push_back(id);
    }
    std::sort(roots.begin(), roots.end());
    for (auto& [id, n] : nodes) std::sort(n.children.begin(), n.children.end());
}

int Scene::child_count(int parent) const {
    if (parent < 0) return (int)roots.size();
    auto it = nodes.find(parent);
    return it == nodes.end() ? 0 : (int)it->second.children.size();
}

int Scene::child_at(int parent, int index) const {
    if (index < 0) return -1;
    if (parent < 0) return index < (int)roots.size() ? roots[(size_t)index] : -1;
    auto it = nodes.find(parent);
    if (it == nodes.end() || index >= (int)it->second.children.size()) return -1;
    return it->second.children[(size_t)index];
}

void Scene::remove(int index) {
    if (nodes.find(index) == nodes.end()) {
        if (eng) {
            eng->entity_instance.erase(index);
            eng->node_events.erase(index);
        }
        return;
    }
    /* 销毁**整棵子树**（Node 树语义：父节点消失则后代一并消失）。
     * 先收集后删除：组件析构/事件表清理都按 id 进行，不能边遍历边 erase。 */
    std::vector<int> doomed;
    std::unordered_set<int> seen;
    std::vector<int> stack{index};
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        auto it = nodes.find(cur);
        if (it == nodes.end() || !seen.insert(cur).second) continue;
        doomed.push_back(cur);
        for (int k : it->second.children) stack.push_back(k);
    }
    detach(index);                       // 只把子树根从父节点/根表摘下
    for (int id : doomed) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        if (eng) for (auto& [t, b] : it->second.comps) destroy_blob(*eng, t, b.bytes);
        nodes.erase(it);
        auto r = std::lower_bound(roots.begin(), roots.end(), id);
        if (r != roots.end() && *r == id) roots.erase(r);
    }
    if (eng) for (int id : doomed) {
        eng->entity_instance.erase(id);
        eng->node_events.erase(id);
    }
    /* 不回收 id（单调分配）→ 之后 entity_valid(旧 id) 恒为 false */
}

}  // namespace maque::rt
