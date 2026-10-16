/* 存储核心（docs/25）：spec（模式）解析 + (uid, session) 命名空间 + 键校验 + 按后端分发 + C ABI。
 *
 * 模式：file:// · mem:// · http(s):// · ws(s)://（每协议一个实现目录，见 storage_internal.h）。
 * spec 缺省（NULL/空）→ mem://（不落盘）；`file://` 不带路径 → XDG 数据目录兜底默认：
 *   $MAQUE_STORAGE_ROOT > $XDG_DATA_HOME/maque/storage > ~/.local/share/maque/storage
 * （**运行时**会把默认根设成 <场景目录>/.maque —— 见 docs/25「默认路径」）。 */
#include "storage_internal.h"

#include <cstdlib>
#include <cstring>
#include <utility>

using namespace maque::storage;

namespace {

struct Storage {
    std::string mode;      // "file" | "mem" | "http" | "ws"
    std::string root;      // 生效根（目录 / mem 命名空间 / URL 基址）——观测
    std::string uid = "default";
    std::string session = "main";
    std::string ns;        // "<uid>/<session>"
    MemStore mem;          // mem:// 状态
    WsTarget ws;           // ws(s):// 目标
};

/* XDG 数据目录兜底：$MAQUE_STORAGE_ROOT > $XDG_DATA_HOME > ~/.local/share（Windows: %APPDATA%） */
std::string default_data_root() {
    if (const char* r = std::getenv("MAQUE_STORAGE_ROOT"); r && *r) return r;
    std::string base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) base = xdg;
#ifdef _WIN32
    if (base.empty()) if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) base = appdata;
    if (base.empty()) base = ".";
    return base + "\\maque\\storage";
#else
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        base = home && *home ? std::string(home) + "/.local/share" : ".";
    }
    return base + "/maque/storage";
#endif
}

/* 解析 spec → 填 Storage（mode/root 等）。返回 MQ_STORAGE_ERR_*。 */
int configure(Storage& st, const std::string& spec) {
    auto strip = [](const std::string& s, const char* scheme) -> const char* {
        const size_t n = std::strlen(scheme);
        return s.rfind(scheme, 0) == 0 ? s.c_str() + n : nullptr;
    };
    if (const char* p = strip(spec, "mem://")) {
        st.mode = "mem";
        st.root = *p ? p : "mem";
        return MQ_STORAGE_OK;
    }
    if (const char* p = strip(spec, "file://")) {
        st.mode = "file";
        st.root = *p ? p : default_data_root();
        return MQ_STORAGE_OK;
    }
    if (const char* p = strip(spec, "http://")) {
        st.mode = "http";
        if (!*p) return MQ_STORAGE_ERR_MODE;
        st.root = std::string("http://") + p;
        while (!st.root.empty() && st.root.back() == '/') st.root.pop_back();   // 基址不含尾 '/'
        return MQ_STORAGE_OK;
    }
    if (const char* p = strip(spec, "https://")) {
        st.mode = "http";
        if (!*p) return MQ_STORAGE_ERR_MODE;
        st.root = std::string("https://") + p;
        while (!st.root.empty() && st.root.back() == '/') st.root.pop_back();
        return MQ_STORAGE_OK;
    }
    if (const char* p = strip(spec, "ws://")) {
        st.mode = "ws";
        if (!*p) return MQ_STORAGE_ERR_MODE;
        st.root = std::string("ws://") + p;
        return MQ_STORAGE_OK;
    }
    if (const char* p = strip(spec, "wss://")) {
        st.mode = "ws";
        if (!*p) return MQ_STORAGE_ERR_MODE;
        st.root = std::string("wss://") + p;
        return MQ_STORAGE_OK;
    }
    return MQ_STORAGE_ERR_MODE;      // 未知 scheme（如 ftp://）
}

/* "ws(s)://host[:port]/path" → WsTarget（host/port/path/tls）。 */
int parse_ws(const std::string& url, WsTarget& out) {
    const bool tls = url.rfind("wss://", 0) == 0;
    const size_t base = tls ? 6 : 5;
    const size_t slash = url.find('/', base);
    const std::string authority = url.substr(base, slash == std::string::npos ? std::string::npos : slash - base);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (authority.empty()) return MQ_STORAGE_ERR_MODE;
    auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        const long port = std::strtol(authority.c_str() + colon + 1, nullptr, 10);
        if (port <= 0 || port > 65535) return MQ_STORAGE_ERR_MODE;
        out.port = (int)port;
    } else {
        out.host = authority;
        out.port = tls ? 443 : 80;
    }
    out.tls = tls;
    return MQ_STORAGE_OK;
}

/* 按模式分发（唯一的路由点）。 */
int apply(Storage& st, const Request& req, Response& out) {
    if (st.mode == "mem")  return mem_apply(st.mem, req, out);
    if (st.mode == "file") return file_apply(st.root, req, out);
    if (st.mode == "http") return http_apply(st.root, req, out);
    if (st.mode == "ws")   return ws_apply(st.ws, req, out);
    return MQ_STORAGE_ERR_MODE;
}

} // namespace

extern "C" {

mq_storage* mq_storage_create(const char* spec, const char* uid, const char* session) {
    Storage st;
    st.uid = (uid && *uid) ? uid : "default";
    st.session = (session && *session) ? session : "main";
    if (!ns_is_valid(st.uid, st.session)) return nullptr;
    st.ns = ns_of(st.uid, st.session);
    const int rc = configure(st, spec && *spec ? spec : "mem://");
    if (rc != MQ_STORAGE_OK) return nullptr;
    if (st.mode == "ws") {
        const int wrc = parse_ws(st.root, st.ws);
        if (wrc != MQ_STORAGE_OK) return nullptr;
    }
    return reinterpret_cast<mq_storage*>(new Storage(std::move(st)));
}

void mq_storage_destroy(mq_storage* s) { delete reinterpret_cast<Storage*>(s); }

const char* mq_storage_mode(mq_storage* s) {
    auto* st = reinterpret_cast<Storage*>(s);
    return st ? st->mode.c_str() : "";
}

const char* mq_storage_root(mq_storage* s) {
    auto* st = reinterpret_cast<Storage*>(s);
    return st ? st->root.c_str() : "";
}

const char* mq_storage_ns(mq_storage* s) {
    auto* st = reinterpret_cast<Storage*>(s);
    return st ? st->ns.c_str() : "";
}

int mq_storage_put(mq_storage* s, const char* key, const void* data, size_t size) {
    auto* st = reinterpret_cast<Storage*>(s);
    if (!st || !key || (!data && size)) return MQ_STORAGE_ERR_ARG;
    if (!key_is_valid(key)) return MQ_STORAGE_ERR_ARG;
    Request req; req.op = Op::Put; req.ns = st->ns; req.key = key; req.data = data; req.size = size;
    Response res;
    return apply(*st, req, res);
}

int mq_storage_get(mq_storage* s, const char* key, void** out, size_t* out_size) {
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    auto* st = reinterpret_cast<Storage*>(s);
    if (!st || !key) return MQ_STORAGE_ERR_ARG;
    if (!key_is_valid(key)) return MQ_STORAGE_ERR_ARG;
    Request req; req.op = Op::Get; req.ns = st->ns; req.key = key;
    Response res;
    const int rc = apply(*st, req, res);
    if (rc != MQ_STORAGE_OK) return rc;
    // 分配权在本模块（与 mq_storage_free 配对）
    char* buf = (char*)std::malloc(res.value.size() + 1);
    if (!buf) return MQ_STORAGE_ERR_NOMEM;
    if (!res.value.empty()) std::memcpy(buf, res.value.data(), res.value.size());
    buf[res.value.size()] = '\0';
    if (out) *out = buf; else std::free(buf);
    if (out_size) *out_size = res.value.size();
    return MQ_STORAGE_OK;
}

void mq_storage_free(void* p) { std::free(p); }

int mq_storage_remove(mq_storage* s, const char* key) {
    auto* st = reinterpret_cast<Storage*>(s);
    if (!st || !key) return MQ_STORAGE_ERR_ARG;
    if (!key_is_valid(key)) return MQ_STORAGE_ERR_ARG;
    Request req; req.op = Op::Remove; req.ns = st->ns; req.key = key;
    Response res;
    return apply(*st, req, res);
}

int mq_storage_exists(mq_storage* s, const char* key) {
    auto* st = reinterpret_cast<Storage*>(s);
    if (!st || !key) return 0;
    if (!key_is_valid(key)) return 0;
    Request req; req.op = Op::Get; req.ns = st->ns; req.key = key;
    Response res;
    return apply(*st, req, res) == MQ_STORAGE_OK ? 1 : 0;
}

int mq_storage_keys(mq_storage* s, char* out, size_t cap) {
    auto* st = reinterpret_cast<Storage*>(s);
    if (!st) return MQ_STORAGE_ERR_ARG;
    Request req; req.op = Op::Keys; req.ns = st->ns;
    Response res;
    const int rc = apply(*st, req, res);
    if (rc != MQ_STORAGE_OK) return rc;
    if (out && cap) {
        const size_t n = res.json.size() < cap - 1 ? res.json.size() : cap - 1;
        std::memcpy(out, res.json.data(), n);
        out[n] = '\0';
    }
    return MQ_STORAGE_OK;
}

} // extern "C"
