#include "core/vfs/mq_vfs.h"

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_map>

#ifdef MQ_HAVE_CURL
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Mount {
    std::string mountpoint;   // 归一化，以 '/' 开头
    std::string source;       // 本地目录（已去掉 file:// 前缀）
};

struct Vfs {
    std::vector<Mount> mounts;   // 后挂载优先（最长前缀为第一关键字）
    std::unordered_map<std::string, std::vector<uint8_t>> builtins;   // builtin: 资源（内建）
};

std::string normalize_mount(const std::string& m) {
    std::string s = m.empty() ? "/" : m;
    if (s[0] != '/') s = "/" + s;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string strip_scheme(const std::string& src) {
    const std::string p = "file://";
    if (src.rfind(p, 0) == 0) return src.substr(p.size());
    return src;
}

/* 找到匹配 path（最长前缀）的挂载；返回相对路径。 */
const Mount* resolve(Vfs* v, const std::string& path, std::string& rel_out) {
    const Mount* best = nullptr;
    size_t best_len = 0;
    for (const auto& m : v->mounts) {
        const std::string& mp = m.mountpoint;
        bool match = (mp == "/") || (path.rfind(mp, 0) == 0 &&
                      (path.size() == mp.size() || path[mp.size()] == '/'));
        if (match && mp.size() >= best_len) { best = &m; best_len = mp.size(); }
    }
    if (!best) return nullptr;
    std::string rel = path;
    if (best->mountpoint != "/") rel = rel.substr(best->mountpoint.size());
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    rel_out = rel;
    return best;
}

#ifdef MQ_HAVE_CURL
size_t curl_write(char* ptr, size_t sz, size_t nm, void* ud) {
    ((std::string*)ud)->append(ptr, sz * nm);
    return sz * nm;
}
/* 经 libcurl 抓取 URL（http/https/file…）。*/
int curl_fetch(const std::string& url, void** out, size_t* out_size) {
    CURL* c = curl_easy_init();
    if (!c) return 10;
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return 11;
    char* buf = (char*)std::malloc(body.size() + 1);
    if (!buf) return 12;
    if (!body.empty()) std::memcpy(buf, body.data(), body.size());
    buf[body.size()] = '\0';
    if (out) *out = buf; else std::free(buf);
    if (out_size) *out_size = body.size();
    return 0;
}
#endif

} // namespace

extern "C" {

mq_vfs* mq_vfs_create(void) { return reinterpret_cast<mq_vfs*>(new Vfs()); }

void mq_vfs_destroy(mq_vfs* vfs) { delete reinterpret_cast<Vfs*>(vfs); }

int mq_vfs_mount(mq_vfs* vfs, const char* mountpoint, const char* source) {
    if (!vfs || !mountpoint || !source) return 1;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    v->mounts.push_back(Mount{ normalize_mount(mountpoint), strip_scheme(source) });
    return 0;
}

void mq_vfs_register_builtin(mq_vfs* vfs, const char* logical, const void* data, size_t size) {
    if (!vfs || !logical) return;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    auto& b = v->builtins[logical];
    b.assign((const uint8_t*)data, (const uint8_t*)data + size);
}

int mq_vfs_exists(mq_vfs* vfs, const char* path) {
    void* data = nullptr; size_t size = 0;
    if (mq_vfs_read(vfs, path, &data, &size) != 0) return 0;
    mq_vfs_free(data);
    return 1;
}

int mq_vfs_read(mq_vfs* vfs, const char* path, void** out, size_t* out_size) {
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    if (!vfs || !path) return 1;
    // builtin: 内建资源（引擎提供，区分于 assets）
    {
        const char* pfx = "builtin:";
        if (std::strncmp(path, pfx, 8) == 0) {
            auto* v = reinterpret_cast<Vfs*>(vfs);
            auto it = v->builtins.find(path + 8);
            if (it == v->builtins.end()) return 5;
            size_t n = it->second.size();
            char* buf = (char*)std::malloc(n + 1);
            if (!buf) return 4;
            if (n) std::memcpy(buf, it->second.data(), n);
            buf[n] = '\0';
            if (out) *out = buf; else std::free(buf);
            if (out_size) *out_size = n;
            return 0;
        }
    }
#ifdef MQ_HAVE_CURL
    // 直接 URL（http/https/file）→ 经 libcurl 抓取（浏览器式，docs/04/16）
    if (std::strstr(path, "://")) return curl_fetch(path, out, out_size);
#endif
    auto* v = reinterpret_cast<Vfs*>(vfs);
    std::string rel;
    const Mount* m = resolve(v, path, rel);
    if (!m) return 2;
    fs::path full = fs::path(m->source) / rel;
    std::ifstream f(full, std::ios::binary);
    if (!f) return 3;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n < 0) return 3;
    auto* buf = (char*)std::malloc((size_t)n + 1);
    if (!buf) return 4;
    f.read(buf, n);
    buf[n] = '\0';
    if (out) *out = buf; else std::free(buf);
    if (out_size) *out_size = (size_t)n;
    return 0;
}

void mq_vfs_free(void* p) { std::free(p); }

} // extern "C"
