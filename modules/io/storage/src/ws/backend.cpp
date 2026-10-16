/* ws(s):// 后端：WebSocket 客户端（libwebsockets）——**文本帧 JSON** 固定协议（docs/25）：
 *   → {"op":"get","ns":"<uid>/<session>","key":"a/b"}
 *   ← {"value":"…","enc":"utf8"|"b64"}   |   {"error":"notfound"|"arg"|…}
 *   → {"op":"put","ns":…,"key":…,"enc":…,"value":…}
 *   → {"op":"del","ns":…,"key":…}
 *   → {"op":"keys","ns":…}   ← {"keys":["a","b/c"]}
 * 每次操作一条连接（冷路径：存档/配置量级）→ 状态机简单、失败即报错；协议名 "maque.storage.v1"。 */
#include "../storage_internal.h"

#include <chrono>
#include <cstring>
#include <mutex>

#ifdef MQ_HAVE_LWS
#include <libwebsockets.h>
#endif

namespace maque::storage {

std::string ws_frame(const Request& req) {
    std::string out = "{\"op\":\"";
    switch (req.op) {
        case Op::Get:    out += "get"; break;
        case Op::Put:    out += "put"; break;
        case Op::Remove: out += "del"; break;
        case Op::Keys:   out += "keys"; break;
    }
    out += "\",\"ns\":\"" + json_escape(req.ns) + "\"";
    if (req.op != Op::Keys) out += ",\"key\":\"" + json_escape(req.key) + "\"";
    if (req.op == Op::Put) {
        const auto* bytes = (const uint8_t*)req.data;
        if (utf8_ok(bytes, req.size)) {
            out += ",\"enc\":\"utf8\",\"value\":\"" + json_escape(std::string((const char*)bytes, req.size)) + "\"";
        } else {
            out += ",\"enc\":\"b64\",\"value\":\"" + base64_encode(bytes, req.size) + "\"";
        }
    }
    out += "}";
    return out;
}

int ws_decode(const std::string& json, Op op, Response& out) {
    std::string err;
    if (json_get_str(json, "error", err) && !err.empty()) {
        if (err == "notfound") return MQ_STORAGE_ERR_NOTFOUND;
        if (err == "arg") return MQ_STORAGE_ERR_ARG;
        if (err == "unsupported") return MQ_STORAGE_ERR_UNSUPPORTED;
        if (err == "io") return MQ_STORAGE_ERR_IO;
        return MQ_STORAGE_ERR_PROTO;
    }
    std::string value;
    if (json_get_str(json, "value", value)) {
        std::string enc = "utf8";
        json_get_str(json, "enc", enc);                       // 缺省 utf8
        if (enc == "b64") {
            if (!base64_decode(value, out.value)) return MQ_STORAGE_ERR_PROTO;
        } else {
            out.value.assign((const uint8_t*)value.data(), (const uint8_t*)value.data() + value.size());
        }
        return MQ_STORAGE_OK;
    }
    std::vector<std::string> keys;
    if (json_get_str_array(json, "keys", keys)) {
        out.json = keys_json(keys);
        return MQ_STORAGE_OK;
    }
    // put / del 的成功响应只有 {"ok":true}（无 value/keys）——没有 error 即成功
    if (op == Op::Put || op == Op::Remove) return MQ_STORAGE_OK;
    return MQ_STORAGE_ERR_PROTO;
}

#ifdef MQ_HAVE_LWS

static const char* kProtoName = "maque.storage.v1";

struct WsOp {
    std::string request;      // 待发帧（文本）
    std::string rx;           // 接收累积（单帧响应）
    bool established = false;
    bool sent = false;
    bool done = false;
    bool failed = false;
};

static int ws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/, void* in, size_t len) {
    auto* op = (WsOp*)lws_context_user(lws_get_context(wsi));
    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            if (op) { op->failed = true; op->done = true; }
            return 0;
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            if (op) { op->established = true; lws_callback_on_writable(wsi); }
            return 0;
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (op && !op->sent) {
                std::vector<unsigned char> buf(LWS_PRE + op->request.size());
                std::memcpy(buf.data() + LWS_PRE, op->request.data(), op->request.size());
                const int n = lws_write(wsi, buf.data() + LWS_PRE, op->request.size(), LWS_WRITE_TEXT);
                op->sent = (n == (int)op->request.size());
                if (!op->sent) { op->failed = true; op->done = true; }
            }
            return 0;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (op && in && len) op->rx.append((const char*)in, len);
            if (op && lws_is_final_fragment(wsi)) op->done = true;
            return 0;
        case LWS_CALLBACK_CLIENT_CLOSED:
            if (op && !op->done) { op->failed = true; op->done = true; }
            return 0;
        default:
            return 0;
    }
}

static struct lws_protocols kProtos[] = {
    { kProtoName, ws_cb, 0, 4096, 0, nullptr, 0 },
    LWS_PROTOCOL_LIST_TERM
};

/* 一条连接完成一次请求-响应；返回 MQ_STORAGE_ERR_*（超时/连接失败 → IO）。 */
static int ws_perform(const WsTarget& t, const std::string& request, std::string& response) {
    static std::once_flag once;
    std::call_once(once, [] {
        lws_set_log_level(LLL_ERR, nullptr);          // 静默（错误仍可见）
    });

    WsOp op;
    op.request = request;

    struct lws_context_creation_info info;
    std::memset(&info, 0, sizeof info);
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = kProtos;
    info.user = &op;
    struct lws_context* ctx = lws_create_context(&info);
    if (!ctx) return MQ_STORAGE_ERR_IO;

    struct lws_client_connect_info ci;
    std::memset(&ci, 0, sizeof ci);
    ci.context = ctx;
    ci.address = t.host.c_str();
    ci.port = t.port;
    ci.path = t.path.c_str();
    ci.host = t.host.c_str();
    ci.origin = t.host.c_str();
    ci.protocol = kProtoName;
    ci.ssl_connection = t.tls ? LCCSCF_USE_SSL : 0;
    ci.pwsi = nullptr;
    if (!lws_client_connect_via_info(&ci)) {
        lws_context_destroy(ctx);
        return MQ_STORAGE_ERR_IO;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!op.done && std::chrono::steady_clock::now() < deadline) {
        lws_service(ctx, 50);
    }
    lws_context_destroy(ctx);

    if (!op.done || op.failed || !op.sent) return MQ_STORAGE_ERR_IO;
    response = op.rx;
    return MQ_STORAGE_OK;
}

#endif   // MQ_HAVE_LWS

int ws_apply(const WsTarget& target, const Request& req, Response& out) {
#ifdef MQ_HAVE_LWS
    std::string response;
    const int rc = ws_perform(target, ws_frame(req), response);
    if (rc != MQ_STORAGE_OK) return rc;
    return ws_decode(response, req.op, out);
#else
    (void)target; (void)req; (void)out;
    return MQ_STORAGE_ERR_UNSUPPORTED;        // 未链接 libwebsockets（构建期裁剪）
#endif
}

} // namespace maque::storage
