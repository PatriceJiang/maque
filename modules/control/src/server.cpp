/* 控制端点（inspector/MCP 用；plans/17 §3、docs/09/11）：
 *   · 单线程 + 非阻塞 IO，只在**帧边界** poll → 不引入线程、不改确定性（docs/10）
 *   · 行分隔 JSON-RPC 2.0，与 stdin 路径**同一个** mq_control_dispatch（协议单一真相）
 *   · 仅回环：tcp:PORT → 127.0.0.1；uds:PATH → Unix domain socket（默认 /tmp/maque-<pid>.sock）
 *   · 只读会话：变更类方法（control_list 里 mutating: true）被拒绝并计数（D2 默认只读） */
#include "abi/control.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <print>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kMaxClients = 4;
constexpr size_t kLineCap = 64 * 1024;

struct Client {
    int fd = -1;
    std::string in;          /* 收缓冲（按行切分） */
    std::string out;         /* 发缓冲（写不完留到下次 poll） */
};

} // namespace

struct mq_control_server {
    mq_control* control = nullptr;
    int listen_fd = -1;
    bool read_only = true;
    bool unix_socket = false;
    std::string path;                 /* UDS 路径（退出时 unlink） */
    std::string url;                  /* tcp://127.0.0.1:7777 或 uds:///tmp/x.sock */
    Client clients[kMaxClients];
    uint32_t handled = 0;
    uint32_t rejected = 0;
};

namespace {

bool set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
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
        s->listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->listen_fd < 0) { delete s; return nullptr; }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", s->path.c_str());
        ::unlink(s->path.c_str());
        if (::bind(s->listen_fd, (sockaddr*)&addr, sizeof addr) != 0) {
            std::print(stderr, "control: uds bind 失败 {}: {}\n", s->path, std::strerror(errno));
            ::close(s->listen_fd); delete s; return nullptr;
        }
        s->url = "uds://" + s->path;
    } else {
        s->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s->listen_fd < 0) { delete s; return nullptr; }
        int one = 1;
        ::setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);   /* 仅回环（默认不开公网） */
        addr.sin_port = ::htons((uint16_t)std::atoi(s->path.c_str()));
        if (::bind(s->listen_fd, (sockaddr*)&addr, sizeof addr) != 0) {
            std::print(stderr, "control: tcp bind 失败 127.0.0.1:{}: {}\n", s->path, std::strerror(errno));
            ::close(s->listen_fd); delete s; return nullptr;
        }
        s->url = "tcp://127.0.0.1:" + s->path;
    }
    if (::listen(s->listen_fd, kMaxClients) != 0 || !set_nonblock(s->listen_fd)) {
        ::close(s->listen_fd); delete s; return nullptr;
    }
    return s;
}

extern "C" uint32_t mq_control_server_clients(mq_control_server* s) {
    if (!s) return 0;
    uint32_t n = 0;
    for (const Client& c : s->clients) if (c.fd >= 0) ++n;
    return n;
}

extern "C" const char* mq_control_server_url(mq_control_server* s) { return s ? s->url.c_str() : ""; }
extern "C" uint32_t mq_control_server_rejected(mq_control_server* s) { return s ? s->rejected : 0; }

extern "C" int mq_control_server_poll(mq_control_server* s) {
    if (!s) return 0;
    int handled = 0;

    /* ① 接受新连接（非阻塞；满员则立即关闭） */
    for (;;) {
        const int fd = ::accept(s->listen_fd, nullptr, nullptr);
        if (fd < 0) break;
        bool placed = false;
        for (Client& c : s->clients)
            if (c.fd < 0) { c.fd = fd; c.in.clear(); c.out.clear(); placed = true; break; }
        if (!placed) ::close(fd);
        else set_nonblock(fd);
    }

    /* ② 每个客户端：读 → 逐行派发 → 写 */
    for (Client& c : s->clients) {
        if (c.fd < 0) continue;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) {
                c.in.append(buf, (size_t)n);
                if (c.in.size() > kLineCap) { c.in.clear(); break; }   /* 防御：超长请求丢弃 */
                continue;
            }
            if (n == 0) { ::close(c.fd); c.fd = -1; break; }           /* 对端关闭 */
            break;                                                      /* EAGAIN */
        }
        if (c.fd < 0) continue;

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
            const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n > 0) { c.out.erase(0, (size_t)n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;   /* 留到下次 poll */
            ::close(c.fd); c.fd = -1; c.out.clear();
            break;
        }
    }
    s->handled += (uint32_t)handled;
    return handled;
}

extern "C" void mq_control_server_destroy(mq_control_server* s) {
    if (!s) return;
    if (std::getenv("MAQUE_TRACE_INSPECT")) std::print("[ctl] destroy unix={} path={}\n", s->unix_socket, s->path);
    for (Client& c : s->clients) if (c.fd >= 0) ::close(c.fd);
    if (s->listen_fd >= 0) ::close(s->listen_fd);
    if (s->unix_socket && !s->path.empty()) ::unlink(s->path.c_str());
    delete s;
}
