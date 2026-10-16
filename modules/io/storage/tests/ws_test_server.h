/* 单测用最小 ws 服务端（libwebsockets）：实现 storage 的**固定帧协议**（docs/25），
 * 让 ws 后端能真连真收发（不依赖外部服务）。服务端把值的编码（utf8/b64）原样存储 → 回放，
 * 因此不需要解码 base64。协议名与客户端一致："maque.storage.v1"。 */
#pragma once

#ifdef MQ_HAVE_LWS

#include <libwebsockets.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

struct WsServer {
    /* 每连接状态：keyed by wsi（lws 的 per-session 区不调 C++ 构造 → 不用非平凡类型） */
    struct Conn { std::string rx; std::string pending; };

    struct lws_context* ctx = nullptr;
    std::thread th;
    std::atomic<bool> stop{ false };
    int port = 0;
    std::mutex mu;
    std::map<std::string, std::pair<std::string, std::string>> items;   // "<ns>/<key>" → (enc, value)
    std::map<struct lws*, Conn> conns;                                  // 每连接收发缓冲

    /* ---- 极简 JSON 字段提取（帧是本测试自产自销的固定形状）---- */
    static bool field(const std::string& j, const char* name, std::string& out) {
        const std::string needle = std::string("\"") + name + "\":\"";
        const size_t p = j.find(needle);
        if (p == std::string::npos) return false;
        std::string raw;
        for (size_t i = p + needle.size(); i < j.size(); ++i) {
            if (j[i] == '\\') { raw.push_back(j[i]); if (i + 1 < j.size()) raw.push_back(j[++i]); continue; }
            if (j[i] == '"') break;
            raw.push_back(j[i]);
        }
        out.clear();
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '\\') { out.push_back(raw[i]); continue; }
            const char e = (i + 1 < raw.size()) ? raw[++i] : '\0';
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                default: out.push_back(e); break;
            }
        }
        return true;
    }
    static std::string escape(const std::string& s) {
        std::string out;
        for (const char c : s) {
            if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out.push_back(c);
        }
        return out;
    }

    /* 调用者需持有 mu */
    std::string handle_frame_locked(const std::string& json) {
        std::string op, ns, key, enc = "utf8", value;
        field(json, "op", op);
        field(json, "ns", ns);
        field(json, "key", key);
        field(json, "enc", enc);
        field(json, "value", value);
        if (op == "get") {
            auto it = items.find(ns + "/" + key);
            if (it == items.end()) return "{\"error\":\"notfound\"}";
            return "{\"value\":\"" + escape(it->second.second) + "\",\"enc\":\"" + it->second.first + "\"}";
        }
        if (op == "put") {
            items[ns + "/" + key] = { enc, value };
            return "{\"ok\":true}";
        }
        if (op == "del") {
            if (items.erase(ns + "/" + key) == 0) return "{\"error\":\"notfound\"}";
            return "{\"ok\":true}";
        }
        if (op == "keys") {
            std::string out = "{\"keys\":[";
            bool first = true;
            const std::string prefix = ns + "/";
            for (const auto& [k, _] : items) {
                if (k.rfind(prefix, 0) != 0) continue;
                if (!first) out.push_back(',');
                out += "\"" + escape(k.substr(prefix.size())) + "\"";
                first = false;
            }
            return out + "]}";
        }
        return "{\"error\":\"unsupported\"}";
    }

    static int cb(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/, void* in, size_t len) {
        auto* self = (WsServer*)lws_context_user(lws_get_context(wsi));
        if (!self) return 0;
        switch (reason) {
            case LWS_CALLBACK_ESTABLISHED: {
                std::lock_guard g(self->mu);
                self->conns[wsi] = Conn{};
                return 0;
            }
            case LWS_CALLBACK_RECEIVE: {
                std::string reply;
                {
                    std::lock_guard g(self->mu);
                    Conn& c = self->conns[wsi];
                    if (in && len) c.rx.append((const char*)in, len);
                    if (!lws_is_final_fragment(wsi)) return 0;
                    reply = self->handle_frame_locked(c.rx);
                    c.rx.clear();
                    c.pending = std::move(reply);
                }
                lws_callback_on_writable(wsi);
                return 0;
            }
            case LWS_CALLBACK_SERVER_WRITEABLE: {
                std::string pending;
                {
                    std::lock_guard g(self->mu);
                    auto it = self->conns.find(wsi);
                    if (it == self->conns.end() || it->second.pending.empty()) return 0;
                    pending = it->second.pending;
                    it->second.pending.clear();
                }
                std::vector<unsigned char> buf(LWS_PRE + pending.size());
                std::memcpy(buf.data() + LWS_PRE, pending.data(), pending.size());
                lws_write(wsi, buf.data() + LWS_PRE, pending.size(), LWS_WRITE_TEXT);
                return 0;
            }
            case LWS_CALLBACK_CLOSED: {
                std::lock_guard g(self->mu);
                self->conns.erase(wsi);
                return 0;
            }
            default:
                return 0;
        }
    }

    static const struct lws_protocols* protos() {
        static const struct lws_protocols list[] = {
            { "maque.storage.v1", cb, 0, 4096, 0, nullptr, 0 },
            LWS_PROTOCOL_LIST_TERM
        };
        return list;
    }

    bool start() {
        lws_set_log_level(LLL_ERR, nullptr);
        struct lws_context_creation_info info;
        std::memset(&info, 0, sizeof info);
        info.port = 0;                        // 由内核分配：创建后再读实际端口
        info.protocols = protos();
        info.user = this;
        ctx = lws_create_context(&info);
        if (!ctx) return false;
        th = std::thread([this] {
            while (!stop) lws_service(ctx, 20);
        });
        /* 等端口就绪（lws 在创建时绑定） */
        for (int i = 0; i < 100 && port == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            port = lws_get_vhost_listen_port(lws_get_vhost_by_name(ctx, "default"));
        }
        return port > 0;
    }

    ~WsServer() {
        stop = true;
        if (ctx) lws_cancel_service(ctx);
        if (th.joinable()) th.join();
        if (ctx) lws_context_destroy(ctx);
    }
};

#endif   // MQ_HAVE_LWS
