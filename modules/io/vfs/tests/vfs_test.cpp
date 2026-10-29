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

/* 极简 HTTP 服务器（仅用于本测试，避免依赖外部网络）：GET 命中文件表 → 200，否则 404。
 * 请求路径按 %XX 解码后查表 —— 因此能验证 VFS 侧确实做了逐段 percent-encode。
 * 只实现 Linux/macOS 路径（其它平台跳过 URL 相关用例）。 */
#if defined(MQ_HAVE_CURL) && defined(MQ_TEST_HTTP)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

struct HttpServer {
    int fd = -1;
    int port = 0;
    std::thread th;
    std::atomic<int> requests{ 0 };
    std::atomic<bool> stop{ false };
    std::map<std::string, std::string> files;    // 解码后的路径 → 内容
    std::mutex mu;

    bool start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;                        // 内核分配空闲端口
        if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0) return false;
        socklen_t len = sizeof addr;
        if (::getsockname(fd, (sockaddr*)&addr, &len) != 0) return false;
        port = ntohs(addr.sin_port);
        if (::listen(fd, 8) != 0) return false;
        th = std::thread([this] {
            while (!stop) {
                const int c = ::accept(fd, nullptr, nullptr);
                if (c < 0) break;
                handle(c);
                ::close(c);
            }
        });
        return true;
    }
    ~HttpServer() {
        stop = true;
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (th.joinable()) th.join();
        if (fd >= 0) ::close(fd);
    }
    void set(std::string path, std::string body) {
        std::lock_guard g(mu);
        files[std::move(path)] = std::move(body);
    }

private:
    static std::string decode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0'
                                             : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; };
                const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
                if (hi >= 0 && lo >= 0) { out.push_back((char)(hi * 16 + lo)); i += 2; continue; }
            }
            out.push_back(s[i]);
        }
        return out;
    }
    void handle(int c) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) return;
            req.append(buf, (size_t)n);
        }
        const size_t sp1 = req.find(' ');
        const size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        const std::string path = decode(req.substr(sp1 + 1, sp2 - sp1 - 1));
        requests.fetch_add(1);
        std::string body;
        bool found = false;
        { std::lock_guard g(mu);
          const auto it = files.find(path);
          if (it != files.end()) { body = it->second; found = true; } }
        const std::string head = found ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n";
        const std::string resp = head + "Content-Length: " + std::to_string(body.size())
                               + "\r\nConnection: close\r\n\r\n" + body;
        ::send(c, resp.data(), resp.size(), 0);
    }
};

#elif defined(MQ_HAVE_CURL)

/* 非 POSIX 平台：不建本地 HTTP 服务器（URL 挂载用例跳过）。 */
struct HttpServer { bool start() { return false; } };

#endif

namespace fs = std::filesystem;
#include "maque_test_fs.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void put(const fs::path& p, const std::string& s) { maque_put_file(p, s); }
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
    fs::path root;
    if (!maque_test_setup_dir("vfs", root)) return 77;
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

#if defined(MQ_HAVE_CURL) && defined(MQ_TEST_HTTP)
    // ---- http:// 目录挂载（docs/04 §4/§5）：URL 基址 + 相对路径拼接 + 会话缓存 ----
    {
        HttpServer srv;
        CHECK(srv.start());
        srv.set("/cdn/a.txt", "remote-a");
        srv.set("/cdn/sub/b.txt", "remote-b");
        srv.set("/cdn/空 格.txt", "unicode-ok");          // 逐段 percent-encode 往返
        srv.set("/other/a.txt", "other-a");
        const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);

        CHECK(mq_vfs_mount(vfs, "/cdn", (base + "/cdn").c_str()) == MQ_VFS_OK);   // 结尾不带 '/' 也等价
        CHECK(readall(vfs, "/cdn/a.txt") == "remote-a");
        CHECK(readall(vfs, "/cdn/sub/b.txt") == "remote-b");
        CHECK(readall(vfs, "/cdn/空 格.txt") == "unicode-ok");
        CHECK(mq_vfs_exists(vfs, "/cdn/a.txt"));

        // 会话缓存：同一 URL 只走一次网络（装载期可复现，docs/04 §8）
        const int reqs = srv.requests.load();
        CHECK(readall(vfs, "/cdn/a.txt") == "remote-a");
        CHECK(readall(vfs, "/cdn/a.txt") == "remote-a");
        CHECK(srv.requests.load() == reqs);

        // 404 → NOTFOUND（HTTP 状态码映射）
        void* d = nullptr; size_t n = 0;
        CHECK(mq_vfs_read(vfs, "/cdn/missing.txt", &d, &n) == MQ_VFS_ERR_NOTFOUND);

        // 最长前缀 + 后挂载优先：另一个 URL 目录遮蔽 /cdn/sub
        CHECK(mq_vfs_mount(vfs, "/cdn/sub", (base + "/other/").c_str()) == MQ_VFS_OK);
        CHECK(readall(vfs, "/cdn/sub/b.txt") == "<err>");
        CHECK(readall(vfs, "/cdn/sub/a.txt") == "other-a");

        // 直接 URL（不经挂载）与挂载共享同一缓存
        const int reqs2 = srv.requests.load();
        CHECK(readall(vfs, (base + "/cdn/a.txt").c_str()) == "remote-a");
        CHECK(srv.requests.load() == reqs2);

        // 远程资产完整性（ADR-0030）：哈希校验读
        const uint64_t h = mq_vfs_hash_path(vfs, "/cdn/a.txt");
        CHECK(h == mq_vfs_hash("remote-a", 8));
        CHECK(mq_vfs_read_verified(vfs, "/cdn/a.txt", h, &d, &n) == MQ_VFS_OK);
        mq_vfs_free(d);
        CHECK(mq_vfs_read_verified(vfs, "/cdn/a.txt", h ^ 1u, &d, &n) == MQ_VFS_ERR_HASH);

        // 路径穿越防护（docs/04 §9）：VFS 层统一拦截 ".." 段（'/' 与 '\' 都算分隔符）
        CHECK(mq_vfs_read(vfs, "/cdn/../../etc/passwd", &d, &n) == MQ_VFS_ERR_ARG);
        CHECK(mq_vfs_read(vfs, "/cdn/..\\..\\windows", &d, &n) == MQ_VFS_ERR_ARG);
        CHECK(mq_vfs_read(vfs, "/cdn/../a.txt", &d, &n) == MQ_VFS_ERR_ARG);
        CHECK(mq_vfs_resolve(vfs, "/cdn/../a.txt", nullptr, 0) == MQ_VFS_ERR_ARG);
        CHECK(readall(vfs, "/cdn/back\\slash.txt") == "<err>");    // 反斜杠本身是合法文件名字符

        // resolve 诊断：URL 挂载返回完整 URL（观测/调试）
        char res[256] = { 0 };
        CHECK(mq_vfs_resolve(vfs, "/cdn/a.txt", res, sizeof res) == MQ_VFS_OK);
        CHECK(std::string(res) == base + "/cdn/a.txt");

        // 非法 URL 源（只有 scheme）
        CHECK(mq_vfs_mount(vfs, "/bad", "http://") == MQ_VFS_ERR_ARG);
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

    // ---- 同一挂载点重新挂载：**后挂载优先**必须对同源重复挂载也成立 ----
    // （director.loadScene 每次把新场景目录挂到 "/"，切回之前加载过的目录时旧条目必须重新回到表尾）
    {
        put(root / "sceneA" / "s.scn", "A");
        put(root / "sceneB" / "s.scn", "B");
        mq_vfs* v2 = mq_vfs_create();
        CHECK(v2 != nullptr);
        CHECK(mq_vfs_mount(v2, "/", ("file://" + (root / "sceneA").string()).c_str()) == MQ_VFS_OK);
        CHECK(readall(v2, "/s.scn") == "A");
        CHECK(mq_vfs_mount(v2, "/", ("file://" + (root / "sceneB").string()).c_str()) == MQ_VFS_OK);
        CHECK(readall(v2, "/s.scn") == "B");
        CHECK(mq_vfs_mount(v2, "/", ("file://" + (root / "sceneA").string()).c_str()) == MQ_VFS_OK);  // 切回
        CHECK(readall(v2, "/s.scn") == "A");
        mq_vfs_destroy(v2);
    }

    // ---- Android APK asset:// 协议：桌面后端明确 UNSUPPORTED，且统一拒绝路径穿越 ----
    {
        mq_vfs* apk = mq_vfs_create();
        CHECK(apk != nullptr);
        CHECK(mq_vfs_mount(apk, "/apk", "asset://scenes") == MQ_VFS_OK);
        char resolved[256] = { 0 };
        CHECK(mq_vfs_resolve(apk, "/apk/ball.scn", resolved, sizeof resolved) == MQ_VFS_OK);
        CHECK(std::string(resolved) == "asset://scenes/ball.scn");
        void* data = nullptr;
        size_t size = 0;
        CHECK(mq_vfs_read(apk, "/apk/ball.scn", &data, &size) == MQ_VFS_ERR_UNSUPPORTED);
        CHECK(mq_vfs_read(apk, "asset://../secret.scn", &data, &size) == MQ_VFS_ERR_ARG);
        mq_vfs_destroy(apk);
    }

    mq_vfs_destroy(vfs);
    fs::remove_all(root);

    if (g_failures == 0) { std::print("vfs_test: all checks passed\n"); return 0; }
    std::print("vfs_test: {} failure(s)\n", g_failures);
    return 1;
}
