/* mem:// 后端：内存命名空间（键 = "<命名空间>/<相对路径>"，由 mq_vfs_add_mem 写入）。
 * 用于测试、生成数据、AI dump（docs/04 §4 P0）。 */
#include "../vfs_internal.h"

namespace maque::vfs {

int mem_read(const BlobTable& namespaces, const std::string& ns, const std::string& rel,
             void** out, size_t* out_size) {
    return mem_lookup(namespaces, ns + "/" + rel, out, out_size);
}

int mem_lookup(const BlobTable& namespaces, const std::string& key, void** out, size_t* out_size) {
    auto it = namespaces.find(key);
    if (it == namespaces.end()) return MQ_VFS_ERR_NOTFOUND;
    return deliver(it->second, out, out_size);
}

} // namespace maque::vfs
