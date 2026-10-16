/* builtin: 后端：引擎内建资源（程序化生成的纹理/材质，由运行时 mq_vfs_register_builtin 注册）。
 * 与 assets 区分：内建资源随引擎走，不依赖游戏资源目录（docs/04 §4 / docs/20）。 */
#include "../vfs_internal.h"

namespace maque::vfs {

int builtin_read(const BlobTable& builtins, const std::string& logical, void** out, size_t* out_size) {
    auto it = builtins.find(logical);
    if (it == builtins.end()) return MQ_VFS_ERR_NOTFOUND;
    return deliver(it->second, out, out_size);
}

} // namespace maque::vfs
