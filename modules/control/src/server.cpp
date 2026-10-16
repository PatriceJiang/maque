/* 控制端点（inspector/MCP 用；plans/17 §3、docs/09/11）：
 *   · 单线程 + 非阻塞 IO，只在**帧边界** poll → 不引入线程、不改确定性（docs/10）
 *   · 行分隔 JSON-RPC 2.0，与 stdin 路径**同一个** mq_control_dispatch（协议单一真相）
 *   · 仅回环：tcp:PORT → 127.0.0.1；uds:PATH → Unix domain socket（默认 /tmp/maque-<pid>.sock）
 *   · 只读会话：变更类方法（control_list 里 mutating: true）被拒绝并计数（D2 默认只读）
 *   · 平台：Windows 走 Winsock2（链接 ws2_32），Linux 走 BSD socket。
 *     **Windows 不支持 uds:**（AF_UNIX 在 Windows 的命名/权限语义与 POSIX 不同，
 *     不引入这种不确定性）→ create 明确失败并提示改用 tcp:端口。 */
#include "abi/control.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <cctype>
#include <cstdlib>
#include <vector>
#include <sys/stat.h>          /* POSIX 与 MSVC 都有（Windows 侧为 _stat 系列所在头） */

#if defined(_WIN32)
/* winsock2.h 必须先于 windows.h；本文件不包含 windows.h。
 * WIN32_LEAN_AND_MEAN / NOMINMAX 只在本 .cpp 内生效，不泄漏到头文件。 */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>        /* WSAStartup/socket/accept/recv/send/closesocket/ioctlsocket */
#  include <ws2tcpip.h>        /* sockaddr_in、inet_pton/inet_ntop 兼容宏 */
#else
#  include <fcntl.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

namespace {

constexpr int kMaxClients = 4;
constexpr size_t kLineCap = 64 * 1024;

#if defined(_WIN32)
using mq_socket_t = SOCKET;                    /* Windows：句柄（UINT_PTR），不是 int */
constexpr mq_socket_t kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;                  /* Windows 无 SIGPIPE，无需 MSG_NOSIGNAL */
#else
using mq_socket_t = int;
constexpr mq_socket_t kInvalidSocket = -1;
constexpr int kSendFlags = MSG_NOSIGNAL;       /* Linux：避免对端已关时 SIGPIPE 杀进程 */
#endif

bool socket_valid(mq_socket_t s) { return s != kInvalidSocket; }

/* 关闭：Windows 是 closesocket（与 CRT 的 close 不同族，混用会泄漏句柄） */
void socket_close(mq_socket_t s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/* 非阻塞：Windows 用 ioctlsocket(FIONBIO)，POSIX 用 fcntl(O_NONBLOCK) */
bool set_nonblock(mq_socket_t s) {
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int fl = ::fcntl(s, F_GETFL, 0);
    return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

/* “暂时无数据 / 写不下” → 留到下次 poll（Windows 的 errno 不可靠，用 WSAEWOULDBLOCK） */
bool socket_would_block() {
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* 启动期诊断文本（仅失败路径用） */
std::string socket_error_text() {
#if defined(_WIN32)
    return "wsa " + std::to_string((int)::WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

/* UDS 文件清理：Windows 不走 uds 分支（见 create），此处为无操作 */
void remove_uds_path(const std::string& path) {
#if defined(_WIN32)
    (void)path;
#else
    ::unlink(path.c_str());
#endif
}

struct Client {
    mq_socket_t fd = kInvalidSocket;
    std::string in;          /* 收缓冲（按行切分） */
    std::string out;         /* 发缓冲（写不完留到下次 poll） */
};

} // namespace

struct mq_control_server {
    mq_control* control = nullptr;
    mq_socket_t listen_fd = kInvalidSocket;
    bool read_only = true;
    bool unix_socket = false;
#if defined(_WIN32)
    bool wsa_ready = false;           /* WSAStartup 成功 → destroy 时 WSACleanup（成对） */
#endif
    std::string path;                 /* UDS 路径（退出时 unlink） */
    std::string url;                  /* tcp://127.0.0.1:7777 或 uds:///tmp/x.sock */
    Client clients[kMaxClients];
    uint32_t handled = 0;
    uint32_t rejected = 0;
};

namespace {

/* 启动失败：关掉已开 socket + 撤销 WSA + 释放（各失败路径统一走这里） */
mq_control_server* server_fail(mq_control_server* s) {
    if (socket_valid(s->listen_fd)) { socket_close(s->listen_fd); s->listen_fd = kInvalidSocket; }
#if defined(_WIN32)
    if (s->wsa_ready) { ::WSACleanup(); s->wsa_ready = false; }
#endif
    delete s;
    return nullptr;
}

/* 只读会话拒绝变更类方法：返回 JSON-RPC 错误响应（协议与普通错误一致） */
std::string deny_response(const std::string& line) {
    /* 取出 id（不引第三方 JSON：id 是数字或字符串，位置解析足够） */
    std::string id = "null";
    const size_t p = line.find("\"id\"");
    if (p != std::string::npos) {
        size_t i = line.find(':', p);
        if (i != std::string::npos) {
            ++i;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            const size_t b = i;
            if (i < line.size() && line[i] == '"') {
                ++i;
                while (i < line.size() && line[i] != '"') ++i;
                if (i < line.size()) ++i;
                id = line.substr(b, i - b);
            } else {
                while (i < line.size() && (isdigit((unsigned char)line[i]) || line[i] == '-' ||
                                           line[i] == '.' || line[i] == 'e' || line[i] == '+')) ++i;
                if (i > b) id = line.substr(b, i - b);
            }
        }
    }
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"error\":{\"code\":-32001,\"message\":\"read-only session: mutating method rejected\"}}";
}

std::string method_of(const std::string& line) {
    const size_t p = line.find("\"method\"");
    if (p == std::string::npos) return {};
    size_t i = line.find(':', p);
    if (i == std::string::npos) return {};
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != '"') return {};
    const size_t b = ++i;
    while (i < line.size() && line[i] != '"') ++i;
    return line.substr(b, i - b);
}

} // namespace

extern "C" mq_control_server* mq_control_server_create(mq_control* c, const char* spec, int read_only) {
    if (!c || !spec || !*spec) return nullptr;
    auto* s = new mq_control_server();
    s->control = c;
    s->read_only = read_only != 0;

#if defined(_WIN32)
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::print(stderr, "control: WSAStartup 失败（Winsock2 不可用）\n");
        delete s; return nullptr;
    }
    s->wsa_ready = true;
#endif

    std::string sp = spec;
    if (sp.rfind("uds:", 0) == 0) {
        s->unix_socket = true;
        s->path = sp.substr(4);
        if (s->path.empty()) s->path = "/tmp/maque.sock";
    } else {
        std::string port = sp.rfind("tcp:", 0) == 0 ? sp.substr(4) : sp;
        if (port.empty()) port = "7777";
        s->path = port;                                   /* 端口（url 里再组） */
    }

    if (s->unix_socket) {
#if defined(_WIN32)
        /* Windows 无 POSIX 语义的 Unix domain socket：明确不支持，不做 AF_UNIX 近似 */
        std::print(stderr, "control: uds 端点 '{}' 在 Windows 上不支持"
                           "（AF_UNIX 语义不同）——请改用 tcp:端口\n", spec);
        return server_fail(s);
#else
        s->listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (!socket_valid(s->listen_fd)) return server_fail(s);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", s->path.c_str());
        ::unlink(s->path.c_str());
        if (::bind(s->listen_fd, (sockaddr*)&addr, (socklen_t)sizeof addr) != 0) {
            std::print(stderr, "control: uds bind 失败 {}: {}\n", s->path, socket_error_text());
            return server_fail(s);
        }
        s->url = "uds://" + s->path;
#endif
    } else {
        s->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!socket_valid(s->listen_fd)) return server_fail(s);
        const int one = 1;
        ::setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), (int)sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);   /* 仅回环（默认不开公网） */
        addr.sin_port = ::htons((uint16_t)std::atoi(s->path.c_str()));
        if (::bind(s->listen_fd, (sockaddr*)&addr, (int)sizeof addr) != 0) {
            std::print(stderr, "control: tcp bind 失败 127.0.0.1:{}: {}\n", s->path, socket_error_text());
            return server_fail(s);
        }
        s->url = "tcp://127.0.0.1:" + s->path;
    }
    if (::listen(s->listen_fd, kMaxClients) != 0 || !set_nonblock(s->listen_fd)) {
        return server_fail(s);
    }
    return s;
}

extern "C" uint32_t mq_control_server_clients(mq_control_server* s) {
    if (!s) return 0;
    uint32_t n = 0;
    for (const Client& c : s->clients) if (socket_valid(c.fd)) ++n;
    return n;
}

extern "C" const char* mq_control_server_url(mq_control_server* s) { return s ? s->url.c_str() : ""; }
extern "C" uint32_t mq_control_server_rejected(mq_control_server* s) { return s ? s->rejected : 0; }

extern "C" int mq_control_server_poll(mq_control_server* s) {
    if (!s) return 0;
    int handled = 0;

    /* ① 接受新连接（非阻塞；满员则立即关闭） */
    for (;;) {
        const mq_socket_t fd = ::accept(s->listen_fd, nullptr, nullptr);
        if (!socket_valid(fd)) break;
        bool placed = false;
        for (Client& c : s->clients)
            if (!socket_valid(c.fd)) { c.fd = fd; c.in.clear(); c.out.clear(); placed = true; break; }
        if (!placed) socket_close(fd);
        else set_nonblock(fd);
    }

    /* ② 每个客户端：读 → 逐行派发 → 写 */
    for (Client& c : s->clients) {
        if (!socket_valid(c.fd)) continue;
        char buf[4096];
        for (;;) {
            const auto n = ::recv(c.fd, buf, (int)sizeof buf, 0);
            if (n > 0) {
                c.in.append(buf, (size_t)n);
                if (c.in.size() > kLineCap) { c.in.clear(); break; }   /* 防御：超长请求丢弃 */
                continue;
            }
            if (n == 0) { socket_close(c.fd); c.fd = kInvalidSocket; break; }   /* 对端关闭 */
            break;                                                              /* EAGAIN */
        }
        if (!socket_valid(c.fd)) continue;

        size_t pos = 0;
        for (;;) {
            const size_t nl = c.in.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = c.in.substr(pos, nl - pos);
            pos = nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            const std::string method = method_of(line);
            std::string resp;
            if (s->read_only && !method.empty() && mq_control_is_mutating(method.c_str())) {
                resp = deny_response(line);            /* 只读会话：拒绝并计数（D2） */
                ++s->rejected;
            } else {
                const char* r = mq_control_dispatch(s->control, line.c_str());
                resp = r ? r : "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"no response\"}}";
            }
            c.out += resp;
            c.out += '\n';
            ++handled;
        }
        if (pos > 0) c.in.erase(0, pos);

        while (!c.out.empty()) {
            /* Windows 的 send 长度是 int：按 INT_MAX 分块（size ≤ INT_MAX 时与原来逐字节等价） */
            const size_t chunk = c.out.size() > (size_t)INT_MAX ? (size_t)INT_MAX : c.out.size();
            const auto n = ::send(c.fd, c.out.data(), (int)chunk, kSendFlags);
            if (n > 0) { c.out.erase(0, (size_t)n); continue; }
            if (n < 0 && socket_would_block()) break;   /* 留到下次 poll */
            socket_close(c.fd); c.fd = kInvalidSocket; c.out.clear();
            break;
        }
    }
    s->handled += (uint32_t)handled;
    return handled;
}

extern "C" void mq_control_server_destroy(mq_control_server* s) {
    if (!s) return;
    if (std::getenv("MAQUE_TRACE_INSPECT")) std::print("[ctl] destroy unix={} path={}\n", s->unix_socket, s->path);
    for (Client& c : s->clients) if (socket_valid(c.fd)) socket_close(c.fd);
    if (socket_valid(s->listen_fd)) socket_close(s->listen_fd);
    if (s->unix_socket && !s->path.empty()) remove_uds_path(s->path);
#if defined(_WIN32)
    if (s->wsa_ready) { ::WSACleanup(); s->wsa_ready = false; }
#endif
    delete s;
}
