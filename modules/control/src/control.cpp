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
    mq_control_handler tools_fn = nullptr;
    void* tools_user = nullptr;
    mq_control_tools_json_fn tools_json = nullptr;   // 生成物：类型化 inputSchema
    void* tools_json_user = nullptr;
};

/* method → MCP 工具描述（domain.method 与 CDP 同构，docs/11 §3/§9）。 */
static const char* method_domain(const std::string& m) {
    const size_t dot = m.find('.');
    if (dot == std::string::npos) return "General";
    static thread_local std::string d;
    d = m.substr(0, dot);
    return d.c_str();
}

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

uint32_t mq_control_method_count(mq_control* c) {
    return c ? (uint32_t)c->methods.size() : 0u;
}
const char* mq_control_method_at(mq_control* c, uint32_t index) {
    if (!c || index >= c->methods.size()) return nullptr;
    auto it = c->methods.begin();
    std::advance(it, index);
    return it->first.c_str();
}

void mq_control_set_tools_handler(mq_control* c, mq_control_handler fn, void* user) {
    if (!c) return;
    c->tools_fn = fn;
    c->tools_user = user;
}

void mq_control_set_tools_json(mq_control* c, mq_control_tools_json_fn fn, void* user) {
    if (!c) return;
    c->tools_json = fn;
    c->tools_json_user = user;
}

/* MCP 包装：tools/list 由已注册方法生成（能力即工具，docs/11 §5）。 */
const char* mq_control_dispatch_mcp(mq_control* c, const char* request) {
    if (!c) return "{}";
    mq_json* dom = mq_json_parse(request ? request : "{}", request ? std::strlen(request) : 2);
    if (!dom) { c->response = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"parse error\"}}"; return c->response.c_str(); }

    std::string idtxt = "null";
    { const mq_json* idj = mq_json_get(dom, "id");
      const char* sid = mq_json_str(idj, nullptr);
      if (sid) idtxt = quote(sid);
      else if (idj) idtxt = std::to_string((long long)mq_json_num(idj, 0)); }

    const std::string method_s = mq_json_str(mq_json_get(dom, "method"), "");
    const char* method = method_s.c_str();       // 复制：dom 随即可能被释放（悬垂指针防护）
    const mq_json* params = mq_json_get(dom, "params");
    std::string result = "null";

    if (std::strcmp(method, "initialize") == 0) {
        result = "{\"protocolVersion\":\"2024-11-05\",\"serverInfo\":{\"name\":\"maque\",\"version\":\"1\"},"
                 "\"capabilities\":{\"tools\":{}}}";
    } else if (std::strcmp(method, "tools/list") == 0) {
        /* 优先用 codegen 生成的类型化工具表（docs/21 §6.3：同一 DSL 再生成 MCP 工具）。 */
        if (c->tools_json) {
            const char* gen = c->tools_json(c->tools_json_user);
            if (gen && gen[0]) { result = std::string("{\"tools\":") + gen + "}"; mq_json_free(dom);
                                 c->response = "{\"jsonrpc\":\"2.0\",\"id\":" + idtxt + ",\"result\":" + result + "}";
                                 return c->response.c_str(); }
        }
        std::string list = "{\"tools\":[";
        bool first = true;
        for (auto& [name, e] : c->methods) {
            (void)e;
            if (!first) list += ",";
            first = false;
            const std::string dom_name = method_domain(name);
            list += "{\"name\":\"" + name + "\",\"description\":\"maque " + dom_name +
                    " domain method\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":true}}";
        }
        list += "]}";
        result = list;
    } else if (std::strcmp(method, "tools/call") == 0) {
        const char* name = mq_json_str(mq_json_get(params, "name"), nullptr);
        const mq_json* args = mq_json_get(params, "arguments");
        if (!name) {
            result = "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"missing tool name\"}]}";
        } else {
            auto it = c->methods.find(name);
            if (it != c->methods.end()) {
                std::vector<char> buf(8192);
                for (int attempt = 0; attempt < 5; ++attempt) {
                    buf[0] = '\0';
                    it->second.fn(it->second.user, name, args, buf.data(), buf.size());
                    if (std::strlen(buf.data()) + 1 < buf.size()) break;
                    buf.resize(buf.size() * 4);
                }
                result = std::string("{\"content\":[{\"type\":\"text\",\"text\":") +
                         quote(buf[0] ? buf.data() : "null") + "}]}";
            } else if (c->tools_fn) {
                std::vector<char> buf(8192);
                buf[0] = '\0';
                const int rc = c->tools_fn(c->tools_user, name, args, buf.data(), buf.size());
                result = rc == 0 && buf[0]
                       ? std::string("{\"content\":[{\"type\":\"text\",\"text\":") + quote(buf.data()) + "}]}"
                       : std::string("{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"unknown tool\"}]}");
            } else {
                result = "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"unknown tool: " +
                         std::string(name) + "\"}]}";
            }
        }
    } else if (std::strncmp(method, "notifications/", 14) == 0) {
        result = "{}";                                     // 通知：无响应体语义，返回空结果
    } else {
        mq_json_free(dom);
        c->response = "{\"jsonrpc\":\"2.0\",\"id\":" + idtxt +
                      ",\"error\":{\"code\":-32601,\"message\":\"mcp method not found: " +
                      std::string(method) + "\"}}";
        return c->response.c_str();
    }

    mq_json_free(dom);
    c->response = "{\"jsonrpc\":\"2.0\",\"id\":" + idtxt + ",\"result\":" + result + "}";
    return c->response.c_str();
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
        /* 结果缓冲按需增长：大 dump（图集/画面抓帧可达数百 KB）必须能一次装下。
         * 曾因增长次数不足 + 步长偏小，307KB 的响应被截到 32KB（表现为图像错乱）。 */
        std::vector<char> buf(8192);
        int rc = 0;
        for (int attempt = 0; attempt < 8 && buf.size() < (8u << 20); ++attempt) {
            buf[0] = '\0';
            rc = it->second.fn(it->second.user, method, params, buf.data(), buf.size());
            const size_t used = std::strlen(buf.data());
            if (used + 1 < buf.size()) break;          // 未被截断
            buf.resize(buf.size() * 8);                // 步长 ×8（8192 → 64K → 512K → 4M → 8M）
        }
        if (rc != 0 || buf[0] == '\0') std::strcpy(buf.data(), "null");
        c->response = "{\"jsonrpc\":\"2.0\",\"id\":" + idtxt + ",\"result\":" + buf.data() + "}";
    }
    mq_json_free(dom);
    return c->response.c_str();
}

} // extern "C"
