// VFS 单元测试（docs/04）：file/mem/zip 后端 + 最长前缀挂载 + 内容哈希校验
#include "core/vfs/mq_vfs.h"
#include <print>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}
static std::string readall(mq_vfs* v, const char* path) {
    void* d = nullptr; size_t n = 0;
    if (mq_vfs_read(v, path, &d, &n) != MQ_VFS_OK) return "<err>";
    std::string s((char*)d, n); mq_vfs_free(d); return s;
}

/* 生成 stored（未压缩）zip：本地头 + 数据 + 中央目录 + EOCD。 */
static std::string make_stored_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, cd;
    for (auto& [name, data] : files) {
        const uint32_t off = (uint32_t)out.size();
        auto put16 = [&](std::string& s, uint16_t v) { s.push_back((char)(v & 0xFF)); s.push_back((char)(v >> 8)); };
        auto put32 = [&](std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 0xFF)); };
        std::string lh;
        put32(lh, 0x04034b50); put16(lh, 20); put16(lh, 0); put16(lh, 0); put16(lh, 0); put16(lh, 0);
        put32(lh, 0); put32(lh, (uint32_t)data.size()); put32(lh, (uint32_t)data.size());
        put16(lh, (uint16_t)name.size()); put16(lh, 0);
        out += lh + name + data;
        std::string ch;
        put32(ch, 0x02014b50); put16(ch, 20); put16(ch, 20); put16(ch, 0); put16(ch, 0);
        put16(ch, 0); put16(ch, 0); put32(ch, 0); put32(ch, (uint32_t)data.size()); put32(ch, (uint32_t)data.size());
        put16(ch, (uint16_t)name.size()); put16(ch, 0); put16(ch, 0); put16(ch, 0); put16(ch, 0);
        put32(ch, 0); put32(ch, off);
        cd += ch + name;
    }
    auto put16 = [](std::string& s, uint16_t v) { s.push_back((char)(v & 0xFF)); s.push_back((char)(v >> 8)); };
    auto put32 = [](std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 0xFF)); };
    std::string eocd;
    put32(eocd, 0x06054b50); put16(eocd, 0); put16(eocd, 0);
    put16(eocd, (uint16_t)files.size()); put16(eocd, (uint16_t)files.size());
    put32(eocd, (uint32_t)cd.size()); put32(eocd, (uint32_t)out.size()); put16(eocd, 0);
    return out + cd + eocd;
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

    // ---- mem:// 挂载（内存后端）：键 = "内存命名空间/相对路径" ----
    {
        const std::string a = "mem-file";
        const std::string b = "nested";
        CHECK(mq_vfs_add_mem(vfs, "assets/game.txt", a.data(), a.size()) == MQ_VFS_OK);
        CHECK(mq_vfs_add_mem(vfs, "assets/sub/deep.txt", b.data(), b.size()) == MQ_VFS_OK);
        CHECK(mq_vfs_mount(vfs, "/assets", "mem://assets") == MQ_VFS_OK);
        put(root / "base" / "assets" / "game.txt", "disk-file");   // 磁盘同名：mem 挂载后挂 → 优先
        CHECK(readall(vfs, "/assets/game.txt") == "mem-file");
        CHECK(readall(vfs, "/assets/sub/deep.txt") == "nested");
        CHECK(readall(vfs, "/assets/missing.txt") == "<err>");
    }

    // ---- zip:// 归档后端（stored 项）----
    {
        const std::string zip = make_stored_zip({ { "hello.txt", "from-zip" },
                                                  { "sub/deep.txt", "deep" } });
        const fs::path zp = root / "assets.zip";
        { std::ofstream f(zp, std::ios::binary); f.write(zip.data(), (std::streamsize)zip.size()); }
        CHECK(mq_vfs_mount_zip(vfs, "/zip", zp.string().c_str()) == MQ_VFS_OK);
        CHECK(readall(vfs, "/zip/hello.txt") == "from-zip");
        CHECK(readall(vfs, "/zip/sub/deep.txt") == "deep");
        CHECK(readall(vfs, "/zip/nope.txt") == "<err>");
    }

    // ---- 内容哈希 + 校验读（OD-9）----
    {
        const uint64_t h = mq_vfs_hash_path(vfs, "/a.txt");
        CHECK(h == mq_vfs_hash("hello", 5));
        void* d2 = nullptr; size_t n2 = 0;
        CHECK(mq_vfs_read_verified(vfs, "/a.txt", h, &d2, &n2) == MQ_VFS_OK);
        mq_vfs_free(d2);
        CHECK(mq_vfs_read_verified(vfs, "/a.txt", h ^ 1u, &d2, &n2) == MQ_VFS_ERR_HASH);
        CHECK(mq_vfs_read_verified(vfs, "/a.txt", 0, &d2, &n2) == MQ_VFS_OK);   // 0 = 不校验
        mq_vfs_free(d2);
        CHECK(mq_vfs_hash_path(vfs, "/missing") == 0);
    }

    // ---- res:// 别名 + 解析诊断 ----
    {
        CHECK(readall(vfs, "res:///a.txt") == "hello");
        char buf[256] = { 0 };
        CHECK(mq_vfs_resolve(vfs, "/zip/hello.txt", buf, sizeof buf) == MQ_VFS_OK);
        CHECK(std::string(buf).rfind("zip://", 0) == 0);
    }

    mq_vfs_destroy(vfs);
    fs::remove_all(root);

    if (g_failures == 0) { std::print("vfs_test: all checks passed\n"); return 0; }
    std::print("vfs_test: {} failure(s)\n", g_failures);
    return 1;
}
