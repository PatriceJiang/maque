/* VFS 内部接口（**模块内 C++**；跨模块只走 C ABI —— abi/vfs.h，docs/02/21）。
 *
 * 目录布局：一个协议一个目录（docs/04 §4 协议清单），核心只做挂载 + 分发：
 *   vfs.cpp               核心：实例状态、挂载表、最长前缀匹配、按后端分发、C ABI 入口
 *   util.cpp              共享工具：挂载点归一化 / scheme 解析 / 路径穿越防护 / 缓冲交付
 *   file/backend.cpp      file://     本地目录
 *   mem/backend.cpp       mem://      内存命名空间（键 = "namespace/rel"）
 *   zip/backend.cpp       zip://      归档（stored；挂载时解析一次中央目录）
 *   url/backend.cpp       http(s)://  libcurl 抓取 + 会话内容缓存 + 逐段 percent-encode
 *   builtin/backend.cpp   builtin:    引擎内建资源（程序化生成，区别于 assets）
 *
 * 后端约定：
 *   · rel = 挂载点之后的相对路径（无前导 '/'）；返回 MQ_VFS_ERR_*；
 *   · 后端**不持有实例状态**（需要什么都由调用方传入）→ 可单独替换/单测；
 *   · 写进 out 的缓冲一律经 deliver()（malloc + '\0' 结尾，调用方 mq_vfs_free）。 */
#ifndef MAQUE_VFS_INTERNAL_H
#define MAQUE_VFS_INTERNAL_H

#include "core/vfs/mq_vfs.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace maque::vfs {

/* 挂载点的后端类型（协议）。 */
enum class Backend { File, Mem, Zip, Url };

struct Mount {
    std::string mountpoint;   // 归一化，以 '/' 开头
    std::string source;       // 本地目录 / mem 命名空间 / zip 路径 / URL 基址（以 '/' 结尾）
    Backend     backend = Backend::File;
};

using Blob = std::vector<uint8_t>;
using BlobTable = std::unordered_map<std::string, Blob>;   // mem 命名空间表 / builtin 逻辑名表

/* ---- 共享工具（util.cpp）---- */

/* 把内容交给调用方（out 为 nullptr 则只测可达性）：malloc + '\0' 结尾。 */
int deliver(const Blob& src, void** out, size_t* out_size);

std::string normalize_mount(const std::string& m);

/* 去掉协议前缀（mem:// / zip:// 置 backend；file:// 与 http(s):// 保留语义由调用方判断）。 */
std::string strip_scheme(const std::string& src, Backend* backend);

/* 路径穿越防护（docs/04 §9）：拒绝 ".." 段（'/' 与 '\' 都当分隔符）—— 所有后端统一在此拦截。
 * 注意：'\' 本身是合法文件名字符（Linux），只有构成 ".." 段时才拒绝（asset 用例 back\slash.scn）。 */
bool path_is_safe(const std::string& p);

/* ---- file://（file/backend.cpp）---- */

/* 读 <dir>/<rel>；不存在 → NOTFOUND，打不开 → IO。 */
int file_read(const std::string& dir, const std::string& rel, void** out, size_t* out_size);

/* ---- mem://（mem/backend.cpp）---- */

/* 挂载视图：条目键 = "<命名空间>/<rel>"。 */
int mem_read(const BlobTable& namespaces, const std::string& ns, const std::string& rel,
             void** out, size_t* out_size);

/* 裸键查找（无挂载点的语法糖：挂载 source 本身就是 mem 名字时）。命中 → 交付，否则 NOTFOUND。 */
int mem_lookup(const BlobTable& namespaces, const std::string& key, void** out, size_t* out_size);

/* ---- zip://（zip/backend.cpp）---- */

/* zip 归档条目（仅 stored/未压缩：docs/04 归档后端最小切片）。 */
struct ZipEntry { std::string name; size_t offset = 0; size_t size = 0; };

struct ZipArchive {
    std::string path;
    std::unordered_map<std::string, ZipEntry> entries;
};

/* 已打开归档集合（挂载时解析一次；同路径只解析一次）。 */
struct ZipStore {
    std::vector<ZipArchive> archives;
    std::unordered_map<std::string, size_t> index;    // 路径 → archives 下标
};

/* 打开（解析中央目录）并登记；已登记则直接成功。 */
int zip_open(ZipStore& store, const std::string& path);

/* 从 <source> 归档里读条目 <rel>。 */
int zip_read(const ZipStore& store, const std::string& source, const std::string& rel,
             void** out, size_t* out_size);

/* ---- http(s)://（url/backend.cpp）---- */

/* 会话内容缓存（docs/04 §8）：同 URL 一次进程内只走一次网络（预算见 url/backend.cpp）。 */
struct UrlCache {
    BlobTable blobs;      // URL → 内容
    size_t bytes = 0;     // 已缓存字节数
};

bool is_url(const std::string& source);

/* 目录基址归一化：结尾恰好一个 '/'（"https://cdn/x" 与 "https://cdn/x/" 等价）。 */
std::string url_normalize_base(std::string base);

/* 相对路径 → URL 片段：逐段 percent-encode（'/' 保留为分隔符）；".."/"." 段 → ERR_ARG。 */
int url_encode_rel(const std::string& rel, std::string& out);

/* 读 URL：命中缓存直接交付，否则抓取并按预算入缓存。未链接 libcurl → UNSUPPORTED。 */
int url_read(UrlCache& cache, const std::string& url, void** out, size_t* out_size);

/* ---- builtin:（builtin/backend.cpp）---- */

int builtin_read(const BlobTable& builtins, const std::string& logical, void** out, size_t* out_size);

} // namespace maque::vfs

#endif /* MAQUE_VFS_INTERNAL_H */
