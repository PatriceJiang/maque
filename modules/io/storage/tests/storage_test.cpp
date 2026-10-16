// 存储模块单测（docs/25）：四种模式（mem / file / http / ws）语义一致 + 命名空间/键校验 + 原子写。
// http 用测试内建 HTTP 服务器；ws 用测试内建 libwebsockets 服务端（都不依赖外部服务/网络）。
#include "core/storage/mq_storage.h"

#include <print>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
#include "maque_test_fs.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static std::string get_str(mq_storage* s, const char* key, int* rc = nullptr) {
    void* d = nullptr; size_t n = 0;
    const int r = mq_storage_get(s, key, &d, &n);
    if (rc) *rc = r;
    if (r != MQ_STORAGE_OK) return "<err>";
    std::string out((char*)d, n);
    mq_storage_free(d);
    return out;
}
static int put_str(mq_storage* s, const char* key, const std::string& v) {
    return mq_storage_put(s, key, v.data(), v.size());
}
static std::string keys_of(mq_storage* s) {
    char buf[4096] = { 0 };
    if (mq_storage_keys(s, buf, sizeof buf) != MQ_STORAGE_OK) return "<err>";
    return buf;
}

/* ============================ 测试内建 HTTP 服务端（REST 固定协议） ============================ */
/* 平台：POSIX 用 BSD socket，Windows 用 Winsock2（需链接 ws2_32）。
 * 这里只用**阻塞** socket + 一个测试线程（不涉及引擎的单线程/确定性约束）。 */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>          /* 必须先于 windows.h；本文件不含 windows.h */
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
using test_socket_t = SOCKET;                        /* Windows：句柄（UINT_PTR），不是 int */
constexpr test_socket_t kTestInvalidSocket = INVALID_SOCKET;
using test_socklen_t = int;
#else
using test_socket_t = int;
constexpr test_socket_t kTestInvalidSocket = -1;
using test_socklen_t = socklen_t;
#endif

bool test_socket_valid(test_socket_t s) { return s != kTestInvalidSocket; }

void test_socket_close(test_socket_t s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/* 环境变量（file:// 裸 spec 的默认根）：Windows 无 setenv/unsetenv，用 _putenv_s */
void test_setenv(const char* k, const char* v) {
#if defined(_WIN32)
    ::_putenv_s(k, v);
#else
    ::setenv(k, v, 1);
#endif
}

void test_unsetenv(const char* k) {
#if defined(_WIN32)
    ::_putenv_s(k, "");
#else
    ::unsetenv(k);
#endif
}

} // namespace

struct HttpStoreServer {
    test_socket_t fd = kTestInvalidSocket;
    int port = 0;
    std::thread th;
    std::mutex mu;
    std::map<std::string, std::string> items;      // 路径（"/<ns>/<key>"）→ 值
    std::atomic<bool> stop{ false };
#if defined(_WIN32)
    bool wsa_ready = false;                        // WSAStartup 成功 → 析构时 WSACleanup（成对）
#endif

    bool start() {
#if defined(_WIN32)
        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_ready = true;
#endif
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!test_socket_valid(fd)) return false;
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), (int)sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, (sockaddr*)&a, (int)sizeof a) != 0) return false;
        test_socklen_t len = (test_socklen_t)sizeof a;
        if (::getsockname(fd, (sockaddr*)&a, &len) != 0) return false;
        port = ntohs(a.sin_port);
        if (::listen(fd, 8) != 0) return false;
        th = std::thread([this] {
            while (!stop) {
                const test_socket_t c = ::accept(fd, nullptr, nullptr);
                if (!test_socket_valid(c)) break;
                handle(c);
                test_socket_close(c);
            }
        });
        return true;
    }
    ~HttpStoreServer() {
        stop = true;
#if defined(_WIN32)
        /* Windows：shutdown 对监听 socket 无效（WSAENOTCONN）且不会解除阻塞的 accept；
           只有 closesocket 能让它立刻失败返回 → 线程才能退出 */
        if (test_socket_valid(fd)) { test_socket_close(fd); fd = kTestInvalidSocket; }
        if (th.joinable()) th.join();
        if (wsa_ready) { ::WSACleanup(); wsa_ready = false; }
#else
        if (test_socket_valid(fd)) ::shutdown(fd, SHUT_RDWR);
        if (th.joinable()) th.join();
        if (test_socket_valid(fd)) { test_socket_close(fd); fd = kTestInvalidSocket; }
#endif
    }

private:
    static std::string decode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                auto hex = [](char c) {
                    return c >= '0' && c <= '9' ? c - '0'
                         : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
                };
                const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
                if (hi >= 0 && lo >= 0) { out.push_back((char)(hi * 16 + lo)); i += 2; continue; }
            }
            out.push_back(s[i]);
        }
        return out;
    }
    void handle(test_socket_t c) {
        std::string req;
        char buf[4096];
        size_t header_end = std::string::npos;
        size_t content_len = 0;
        while (true) {
            const auto n = ::recv(c, buf, (int)sizeof buf, 0);
            if (n <= 0) return;
            req.append(buf, (size_t)n);
            if (header_end == std::string::npos) {
                header_end = req.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const size_t p = req.find("Content-Length:");
                    if (p != std::string::npos && p < header_end) content_len = (size_t)std::strtoul(req.c_str() + p + 15, nullptr, 10);
                }
            }
            if (header_end != std::string::npos && req.size() >= header_end + 4 + content_len) break;
            if (req.size() > (4u << 20)) return;
        }
        const size_t sp1 = req.find(' ');
        const size_t sp2 = req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        const std::string method = req.substr(0, sp1);
        std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string body = req.substr(header_end + 4, content_len);

        std::string status = "HTTP/1.1 200 OK\r\n";
        std::string payload;
        {
            std::lock_guard g(mu);
            const size_t q = target.find('?');
            const std::string path = decode(q == std::string::npos ? target : target.substr(0, q));
            const std::string query = q == std::string::npos ? "" : target.substr(q + 1);
            if (method == "GET" && query == "keys=1") {
                std::string out = "[";
                bool first = true;
                std::string base_path = path;
                while (base_path.size() > 1 && base_path.back() == '/') base_path.pop_back();
                const std::string prefix = base_path + "/";
                for (const auto& [k, _] : items) {
                    if (k.rfind(prefix, 0) != 0) continue;
                    if (!first) out.push_back(',');
                    out += '"' + k.substr(prefix.size()) + '"';
                    first = false;
                }
                payload = out + "]";
            } else if (method == "GET") {
                auto it = items.find(path);
                if (it == items.end()) status = "HTTP/1.1 404 Not Found\r\n";
                else payload = it->second;
            } else if (method == "PUT") {
                items[path] = body;
            } else if (method == "DELETE") {
                if (items.erase(path) == 0) status = "HTTP/1.1 404 Not Found\r\n";
            } else {
                status = "HTTP/1.1 405 Method Not Allowed\r\n";
            }
        }
        const std::string resp = status + "Content-Length: " + std::to_string(payload.size())
                               + "\r\nConnection: close\r\n\r\n" + payload;
        ::send(c, resp.data(), (int)resp.size(), 0);
    }
};

#include "ws_test_server.h"      // 单测内建 ws 服务端（lws；见该文件头注释）

int main() {
    // ---------------- mem://（默认模式）----------------
    {
        mq_storage* s = mq_storage_create(nullptr, "alice", "main");
        CHECK(s != nullptr);
        CHECK(std::strcmp(mq_storage_mode(s), "mem") == 0);
        CHECK(std::strcmp(mq_storage_ns(s), "alice/main") == 0);
        CHECK(put_str(s, "score", "42") == MQ_STORAGE_OK);
        CHECK(get_str(s, "score") == "42");
        CHECK(mq_storage_exists(s, "score") == 1);
        CHECK(mq_storage_exists(s, "nope") == 0);
        CHECK(get_str(s, "nope") == "<err>");
        CHECK(mq_storage_remove(s, "nope") == MQ_STORAGE_ERR_NOTFOUND);
        CHECK(mq_storage_remove(s, "score") == MQ_STORAGE_OK);
        CHECK(mq_storage_exists(s, "score") == 0);

        // 子目录键 + keys（排序、确定性）
        CHECK(put_str(s, "a", "1") == MQ_STORAGE_OK);
        CHECK(put_str(s, "saves/slot1", "one") == MQ_STORAGE_OK);
        CHECK(put_str(s, "saves/slot2", "two") == MQ_STORAGE_OK);
        CHECK(keys_of(s) == "[\"a\",\"saves/slot1\",\"saves/slot2\"]");

        // 二进制值（含 NUL）原样往返
        const char bin[] = { 'a', '\0', 'b', '\xff' };
        CHECK(mq_storage_put(s, "bin", bin, sizeof bin) == MQ_STORAGE_OK);
        void* d = nullptr; size_t n = 0;
        CHECK(mq_storage_get(s, "bin", &d, &n) == MQ_STORAGE_OK && n == sizeof bin && std::memcmp(d, bin, n) == 0);
        mq_storage_free(d);

        // 非法键 → ARG
        CHECK(put_str(s, "", "x") == MQ_STORAGE_ERR_ARG);
        CHECK(put_str(s, "/abs", "x") == MQ_STORAGE_ERR_ARG);
        CHECK(put_str(s, "../escape", "x") == MQ_STORAGE_ERR_ARG);
        CHECK(put_str(s, "a//b", "x") == MQ_STORAGE_ERR_ARG);
        CHECK(put_str(s, "a/./b", "x") == MQ_STORAGE_ERR_ARG);

        // uid/session 隔离：同 mem 命名空间下不同 ns 互不可见（两个实例各自独立也不可见）
        mq_storage* other = mq_storage_create("mem://", "bob", "main");
        CHECK(other != nullptr);
        CHECK(mq_storage_exists(other, "a") == 0);
        CHECK(put_str(other, "a", "bob") == MQ_STORAGE_OK);
        CHECK(get_str(s, "a") == "1");                 // alice 的 a 不受影响
        mq_storage_destroy(other);

        // 非法 uid/session 与未知模式 → create 返回 nullptr
        CHECK(mq_storage_create("mem://", "bad/uid", "main") == nullptr);
        CHECK(mq_storage_create("ftp://host/x", "u", "s") == nullptr);
        mq_storage_destroy(s);
    }

    // ---------------- file://（显式目录：原子写 + 目录即键空间）----------------
    {
        fs::path root;
        if (!maque_test_setup_dir("storage", root)) { std::print(stderr, "storage_test: 临时目录不可写\n"); return 77; }
        fs::remove_all(root);
        mq_storage* s = mq_storage_create(("file://" + root.string()).c_str(), "alice", "run1");
        CHECK(s != nullptr);
        CHECK(std::strcmp(mq_storage_mode(s), "file") == 0);
        CHECK(put_str(s, "save/slot1", "hello") == MQ_STORAGE_OK);
        CHECK(get_str(s, "save/slot1") == "hello");
        CHECK(fs::exists(root / "alice" / "run1" / "save" / "slot1"));
        CHECK(!fs::exists(root / "alice" / "run1" / "save" / "slot1.tmp"));   // 原子写不留 tmp
        CHECK(put_str(s, "save/slot1", "hello2") == MQ_STORAGE_OK);           // 覆盖
        CHECK(get_str(s, "save/slot1") == "hello2");
        CHECK(keys_of(s) == "[\"save/slot1\"]");
        CHECK(mq_storage_remove(s, "save/slot1") == MQ_STORAGE_OK);
        CHECK(get_str(s, "save/slot1", nullptr) == "<err>");

        // session 隔离：不同 session 目录互不可见
        mq_storage* other = mq_storage_create(("file://" + root.string()).c_str(), "alice", "run2");
        CHECK(other != nullptr);
        CHECK(put_str(s, "k", "run1-value") == MQ_STORAGE_OK);
        CHECK(put_str(other, "k", "run2-value") == MQ_STORAGE_OK);
        CHECK(get_str(s, "k") == "run1-value");
        CHECK(get_str(other, "k") == "run2-value");
        mq_storage_destroy(other);

        // 跨实例持久：重建实例仍读到（同一进程；跨进程由 e2e 覆盖）
        mq_storage_destroy(s);
        s = mq_storage_create(("file://" + root.string()).c_str(), "alice", "run1");
        CHECK(get_str(s, "k") == "run1-value");
        mq_storage_destroy(s);
        fs::remove_all(root);
    }

    // ---------------- file:// 裸 spec：默认根（MAQUE_STORAGE_ROOT / XDG_DATA_HOME 兜底）----------------
    {
        fs::path root;
        if (!maque_test_setup_dir("storage-default", root)) { std::print(stderr, "storage_test: 临时目录不可写\n"); return 77; }
        fs::remove_all(root);
        test_setenv("MAQUE_STORAGE_ROOT", root.string().c_str());
        mq_storage* s = mq_storage_create("file://", "u", "s");
        CHECK(s != nullptr);
        CHECK(std::string(mq_storage_root(s)) == root.string());
        CHECK(put_str(s, "k", "v") == MQ_STORAGE_OK);
        CHECK(fs::exists(root / "u" / "s" / "k"));
        mq_storage_destroy(s);
        test_unsetenv("MAQUE_STORAGE_ROOT");
        fs::remove_all(root);
    }

    // ---------------- http://（REST 固定协议）----------------
    {
        HttpStoreServer srv;
        CHECK(srv.start());
        const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);
        mq_storage* s = mq_storage_create(base.c_str(), "alice", "main");
        CHECK(s != nullptr);
        CHECK(std::strcmp(mq_storage_mode(s), "http") == 0);
        CHECK(put_str(s, "cloud/score", "99") == MQ_STORAGE_OK);
        CHECK(get_str(s, "cloud/score") == "99");
        CHECK(mq_storage_exists(s, "cloud/score") == 1);
        CHECK(get_str(s, "cloud/missing") == "<err>");
        CHECK(keys_of(s) == "[\"cloud/score\"]");
        // 需要 percent-encode 的键（空格 / 中文 / '?&'）
        CHECK(put_str(s, "a b/中文?&", "unik") == MQ_STORAGE_OK);
        CHECK(get_str(s, "a b/中文?&") == "unik");
        CHECK(mq_storage_remove(s, "cloud/score") == MQ_STORAGE_OK);
        CHECK(mq_storage_remove(s, "cloud/score") == MQ_STORAGE_ERR_NOTFOUND);
        mq_storage_destroy(s);
    }

    // ---------------- ws://（JSON 帧固定协议；测试内建 lws 服务端）----------------
#ifdef MQ_HAVE_LWS
    {
        WsServer srv;
        CHECK(srv.start());
        if (srv.port > 0) {
            const std::string url = "ws://127.0.0.1:" + std::to_string(srv.port) + "/storage";
            mq_storage* s = mq_storage_create(url.c_str(), "alice", "main");
            CHECK(s != nullptr);
            CHECK(std::strcmp(mq_storage_mode(s), "ws") == 0);
            CHECK(put_str(s, "cloud/save", "ws-value") == MQ_STORAGE_OK);
            CHECK(get_str(s, "cloud/save") == "ws-value");
            CHECK(mq_storage_exists(s, "cloud/save") == 1);
            CHECK(get_str(s, "cloud/nope") == "<err>");
            CHECK(keys_of(s) == "[\"cloud/save\"]");
            // 二进制值 → base64 帧往返
            const char bin[] = { '\x01', '\0', '\x7f', (char)0xff };
            CHECK(mq_storage_put(s, "bin", bin, sizeof bin) == MQ_STORAGE_OK);
            void* d = nullptr; size_t n = 0;
            CHECK(mq_storage_get(s, "bin", &d, &n) == MQ_STORAGE_OK && n == sizeof bin &&
                  std::memcmp(d, bin, n) == 0);
            mq_storage_free(d);
            CHECK(mq_storage_remove(s, "cloud/save") == MQ_STORAGE_OK);
            CHECK(mq_storage_remove(s, "cloud/save") == MQ_STORAGE_ERR_NOTFOUND);
            mq_storage_destroy(s);
        }
    }
#endif

    if (g_failures) { std::print("storage_test: {} failure(s)\n", g_failures); return 1; }
    std::print("storage_test: all checks passed\n");
    return 0;
}
