/* http(s):// 后端：REST 固定协议（docs/25）
 *   GET    <base>/<ns>/<key>     → 200 值 | 404 不存在
 *   PUT    <base>/<ns>/<key>      请求体 = 值           → 2xx
 *   DELETE <base>/<ns>/<key>     → 2xx | 404
 *   GET    <base>/<ns>?keys=1    → 200 JSON 数组（键列表）
 * 与 file/mem 语义一致（get/put/remove/keys）；服务端只需实现这四条路由。 */
#include "../storage_internal.h"

#include <mutex>

#ifdef MQ_HAVE_CURL
#include <curl/curl.h>
#endif

namespace maque::storage {

#ifdef MQ_HAVE_CURL

constexpr long kConnectTimeoutSec = 10;
constexpr long kRequestTimeoutSec = 60;

static void curl_ensure_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static size_t curl_write(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* out = (Blob*)ud;
    out->insert(out->end(), (uint8_t*)ptr, (uint8_t*)ptr + sz * nm);
    return sz * nm;
}

/* HTTP 状态码 → 存储错误码。 */
static int status_to_err(long status) {
    if (status == 404 || status == 410) return MQ_STORAGE_ERR_NOTFOUND;
    if (status >= 200 && status < 300) return MQ_STORAGE_OK;
    return MQ_STORAGE_ERR_PROTO;
}

static int http_request(const std::string& url, const char* method, const void* body, size_t body_size,
                        Blob& out_body) {
    curl_ensure_init();
    CURL* c = curl_easy_init();
    if (!c) return MQ_STORAGE_ERR_IO;
    out_body.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out_body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "maque-storage/1");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, kRequestTimeoutSec);
    if (body || body_size) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, (const char*)body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_size);
    }
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return MQ_STORAGE_ERR_IO;
    return status_to_err(status);
}

#endif   // MQ_HAVE_CURL

int http_apply(const std::string& base_url, const Request& req, Response& out) {
#ifdef MQ_HAVE_CURL
    const std::string item = base_url + url_path(req.ns, req.key);
    switch (req.op) {
        case Op::Get:
            return http_request(item, "GET", nullptr, 0, out.value);
        case Op::Put:
            return http_request(item, "PUT", req.data, req.size, out.value);   // 响应体不关心
        case Op::Remove:
            return http_request(item, "DELETE", nullptr, 0, out.value);
        case Op::Keys: {
            /* 命名空间末尾带 '/' 才能拼出 "?keys=1"（服务端按前缀路由） */
            std::string ns_url = base_url + url_path(req.ns, "");
            while (ns_url.size() > base_url.size() && ns_url.back() == '/') ns_url.pop_back();
            Blob body;
            const int rc = http_request(ns_url + "?keys=1", "GET", nullptr, 0, body);
            if (rc != MQ_STORAGE_OK) return rc;
            out.json.assign((const char*)body.data(), body.size());
            return out.json.empty() ? MQ_STORAGE_ERR_PROTO : MQ_STORAGE_OK;
        }
    }
    return MQ_STORAGE_ERR_ARG;
#else
    (void)base_url; (void)req; (void)out;
    return MQ_STORAGE_ERR_UNSUPPORTED;      // 未链接 libcurl（构建期裁剪）
#endif
}

} // namespace maque::storage
