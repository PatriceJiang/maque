// asset 单元测试（docs/08 / plans/08）：导入 / 种类判定 / 稳定 ID / 依赖图 /
// cook / pack / dump + 确定性 + 错误路径。独立单测：仅依赖本模块 + mq_vfs（file:// 挂载）。
#include "asset/mq_asset.h"
#include "core/vfs/mq_vfs.h"

#include <print>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "<err>";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t rd_u32(const std::string& b, size_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(b[off + i])) << (8 * i);
    return v;
}

static uint64_t rd_u64(const std::string& b, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(b[off + i])) << (8 * i);
    return v;
}

/* 归档读回（本切片不开公共读接口；测试经普通文件 IO 验证往返）。 */
struct PackEntry { uint64_t id = 0, hash = 0; uint32_t kind = 0; std::string path, data; };
static bool parse_pack(const std::string& b, std::vector<PackEntry>& out, uint64_t& manifest) {
    out.clear();
    if (b.size() < 24u || b.compare(0, 4, "MQAP") != 0) return false;
    if (rd_u32(b, 4) != 1u) return false;
    const uint32_t n = rd_u32(b, 8);
    size_t off = 12;
    for (uint32_t i = 0; i < n; ++i) {
        if (off + 24u > b.size()) return false;
        PackEntry e;
        e.id = rd_u64(b, off); off += 8;
        e.hash = rd_u64(b, off); off += 8;
        e.kind = rd_u32(b, off); off += 4;
        const uint32_t plen = rd_u32(b, off); off += 4;
        if (off + plen + 4u > b.size()) return false;
        e.path.assign(b, off, plen); off += plen;
        const uint32_t dlen = rd_u32(b, off); off += 4;
        if (off + dlen > b.size()) return false;
        e.data.assign(b, off, dlen); off += dlen;
        out.push_back(std::move(e));
    }
    if (off + 8u != b.size()) return false;
    manifest = rd_u64(b, off);
    return true;
}


/* ---- ABI 守卫（docs/02 R7：跨模块 ABI 冻结；DSL 生成头若漂移则编译失败）---- */
static_assert(sizeof(mq_asset_info) == 552, "mq_asset_info ABI 变更");
static_assert(alignof(mq_asset_info) == 8, "mq_asset_info 对齐变更");
static_assert(offsetof(mq_asset_info, id) == 0, "字段偏移变更");
static_assert(offsetof(mq_asset_info, kind) == 8, "字段偏移变更");
static_assert(offsetof(mq_asset_info, source) == 12, "字段偏移变更");
static_assert(offsetof(mq_asset_info, cooked) == 268, "字段偏移变更");
static_assert(offsetof(mq_asset_info, source_hash) == 528, "字段偏移变更");
static_assert(offsetof(mq_asset_info, cooked_hash) == 536, "字段偏移变更");
static_assert(offsetof(mq_asset_info, dep_count) == 544, "字段偏移变更");
static_assert(offsetof(mq_asset_info, flags) == 548, "字段偏移变更");
static_assert(MQ_ASSET_ABI_VERSION == 1u, "ABI 版本变更");

int main() {
    const fs::path root = fs::temp_directory_path() / "maque_asset_test";
    fs::remove_all(root);

    const std::string hero = "PNG-hero-0123456789";
    const std::string wall = "{\"roughness\":0.5}";
    put(root / "hero.png", hero);
    put(root / "twin.png", hero);                 // 同内容、不同路径
    put(root / "wall.mat.json", wall);
    put(root / "rock.obj", "v 0 0 0\n");
    put(root / "title.ttf", "TTF-bytes");
    put(root / "walk.anim.json", "{\"fps\":30}");
    put(root / "level.scn", "{scene:level}");
    put(root / "main.js", "log('hi')");
    put(root / "data.bin", "opaque");
    put(root / "empty.dat", "");                  // 空文件
    put(root / "garbage.dat", std::string("\xFF\xFE\x00\x01\x80", 5));   // 非 UTF8
    put(root / "q\"uote.scn", "{q}");             // JSON 转义用
    put(root / "back\\slash.scn", "{b}");
    for (const char* n : { "a", "b", "c", "d" }) put(root / "graph" / (std::string(n) + ".scn"), "{g}");

    mq_vfs* vfs = mq_vfs_create();
    CHECK(vfs != nullptr);
    CHECK(mq_vfs_mount(vfs, "/", ("file://" + root.string()).c_str()) == MQ_VFS_OK);

    mq_asset_info info{};
    char dep[256] = { 0 };
    char missing[256] = { 0 };
    char order[8][256] = { { 0 } };
    char order2[8][256] = { { 0 } };

    mq_asset_db* db = mq_asset_create(vfs);
    CHECK(db != nullptr);

    // ---- 1. import + find + count；重复 / 缺失 ----
    CHECK(mq_asset_import(db, "/hero.png", &info) == MQ_ASSET_OK);
    CHECK(info.id != 0);
    CHECK(info.kind == MQ_ASSET_KIND_TEXTURE);
    CHECK(std::strcmp(info.source, "/hero.png") == 0);
    CHECK(info.cooked[0] == '\0');
    CHECK(info.source_hash == mq_vfs_hash(hero.data(), hero.size()));
    CHECK(info.cooked_hash == 0);
    CHECK(info.dep_count == 0);
    CHECK((info.flags & MQ_ASSET_FLAG_COOKED) == 0);
    CHECK(mq_asset_count(db) == 1);
    const uint64_t hero_id = info.id;

    CHECK(mq_asset_import(db, "/hero.png", &info) == MQ_ASSET_ERR_DUPLICATE);
    CHECK(mq_asset_count(db) == 1);
    CHECK(mq_asset_import(db, "/nope.png", &info) == MQ_ASSET_ERR_NOT_FOUND);
    CHECK(mq_asset_count(db) == 1);
    CHECK(mq_asset_find(db, "/hero.png", &info) == MQ_ASSET_OK);
    CHECK(info.id == hero_id);
    CHECK(mq_asset_find(db, "/nope.png", &info) == MQ_ASSET_ERR_NOT_FOUND);
    CHECK(mq_asset_find_by_id(db, hero_id, &info) == MQ_ASSET_OK);
    CHECK(std::strcmp(info.source, "/hero.png") == 0);
    CHECK(mq_asset_find_by_id(db, 0xDEADBEEFULL, &info) == MQ_ASSET_ERR_NOT_FOUND);
    CHECK(mq_asset_info_at(db, 0, &info) == MQ_ASSET_OK);
    CHECK(std::strcmp(info.source, "/hero.png") == 0);
    CHECK(mq_asset_info_at(db, 1, &info) == MQ_ASSET_ERR_NOT_FOUND);

    // ---- 2. 扩展名 → kind ----
    CHECK(mq_asset_import(db, "/wall.mat.json", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_MATERIAL);
    CHECK(mq_asset_import(db, "/rock.obj", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_MESH);
    CHECK(mq_asset_import(db, "/title.ttf", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_FONT);
    CHECK(mq_asset_import(db, "/walk.anim.json", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_ANIMATION);
    CHECK(mq_asset_import(db, "/level.scn", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_SCENE);
    CHECK(mq_asset_import(db, "/main.js", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_SCRIPT);
    CHECK(mq_asset_import(db, "/data.bin", &info) == MQ_ASSET_OK);
    CHECK(info.kind == MQ_ASSET_KIND_OTHER);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_TEXTURE) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_MATERIAL) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_MESH) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_FONT) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_ANIMATION) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_SCENE) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_SCRIPT) == 1);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_OTHER) == 1);
    CHECK(mq_asset_count(db) == 8);

    // info_at 顺序 = source 路径字典序（确定性）
    {
        static const char* const kSorted[] = { "/data.bin", "/hero.png", "/level.scn", "/main.js",
                                               "/rock.obj", "/title.ttf", "/walk.anim.json",
                                               "/wall.mat.json" };
        for (uint32_t i = 0; i < 8; ++i) {
            CHECK(mq_asset_info_at(db, i, &info) == MQ_ASSET_OK);
            CHECK(std::strcmp(info.source, kSorted[i]) == 0);
        }
    }

    // ---- 3. 内容寻址 ID：同内容不同路径 → 不同 ID；新建 db 重导入 → 同 ID ----
    CHECK(mq_asset_import(db, "/twin.png", &info) == MQ_ASSET_OK);
    CHECK(info.source_hash == mq_vfs_hash(hero.data(), hero.size()));
    CHECK(info.id != hero_id);

    mq_asset_db* db2 = mq_asset_create(vfs);
    CHECK(db2 != nullptr);
    CHECK(mq_asset_import(db2, "/hero.png", &info) == MQ_ASSET_OK);
    CHECK(info.id == hero_id);                       // 同路径 + 同内容 → 恒定 ID

    // ---- 4. 依赖图（独立 db，仅 4 个场景） ----
    mq_asset_db* g = mq_asset_create(vfs);
    CHECK(g != nullptr);
    for (const char* n : { "a", "b", "c", "d" }) {
        const std::string p = std::string("/graph/") + n + ".scn";
        CHECK(mq_asset_import(g, p.c_str(), &info) == MQ_ASSET_OK);
        CHECK(info.kind == MQ_ASSET_KIND_SCENE);
    }
    CHECK(mq_asset_count(g) == 4);
    CHECK(mq_asset_add_dependency(g, "/graph/c.scn", "/graph/b.scn") == MQ_ASSET_OK);
    CHECK(mq_asset_add_dependency(g, "/graph/c.scn", "/graph/a.scn") == MQ_ASSET_OK);
    CHECK(mq_asset_add_dependency(g, "/graph/c.scn", "/graph/a.scn") == MQ_ASSET_OK);   // 幂等
    CHECK(mq_asset_add_dependency(g, "/graph/d.scn", "/graph/c.scn") == MQ_ASSET_OK);
    CHECK(mq_asset_dependency_count(g, "/graph/c.scn") == 2);
    CHECK(mq_asset_dependency_count(g, "/graph/d.scn") == 1);
    CHECK(mq_asset_dependency_count(g, "/graph/a.scn") == 0);
    CHECK(mq_asset_dependency_count(g, "/graph/zzz.scn") == 0);
    CHECK(mq_asset_dependency_count(g, nullptr) == 0);
    CHECK(mq_asset_dependency_at(g, "/graph/c.scn", 0, dep, sizeof dep) == MQ_ASSET_OK);
    CHECK(std::strcmp(dep, "/graph/a.scn") == 0);
    CHECK(mq_asset_dependency_at(g, "/graph/c.scn", 1, dep, sizeof dep) == MQ_ASSET_OK);
    CHECK(std::strcmp(dep, "/graph/b.scn") == 0);
    CHECK(mq_asset_dependency_at(g, "/graph/c.scn", 2, dep, sizeof dep) == MQ_ASSET_ERR_NOT_FOUND);
    CHECK(mq_asset_dependency_at(g, "/graph/a.scn", 0, dep, sizeof dep) == MQ_ASSET_ERR_NOT_FOUND);
    CHECK(mq_asset_add_dependency(g, "/graph/zzz.scn", "/graph/a.scn") == MQ_ASSET_ERR_NOT_FOUND);

    // 拓扑序：被依赖者在前；同层按路径字典序；两次调用一致
    CHECK(mq_asset_validate(g, missing, sizeof missing) == MQ_ASSET_OK);
    CHECK(mq_asset_toposort(g, order, 8) == 4);
    CHECK(std::strcmp(order[0], "/graph/a.scn") == 0);
    CHECK(std::strcmp(order[1], "/graph/b.scn") == 0);
    CHECK(std::strcmp(order[2], "/graph/c.scn") == 0);
    CHECK(std::strcmp(order[3], "/graph/d.scn") == 0);
    CHECK(mq_asset_toposort(g, order2, 8) == 4);
    CHECK(std::memcmp(order, order2, sizeof order) == 0);
    CHECK(mq_asset_toposort(g, order2, 2) == 2);      // 容量不足：只写前 cap 项
    CHECK(std::strcmp(order2[0], "/graph/a.scn") == 0);

    // 缺失依赖：validate 报 DEPENDENCY + 首条缺失路径；flags bit1 置位
    CHECK(mq_asset_add_dependency(g, "/graph/d.scn", "/graph/missing.scn") == MQ_ASSET_OK);
    std::memset(missing, 0, sizeof missing);
    CHECK(mq_asset_validate(g, missing, sizeof missing) == MQ_ASSET_ERR_DEPENDENCY);
    CHECK(std::strcmp(missing, "/graph/missing.scn") == 0);
    CHECK(mq_asset_find(g, "/graph/d.scn", &info) == MQ_ASSET_OK);
    CHECK((info.flags & MQ_ASSET_FLAG_MISSING_DEP) != 0);
    CHECK(mq_asset_find(g, "/graph/a.scn", &info) == MQ_ASSET_OK);
    CHECK((info.flags & MQ_ASSET_FLAG_MISSING_DEP) == 0);
    CHECK(mq_asset_dependency_count(g, "/graph/d.scn") == 2);

    // 环：环上节点不输出（不挂起、数量确定），环外节点仍按序输出
    CHECK(mq_asset_add_dependency(g, "/graph/a.scn", "/graph/d.scn") == MQ_ASSET_OK);
    std::memset(order2, 0, sizeof order2);
    CHECK(mq_asset_toposort(g, order2, 8) == 1);
    CHECK(std::strcmp(order2[0], "/graph/b.scn") == 0);

    // ---- 5. cook 往返：MQAK 头 + 源哈希 + 两次逐字节一致 ----
    const fs::path c1 = root / "out" / "hero.mqak";
    CHECK(mq_asset_cook(db, "/hero.png", c1.string().c_str()) == MQ_ASSET_OK);
    const std::string blob = read_file(c1);
    CHECK(blob.size() == 24u + hero.size());
    CHECK(blob.compare(0, 4, "MQAK") == 0);
    CHECK(rd_u32(blob, 4) == 1u);
    CHECK(rd_u64(blob, 8) == mq_vfs_hash(hero.data(), hero.size()));
    CHECK(rd_u32(blob, 16) == static_cast<uint32_t>(MQ_ASSET_KIND_TEXTURE));
    CHECK(rd_u32(blob, 20) == hero.size());
    CHECK(blob.compare(24, hero.size(), hero) == 0);
    CHECK(mq_asset_find(db, "/hero.png", &info) == MQ_ASSET_OK);
    CHECK((info.flags & MQ_ASSET_FLAG_COOKED) != 0);
    CHECK(std::strcmp(info.cooked, c1.string().c_str()) == 0);
    CHECK(info.cooked_hash == mq_vfs_hash(blob.data(), blob.size()));
    const fs::path c2 = root / "out" / "nested" / "hero-again.mqak";   // 目录自动创建
    CHECK(mq_asset_cook(db, "/hero.png", c2.string().c_str()) == MQ_ASSET_OK);
    CHECK(read_file(c2) == blob);
    CHECK(mq_asset_cook(db, "/nope.png", c1.string().c_str()) == MQ_ASSET_ERR_NOT_FOUND);
    put(root / "blocker", "x");                       // 父路径是普通文件 → 写出必然失败
    CHECK(mq_asset_cook(db, "/hero.png", (root / "blocker" / "x.mqak").string().c_str())
          == MQ_ASSET_ERR_IO);

    // ---- 6. pack 往返：2 个资源（含依赖闭包）+ 同一输入逐字节一致 ----
    CHECK(mq_asset_add_dependency(db, "/wall.mat.json", "/hero.png") == MQ_ASSET_OK);
    CHECK(mq_asset_add_dependency(db, "/level.scn", "/wall.mat.json") == MQ_ASSET_OK);
    CHECK(mq_asset_add_dependency(db, "/level.scn", "/hero.png") == MQ_ASSET_OK);
    CHECK(mq_asset_validate(db, missing, sizeof missing) == MQ_ASSET_OK);

    const char* roots1[1] = { "/wall.mat.json" };
    const fs::path p1 = root / "out" / "wall.mqap";
    CHECK(mq_asset_pack(db, roots1, 1, p1.string().c_str()) == MQ_ASSET_OK);
    const std::string pak = read_file(p1);
    CHECK(pak.compare(0, 4, "MQAP") == 0);
    CHECK(rd_u32(pak, 4) == 1u);
    CHECK(rd_u32(pak, 8) == 2u);                      // 闭包 = hero.png + wall.mat.json
    std::vector<PackEntry> entries;
    uint64_t manifest = 0;
    CHECK(parse_pack(pak, entries, manifest));
    CHECK(entries.size() == 2);
    CHECK(manifest == mq_vfs_hash(pak.data(), pak.size() - 8));
    CHECK(entries[0].path == "/hero.png");            // 按路径字典序
    CHECK(entries[0].id == hero_id);
    CHECK(entries[0].hash == mq_vfs_hash(hero.data(), hero.size()));
    CHECK(entries[0].kind == static_cast<uint32_t>(MQ_ASSET_KIND_TEXTURE));
    CHECK(entries[0].data == hero);
    CHECK(entries[1].path == "/wall.mat.json");
    CHECK(entries[1].hash == mq_vfs_hash(wall.data(), wall.size()));
    CHECK(entries[1].kind == static_cast<uint32_t>(MQ_ASSET_KIND_MATERIAL));
    CHECK(entries[1].data == wall);

    const fs::path p2 = root / "out" / "wall-again.mqap";
    CHECK(mq_asset_pack(db, roots1, 1, p2.string().c_str()) == MQ_ASSET_OK);
    CHECK(read_file(p2) == pak);                      // 确定性
    CHECK(mq_asset_pack(g, roots1, 1, p2.string().c_str()) == MQ_ASSET_ERR_NOT_FOUND);
    const char* missing_dep_roots[1] = { "/graph/d.scn" };
    CHECK(mq_asset_pack(g, missing_dep_roots, 1, p2.string().c_str()) == MQ_ASSET_ERR_DEPENDENCY);
    const char* none[1] = { nullptr };
    CHECK(mq_asset_pack(db, none, 1, p2.string().c_str()) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_pack(db, roots1, 0, (root / "out" / "empty.mqap").string().c_str()) == MQ_ASSET_OK);
    const std::string empty_pak = read_file(root / "out" / "empty.mqap");
    CHECK(empty_pak.size() == 20u);
    CHECK(empty_pak.compare(0, 4, "MQAP") == 0);
    CHECK(rd_u32(empty_pak, 8) == 0u);

    // ---- 7. dump_json：合法 JSON、含路径、字节幂等 ----
    const char* js = mq_asset_dump_json(db);
    CHECK(js != nullptr);
    const std::string j1 = js ? js : "";
    CHECK(!j1.empty() && j1.front() == '{');
    CHECK(j1.find("\n}\n") != std::string::npos);
    CHECK(j1.find("\"count\": 9") != std::string::npos);
    CHECK(j1.find("/hero.png") != std::string::npos);
    CHECK(j1.find("/wall.mat.json") != std::string::npos);
    CHECK(j1.find("/level.scn") != std::string::npos);
    CHECK(j1.find("\"deps\"") != std::string::npos);
    CHECK(j1.find("\"cooked_state\": true") != std::string::npos);
    CHECK(j1.find(",}") == std::string::npos);
    CHECK(j1.find(",]") == std::string::npos);
    CHECK(j1.find(": ,") == std::string::npos);
    CHECK(j1.find("null") == std::string::npos);
    CHECK(mq_asset_dump_json(db) == js);
    CHECK(j1 == std::string(mq_asset_dump_json(db)));

    // 路径中的 " 与 \ 必须被转义
    {
        mq_asset_db* esc = mq_asset_create(vfs);
        CHECK(esc != nullptr);
        CHECK(mq_asset_import(esc, "/q\"uote.scn", &info) == MQ_ASSET_OK);
        CHECK(mq_asset_import(esc, "/back\\slash.scn", &info) == MQ_ASSET_OK);
        const std::string ejs = mq_asset_dump_json(esc) ? mq_asset_dump_json(esc) : "";
        CHECK(ejs.find("/q\\\"uote.scn") != std::string::npos);
        CHECK(ejs.find("/back\\\\slash.scn") != std::string::npos);
        mq_asset_destroy(esc);
    }

    // ---- 8. 错误路径：NULL 参数 + 空/非 UTF8 文件 + 纯内存模式 ----
    CHECK(mq_asset_import(nullptr, "/hero.png", &info) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_import(db, nullptr, &info) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_import(db, "", &info) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_import(db, "/hero.png", nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_find(nullptr, "/hero.png", &info) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_find(db, nullptr, &info) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_find(db, "/hero.png", nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_find_by_id(nullptr, 1, &info) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_find_by_id(db, 1, nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_info_at(db, 0, nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_count(nullptr) == 0);
    CHECK(mq_asset_count_kind(nullptr, MQ_ASSET_KIND_TEXTURE) == 0);
    CHECK(mq_asset_add_dependency(nullptr, "/a", "/b") == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_add_dependency(db, "/hero.png", nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_dependency_at(db, "/hero.png", 0, nullptr, 0) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_dependency_at(db, "/hero.png", 0, dep, 0) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_toposort(nullptr, order, 8) == 0);
    CHECK(mq_asset_toposort(db, nullptr, 0) == 0);
    CHECK(mq_asset_validate(nullptr, missing, sizeof missing) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_validate(g, nullptr, 0) == MQ_ASSET_ERR_DEPENDENCY);   // out 可选
    CHECK(mq_asset_cook(nullptr, "/hero.png", "/tmp/x.mqak") == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_cook(db, nullptr, "/tmp/x.mqak") == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_cook(db, "/hero.png", nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_pack(nullptr, roots1, 1, "/tmp/x.mqap") == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_pack(db, nullptr, 1, "/tmp/x.mqap") == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_pack(db, roots1, 1, nullptr) == MQ_ASSET_ERR_ARG);
    CHECK(mq_asset_dump_json(nullptr) == nullptr);

    // 空文件 / 非 UTF8：不崩溃，按扩展名判定，空内容哈希 = FNV-1a("")
    CHECK(mq_asset_import(db, "/empty.dat", &info) == MQ_ASSET_OK);
    CHECK(info.source_hash == mq_vfs_hash("", 0));
    CHECK(info.kind == MQ_ASSET_KIND_OTHER);
    CHECK(mq_asset_import(db, "/garbage.dat", &info) == MQ_ASSET_OK);
    CHECK(info.source_hash == mq_vfs_hash(std::string("\xFF\xFE\x00\x01\x80", 5).data(), 5));
    CHECK(mq_asset_import(db, "/garbage.dat", &info) == MQ_ASSET_ERR_DUPLICATE);
    CHECK(mq_asset_dump_json(db) != nullptr);
    CHECK(mq_asset_count_kind(db, MQ_ASSET_KIND_OTHER) == 3);

    // vfs == NULL：纯内存模式（无 IO 后端，源内容视为空）
    {
        mq_asset_db* mem = mq_asset_create(nullptr);
        CHECK(mem != nullptr);
        CHECK(mq_asset_count(mem) == 0);
        CHECK(mq_asset_import(mem, "/virtual/mem.scn", &info) == MQ_ASSET_OK);
        CHECK(info.kind == MQ_ASSET_KIND_SCENE);
        CHECK(info.source_hash == mq_vfs_hash("", 0));
        CHECK(mq_asset_import(mem, "/virtual/dep.mat.json", &info) == MQ_ASSET_OK);
        CHECK(mq_asset_add_dependency(mem, "/virtual/mem.scn", "/virtual/dep.mat.json") == MQ_ASSET_OK);
        CHECK(mq_asset_validate(mem, missing, sizeof missing) == MQ_ASSET_OK);
        CHECK(mq_asset_toposort(mem, order, 8) == 2);
        CHECK(std::strcmp(order[0], "/virtual/dep.mat.json") == 0);
        CHECK(std::strcmp(order[1], "/virtual/mem.scn") == 0);
        CHECK(mq_asset_dump_json(mem) != nullptr);
        mq_asset_destroy(mem);
    }

    mq_asset_destroy(nullptr);                        // 幂等
    mq_asset_destroy(db2);
    mq_asset_destroy(g);
    mq_asset_destroy(db);
    mq_vfs_destroy(vfs);
    fs::remove_all(root);

    if (g_failures == 0) { std::print("asset_test: all checks passed\n"); return 0; }
    std::print("asset_test: {} failure(s)\n", g_failures);
    return 1;
}
