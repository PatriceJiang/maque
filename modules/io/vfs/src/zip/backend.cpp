/* zip:// 后端：归档（stored/未压缩项；挂载时解析一次中央目录，读取按偏移直接读）。
 * docs/04 §4 P0：与 ai 打包产物对接（完整 deflate 解压留待需要时补）。 */
#include "../vfs_internal.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace maque::vfs {

/* 解析 zip 中央目录（stored 项）。返回 0 成功。 */
static int parse_zip(const std::string& path, ZipArchive& out) {
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
            unsigned char lh[30] = { 0 };                 // 零初始化：读取失败时不得使用未初始化字节
            f.read((char*)lh, sizeof lh);
            if (f.gcount() == (std::streamsize)sizeof lh &&
                lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04) {
                const uint32_t ln = (uint32_t)lh[26] | ((uint32_t)lh[27] << 8);
                const uint32_t le = (uint32_t)lh[28] | ((uint32_t)lh[29] << 8);
                out.entries[name] = ZipEntry{ name, (size_t)local_off + 30 + ln + le, usize };
            }
        }
        pos += 46 + name_len + extra_len + comment_len;
    }
    return out.entries.empty() ? MQ_VFS_ERR_FORMAT : MQ_VFS_OK;
}

int zip_open(ZipStore& store, const std::string& path) {
    if (store.index.find(path) != store.index.end()) return MQ_VFS_OK;   // 已解析
    ZipArchive z;
    z.path = path;
    const int rc = parse_zip(path, z);
    if (rc != MQ_VFS_OK) return rc;
    store.index[path] = store.archives.size();
    store.archives.push_back(std::move(z));
    return MQ_VFS_OK;
}

int zip_read(const ZipStore& store, const std::string& source, const std::string& rel,
             void** out, size_t* out_size) {
    auto zi = store.index.find(source);
    if (zi == store.index.end()) return MQ_VFS_ERR_NOTFOUND;
    const ZipArchive& z = store.archives[zi->second];
    auto e = z.entries.find(rel);
    if (e == z.entries.end()) return MQ_VFS_ERR_NOTFOUND;
    std::ifstream f(z.path, std::ios::binary);
    if (!f) return MQ_VFS_ERR_IO;
    Blob data(e->second.size);
    f.seekg((std::streamoff)e->second.offset);
    if (e->second.size) f.read((char*)data.data(), (std::streamsize)e->second.size);
    return deliver(data, out, out_size);
}

} // namespace maque::vfs
