/* VFS 核心（docs/04）：实例状态 + 挂载表 + 最长前缀匹配 + 按协议分发 + C ABI 入口。
 *
 * 协议实现各自独立成目录（见 vfs_internal.h）：file/ · mem/ · zip/ · url/ · builtin/；
 * 本文件只做"挂载点 → 后端"的路由，不掺协议细节。
 * 确定性：读路径不做隐式异步；URL 挂载带会话内容缓存（同一 URL 只抓一次）；哈希为纯函数。 */
#include "vfs_internal.h"
#include "core/tracy/mq_tracy.h"

#include <cstdlib>
#include <cstring>

using namespace maque::vfs;

namespace {

struct Vfs {
    std::vector<Mount> mounts;          // 后挂载优先（最长前缀）
    BlobTable          builtins;        // builtin: 资源（程序化生成）
    BlobTable          mems;            // mem:// 内容（键 = "命名空间/rel"）
    ZipStore           zips;            // 已解析归档
    UrlCache           urls;            // http(s) 会话内容缓存
};

/* 找到匹配 path（最长前缀）的挂载；返回相对路径。 */
const Mount* resolve(const Vfs* v, const std::string& path, std::string& rel_out) {
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

} // namespace

extern "C" {

mq_vfs* mq_vfs_create(void) { return reinterpret_cast<mq_vfs*>(new Vfs()); }

void mq_vfs_destroy(mq_vfs* vfs) { delete reinterpret_cast<Vfs*>(vfs); }

int mq_vfs_mount(mq_vfs* vfs, const char* mountpoint, const char* source) {
    if (!vfs || !mountpoint || !source) return MQ_VFS_ERR_ARG;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    Backend b = Backend::File;
    std::string src = strip_scheme(source, &b);
    /* http(s):// 目录挂载（docs/04 §4/§5）：source = URL 基址，读取时拼相对路径 */
    if (b == Backend::File && is_url(src)) {
        if (src.size() <= 8) return MQ_VFS_ERR_ARG;        // 只有 scheme，没有主机
#ifndef MQ_HAVE_CURL
        return MQ_VFS_ERR_UNSUPPORTED;                     // 未链接 libcurl（构建期裁剪）
#else
        b = Backend::Url;
        src = url_normalize_base(std::move(src));
#endif
    }
    if (b == Backend::Zip) {
        const int rc = zip_open(v->zips, src);
        if (rc != MQ_VFS_OK) return rc;
    }
    const std::string mp = normalize_mount(mountpoint);
    /* 幂等：同一 (挂载点, 来源) 重复挂载不追加（切场景/重复应用 MAQUE_MOUNT 时不堆积） */
    for (const auto& m : v->mounts)
        if (m.mountpoint == mp && m.source == src && m.backend == b) return MQ_VFS_OK;
    v->mounts.push_back(Mount{ mp, std::move(src), b });
    return MQ_VFS_OK;
}

int mq_vfs_add_mem(mq_vfs* vfs, const char* name, const void* data, size_t size) {
    if (!vfs || !name || (!data && size)) return MQ_VFS_ERR_ARG;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    auto& b = v->mems[name];
    b.assign((const uint8_t*)data, (const uint8_t*)data + size);
    return MQ_VFS_OK;
}

int mq_vfs_mount_zip(mq_vfs* vfs, const char* mountpoint, const char* zip_path) {
    if (!zip_path) return MQ_VFS_ERR_ARG;
    std::string src = "zip://";
    src += zip_path;
    return mq_vfs_mount(vfs, mountpoint, src.c_str());
}

void mq_vfs_register_builtin(mq_vfs* vfs, const char* logical, const void* data, size_t size) {
    if (!vfs || !logical) return;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    auto& b = v->builtins[logical];
    b.assign((const uint8_t*)data, (const uint8_t*)data + size);
}

int mq_vfs_exists(mq_vfs* vfs, const char* path) {
    void* data = nullptr; size_t size = 0;
    if (mq_vfs_read(vfs, path, &data, &size) != MQ_VFS_OK) return 0;
    mq_vfs_free(data);
    return 1;
}

int mq_vfs_read(mq_vfs* vfs, const char* path, void** out, size_t* out_size) {
    MQ_TRACY_ZONE_N(vfs, "read");
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    if (!vfs || !path) return MQ_VFS_ERR_ARG;
    auto* v = reinterpret_cast<Vfs*>(vfs);

    // builtin: 内建资源（引擎提供，区分于 assets）
    if (std::strncmp(path, "builtin:", 8) == 0) return builtin_read(v->builtins, path + 8, out, out_size);
    // file:// 直读本地文件（不经挂载）；res:// 为资源根别名（docs/20）
    if (std::strncmp(path, "file://", 7) == 0) return file_read("", path + 7, out, out_size);
    std::string p = path;
    if (p.rfind("res://", 0) == 0) p = p.substr(5);

    // 路径穿越防护（docs/04 §9）：所有后端统一拦截 ".." 段
    if (!path_is_safe(p)) return MQ_VFS_ERR_ARG;

    // 直接 URL（http(s)/file）→ 经 libcurl 抓取（浏览器式，docs/04/16）
    if (p.find("://") != std::string::npos && p.rfind("file://", 0) != 0)
        return url_read(v->urls, p, out, out_size);

    std::string rel;
    const Mount* m = resolve(v, p, rel);
    if (!m) return MQ_VFS_ERR_NO_MOUNT;

    switch (m->backend) {
        case Backend::Mem:
            /* mem 挂载：source 即"内存命名空间"，条目键为 "namespace/rel"（docs/04 内存后端） */
            return mem_read(v->mems, m->source, rel, out, out_size);
        case Backend::Zip:
            return zip_read(v->zips, m->source, rel, out, out_size);
        case Backend::Url: {
            /* URL 目录挂载：基址 + 逐段编码的相对路径（docs/04 §4 http(s) 后端） */
            std::string encoded;
            const int rc = url_encode_rel(rel, encoded);
            if (rc != MQ_VFS_OK) return rc;
            return url_read(v->urls, m->source + encoded, out, out_size);
        }
        case Backend::File:
        default:
            // mem:// 名字直接命中（无挂载点时的语法糖）
            if (int rc = mem_lookup(v->mems, rel.empty() ? m->source : rel, out, out_size); rc != MQ_VFS_ERR_NOTFOUND)
                return rc;
            return file_read(m->source, rel, out, out_size);
    }
}

void mq_vfs_free(void* p) { std::free(p); }

uint64_t mq_vfs_hash(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t mq_vfs_hash_path(mq_vfs* vfs, const char* path) {
    void* d = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, path, &d, &n) != MQ_VFS_OK) return 0;
    const uint64_t h = mq_vfs_hash(d, n);
    mq_vfs_free(d);
    return h;
}

int mq_vfs_read_verified(mq_vfs* vfs, const char* path, uint64_t expect, void** out, size_t* out_size) {
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    void* d = nullptr; size_t n = 0;
    const int rc = mq_vfs_read(vfs, path, &d, &n);
    if (rc != MQ_VFS_OK) return rc;
    const uint64_t h = mq_vfs_hash(d, n);
    if (expect && h != expect) { mq_vfs_free(d); return MQ_VFS_ERR_HASH; }
    if (out) *out = d; else mq_vfs_free(d);
    if (out_size) *out_size = n;
    return MQ_VFS_OK;
}

int mq_vfs_resolve(mq_vfs* vfs, const char* path, char* out, size_t out_cap) {
    if (!vfs || !path) return MQ_VFS_ERR_ARG;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    std::string p = path;
    if (p.rfind("res://", 0) == 0) p = p.substr(5);
    if (!path_is_safe(p)) return MQ_VFS_ERR_ARG;
    std::string rel;
    const Mount* m = resolve(v, p, rel);
    if (!m) return MQ_VFS_ERR_NO_MOUNT;
    std::string full;
    if (m->backend == Backend::Url) {
        std::string encoded;
        const int rc = url_encode_rel(rel, encoded);
        if (rc != MQ_VFS_OK) return rc;
        full = m->source + encoded;
    } else {
        const char* kind = m->backend == Backend::Mem ? "mem" : m->backend == Backend::Zip ? "zip" : "file";
        full = std::string(kind) + "://" + m->source + "/" + rel;
    }
    if (out && out_cap) {
        std::strncpy(out, full.c_str(), out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return MQ_VFS_OK;
}

} // extern "C"
