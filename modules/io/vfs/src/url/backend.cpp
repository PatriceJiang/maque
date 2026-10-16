/* http(s):// 后端：URL 目录挂载（docs/04 §4 P1）。
 *   mq_vfs_mount(vfs, "/cdn", "https://host/base/")  →  读 "/cdn/a b.png" = GET "https://host/base/a%20b.png"
 * 抓取：libcurl（重定向跟随 / 超时 / 自动解压 / NOSIGNAL）；会话内容缓存避免重复下载（§8）。
 * 未链接 libcurl（构建期裁剪）：挂载返回 MQ_VFS_ERR_UNSUPPORTED（不静默降级）。 */
#include "../vfs_internal.h"

#include <mutex>

#ifdef MQ_HAVE_CURL
#include <curl/curl.h>
#endif

namespace maque::vfs {

#ifdef MQ_HAVE_CURL

/* 单次链接超时 / 整体超时（秒）。冷路径（装载期）使用，避免服务器无响应时挂死。 */
constexpr long kConnectTimeoutSec = 10;
constexpr long kRequestTimeoutSec = 60;

/* 会话缓存预算：单条目上限 32 MiB、总量上限 256 MiB（超过则不入缓存，直接返回本次结果）。 */
constexpr size_t kUrlCacheMaxBody = size_t(32) << 20;
constexpr size_t kUrlCacheLimit   = size_t(256) << 20;

static void curl_ensure_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static size_t curl_write(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* out = (Blob*)ud;
    out->insert(out->end(), (uint8_t*)ptr, (uint8_t*)ptr + sz * nm);
    return sz * nm;
}

/* 抓取 URL → body。HTTP 状态码映射到 VFS 错误码（404/410 → NOTFOUND，其余 4xx/5xx → IO）。 */
static int curl_fetch_bytes(const std::string& url, Blob& body) {
    curl_ensure_init();
    CURL* c = curl_easy_init();
    if (!c) return MQ_VFS_ERR_IO;
    body.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);              // 不装信号处理器（可在任意线程用）
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");       // gzip/deflate/br（若 libcurl 支持）
    curl_easy_setopt(c, CURLOPT_USERAGENT, "maque-vfs/1");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, kRequestTimeoutSec);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return MQ_VFS_ERR_IO;
    if (status == 404 || status == 410) return MQ_VFS_ERR_NOTFOUND;
    if (status >= 400) return MQ_VFS_ERR_IO;               // 401/403/5xx：统一按 IO（鉴权面二期）
    return MQ_VFS_OK;
}

#endif   // MQ_HAVE_CURL

bool is_url(const std::string& source) {
    return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
}

std::string url_normalize_base(std::string base) {
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    base.push_back('/');
    return base;
}

int url_encode_rel(const std::string& rel, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i < rel.size()) {
        if (rel[i] == '/') { out.push_back('/'); ++i; continue; }
        const size_t j = rel.find('/', i);
        const std::string seg = rel.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == ".." || seg == ".") return MQ_VFS_ERR_ARG;
#ifdef MQ_HAVE_CURL
        char* esc = curl_easy_escape(nullptr, seg.c_str(), (int)seg.size());
        if (!esc) return MQ_VFS_ERR_NOMEM;
        out += esc;
        curl_free(esc);
#else
        out += seg;
#endif
        i = (j == std::string::npos) ? rel.size() : j;
    }
    return MQ_VFS_OK;
}

/* 读 URL（docs/04 §8）：命中会话缓存直接返回副本；否则抓取并按预算入缓存。
 * 缓存使同一 URL 在一次进程内只走一次网络 → 装载期可复现、CI 可缓存。 */
int url_read(UrlCache& cache, const std::string& url, void** out, size_t* out_size) {
#ifdef MQ_HAVE_CURL
    if (auto hit = cache.blobs.find(url); hit != cache.blobs.end())
        return deliver(hit->second, out, out_size);
    Blob body;
    const int rc = curl_fetch_bytes(url, body);
    if (rc != MQ_VFS_OK) return rc;
    if (body.size() <= kUrlCacheMaxBody && cache.bytes + body.size() <= kUrlCacheLimit) {
        cache.bytes += body.size();
        auto it = cache.blobs.emplace(url, std::move(body)).first;
        return deliver(it->second, out, out_size);
    }
    return deliver(body, out, out_size);
#else
    (void)cache; (void)url; (void)out; (void)out_size;
    return MQ_VFS_ERR_UNSUPPORTED;
#endif
}

} // namespace maque::vfs
