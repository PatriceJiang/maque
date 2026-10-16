// watcher 单元测试（docs/08 热重载）：帧边界检测、增/删/改、注入式时间戳。
#include "core/watcher/mq_watcher.h"

#include <print>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

struct Log { std::vector<std::string> events; };
static int on_change(void* user, const char* path, mq_watch_kind kind) {
    auto* l = (Log*)user;
    const char* names[] = { "modified", "removed", "added" };
    l->events.push_back(std::string(names[kind]) + ":" + path);
    return 0;
}

#include "maque_test_fs.h"

int main() {
    fs::path root;
    if (!maque_test_setup_dir("watcher", root)) return 77;
    fs::create_directories(root);
    const fs::path a = root / "a.js";
    const fs::path b = root / "b.scn";
    maque_put_file(a, "let x = 1;\n");

    mq_watcher* w = mq_watcher_create();
    CHECK(w != nullptr);
    CHECK(mq_watcher_add(w, a.string().c_str()) >= 0);
    CHECK(mq_watcher_add(w, b.string().c_str()) >= 0);
    CHECK(mq_watcher_count(w) == 2);
    CHECK(mq_watcher_add(w, a.string().c_str()) == 0);          // 幂等

    Log log;
    CHECK(mq_watcher_poll(w, on_change, &log) == 0);            // 无变化
    CHECK(log.events.empty());

    maque_put_file(a, "let x = 2; // longer\n");
    CHECK(mq_watcher_poll(w, on_change, &log) == 1);
    CHECK(log.events.size() == 1);
    CHECK(log.events[0] == "modified:" + a.string());

    log.events.clear();
    maque_put_file(b, "scene");
    CHECK(mq_watcher_poll(w, on_change, &log) == 1);
    CHECK(log.events[0] == "added:" + b.string());

    log.events.clear();
    fs::remove(a);
    CHECK(mq_watcher_poll(w, on_change, &log) == 1);
    CHECK(log.events[0] == "removed:" + a.string());

    CHECK(mq_watcher_set_stamp(w, b.string().c_str(), 12345) == 0);
    CHECK(mq_watcher_get_stamp(w, b.string().c_str()) == 12345u);
    log.events.clear();
    CHECK(mq_watcher_poll(w, on_change, &log) == 1);            // 注入值 != 真实值 → 报变化
    CHECK(log.events.size() == 1);

    CHECK(mq_watcher_remove(w, b.string().c_str()) == 0);
    CHECK(mq_watcher_count(w) == 1);
    CHECK(mq_watcher_path_at(w, 0) != nullptr);
    CHECK(mq_watcher_path_at(w, 9) == nullptr);

    mq_watcher_destroy(w);
    fs::remove_all(root);

    if (g_failures == 0) { std::print("watcher_test: all checks passed\n"); return 0; }
    std::print("watcher_test: {} failure(s)\n", g_failures);
    return 1;
}
