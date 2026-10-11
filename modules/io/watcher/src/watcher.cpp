/* 热重载监视实现（docs/08）：轮询式、无线程、无墙钟参与模拟判定。
 * 时间戳 = 文件大小(低 32 位) ⊕ mtime 纳秒(高 32 位)；只看"是否变化"，
 * 不解释绝对时间 → 回放不受影响（docs/10 D3）。 */
#include "core/watcher/mq_watcher.h"

#include <string>
#include <vector>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct Target {
    std::string path;
    uint64_t    stamp = 0;
};

} // namespace

struct mq_watcher {
    std::vector<Target> targets;
};

extern "C" {

mq_watcher* mq_watcher_create(void) { return new mq_watcher(); }
void        mq_watcher_destroy(mq_watcher* w) { delete w; }

uint64_t mq_watcher_stat(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    const fs::path p(path);
    if (!fs::exists(p, ec) || ec) return 0;
    const auto size = fs::file_size(p, ec);
    if (ec) return 0;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    const uint64_t s = (uint64_t)size;
    const uint64_t m = (uint64_t)t.time_since_epoch().count();
    return (s & 0xFFFFFFFFull) | (m << 32);
}

int mq_watcher_add(mq_watcher* w, const char* path) {
    if (!w || !path) return -1;
    for (size_t i = 0; i < w->targets.size(); ++i)
        if (w->targets[i].path == path) return (int)i;
    Target t;
    t.path = path;
    t.stamp = mq_watcher_stat(path);
    w->targets.push_back(t);
    return (int)w->targets.size() - 1;
}

int mq_watcher_remove(mq_watcher* w, const char* path) {
    if (!w || !path) return 1;
    for (auto it = w->targets.begin(); it != w->targets.end(); ++it)
        if (it->path == path) { w->targets.erase(it); return 0; }
    return 2;
}

uint32_t mq_watcher_count(mq_watcher* w) { return w ? (uint32_t)w->targets.size() : 0u; }

const char* mq_watcher_path_at(mq_watcher* w, uint32_t index) {
    if (!w || index >= w->targets.size()) return nullptr;
    return w->targets[index].path.c_str();
}

uint32_t mq_watcher_poll(mq_watcher* w, mq_watch_fn fn, void* user) {
    if (!w) return 0;
    uint32_t changed = 0;
    for (auto& t : w->targets) {
        const uint64_t now = mq_watcher_stat(t.path.c_str());
        if (now == t.stamp) continue;
        const uint64_t before = t.stamp;
        t.stamp = now;
        ++changed;
        if (!fn) continue;
        mq_watch_kind kind = MQ_WATCH_MODIFIED;
        if (now == 0) kind = MQ_WATCH_REMOVED;
        else if (before == 0) kind = MQ_WATCH_ADDED;
        if (fn(user, t.path.c_str(), kind) != 0) break;
    }
    return changed;
}

int mq_watcher_set_stamp(mq_watcher* w, const char* path, uint64_t stamp) {
    if (!w || !path) return 1;
    for (auto& t : w->targets)
        if (t.path == path) { t.stamp = stamp; return 0; }
    return 2;
}

uint64_t mq_watcher_get_stamp(mq_watcher* w, const char* path) {
    if (!w || !path) return 0;
    for (auto& t : w->targets)
        if (t.path == path) return t.stamp;
    return 0;
}

} // extern "C"
