// 二进制 codec + 快照池单元测试（docs/10 §4/§5/§8）
// 覆盖：逐位可逆往返（含动态数组/字符串/瞬态跳过）、快照池复用、克隆、校验和、损坏输入防御。
#include "core/persist/mq_persist_binary.h"
#include "core/reflect/mq_reflect.h"
#include "maque_types.h"

#include <print>
#include <vector>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    mq_reflect_ctx* C = mq_reflect_create();
    mq_reflect_load_schema(C);

    // ---- 1) 简单 struct（标量 + 字符串）往返 ----
    {
        const mq_type_desc* t = mq_reflect_type_by_name(C, "scn.Scene");
        CHECK(t != nullptr);
        scn::Scene a{};
        a.title = "往返测试"; a.script = "ball.js";
        a.width = 400; a.height = 300; a.seed = 12345u; a.views = 2; a.eye_sep = 0.5F;

        std::vector<uint8_t> buf(4096);
        size_t n = mq_persist_encode(C, t, &a, buf.data(), buf.size());
        CHECK(n > 0);

        scn::Scene b{};
        long used = mq_persist_decode(C, t, buf.data(), n, &b);
        CHECK(used == (long)n);
        CHECK(b.title == a.title && b.script == a.script);
        CHECK(b.width == a.width && b.height == a.height && b.seed == a.seed);
        CHECK(b.views == a.views && b.eye_sep == a.eye_sep);

        // 逐位可逆：再次编码必须与首次字节一致
        std::vector<uint8_t> buf2(4096);
        size_t n2 = mq_persist_encode(C, t, &b, buf2.data(), buf2.size());
        CHECK(n2 == n && std::memcmp(buf.data(), buf2.data(), n) == 0);
    }

    // ---- 2) 动态数组（T[]）往返 ----
    {
        const mq_type_desc* t = mq_reflect_type_by_name(C, "asset.AudioCurve");
        CHECK(t != nullptr);
        const mq_field_desc* pts = mq_reflect_field(t, "points");
        CHECK(pts && pts->container_resize && pts->container_size && pts->container_elem);

        asset::AudioCurve a{};
        pts->container_resize(&a.points, 3);
        for (uint32_t i = 0; i < 3; ++i) {
            math::Vec2* p = (math::Vec2*)pts->container_elem(&a.points, i);
            p->x = (float)i * 1.5F; p->y = (float)i * -2.0F;
        }
        CHECK(pts->container_size(&a.points) == 3u);

        std::vector<uint8_t> buf(8192);
        size_t n = mq_persist_encode(C, t, &a, buf.data(), buf.size());
        CHECK(n > 0);
        asset::AudioCurve b{};
        CHECK(mq_persist_decode(C, t, buf.data(), n, &b) == (long)n);
        CHECK(pts->container_size(&b.points) == 3u);
        const math::Vec2* p1 = (const math::Vec2*)pts->container_elem(&b.points, 1);
        CHECK(p1->x == 1.5F && p1->y == -2.0F);

        std::vector<uint8_t> buf2(8192);
        size_t n2 = mq_persist_encode(C, t, &b, buf2.data(), buf2.size());
        CHECK(n2 == n && std::memcmp(buf.data(), buf2.data(), n) == 0);
    }

    // ---- 2b) 瞬态字段不入快照（anim.Skeleton.poses 为 transient）----
    {
        const mq_type_desc* t = mq_reflect_type_by_name(C, "anim.Skeleton");
        const mq_field_desc* poses = mq_reflect_field(t, "poses");
        CHECK(poses != nullptr);
        CHECK((poses->attrs_flags & MQ_ATTR_TRANSIENT) != 0);
        anim::Skeleton a{};
        poses->container_resize(&a.poses, 2);
        std::vector<uint8_t> buf(8192);
        size_t n = mq_persist_encode(C, t, &a, buf.data(), buf.size());
        anim::Skeleton b{};
        CHECK(mq_persist_decode(C, t, buf.data(), n, &b) == (long)n);
        CHECK(poses->container_size(&b.poses) == 0u);      // 瞬态：不恢复
    }

    // ---- 3) 快照池：循环复用 / 帧号 / 描述 / 校验和 ----
    {
        mq_snapshot_pool* pool = mq_snapshot_pool_create(2, 1024);
        mq_snapshot* s0 = mq_snapshot_pool_acquire(pool);
        mq_snapshot* s1 = mq_snapshot_pool_acquire(pool);
        mq_snapshot* s2 = mq_snapshot_pool_acquire(pool);
        CHECK(s0 && s1 && s2);
        CHECK(s0 != s1 && s1 != s2);
        CHECK(s2 == s0);                        // 池满 → 循环复用最旧者

        void* w = mq_snapshot_begin(s0, 64);
        CHECK(w != nullptr);
        std::memcpy(w, "maque-snapshot", 14);
        mq_snapshot_commit(s0, 14);
        size_t sz = 0;
        const void* bytes = mq_snapshot_bytes(s0, &sz);
        CHECK(sz == 14 && bytes != nullptr);
        uint64_t sum = mq_snapshot_checksum(s0);
        CHECK(sum != 0);

        mq_snapshot_set_frame(s0, 42);
        CHECK(mq_snapshot_frame(s0) == 42u);
        mq_snapshot_set_desc(s0, "frame 42 before physics");
        CHECK(std::strcmp(mq_snapshot_desc(s0), "frame 42 before physics") == 0);

        mq_snapshot* cl = mq_snapshot_clone(s0);
        CHECK(mq_snapshot_checksum(cl) == sum);
        CHECK(mq_snapshot_frame(cl) == 42u);
        // 克隆独立：改原快照不影响克隆
        void* w2 = mq_snapshot_begin(s0, 64);
        std::memcpy(w2, "changed", 7);
        mq_snapshot_commit(s0, 7);
        CHECK(mq_snapshot_checksum(cl) == sum);

        mq_snapshot_destroy(cl);
        mq_snapshot_pool_destroy(pool);
    }

    // ---- 4) 损坏输入防御（长度不足 / 巨量数组声明）----
    {
        const mq_type_desc* t = mq_reflect_type_by_name(C, "scn.Scene");
        scn::Scene s{};
        CHECK(mq_persist_decode(C, t, "ab", 2, &s) < 0);          // 截断
        uint8_t junk[16] = { 0xFF, 0xFF, 0xFF, 0xFF };
        const mq_type_desc* ac = mq_reflect_type_by_name(C, "asset.AudioCurve");
        asset::AudioCurve acv{};
        CHECK(mq_persist_decode(C, ac, junk, sizeof junk, &acv) < 0);
    }

    // ---- 5) 反射 → JSON 文本（与 load_json 往返一致；瞬态字段不落盘）----
    {
        const mq_type_desc* t = mq_reflect_type_by_name(C, "scn.Scene");
        scn::Scene a{};
        a.title = "往返\"测试";            // 含需转义的引号
        a.script = "ball.js";
        a.width = 400; a.height = 300; a.seed = 42u; a.views = 2; a.eye_sep = 0.25F;
        std::vector<char> buf(4096);
        const size_t n = mq_persist_write_json(C, t, &a, buf.data(), buf.size());
        CHECK(n > 0 && n + 1 <= buf.size());
        CHECK(buf[0] == '{' && buf[n - 1] == '}');

        scn::Scene b{};
        CHECK(mq_persist_load_json(C, t, buf.data(), n, &b) == 0);
        CHECK(b.title == a.title && b.script == a.script);
        CHECK(b.width == a.width && b.height == a.height && b.seed == a.seed);

        // 决定论：二次写出字节一致
        std::vector<char> buf2(4096);
        const size_t n2 = mq_persist_write_json(C, t, &b, buf2.data(), buf2.size());
        CHECK(n2 == n && std::memcmp(buf.data(), buf2.data(), n) == 0);

        // 缓冲不足：返回所需长度且不写坏
        char small[4] = { 'x', 'x', 'x', 'x' };
        CHECK(mq_persist_write_json(C, t, &a, small, sizeof small) > sizeof small);
        CHECK(small[0] == 'x');
    }

    // ---- 5b) 字符串字段转义 + 动态数组（非瞬态）往返 ----
    {
        const mq_type_desc* t = mq_reflect_type_by_name(C, "asset.AudioCurve");
        const mq_field_desc* pts = mq_reflect_field(t, "points");
        CHECK(pts && pts->container_resize);
        asset::AudioCurve a{};
        pts->container_resize(&a.points, 2);
        ((math::Vec2*)pts->container_elem(&a.points, 0))->x = 1.5F;
        ((math::Vec2*)pts->container_elem(&a.points, 1))->y = -2.5F;
        std::vector<char> buf(4096);
        const size_t n = mq_persist_write_json(C, t, &a, buf.data(), buf.size());
        CHECK(n > 0);
        std::string text(buf.data(), n);
        CHECK(text.find("\"points\"") != std::string::npos);
        asset::AudioCurve b{};
        CHECK(mq_persist_load_json(C, t, buf.data(), n, &b) == 0);
        CHECK(pts->container_size(&b.points) == 2u);
        CHECK(((math::Vec2*)pts->container_elem(&b.points, 0))->x == 1.5F);
        CHECK(((math::Vec2*)pts->container_elem(&b.points, 1))->y == -2.5F);
    }

    mq_reflect_destroy(C);
    if (g_failures == 0) { std::print("snapshot_test: all checks passed\n"); return 0; }
    std::print("snapshot_test: {} failure(s)\n", g_failures);
    return 1;
}
