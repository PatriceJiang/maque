/* Inspector 客户端：与游戏进程的控制端点通信（plans/17 §4）。
 *   · 行分隔 JSON-RPC 2.0（与游戏内 stdin 路径同一协议）
 *   · 非阻塞：send 立即返回，响应在 poll() 里按 id 配对
 *   · 无第三方 JSON 依赖：请求组装用字符串，响应用**轻量扫描**取字段（够面板用）
 *   · tcp:host:port 与 uds:/path 两种传输
 *   · 平台：Windows 走 Winsock2（链接 ws2_32），Linux 走 BSD socket。socket 在 Windows 是**句柄**
 *     （SOCKET）→ 一律用 INVALID_SOCKET 判定，不用 fd < 0。
 *     **Windows 不支持 uds:**（AF_UNIX 语义与 POSIX 不同）→ connect 明确失败并提示改用 tcp:端口。 */
#pragma once
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
/* winsock2.h 必须先于 windows.h：否则 windows.h 会拉进 winsock 1.1，函数原型与常量冲突。
 * 本头不含 windows.h；WIN32_LEAN_AND_MEAN/NOMINMAX 只影响本 TU 的后续包含。 */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>        /* WSAStartup/socket/connect/send/recv/closesocket/ioctlsocket */
#  include <ws2tcpip.h>        /* sockaddr_in、inet_pton */
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace insp {

#if defined(_WIN32)
using socket_t = SOCKET;                        /* 句柄（UINT_PTR）：不能与 int 互换 */
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
using socklen_len = int;                        /* 长度参数类型：Windows 是 int，POSIX 是 socklen_t */
inline constexpr int kSendFlags = 0;            /* Windows 无 SIGPIPE，不需要 MSG_NOSIGNAL */
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
using socklen_len = socklen_t;
inline constexpr int kSendFlags = MSG_NOSIGNAL; /* 避免对端已关时 SIGPIPE 杀进程 */
#endif

inline bool socket_valid(socket_t s) { return s != kInvalidSocket; }

/* 关闭：Windows 是 closesocket（与 CRT 的 close 不同族，混用会漏句柄） */
inline void socket_close(socket_t s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/* 失败原因文本（Windows 的 errno 不可靠 → 用 WSA 错误码） */
inline std::string socket_error_text() {
#if defined(_WIN32)
    return "wsa " + std::to_string((int)::WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

/* 非阻塞：Windows 用 ioctlsocket(FIONBIO)，POSIX 用 fcntl(O_NONBLOCK) */
inline bool set_nonblock(socket_t s) {
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int fl = ::fcntl(s, F_GETFL, 0);
    return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

/* "暂时收不到 / 写不下" → 留到下次 poll */
inline bool socket_would_block() {
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

struct Reply {
    bool ok = false;
    std::string raw;            /* 原始响应（控制台面板直接显示） */
    std::string error;
};

class Client {
public:
    Client() = default;
    Client(const Client&) = delete;              /* 持有 socket 句柄与 WSA 引用：不可拷贝 */
    Client& operator=(const Client&) = delete;
    ~Client() { disconnect(); wsa_shutdown(); }

    bool connect(const std::string& spec, std::string* err = nullptr) {
        disconnect();
        spec_ = spec;
        /* Windows：任何连接路径都要先有 Winsock（进程级引用计数）；与析构里的 WSACleanup 成对。
         * POSIX：空操作（无进程级网络初始化）。 */
        if (!wsa_start()) {
            if (err) *err = "WSAStartup 失败（Winsock2 不可用）";
            return false;
        }
        if (spec.rfind("uds:", 0) == 0) {
#if defined(_WIN32)
            /* Windows 无 POSIX 语义的 AF_UNIX 端点：明确不支持（与 modules/control 的端点一致），
             * 不做 AF_UNIX 近似 —— 失败契约与"连不上"相同，调用方据此提示改用 tcp:端口 */
            if (err) *err = "Windows 不支持 uds: 端点（AF_UNIX 语义不同）—— 请改用 tcp:端口";
            return false;
#else
            const std::string path = spec.substr(4);
            fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (!socket_valid(fd_)) { if (err) *err = socket_error_text(); return false; }
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
            if (::connect(fd_, (sockaddr*)&a, (socklen_len)sizeof a) != 0) {
                if (err) *err = socket_error_text();
                socket_close(fd_); fd_ = kInvalidSocket;
                return false;
            }
#endif
        } else {
            std::string host = "127.0.0.1";
            std::string port = spec;
            if (spec.rfind("tcp:", 0) == 0) port = spec.substr(4);
            const size_t colon = port.find(':');
            if (colon != std::string::npos) { host = port.substr(0, colon); port = port.substr(colon + 1); }
            fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (!socket_valid(fd_)) { if (err) *err = socket_error_text(); return false; }
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = ::htons((uint16_t)std::atoi(port.c_str()));
            if (host == "localhost") host = "127.0.0.1";
            ::inet_pton(AF_INET, host.c_str(), &a.sin_addr);
            if (::connect(fd_, (sockaddr*)&a, (socklen_len)sizeof a) != 0) {
                if (err) *err = socket_error_text();
                socket_close(fd_); fd_ = kInvalidSocket;
                return false;
            }
        }
        /* 非阻塞语义与原来一致（连上后置非阻塞）；置失败即断开，不能把阻塞 socket 留给轮询循环 */
        if (!set_nonblock(fd_)) {
            if (err) *err = socket_error_text();
            socket_close(fd_); fd_ = kInvalidSocket;
            return false;
        }
        return true;
    }

    void disconnect() {
        if (socket_valid(fd_)) socket_close(fd_);
        fd_ = kInvalidSocket;
        in_.clear();
        pending_.clear();
    }
    bool connected() const { return socket_valid(fd_); }
    const std::string& spec() const { return spec_; }

    /* 发送请求（立即返回）；id 自增，响应在 poll() 里配对 */
    int send(const std::string& method, const std::string& params_json = "") {
        if (!socket_valid(fd_)) return -1;
        const int id = ++next_id_;
        std::string req = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                          ",\"method\":\"" + method + "\"";
        if (!params_json.empty()) req += ",\"params\":" + params_json;
        req += "}\n";
        if (!send_all(req)) return -1;
        pending_[id] = method;
        return id;
    }

    /* 发送**原始** JSON 行（控制台面板：用户手写请求） */
    bool send_raw(const std::string& line) {
        if (!socket_valid(fd_)) return false;
        std::string req = line;
        if (req.empty() || req.back() != '\n') req += '\n';
        return send_all(req);
    }

    struct Done { int id; std::string method; Reply reply; };

    /* 非阻塞收：返回本次完成的响应（id + method + Reply） */
    std::vector<Done> poll() {
        std::vector<Done> done;
        if (!socket_valid(fd_)) return done;
        char buf[8192];
        for (;;) {
            const auto n = ::recv(fd_, buf, (int)sizeof buf, 0);
            if (n > 0) { in_.append(buf, (size_t)n); continue; }
            if (n == 0) { disconnect(); return done; }
            break;                                  /* EAGAIN / WSAEWOULDBLOCK：留到下次 poll */
        }
        size_t pos = 0;
        for (;;) {
            const size_t nl = in_.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = in_.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.empty()) continue;
            Reply r;
            r.raw = line;
            const int id = (int)num_field(line, "id");
            const size_t ep = line.find("\"error\"");
            if (ep != std::string::npos) r.error = str_field(line.substr(ep), "message");
            else r.ok = true;
            auto it = pending_.find(id);
            const std::string method = it != pending_.end() ? it->second : std::string("?");
            if (it != pending_.end()) pending_.erase(it);
            done.push_back(Done{ id, method, std::move(r) });
        }
        if (pos > 0) in_.erase(0, pos);
        return done;
    }

    /* ---- 轻量 JSON 取值（面板用；不引入第三方库）---- */
    static double num_field(const std::string& s, const char* key, double def = 0.0) {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = s.find(k);
        if (p == std::string::npos) return def;
        p = s.find(':', p + k.size());
        if (p == std::string::npos) return def;
        return std::atof(s.c_str() + p + 1);
    }
    static std::string str_field(const std::string& s, const char* key) {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = s.find(k);
        if (p == std::string::npos) return {};
        p = s.find(':', p + k.size());
        if (p == std::string::npos) return {};
        ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
        if (p >= s.size() || s[p] != '"') return {};
        const size_t b = ++p;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) { out += s[p + 1]; p += 2; continue; }
            out += s[p++];
        }
        (void)b;
        return out;
    }

private:
    /* 发送整行（非阻塞，写不完继续写）：Windows 的 send 长度参数是 int → 超过 INT_MAX 分块 */
    bool send_all(const std::string& req) {
        size_t off = 0;
        while (off < req.size()) {
            const size_t rest = req.size() - off;
            const int chunk = rest > (size_t)INT_MAX ? INT_MAX : (int)rest;
            const auto n = ::send(fd_, req.data() + off, chunk, kSendFlags);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && socket_would_block()) continue;
            disconnect();                       /* 硬错误：断开（与原来一致） */
            return false;
        }
        return true;
    }

#if defined(_WIN32)
    /* WSAStartup 是进程级引用计数：本实例只启动一次，析构里配一次 WSACleanup（不泄漏） */
    bool wsa_start() {
        if (wsa_ready_) return true;
        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_ready_ = true;
        return true;
    }
    void wsa_shutdown() {
        if (wsa_ready_) { ::WSACleanup(); wsa_ready_ = false; }
    }
#else
    /* POSIX 无进程级网络初始化 → 空实现（让 connect/析构的调用点不分平台） */
    bool wsa_start() { return true; }
    void wsa_shutdown() {}
#endif

    socket_t fd_ = kInvalidSocket;
    int next_id_ = 0;
#if defined(_WIN32)
    bool wsa_ready_ = false;
#endif
    std::string spec_;
    std::string in_;
    std::unordered_map<int, std::string> pending_;
};

} // namespace insp
