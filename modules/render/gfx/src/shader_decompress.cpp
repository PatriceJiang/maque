/* 内建 shader 解压实现（gfx 内部，非生成物）。见 shader_decompress.h。 */
#include "shader_decompress.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef MQ_HAVE_ZSTD
#include <zstd.h>
#endif

namespace {

/* data 指针 → 解压后的文本（进程生命周期内缓存；WGSL 文本不可变，无需失效）。 */
std::mutex g_mu;
std::unordered_map<const void*, std::string> g_cache;

const char* unpack(const char* name, const unsigned char* data, unsigned int size,
                   unsigned int raw_size, int compressed) {
    if (!data || raw_size == 0) return nullptr;
    std::string text;
    if (compressed) {
#ifdef MQ_HAVE_ZSTD
        text.resize(raw_size);
        const size_t n = ZSTD_decompress(text.data(), text.size(), data, size);
        if (ZSTD_isError(n) || n != raw_size) {
            std::fprintf(stderr, "gfx: 内建 shader '%s' 解压失败（%s；blob %u → raw %u）\n",
                         name ? name : "?", ZSTD_getErrorName(n), size, raw_size);
            return nullptr;
        }
#else
        std::fprintf(stderr, "gfx: 内建 shader '%s' 是压缩的，但本构建未链接 libzstd"
                             "（用 -DMAQUE_BUILTIN_SHADER_COMPRESS=OFF 重新打包）\n",
                     name ? name : "?");
        return nullptr;
#endif
    } else {
        text.assign(reinterpret_cast<const char*>(data), raw_size);
    }
    text.push_back('\0');                 // c_str() 本已 NUL 结尾；显式加一个便于 memcpy/长度断言
    return g_cache.emplace(data, std::move(text)).first->second.c_str();
}

} // namespace

extern "C" const char* mq_shader_unpack(const char* name, const unsigned char* data, unsigned int size,
                                        unsigned int raw_size, int compressed) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (auto it = g_cache.find(data); it != g_cache.end()) return it->second.c_str();
    return unpack(name, data, size, raw_size, compressed);
}
