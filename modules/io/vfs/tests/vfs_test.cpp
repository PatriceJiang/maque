// VFS 单元测试（file:// 后端 + 最长前缀挂载）
#include "core/vfs/mq_vfs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}
static std::string readall(mq_vfs* v, const char* path) {
    void* d = nullptr; size_t n = 0;
    if (mq_vfs_read(v, path, &d, &n) != 0) return "<err>";
    std::string s((char*)d, n); mq_vfs_free(d); return s;
}

int main() {
    fs::path root = fs::temp_directory_path() / "maque_vfs_test";
    fs::remove_all(root);
    put(root / "base" / "a.txt", "hello");
    put(root / "base" / "sub" / "b.txt", "world");
    put(root / "over" / "sub" / "b.txt", "override");

    mq_vfs* vfs = mq_vfs_create();
    CHECK(vfs != nullptr);
    CHECK(mq_vfs_mount(vfs, "/", ("file://" + (root / "base").string()).c_str()) == 0);
    CHECK(mq_vfs_mount(vfs, "/sub", ("file://" + (root / "over" / "sub").string()).c_str()) == 0);

    CHECK(readall(vfs, "/a.txt") == "hello");
    CHECK(readall(vfs, "/sub/b.txt") == "override");      // 最长前缀覆盖
    CHECK(mq_vfs_exists(vfs, "/a.txt"));
    CHECK(!mq_vfs_exists(vfs, "/nope.txt"));

    void* d = nullptr; size_t n = 0;
    CHECK(mq_vfs_read(vfs, "/missing", &d, &n) != 0);

#ifdef MQ_HAVE_CURL
    // 直接 URL（经 libcurl）：file:// 本地抓取
    {
        std::string url = "file://" + (root / "base" / "a.txt").string();
        void* u = nullptr; size_t un = 0;
        CHECK(mq_vfs_read(vfs, url.c_str(), &u, &un) == 0);
        if (u) { std::string s((char*)u, un); mq_vfs_free(u); CHECK(s == "hello"); }
    }
#endif

    mq_vfs_destroy(vfs);
    fs::remove_all(root);

    if (g_failures == 0) { std::printf("vfs_test: all checks passed\n"); return 0; }
    std::printf("vfs_test: %d failure(s)\n", g_failures);
    return 1;
}
