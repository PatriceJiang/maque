/* Inspector 客户端：与游戏进程的控制端点通信（plans/17 §4）。
 *   · 行分隔 JSON-RPC 2.0（与游戏内 stdin 路径同一协议）
 *   · 非阻塞：send 立即返回，响应在 poll() 里按 id 配对
 *   · 无第三方 JSON 依赖：请求组装用字符串，响应用**轻量扫描**取字段（够面板用）
 *   · tcp:host:port 与 uds:/path 两种传输 */
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

namespace insp {

struct Reply {
    bool ok = false;
    std::string raw;            /* 原始响应（控制台面板直接显示） */
    std::string error;
};

class Client {
public:
    bool connect(const std::string& spec, std::string* err = nullptr) {
        disconnect();
        spec_ = spec;
        if (spec.rfind("uds:", 0) == 0) {
            const std::string path = spec.substr(4);
            fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd_ < 0) { if (err) *err = std::strerror(errno); return false; }
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
            if (::connect(fd_, (sockaddr*)&a, sizeof a) != 0) {
                if (err) *err = std::strerror(errno);
                ::close(fd_); fd_ = -1;
                return false;
            }
        } else {
            std::string host = "127.0.0.1";
            std::string port = spec;
            if (spec.rfind("tcp:", 0) == 0) port = spec.substr(4);
            const size_t colon = port.find(':');
            if (colon != std::string::npos) { host = port.substr(0, colon); port = port.substr(colon + 1); }
            fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd_ < 0) { if (err) *err = std::strerror(errno); return false; }
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = ::htons((uint16_t)std::atoi(port.c_str()));
            if (host == "localhost") host = "127.0.0.1";
            ::inet_pton(AF_INET, host.c_str(), &a.sin_addr);
            if (::connect(fd_, (sockaddr*)&a, sizeof a) != 0) {
                if (err) *err = std::strerror(errno);
                ::close(fd_); fd_ = -1;
                return false;
            }
        }
        const int fl = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        in_.clear();
        pending_.clear();
    }
    bool connected() const { return fd_ >= 0; }
    const std::string& spec() const { return spec_; }

    /* 发送请求（立即返回）；id 自增，响应在 poll() 里配对 */
    int send(const std::string& method, const std::string& params_json = "") {
        if (fd_ < 0) return -1;
        const int id = ++next_id_;
        std::string req = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                          ",\"method\":\"" + method + "\"";
        if (!params_json.empty()) req += ",\"params\":" + params_json;
        req += "}\n";
        size_t off = 0;
        while (off < req.size()) {
            const ssize_t n = ::send(fd_, req.data() + off, req.size() - off, MSG_NOSIGNAL);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            disconnect();
            return -1;
        }
        pending_[id] = method;
        return id;
    }

    /* 发送**原始** JSON 行（控制台面板：用户手写请求） */
    bool send_raw(const std::string& line) {
        if (fd_ < 0) return false;
        std::string req = line;
        if (req.empty() || req.back() != '\n') req += '\n';
        size_t off = 0;
        while (off < req.size()) {
            const ssize_t n = ::send(fd_, req.data() + off, req.size() - off, MSG_NOSIGNAL);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            disconnect();
            return false;
        }
        return true;
    }

    /* 非阻塞收：返回本次完成的响应（method, Reply） */
    std::vector<std::pair<std::string, Reply>> poll() {
        std::vector<std::pair<std::string, Reply>> done;
        if (fd_ < 0) return done;
        char buf[8192];
        for (;;) {
            const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n > 0) { in_.append(buf, (size_t)n); continue; }
            if (n == 0) { disconnect(); return done; }
            break;
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
            done.emplace_back(method, std::move(r));
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
    int fd_ = -1;
    int next_id_ = 0;
    std::string spec_;
    std::string in_;
    std::unordered_map<int, std::string> pending_;
};

} // namespace insp
