/* VFS 共享工具：挂载点归一化 / scheme 解析 / 路径穿越防护 / 缓冲交付（见 vfs_internal.h）。 */
#include "vfs_internal.h"

#include <cstdlib>
#include <cstring>

namespace maque::vfs {

int deliver(const Blob& src, void** out, size_t* out_size) {
    char* buf = (char*)std::malloc(src.size() + 1);
    if (!buf) return MQ_VFS_ERR_NOMEM;
    if (!src.empty()) std::memcpy(buf, src.data(), src.size());
    buf[src.size()] = '\0';
    if (out) *out = buf; else std::free(buf);
    if (out_size) *out_size = src.size();
    return MQ_VFS_OK;
}

std::string normalize_mount(const std::string& m) {
    std::string s = m.empty() ? "/" : m;
    if (s[0] != '/') s = "/" + s;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string strip_scheme(const std::string& src, Backend* backend) {
    const char* kFile  = "file://";
    const char* kMem   = "mem://";
    const char* kZip   = "zip://";
    const char* kAsset = "asset://";
    if (src.rfind(kMem, 0) == 0)   { if (backend) *backend = Backend::Mem; return src.substr(6); }
    if (src.rfind(kZip, 0) == 0)   { if (backend) *backend = Backend::Zip; return src.substr(6); }
    if (src.rfind(kAsset, 0) == 0) { if (backend) *backend = Backend::Asset; return src.substr(8); }
    if (src.rfind(kFile, 0) == 0) return src.substr(7);
    return src;
}

bool path_is_safe(const std::string& p) {
    size_t i = 0;
    while (i <= p.size()) {
        const size_t j = p.find_first_of("/\\", i);
        const std::string seg = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

} // namespace maque::vfs
