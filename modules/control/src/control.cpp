#include "core/control/mq_control.h"

#include <map>
#include <string>
#include <vector>
#include <cstring>

struct mq_control {
    struct Entry { mq_control_handler fn; void* user; };
    std::map<std::string, Entry> methods;
    std::string response;
    size_t response_cap = 4096;
};

static std::string quote(const char* s) {
    std::string o = "\"";
    for (const char* p = s; p && *p; ++p) {
        if (*p == '"' || *p == '\\') o += '\\';
        o += *p;
    }
    o += '"';
    return o;
}

extern "C" {

mq_control* mq_control_create(void) { return new mq_control(); }
void mq_control_destroy(mq_control* c) { delete c; }

void mq_control_register(mq_control* c, const char* method, mq_control_handler fn, void* user) {
    if (c && method) c->methods[method] = mq_control::Entry{ fn, user };
}

const char* mq_control_dispatch(mq_control* c, const char* request) {
    if (!c) return "{}";
    if (!request) request = "{}";
    mq_json* dom = mq_json_parse(request, std::strlen(request));
    if (!dom) { c->response = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"parse error\"}}"; return c->response.c_str(); }

    // id（数值或字符串）
    std::string idtxt = "null";
    { const mq_json* idj = mq_json_get(dom, "id");
      const char* sid = mq_json_str(idj, nullptr);
      if (sid) idtxt = quote(sid);
      else if (idj) idtxt = std::to_string((long long)mq_json_num(idj, 0)); }

    const char* method = mq_json_str(mq_json_get(dom, "method"), nullptr);
    const mq_json* params = mq_json_get(dom, "params");

    auto it = method ? c->methods.find(method) : c->methods.end();
    if (it == c->methods.end()) {
        c->response = "{\"jsonrpc\":\"2.0\",\"id\":" + idtxt +
                      ",\"error\":{\"code\":-32601,\"message\":\"method not found: " +
                      (method ? method : "") + "\"}}";
    } else {
        /* 结果缓冲按需增长（Types.describe / Scene.entities 等 dump 可能很大）。 */
        std::vector<char> buf(8192);
        int rc = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            buf[0] = '\0';
            rc = it->second.fn(it->second.user, method, params, buf.data(), buf.size());
            const size_t used = std::strlen(buf.data());
            if (used + 1 < buf.size()) break;          // 未被截断
            buf.resize(buf.size() * 4);
        }
        if (rc != 0 || buf[0] == '\0') std::strcpy(buf.data(), "null");
        c->response = "{\"jsonrpc\":\"2.0\",\"id\":" + idtxt + ",\"result\":" + buf.data() + "}";
    }
    mq_json_free(dom);
    return c->response.c_str();
}

} // extern "C"
