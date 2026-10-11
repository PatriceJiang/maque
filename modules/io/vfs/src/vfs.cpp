/* VFS 实现（docs/04）：file / mem / zip / builtin / URL 后端 + 最长前缀挂载。
 * 确定性：读路径不做隐式缓存与异步；哈希为纯函数（FNV-1a 64）。 */
#include "core/vfs/mq_vfs.h"

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <algorithm>

#ifdef MQ_HAVE_CURL
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace {

enum class Backend { File, Mem, Zip };

struct Mount {
    std::string mountpoint;   // 归一化，以 '/' 开头
    std::string source;       // 本地目录 / mem 名 / zip 路径
    Backend     backend = Backend::File;
};

/* zip 归档条目（仅支持 stored/未压缩：docs/04 归档后端最小切片）。 */
struct ZipEntry { std::string name; size_t offset = 0; size_t size = 0; };

struct Zip {
    std::string path;
    std::unordered_map<std::string, ZipEntry> entries;
};

struct Vfs {
    std::vector<Mount> mounts;                                     // 后挂载优先（最长前缀）
    std::unordered_map<std::string, std::vector<uint8_t>> builtins; // builtin: 资源
    std::unordered_map<std::string, std::vector<uint8_t>> mems;     // mem:// 内容
    std::vector<Zip> zips;                                          // 已解析归档
    std::unordered_map<std::string, size_t> zip_index;              // zip 路径 → zips 下标
};

std::string normalize_mount(const std::string& m) {
    std::string s = m.empty() ? "/" : m;
    if (s[0] != '/') s = "/" + s;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

/* 去掉协议前缀（mem:// 与 zip:// 由 backend 标记，其余按本地路径处理）。 */
std::string strip_scheme(const std::string& src, Backend* backend) {
    const char* kFile = "file://";
    const char* kMem  = "mem://";
    const char* kZip  = "zip://";
    if (src.rfind(kMem, 0) == 0)  { if (backend) *backend = Backend::Mem; return src.substr(6); }
    if (src.rfind(kZip, 0) == 0)  { if (backend) *backend = Backend::Zip; return src.substr(6); }
    if (src.rfind(kFile, 0) == 0) return src.substr(7);
    return src;
}

/* 解析 zip 中央目录（stored 项）。返回 0 成功。 */
int parse_zip(const std::string& path, Zip& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return MQ_VFS_ERR_IO;
    f.seekg(0, std::ios::end);
    const std::streamoff total = f.tellg();
    if (total < 22) return MQ_VFS_ERR_FORMAT;
    /* 从尾部找 EOCD（签名 0x06054b50）；容忍最多 64KB 注释 */
    const std::streamoff win = std::min<std::streamoff>(total, 65557);
    std::vector<char> tail((size_t)win);
    f.seekg(total - win);
    f.read(tail.data(), win);
    std::streamoff eocd = -1;
    for (std::streamoff i = win - 22; i >= 0; --i) {
        const unsigned char* p = (const unsigned char*)tail.data() + i;
        if (p[0] == 0x50 && p[1] == 0x4b && p[2] == 0x05 && p[3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) return MQ_VFS_ERR_FORMAT;
    auto u16 = [&](std::streamoff at) -> uint32_t {
        const unsigned char* p = (const unsigned char*)tail.data() + at;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    };
    auto u32 = [&](std::streamoff at) -> uint32_t {
        const unsigned char* p = (const unsigned char*)tail.data() + at;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };
    const uint32_t count = u16(eocd + 10);
    const uint32_t cd_size = u32(eocd + 12);
    const uint32_t cd_off = u32(eocd + 16);
    if (cd_off + cd_size > (uint32_t)total) return MQ_VFS_ERR_FORMAT;

    std::vector<char> cd(cd_size);
    f.seekg(cd_off);
    f.read(cd.data(), (std::streamsize)cd_size);
    size_t pos = 0;
    for (uint32_t i = 0; i < count && pos + 46 <= cd.size(); ++i) {
        const unsigned char* p = (const unsigned char*)cd.data() + pos;
        if (!(p[0] == 0x50 && p[1] == 0x4b && p[2] == 0x01 && p[3] == 0x02)) return MQ_VFS_ERR_FORMAT;
        const uint32_t method = (uint32_t)p[10] | ((uint32_t)p[11] << 8);
        const uint32_t csize = (uint32_t)p[20] | ((uint32_t)p[21] << 8) | ((uint32_t)p[22] << 16) | ((uint32_t)p[23] << 24);
        const uint32_t usize = (uint32_t)p[24] | ((uint32_t)p[25] << 8) | ((uint32_t)p[26] << 16) | ((uint32_t)p[27] << 24);
        const uint32_t name_len = (uint32_t)p[28] | ((uint32_t)p[29] << 8);
        const uint32_t extra_len = (uint32_t)p[30] | ((uint32_t)p[31] << 8);
        const uint32_t comment_len = (uint32_t)p[32] | ((uint32_t)p[33] << 8);
        const uint32_t local_off = (uint32_t)p[42] | ((uint32_t)p[43] << 8) | ((uint32_t)p[44] << 16) | ((uint32_t)p[45] << 24);
        std::string name(cd.data() + pos + 46, name_len);
        if (!name.empty() && name.back() != '/' && method == 0 && csize == usize) {
            /* 读取本地头以获得数据起点（本地 extra 长度可能与中央目录不同） */
            f.seekg(local_off);
            unsigned char lh[30];
            f.read((char*)lh, 30);
            if (lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04) {
                const uint32_t ln = (uint32_t)lh[26] | ((uint32_t)lh[27] << 8);
                const uint32_t le = (uint32_t)lh[28] | ((uint32_t)lh[29] << 8);
                out.entries[name] = ZipEntry{ name, (size_t)local_off + 30 + ln + le, usize };
            }
        }
        pos += 46 + name_len + extra_len + comment_len;
    }
    return out.entries.empty() ? MQ_VFS_ERR_FORMAT : MQ_VFS_OK;
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

int read_file_bytes(const std::string& full, void** out, size_t* out_size) {
    std::ifstream f(full, std::ios::binary);
    if (!f) return MQ_VFS_ERR_NOTFOUND;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n < 0) return MQ_VFS_ERR_IO;
    char* buf = (char*)std::malloc((size_t)n + 1);
    if (!buf) return MQ_VFS_ERR_NOMEM;
    if (n) f.read(buf, n);
    buf[n] = '\0';
    if (out) *out = buf; else std::free(buf);
    if (out_size) *out_size = (size_t)n;
    return MQ_VFS_OK;
}

#ifdef MQ_HAVE_CURL
size_t curl_write(char* ptr, size_t sz, size_t nm, void* ud) {
    ((std::string*)ud)->append(ptr, sz * nm);
    return sz * nm;
}
int curl_fetch(const std::string& url, void** out, size_t* out_size) {
    CURL* c = curl_easy_init();
    if (!c) return MQ_VFS_ERR_IO;
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return MQ_VFS_ERR_IO;
    char* buf = (char*)std::malloc(body.size() + 1);
    if (!buf) return MQ_VFS_ERR_NOMEM;
    if (!body.empty()) std::memcpy(buf, body.data(), body.size());
    buf[body.size()] = '\0';
    if (out) *out = buf; else std::free(buf);
    if (out_size) *out_size = body.size();
    return MQ_VFS_OK;
}
#endif

void* dup_bytes(const std::vector<uint8_t>& src, size_t* out_size) {
    char* buf = (char*)std::malloc(src.size() + 1);
    if (!buf) return nullptr;
    if (!src.empty()) std::memcpy(buf, src.data(), src.size());
    buf[src.size()] = '\0';
    if (out_size) *out_size = src.size();
    return buf;
}

} // namespace

extern "C" {

mq_vfs* mq_vfs_create(void) { return reinterpret_cast<mq_vfs*>(new Vfs()); }

void mq_vfs_destroy(mq_vfs* vfs) { delete reinterpret_cast<Vfs*>(vfs); }

int mq_vfs_mount(mq_vfs* vfs, const char* mountpoint, const char* source) {
    if (!vfs || !mountpoint || !source) return MQ_VFS_ERR_ARG;
    auto* v = reinterpret_cast<Vfs*>(vfs);
    Backend b = Backend::File;
    const std::string src = strip_scheme(source, &b);
    if (b == Backend::Zip) {
        if (v->zip_index.find(src) == v->zip_index.end()) {
            Zip z; z.path = src;
            const int rc = parse_zip(src, z);
            if (rc != MQ_VFS_OK) return rc;
            v->zip_index[src] = v->zips.size();
            v->zips.push_back(std::move(z));
        }
    }
    v->mounts.push_back(Mount{ normalize_mount(mountpoint), src, b });
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
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    if (!vfs || !path) return MQ_VFS_ERR_ARG;
    auto* v = reinterpret_cast<Vfs*>(vfs);

    // builtin: 内建资源（引擎提供，区分于 assets）
    if (std::strncmp(path, "builtin:", 8) == 0) {
        auto it = v->builtins.find(path + 8);
        if (it == v->builtins.end()) return MQ_VFS_ERR_NOTFOUND;
        void* buf = dup_bytes(it->second, out_size);
        if (!buf) return MQ_VFS_ERR_NOMEM;
        if (out) *out = buf; else std::free(buf);
        return MQ_VFS_OK;
    }
    // file:// 直读本地文件（不经挂载）；res:// 为资源根别名（docs/20）
    if (std::strncmp(path, "file://", 7) == 0) return read_file_bytes(path + 7, out, out_size);
    std::string p = path;
    if (p.rfind("res://", 0) == 0) p = p.substr(5);

#ifdef MQ_HAVE_CURL
    // 直接 URL（http/https/file）→ 经 libcurl 抓取（浏览器式，docs/04/16）
    if (p.find("://") != std::string::npos && p.rfind("file://", 0) != 0)
        return curl_fetch(p, out, out_size);
#endif
    std::string rel;
    const Mount* m = resolve(v, p, rel);
    if (!m) return MQ_VFS_ERR_NO_MOUNT;

    switch (m->backend) {
        case Backend::Mem: {
            /* mem 挂载：source 即"内存命名空间"，条目键为 "namespace/rel"（docs/04 内存后端） */
            const std::string key = m->source + "/" + rel;
            auto k = v->mems.find(key);
            if (k == v->mems.end()) return MQ_VFS_ERR_NOTFOUND;
            void* buf = dup_bytes(k->second, out_size);
            if (!buf) return MQ_VFS_ERR_NOMEM;
            if (out) *out = buf; else std::free(buf);
            return MQ_VFS_OK;
        }
        case Backend::Zip: {
            auto zi = v->zip_index.find(m->source);
            if (zi == v->zip_index.end()) return MQ_VFS_ERR_NOTFOUND;
            const Zip& z = v->zips[zi->second];
            auto e = z.entries.find(rel);
            if (e == z.entries.end()) return MQ_VFS_ERR_NOTFOUND;
            std::ifstream f(z.path, std::ios::binary);
            if (!f) return MQ_VFS_ERR_IO;
            std::vector<uint8_t> data(e->second.size);
            f.seekg((std::streamoff)e->second.offset);
            if (e->second.size) f.read((char*)data.data(), (std::streamsize)e->second.size);
            void* buf = dup_bytes(data, out_size);
            if (!buf) return MQ_VFS_ERR_NOMEM;
            if (out) *out = buf; else std::free(buf);
            return MQ_VFS_OK;
        }
        case Backend::File:
        default: {
            // mem:// 名字直接命中（无挂载点时的语法糖）
            auto it = v->mems.find(rel.empty() ? m->source : rel);
            if (it != v->mems.end()) {
                void* buf = dup_bytes(it->second, out_size);
                if (!buf) return MQ_VFS_ERR_NOMEM;
                if (out) *out = buf; else std::free(buf);
                return MQ_VFS_OK;
            }
            return read_file_bytes((fs::path(m->source) / rel).string(), out, out_size);
        }
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
    std::string rel;
    const Mount* m = resolve(v, p, rel);
    if (!m) return MQ_VFS_ERR_NO_MOUNT;
    const char* kind = m->backend == Backend::Mem ? "mem" : m->backend == Backend::Zip ? "zip" : "file";
    std::string full = std::string(kind) + "://" + m->source + "/" + rel;
    if (out && out_cap) {
        std::strncpy(out, full.c_str(), out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return MQ_VFS_OK;
}

} // extern "C"
