/* file:// 后端：本地目录（挂载 source = 目录，读取 <目录>/<rel>）。docs/04 §4 P0。 */
#include "../vfs_internal.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace maque::vfs {

int file_read(const std::string& dir, const std::string& rel, void** out, size_t* out_size) {
    /* dir 为空 = 直接给完整路径（file:// 直读，不经挂载）；否则拼挂载目录 */
    const std::string full = dir.empty() ? rel : (fs::path(dir) / rel).string();
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

} // namespace maque::vfs
