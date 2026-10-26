/* asset.cpp —— maque 资产管线实现（docs/08 §6、plans/08）。
 *
 * 确定性：全部遍历/输出按 source 路径字典序；不使用时间戳、指针地址、locale、随机数。
 *
 * 稳定 ID 公式（内容寻址 + 路径绑定；同内容同 ID、同路径+内容恒定）：
 *     h_path    = mq_vfs_hash(path 字节, len)                     // FNV-1a 64
 *     h_content = mq_vfs_hash(源内容字节, len)                     // FNV-1a 64
 *     mixed     = h_content ^ (h_path + 0x9E3779B97F4A7C15
 *                              + (h_content << 6) + (h_content >> 2))
 *     id        = splitmix64(mixed)；若为 0 则取 0x9E3779B97F4A7C15
 * 于是：同内容不同路径 → 不同 ID（路径参与混合）；同路径同内容 → 恒定 ID。
 *
 * IO：mq_vfs.h 只提供读接口（无 write），故 cook/pack 产物写本地文件系统
 * （docs/08 §2 的 backend file:// 形态）；读源内容一律经 VFS。 */
#include "asset/mq_asset.h"
#include "core/vfs/mq_vfs.h"
#include "core/tracy/mq_tracy.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <queue>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kCookFormatVersion = 1u;
constexpr uint32_t kPackFormatVersion = 1u;

struct Entry {
    uint64_t                 id = 0;
    mq_asset_kind            kind = MQ_ASSET_KIND_OTHER;
    std::string              source;
    std::string              cooked;
    uint64_t                 source_hash = 0;
    uint64_t                 cooked_hash = 0;
    bool                     is_cooked = false;
    std::vector<std::string> deps;   /* 已排序、已去重 */

    Entry() = default;
    Entry(const Entry&) = default;
    Entry& operator=(const Entry&) = default;
    Entry(Entry&&) noexcept = default;
    Entry& operator=(Entry&&) noexcept = default;
};

/* ---- 基本工具 ---- */

uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t hash_bytes(const void* data, size_t size) noexcept {
    return mq_vfs_hash(data ? data : "", size);
}

uint64_t compute_id(std::string_view path, uint64_t content_hash) noexcept {
    const uint64_t h_path = hash_bytes(path.data(), path.size());
    const uint64_t mixed = content_hash
                         ^ (h_path + 0x9E3779B97F4A7C15ULL
                            + (content_hash << 6) + (content_hash >> 2));
    const uint64_t id = mix64(mixed);
    return id ? id : 0x9E3779B97F4A7C15ULL;
}

bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

mq_asset_kind detect_kind(std::string_view path) noexcept {
    if (ends_with(path, ".mat.json") || ends_with(path, ".material.json")) return MQ_ASSET_KIND_MATERIAL;
    if (ends_with(path, ".png") || ends_with(path, ".jpg") || ends_with(path, ".jpeg")
        || ends_with(path, ".gif") || ends_with(path, ".bmp")) return MQ_ASSET_KIND_TEXTURE;
    if (ends_with(path, ".obj") || ends_with(path, ".mesh")) return MQ_ASSET_KIND_MESH;
    if (ends_with(path, ".ttf") || ends_with(path, ".otf")) return MQ_ASSET_KIND_FONT;
    if (ends_with(path, ".anim.json")) return MQ_ASSET_KIND_ANIMATION;
    if (ends_with(path, ".scn")) return MQ_ASSET_KIND_SCENE;
    if (ends_with(path, ".js")) return MQ_ASSET_KIND_SCRIPT;
    if (ends_with(path, ".wav") || ends_with(path, ".ogg") || ends_with(path, ".mp3")) {
        return MQ_ASSET_KIND_AUDIO;
    }
    return MQ_ASSET_KIND_OTHER;
}

const char* kind_name(mq_asset_kind kind) noexcept {
    switch (kind) {
        case MQ_ASSET_KIND_TEXTURE:   return "texture";
        case MQ_ASSET_KIND_MATERIAL:  return "material";
        case MQ_ASSET_KIND_MESH:      return "mesh";
        case MQ_ASSET_KIND_FONT:      return "font";
        case MQ_ASSET_KIND_ANIMATION: return "animation";
        case MQ_ASSET_KIND_SCENE:     return "scene";
        case MQ_ASSET_KIND_SCRIPT:    return "script";
        case MQ_ASSET_KIND_AUDIO:     return "audio";
        case MQ_ASSET_KIND_OTHER:
        default:                      return "other";
    }
}

void copy_cstr(char* dst, size_t cap, const std::string& src) noexcept {
    if (!dst || cap == 0) return;
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    if (n) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

int map_vfs_error(int vfs_rc) noexcept {
    switch (vfs_rc) {
        case MQ_VFS_OK:           return MQ_ASSET_OK;
        case MQ_VFS_ERR_ARG:      return MQ_ASSET_ERR_ARG;
        case MQ_VFS_ERR_NOTFOUND:
        case MQ_VFS_ERR_NO_MOUNT: return MQ_ASSET_ERR_NOT_FOUND;
        case MQ_VFS_ERR_NOMEM:    return MQ_ASSET_ERR_NO_MEM;
        case MQ_VFS_ERR_FORMAT:
        case MQ_VFS_ERR_HASH:     return MQ_ASSET_ERR_FORMAT;
        default:                  return MQ_ASSET_ERR_IO;
    }
}

/* ---- 小端序列化 ---- */

void push_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

void push_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

/* ---- JSON 转义（合法 JSON：转义 " \ 与控制字符 < 0x20） ---- */

constexpr char kHexDigits[] = "0123456789abcdef";

void json_escape(std::string& out, std::string_view s) {
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHexDigits[(c >> 4) & 0xF]);
                    out.push_back(kHexDigits[c & 0xF]);
                } else {
                    out.push_back(ch);
                }
        }
    }
}

void json_str(std::string& out, std::string_view s) {
    out.push_back('"');
    json_escape(out, s);
    out.push_back('"');
}

/* ---- 文件写出（本地文件系统 backend） ---- */

int write_local_file(const char* out_path, const std::string& bytes) {
    if (!out_path || !*out_path) return MQ_ASSET_ERR_ARG;
    const std::filesystem::path p(out_path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return MQ_ASSET_ERR_IO;
    if (!bytes.empty()) f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!f) return MQ_ASSET_ERR_IO;
    f.close();
    if (!f) return MQ_ASSET_ERR_IO;
    return MQ_ASSET_OK;
}

}  // namespace

/* 全部状态在实例内（ADR-0014：无全局可变状态；constexpr 常量不在此列）。 */
struct mq_asset_db {
    mq_vfs*            vfs = nullptr;   /* 借用，不拥有；NULL = 纯内存模式 */
    std::vector<Entry> entries;         /* 按 source 字典序（确定性遍历顺序） */
    std::string        dump_cache;      /* mq_asset_dump_json 结果，归 db */
    bool               dump_dirty = true;
};

namespace {

/* ---- 查询辅助（entries 恒按 source 排序） ---- */

std::vector<Entry>::iterator lower_entry(mq_asset_db* db, std::string_view path) {
    return std::lower_bound(db->entries.begin(), db->entries.end(), path,
                            [](const Entry& e, std::string_view k) { return e.source < k; });
}

const Entry* find_entry(const mq_asset_db* db, std::string_view path) {
    const auto it = std::lower_bound(db->entries.begin(), db->entries.end(), path,
                                     [](const Entry& e, std::string_view k) { return e.source < k; });
    if (it == db->entries.end() || it->source != path) return nullptr;
    return &*it;
}

const Entry* find_entry_by_id(const mq_asset_db* db, uint64_t id) noexcept {
    for (const Entry& e : db->entries)
        if (e.id == id) return &e;
    return nullptr;
}

bool index_of(const mq_asset_db* db, std::string_view path, uint32_t& out) noexcept {
    const auto it = std::lower_bound(db->entries.begin(), db->entries.end(), path,
                                     [](const Entry& e, std::string_view k) { return e.source < k; });
    if (it == db->entries.end() || it->source != path) return false;
    out = static_cast<uint32_t>(it - db->entries.begin());
    return true;
}

bool has_missing_dependency(const mq_asset_db* db, const Entry& e) noexcept {
    for (const std::string& d : e.deps)
        if (!find_entry(db, d)) return true;
    return false;
}

void fill_info(const mq_asset_db* db, const Entry& e, mq_asset_info* out) noexcept {
    std::memset(out, 0, sizeof(*out));
    out->id = e.id;
    out->kind = e.kind;
    copy_cstr(out->source, sizeof(out->source), e.source);
    copy_cstr(out->cooked, sizeof(out->cooked), e.cooked);
    out->source_hash = e.source_hash;
    out->cooked_hash = e.cooked_hash;
    out->dep_count = static_cast<uint32_t>(e.deps.size());
    out->flags = (e.is_cooked ? MQ_ASSET_FLAG_COOKED : 0u)
               | (has_missing_dependency(db, e) ? MQ_ASSET_FLAG_MISSING_DEP : 0u);
}

void insert_entry(mq_asset_db* db, Entry&& e) {
    const auto it = lower_entry(db, e.source);
    db->entries.insert(it, std::move(e));
    db->dump_dirty = true;
}

/* 经 VFS 读源内容；纯内存模式下内容为空。 */
int read_source(const mq_asset_db* db, const char* path, std::string& out) {
    out.clear();
    if (!db->vfs) return MQ_ASSET_OK;   /* 纯内存模式：视为空内容 */
    void* data = nullptr;
    size_t size = 0;
    const int rc = mq_vfs_read(db->vfs, path, &data, &size);
    if (rc != MQ_VFS_OK) {
        if (data) mq_vfs_free(data);
        return map_vfs_error(rc);
    }
    if (data && size) out.assign(static_cast<const char*>(data), size);
    if (data) mq_vfs_free(data);
    return MQ_ASSET_OK;
}

/* ---- 各入口的实现（异常由 guard 兜住，不跨 C ABI） ---- */

int import_impl(mq_asset_db* db, const char* path, mq_asset_info* out) {
    if (!db || !path || !*path || !out) return MQ_ASSET_ERR_ARG;
    if (find_entry(db, path)) return MQ_ASSET_ERR_DUPLICATE;

    std::string content;
    const int rc = read_source(db, path, content);
    if (rc != MQ_ASSET_OK) return rc;

    Entry e;
    e.kind = detect_kind(path);
    e.source = path;
    e.source_hash = hash_bytes(content.data(), content.size());
    e.id = compute_id(e.source, e.source_hash);
    insert_entry(db, std::move(e));

    const Entry* stored = find_entry(db, path);
    if (!stored) return MQ_ASSET_ERR_IO;   /* 不可达：insert 后必然存在 */
    fill_info(db, *stored, out);
    return MQ_ASSET_OK;
}

int find_impl(mq_asset_db* db, const char* path, mq_asset_info* out) {
    if (!db || !path || !out) return MQ_ASSET_ERR_ARG;
    const Entry* e = find_entry(db, path);
    if (!e) return MQ_ASSET_ERR_NOT_FOUND;
    fill_info(db, *e, out);
    return MQ_ASSET_OK;
}

int find_by_id_impl(mq_asset_db* db, uint64_t id, mq_asset_info* out) {
    if (!db || !out) return MQ_ASSET_ERR_ARG;
    const Entry* e = find_entry_by_id(db, id);
    if (!e) return MQ_ASSET_ERR_NOT_FOUND;
    fill_info(db, *e, out);
    return MQ_ASSET_OK;
}

int info_at_impl(mq_asset_db* db, uint32_t index, mq_asset_info* out) {
    if (!db || !out) return MQ_ASSET_ERR_ARG;
    if (index >= db->entries.size()) return MQ_ASSET_ERR_NOT_FOUND;
    fill_info(db, db->entries[index], out);
    return MQ_ASSET_OK;
}

int add_dependency_impl(mq_asset_db* db, const char* path, const char* dep_path) {
    if (!db || !path || !*path || !dep_path || !*dep_path) return MQ_ASSET_ERR_ARG;
    const auto it = lower_entry(db, path);
    if (it == db->entries.end() || it->source != path) return MQ_ASSET_ERR_NOT_FOUND;
    Entry& e = *it;
    const auto d = std::lower_bound(e.deps.begin(), e.deps.end(), std::string_view(dep_path));
    if (d != e.deps.end() && *d == dep_path) return MQ_ASSET_OK;   /* 幂等 */
    e.deps.insert(d, dep_path);
    db->dump_dirty = true;
    return MQ_ASSET_OK;
}

uint32_t dependency_count_impl(mq_asset_db* db, const char* path) {
    if (!db || !path) return 0;
    const Entry* e = find_entry(db, path);
    return e ? static_cast<uint32_t>(e->deps.size()) : 0u;
}

int dependency_at_impl(mq_asset_db* db, const char* path, uint32_t index, char* out, size_t out_cap) {
    if (!db || !path || !out || out_cap == 0) return MQ_ASSET_ERR_ARG;
    const Entry* e = find_entry(db, path);
    if (!e) return MQ_ASSET_ERR_NOT_FOUND;
    if (index >= e->deps.size()) return MQ_ASSET_ERR_NOT_FOUND;
    copy_cstr(out, out_cap, e->deps[index]);
    return MQ_ASSET_OK;
}

uint32_t toposort_impl(mq_asset_db* db, char (*out)[256], uint32_t out_cap) {
    if (!db || !out || out_cap == 0) return 0;
    const uint32_t n = static_cast<uint32_t>(db->entries.size());
    if (n == 0) return 0;

    std::vector<uint32_t> indeg(n, 0);
    std::vector<std::vector<uint32_t>> succ(n);
    for (uint32_t i = 0; i < n; ++i) {
        for (const std::string& dep : db->entries[i].deps) {
            uint32_t j = 0;
            if (!index_of(db, dep, j) || j == i) continue;   /* 缺失依赖/自环不计入度 */
            succ[j].push_back(i);
            ++indeg[i];
        }
    }

    /* 可用集合取路径字典序最小者 → 依赖先出、同层按路径排序，结果唯一。 */
    const auto cmp = [db](uint32_t a, uint32_t b) {
        return db->entries[a].source > db->entries[b].source;
    };
    std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(cmp)> ready(cmp);
    for (uint32_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.push(i);

    uint32_t written = 0;
    while (!ready.empty()) {
        const uint32_t i = ready.top();
        ready.pop();
        if (written < out_cap) {
            copy_cstr(out[written], 256, db->entries[i].source);
            ++written;
        }
        for (const uint32_t k : succ[i])
            if (--indeg[k] == 0) ready.push(k);
    }
    return written;
}

int validate_impl(mq_asset_db* db, char* missing_out, size_t missing_cap) {
    if (!db) return MQ_ASSET_ERR_ARG;
    if (missing_out && missing_cap) missing_out[0] = '\0';
    for (const Entry& e : db->entries) {          /* 已按路径排序 → 首条缺失确定 */
        for (const std::string& d : e.deps) {
            if (find_entry(db, d)) continue;
            if (missing_out && missing_cap) copy_cstr(missing_out, missing_cap, d);
            return MQ_ASSET_ERR_DEPENDENCY;
        }
    }
    return MQ_ASSET_OK;
}

int cook_impl(mq_asset_db* db, const char* path, const char* out_path) {
    if (!db || !path || !*path || !out_path || !*out_path) return MQ_ASSET_ERR_ARG;
    const auto it = lower_entry(db, path);
    if (it == db->entries.end() || it->source != path) return MQ_ASSET_ERR_NOT_FOUND;

    std::string payload;
    const int rc = read_source(db, path, payload);
    if (rc != MQ_ASSET_OK) return rc;

    std::string blob;
    blob.reserve(4 + 4 + 8 + 4 + 4 + payload.size());
    blob += "MQAK";
    push_u32(blob, kCookFormatVersion);
    push_u64(blob, it->source_hash);
    push_u32(blob, static_cast<uint32_t>(it->kind));
    push_u32(blob, static_cast<uint32_t>(payload.size()));
    blob += payload;

    const int wrc = write_local_file(out_path, blob);
    if (wrc != MQ_ASSET_OK) return wrc;

    it->cooked = out_path;
    it->cooked_hash = hash_bytes(blob.data(), blob.size());
    it->is_cooked = true;
    db->dump_dirty = true;
    return MQ_ASSET_OK;
}

int pack_impl(mq_asset_db* db, const char* const* paths, uint32_t count, const char* out_path) {
    if (!db || !out_path || !*out_path) return MQ_ASSET_ERR_ARG;
    if (count > 0 && !paths) return MQ_ASSET_ERR_ARG;

    /* 1) 收集闭包（含入口自身）；缺失依赖 → DEPENDENCY。 */
    std::vector<uint32_t> picked;
    for (uint32_t i = 0; i < count; ++i) {
        if (!paths[i] || !*paths[i]) return MQ_ASSET_ERR_ARG;
        const Entry* e = find_entry(db, paths[i]);
        if (!e) return MQ_ASSET_ERR_NOT_FOUND;
        const uint32_t idx = static_cast<uint32_t>(e - db->entries.data());
        if (std::find(picked.begin(), picked.end(), idx) == picked.end()) picked.push_back(idx);
    }
    std::vector<uint32_t> stack = picked;
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        for (const std::string& d : db->entries[i].deps) {
            const Entry* dep = find_entry(db, d);
            if (!dep) return MQ_ASSET_ERR_DEPENDENCY;
            const uint32_t j = static_cast<uint32_t>(dep - db->entries.data());
            if (std::find(picked.begin(), picked.end(), j) == picked.end()) {
                picked.push_back(j);
                stack.push_back(j);
            }
        }
    }

    /* 2) 按 source 路径字典序（确定性），读取内容。 */
    std::sort(picked.begin(), picked.end(), [db](uint32_t a, uint32_t b) {
        return db->entries[a].source < db->entries[b].source;
    });

    std::string blob;
    blob += "MQAP";
    push_u32(blob, kPackFormatVersion);
    push_u32(blob, static_cast<uint32_t>(picked.size()));
    for (const uint32_t i : picked) {
        const Entry& e = db->entries[i];
        std::string data;
        const int rc = read_source(db, e.source.c_str(), data);
        if (rc != MQ_ASSET_OK) return rc;
        push_u64(blob, e.id);
        push_u64(blob, hash_bytes(data.data(), data.size()));
        push_u32(blob, static_cast<uint32_t>(e.kind));
        push_u32(blob, static_cast<uint32_t>(e.source.size()));
        blob += e.source;
        push_u32(blob, static_cast<uint32_t>(data.size()));
        blob += data;
    }
    push_u64(blob, hash_bytes(blob.data(), blob.size()));   /* manifest_hash 覆盖之前全部字节 */

    return write_local_file(out_path, blob);
}

const char* dump_json_impl(mq_asset_db* db) {
    if (!db) return nullptr;
    if (!db->dump_dirty) return db->dump_cache.c_str();

    std::string out;
    out.reserve(256 + db->entries.size() * 192);
    out += "{\n";
    out += "  \"abi_version\": ";
    out += std::to_string(MQ_ASSET_ABI_VERSION);
    out += ",\n  \"count\": ";
    out += std::to_string(db->entries.size());
    out += ",\n  \"assets\": [";
    if (!db->entries.empty()) out += "\n";
    for (size_t i = 0; i < db->entries.size(); ++i) {
        const Entry& e = db->entries[i];
        out += "    {\n      \"id\": ";
        out += std::to_string(e.id);
        out += ",\n      \"kind\": ";
        json_str(out, kind_name(e.kind));
        out += ",\n      \"kind_id\": ";
        out += std::to_string(static_cast<uint32_t>(e.kind));
        out += ",\n      \"source\": ";
        json_str(out, e.source);
        out += ",\n      \"cooked\": ";
        json_str(out, e.cooked);
        out += ",\n      \"source_hash\": ";
        out += std::to_string(e.source_hash);
        out += ",\n      \"cooked_hash\": ";
        out += std::to_string(e.cooked_hash);
        out += ",\n      \"dep_count\": ";
        out += std::to_string(e.deps.size());
        out += ",\n      \"flags\": ";
        out += std::to_string((e.is_cooked ? MQ_ASSET_FLAG_COOKED : 0u)
                            | (has_missing_dependency(db, e) ? MQ_ASSET_FLAG_MISSING_DEP : 0u));
        out += ",\n      \"cooked_state\": ";
        out += (e.is_cooked ? "true" : "false");
        out += ",\n      \"deps\": [";
        for (size_t d = 0; d < e.deps.size(); ++d) {
            if (d) out += ", ";
            json_str(out, e.deps[d]);
        }
        out += "]\n    }";
        if (i + 1 < db->entries.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n}\n";

    db->dump_cache = std::move(out);
    db->dump_dirty = false;
    return db->dump_cache.c_str();
}

uint32_t count_kind_impl(mq_asset_db* db, mq_asset_kind kind) {
    if (!db) return 0;
    uint32_t n = 0;
    for (const Entry& e : db->entries)
        if (e.kind == kind) ++n;
    return n;
}

/* ---- 异常护栏：任何异常都不得跨越 C ABI（docs/02 R2） ---- */

template <class Fn>
int guard_int(Fn&& fn) noexcept {
    try { return fn(); }
    catch (const std::bad_alloc&) { return MQ_ASSET_ERR_NO_MEM; }
    catch (...) { return MQ_ASSET_ERR_IO; }
}

template <class Fn>
uint32_t guard_u32(Fn&& fn) noexcept {
    try { return fn(); }
    catch (...) { return 0; }
}

template <class Fn>
const char* guard_cstr(Fn&& fn) noexcept {
    try { return fn(); }
    catch (...) { return nullptr; }
}

}  // namespace

extern "C" {

mq_asset_db* mq_asset_create(mq_vfs* vfs) {
    try {
        mq_asset_db* db = new mq_asset_db();
        db->vfs = vfs;
        return db;
    } catch (...) {
        return nullptr;
    }
}

void mq_asset_destroy(mq_asset_db* db) {
    try { delete db; } catch (...) {}
}

int mq_asset_import(mq_asset_db* db, const char* path, mq_asset_info* out) {
    MQ_TRACY_ZONE_N(asset, "import");
    return guard_int([&] { return import_impl(db, path, out); });
}

int mq_asset_find(mq_asset_db* db, const char* path, mq_asset_info* out) {
    return guard_int([&] { return find_impl(db, path, out); });
}

int mq_asset_find_by_id(mq_asset_db* db, uint64_t id, mq_asset_info* out) {
    return guard_int([&] { return find_by_id_impl(db, id, out); });
}

uint32_t mq_asset_count(mq_asset_db* db) {
    return guard_u32([&] { return db ? static_cast<uint32_t>(db->entries.size()) : 0u; });
}

int mq_asset_info_at(mq_asset_db* db, uint32_t index, mq_asset_info* out) {
    return guard_int([&] { return info_at_impl(db, index, out); });
}

int mq_asset_add_dependency(mq_asset_db* db, const char* path, const char* dep_path) {
    return guard_int([&] { return add_dependency_impl(db, path, dep_path); });
}

uint32_t mq_asset_dependency_count(mq_asset_db* db, const char* path) {
    return guard_u32([&] { return dependency_count_impl(db, path); });
}

int mq_asset_dependency_at(mq_asset_db* db, const char* path, uint32_t index,
                           char* out, size_t out_cap) {
    return guard_int([&] { return dependency_at_impl(db, path, index, out, out_cap); });
}

uint32_t mq_asset_toposort(mq_asset_db* db, char (*out)[256], uint32_t out_cap) {
    return guard_u32([&] { return toposort_impl(db, out, out_cap); });
}

int mq_asset_validate(mq_asset_db* db, char* missing_out, size_t missing_cap) {
    return guard_int([&] { return validate_impl(db, missing_out, missing_cap); });
}

int mq_asset_cook(mq_asset_db* db, const char* path, const char* out_path) {
    MQ_TRACY_ZONE_N(asset, "cook");
    return guard_int([&] { return cook_impl(db, path, out_path); });
}

int mq_asset_pack(mq_asset_db* db, const char* const* paths, uint32_t count, const char* out_path) {
    MQ_TRACY_ZONE_N(asset, "pack");
    return guard_int([&] { return pack_impl(db, paths, count, out_path); });
}

const char* mq_asset_dump_json(mq_asset_db* db) {
    return guard_cstr([&] { return dump_json_impl(db); });
}

uint32_t mq_asset_count_kind(mq_asset_db* db, mq_asset_kind kind) {
    return guard_u32([&] { return count_kind_impl(db, kind); });
}

}  // extern "C"
