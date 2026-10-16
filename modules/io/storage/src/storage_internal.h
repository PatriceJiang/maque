/* 存储模块内部接口（**模块内 C++**；跨模块只走 C ABI —— abi/storage.h，docs/02/21）。
 *
 * 模式（spec 的 scheme）→ 一个后端目录（与 VFS 同风格）：
 *   file/backend.cpp   file://      本地目录（默认 <场景目录>/.maque；**原子写** tmp+rename）
 *   mem/backend.cpp    mem://       进程内（默认模式；不落盘）
 *   http/backend.cpp   http(s)://   REST：GET/PUT/DELETE <base>/<ns>/<key> + GET <base>/<ns>?keys=1
 *   ws/backend.cpp     ws(s)://     文本帧 JSON：{op:get|put|del|keys, ns, key, value}（libwebsockets）
 *
 * 固定协议（键空间 <uid>/<session>/<key>，四种传输语义一致）：
 *   get    → 值（缺失 → MQ_STORAGE_ERR_NOTFOUND）
 *   put    → 写入（file 原子；http PUT；ws 帧）
 *   remove → 删除（缺失 → NOTFOUND）
 *   keys   → 该命名空间的键列表（JSON 数组字符串）
 * 后端**不持有实例状态**（状态由核心传入）→ 可单独替换/测试。 */
#ifndef MAQUE_STORAGE_INTERNAL_H
#define MAQUE_STORAGE_INTERNAL_H

#include "core/storage/mq_storage.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace maque::storage {

using Blob = std::vector<uint8_t>;

enum class Op { Get, Put, Remove, Keys };

struct Request {
    Op          op = Op::Get;
    std::string ns;              // "<uid>/<session>"
    std::string key;             // Op::Keys 时忽略
    const void* data = nullptr;  // Op::Put 的值
    size_t      size = 0;
};

struct Response {
    Blob        value;           // Op::Get 的内容
    std::string json;            // Op::Keys 的 JSON 数组
};

/* ---- 共享工具（util.cpp）---- */

/* 段校验（uid/session/键的每个 '/' 分段）：非空、非 "."/".."、无 '\'、无控制字符。 */
bool segment_is_valid(const std::string& seg);

/* 键校验：非空、≤1024 字节、非绝对、分段合法（允许 '/' 形成子目录）。 */
bool key_is_valid(const std::string& key);

/* 命名空间校验：uid 与 session 各是一个合法段。 */
bool ns_is_valid(const std::string& uid, const std::string& session);

std::string ns_of(const std::string& uid, const std::string& session);   // "<uid>/<session>"

/* 键 → 相对路径（"<ns>/<key>"，段原样；供 file 后端，'\0' 安全由调用方保证）。 */
std::string rel_path(const std::string& ns, const std::string& key);

/* 键 → URL 路径片段（逐段 percent-encode，'/' 保留；供 http/ws 后端）。 */
std::string url_path(const std::string& ns, const std::string& key);

/* JSON 辅助：本模块自产自销的**固定**帧，不做通用解析。
 *   keys_json     → 键数组（["a","b/c"]）
 *   json_escape   → 字符串内容转义（不含引号）
 *   json_get_str  → 取顶层字符串字段（含 \uXXXX → UTF-8） */
std::string keys_json(const std::vector<std::string>& keys);
std::string json_escape(const std::string& s);
bool json_get_str(const std::string& json, const char* field, std::string& out);
bool json_get_str_array(const std::string& json, const char* field, std::vector<std::string>& out);

/* base64（ws 帧传二进制值；http/file 直接传字节）+ UTF-8 校验（决定用 utf8 还是 b64 编码）。 */
std::string base64_encode(const uint8_t* data, size_t size);
bool base64_decode(const std::string& in, Blob& out);
bool utf8_ok(const uint8_t* data, size_t size);

/* ---- 后端（每协议一个目录；返回 MQ_STORAGE_ERR_*）---- */

struct MemStore { std::unordered_map<std::string, Blob> items; };   // 键 = "<ns>/<key>"

/* ws 连接目标（解析自 spec） */
struct WsTarget {
    std::string host;      // 主机名（握手的 Host 头用；IPv6 不带方括号）
    std::string path;      // 请求路径（/...）
    int         port = 80;
    bool        tls = false;
};

int mem_apply(MemStore& store, const Request& req, Response& out);
int file_apply(const std::string& dir, const Request& req, Response& out);
int http_apply(const std::string& base_url, const Request& req, Response& out);
int ws_apply(const WsTarget& target, const Request& req, Response& out);

} // namespace maque::storage

#endif /* MAQUE_STORAGE_INTERNAL_H */
