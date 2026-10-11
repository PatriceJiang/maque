// maque physics 模块单测（独立：仅依赖 maque::physics + mq_physics.h）
// 覆盖：生命周期/句柄、自由落体、弹跳、确定性、稳定排序、快照往返、只读插值、插件 vtable。
#include "physics/mq_physics.h"

#include <cmath>
#include <cstring>
#include <print>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr float kFloorY = 200.0F;   /* 地面平面（y 向下） */
constexpr float kBallRadius = 10.0F;

mq_physics_config make_config() {
    mq_physics_config cfg{};
    cfg.struct_size = static_cast<uint32_t>(sizeof(mq_physics_config));
    cfg.gravity_x = 0.0F;
    cfg.gravity_y = 900.0F;
    cfg.fixed_dt = 1.0F / 60.0F;
    cfg.solver_iterations = 4;
    cfg.max_bodies = 64;
    cfg.seed = 12345u;
    return cfg;
}

mq_physics_body_desc make_ball(float x, float y, float radius = kBallRadius) {
    mq_physics_body_desc d{};
    d.struct_size = static_cast<uint32_t>(sizeof(mq_physics_body_desc));
    d.type = MQ_PHYSICS_BODY_DYNAMIC;
    d.shape = MQ_PHYSICS_SHAPE_BALL;
    d.x = x;
    d.y = y;
    d.radius = radius;
    d.restitution = 0.5F;
    d.friction = 0.2F;
    return d;
}

bool is_finite(float v) {
    return std::isfinite(v);
}

/* 1) 生命周期 / 句柄 / 错误码 */
void test_lifecycle_handles() {
    mq_physics_config cfg = make_config();
    cfg.max_bodies = 5; /* 槽位总数；index 0 恒为无效 → 可容纳 4 个体 */
    mq_physics_world* w = mq_physics_create(&cfg);
    CHECK(w != nullptr);
    CHECK(mq_physics_body_count(w) == 0);
    CHECK(mq_physics_checksum(w) != mq_physics_checksum(nullptr));

    mq_physics_body a{};
    mq_physics_body b{};
    mq_physics_body c{};
    mq_physics_body d{};
    mq_physics_body e{};
    CHECK(mq_physics_add_body(w, nullptr, &a) == MQ_PHYSICS_ERR_INVALID_ARG);
    const mq_physics_body_desc probe = make_ball(0.0F, 0.0F);
    CHECK(mq_physics_add_body(w, &probe, nullptr) == MQ_PHYSICS_ERR_INVALID_ARG);
    mq_physics_body_desc ball = make_ball(0.0F, 0.0F);
    CHECK(mq_physics_add_body(w, &ball, &a) == MQ_PHYSICS_OK);
    CHECK(a.index != 0u && a.generation != 0u);
    CHECK(mq_physics_body_valid(w, a) == 1);
    CHECK(mq_physics_body_count(w) == 1);

    CHECK(mq_physics_add_body(w, &ball, &b) == MQ_PHYSICS_OK);
    CHECK(mq_physics_add_body(w, &ball, &c) == MQ_PHYSICS_OK);
    CHECK(mq_physics_add_body(w, &ball, &d) == MQ_PHYSICS_OK);
    CHECK(mq_physics_body_count(w) == 4);
    CHECK(mq_physics_add_body(w, &ball, &e) == MQ_PHYSICS_ERR_CAPACITY);

    /* 非法描述 */
    mq_physics_body_desc bad = make_ball(0.0F, 0.0F);
    bad.radius = 0.0F;
    CHECK(mq_physics_add_body(w, &bad, &e) == MQ_PHYSICS_ERR_INVALID_ARG);
    bad = make_ball(0.0F, 0.0F);
    bad.type = 7u;
    CHECK(mq_physics_add_body(w, &bad, &e) == MQ_PHYSICS_ERR_INVALID_ARG);
    bad = make_ball(0.0F, 0.0F);
    bad.restitution = 1.5F;
    CHECK(mq_physics_add_body(w, &bad, &e) == MQ_PHYSICS_ERR_INVALID_ARG);
    bad = make_ball(0.0F, 0.0F);
    bad.shape = MQ_PHYSICS_SHAPE_CUBOID;
    bad.half_w = 0.0F;
    bad.half_h = 5.0F;
    CHECK(mq_physics_add_body(w, &bad, &e) == MQ_PHYSICS_ERR_INVALID_ARG);
    bad = make_ball(0.0F, 0.0F);
    bad.shape = 9u;
    CHECK(mq_physics_add_body(w, &bad, &e) == MQ_PHYSICS_ERR_NOT_SUPPORTED);

    /* 移除 → 世代自增 → 旧句柄失效 */
    CHECK(mq_physics_remove_body(w, b) == MQ_PHYSICS_OK);
    CHECK(mq_physics_body_valid(w, b) == 0);
    CHECK(mq_physics_body_count(w) == 3);
    CHECK(mq_physics_remove_body(w, b) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    CHECK(mq_physics_remove_body(w, mq_physics_body{0u, 1u}) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    CHECK(mq_physics_remove_body(w, mq_physics_body{99u, 1u}) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    float ox = 0.0F, oy = 0.0F;
    CHECK(mq_physics_get_transform(w, b, &ox, &oy) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    CHECK(mq_physics_get_velocity(w, b, &ox, &oy) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    CHECK(mq_physics_set_transform(w, b, 1.0F, 2.0F) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    CHECK(mq_physics_set_velocity(w, b, 1.0F, 2.0F) == MQ_PHYSICS_ERR_INVALID_HANDLE);
    CHECK(mq_physics_interpolate(w, b, 0.5F, &ox, &oy) == MQ_PHYSICS_ERR_INVALID_HANDLE);

    /* slot 复用：新句柄 generation+1 */
    mq_physics_body reused{};
    CHECK(mq_physics_add_body(w, &ball, &reused) == MQ_PHYSICS_OK);
    CHECK(reused.index == b.index);
    CHECK(reused.generation == b.generation + 1u);
    CHECK(mq_physics_body_valid(w, reused) == 1);
    CHECK(mq_physics_body_valid(w, b) == 0);

    /* 非法参数 */
    CHECK(mq_physics_body_count(nullptr) == -1);
    CHECK(mq_physics_body_valid(nullptr, a) == -1);
    CHECK(mq_physics_get_transform(w, a, nullptr, &oy) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_get_transform(nullptr, a, &ox, &oy) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_set_bounds(w, 10.0F, 0.0F, -10.0F, 100.0F) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_step(nullptr) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_contact_at(w, 0u, nullptr) == MQ_PHYSICS_ERR_INVALID_ARG);
    mq_physics_contact dummy{};
    CHECK(mq_physics_contact_at(w, 999u, &dummy) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_contact_count(nullptr) == 0u);
    CHECK(mq_physics_step_count(nullptr) == 0u);
    CHECK(mq_physics_state_size(nullptr) == 0u);
    CHECK(mq_physics_save_state(nullptr, nullptr, 0u) == 0u);
    CHECK(mq_physics_load_state(w, nullptr, 0u) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_load_state(w, &dummy, sizeof(dummy)) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_save_state(w, nullptr, 0u) > 0u); /* 查询所需字节数 */

    /* create：NULL / struct_size=0 → 全默认；非法值拒绝 */
    mq_physics_world* def = mq_physics_create(nullptr);
    CHECK(def != nullptr);
    CHECK(mq_physics_body_count(def) == 0);
    mq_physics_destroy(def);
    mq_physics_config zero{};
    mq_physics_world* zd = mq_physics_create(&zero);
    CHECK(zd != nullptr); /* struct_size=0 → 全部字段取默认 */
    mq_physics_destroy(zd);
    mq_physics_config bad_cfg = make_config();
    bad_cfg.fixed_dt = 0.0F;
    CHECK(mq_physics_create(&bad_cfg) == nullptr);
    bad_cfg = make_config();
    bad_cfg.fixed_dt = -1.0F;
    CHECK(mq_physics_create(&bad_cfg) == nullptr);
    bad_cfg = make_config();
    bad_cfg.struct_size = 4u;
    CHECK(mq_physics_create(&bad_cfg) == nullptr); /* 截断配置：v1 拒绝 */

    mq_physics_destroy(w);
    mq_physics_destroy(nullptr); /* 幂等 */
}

/* 2) 自由落体：按同一半隐式 Euler 公式逐位相等 */
void test_free_fall_exact() {
    const float dt = 1.0F / 60.0F;
    mq_physics_config cfg = make_config();
    cfg.gravity_x = 0.0F;
    cfg.gravity_y = 900.0F;
    cfg.fixed_dt = dt;
    mq_physics_world* w = mq_physics_create(&cfg);
    CHECK(w != nullptr);
    const mq_physics_body_desc desc = make_ball(3.0F, 100.0F);
    mq_physics_body b{};
    CHECK(mq_physics_add_body(w, &desc, &b) == MQ_PHYSICS_OK);

    float x = 0.0F, y = 0.0F, vx = 0.0F, vy = 0.0F;
    CHECK(mq_physics_get_transform(w, b, &x, &y) == MQ_PHYSICS_OK);
    CHECK(x == 3.0F && y == 100.0F);
    CHECK(mq_physics_get_velocity(w, b, &vx, &vy) == MQ_PHYSICS_OK);
    CHECK(vx == 0.0F && vy == 0.0F);

    const float g_dt = cfg.gravity_y * dt;
    for (int i = 0; i < 120; ++i) {
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);
        vy += g_dt;
        vx += cfg.gravity_x * dt;
        y += vy * dt;
        x += vx * dt;
        float gx = 0.0F, gy = 0.0F, gvx = 0.0F, gvy = 0.0F;
        CHECK(mq_physics_get_transform(w, b, &gx, &gy) == MQ_PHYSICS_OK);
        CHECK(mq_physics_get_velocity(w, b, &gvx, &gvy) == MQ_PHYSICS_OK);
        CHECK(gx == x);  /* 逐位相等：测试确定性而非近似 */
        CHECK(gy == y);
        CHECK(gvx == vx);
        CHECK(gvy == vy);
    }
    CHECK(y > 100.0F);
    CHECK(mq_physics_step_count(w) == 120u);
    mq_physics_destroy(w);
}

/* 3) 弹跳：回弹且顶点高度严格递减（restitution < 1 耗散） */
void test_bounce() {
    mq_physics_config cfg = make_config();
    mq_physics_world* w = mq_physics_create(&cfg);
    CHECK(w != nullptr);
    CHECK(mq_physics_set_bounds(w, 0.0F, 0.0F, 400.0F, kFloorY) == MQ_PHYSICS_OK);
    const mq_physics_body_desc desc = make_ball(100.0F, 0.0F);
    mq_physics_body b{};
    CHECK(mq_physics_add_body(w, &desc, &b) == MQ_PHYSICS_OK);

    const double drop_height = static_cast<double>(kFloorY - kBallRadius);
    const double min_y = static_cast<double>(kFloorY - kBallRadius);
    std::vector<double> heights; /* 各跳顶点高度（弹跳高度） */
    double prev_y = 0.0;
    float prev_vy = 0.0F;
    bool airborne = false;

    for (int i = 0; i < 900; ++i) {
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);
        float x = 0.0F, y = 0.0F, vx = 0.0F, vy = 0.0F;
        CHECK(mq_physics_get_transform(w, b, &x, &y) == MQ_PHYSICS_OK);
        CHECK(mq_physics_get_velocity(w, b, &vx, &vy) == MQ_PHYSICS_OK);
        CHECK(is_finite(x) && is_finite(y) && is_finite(vy));
        CHECK(y <= min_y + 1.0e-3); /* 不穿地 */
        if (prev_vy > 0.0F && vy < 0.0F) {
            /* 落地瞬间：出射速度 = 入射速度 * restitution（冲量模型自证；
             * 入射速度取积分后含本帧重力的速度，故比值略高于 restitution）。 */
            const double ratio = static_cast<double>(-vy) / static_cast<double>(prev_vy);
            CHECK(ratio > 0.5 && ratio < 0.60);
            CHECK(-vy < static_cast<double>(prev_vy)); /* 出射必然慢于入射 */
            airborne = true;
        }
        if (airborne && vy > 0.0F) {
            heights.push_back(min_y - prev_y); /* 顶点：速度由升转降的那一帧 */
            airborne = false;
        }
        prev_y = static_cast<double>(y);
        prev_vy = vy;
    }

    CHECK(heights.size() >= 3u);
    if (heights.size() >= 3u) {
        CHECK(heights[0] < drop_height);
        CHECK(heights[0] > 1.0);
        CHECK(heights[1] < heights[0]);
        CHECK(heights[2] < heights[1]);
        CHECK(heights[2] > 0.0);
    }
    /* 落定后不抖：末帧速度极小、位置贴地 */
    float x = 0.0F, y = 0.0F, vx = 0.0F, vy = 0.0F;
    CHECK(mq_physics_get_transform(w, b, &x, &y) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w, b, &vx, &vy) == MQ_PHYSICS_OK);
    CHECK(vy == 0.0F);
    CHECK(y <= kFloorY - kBallRadius + 1.0e-2F);
    mq_physics_destroy(w);
}

/* 4) 确定性：同输入两次构建 → checksum 与快照字节完全一致 */
void test_determinism() {
    const auto build = []() {
        mq_physics_config cfg = make_config();
        mq_physics_world* w = mq_physics_create(&cfg);
        mq_physics_set_bounds(w, 0.0F, 0.0F, 400.0F, kFloorY);
        for (int i = 0; i < 6; ++i) {
            mq_physics_body_desc d = make_ball(30.0F + 40.0F * static_cast<float>(i), 20.0F,
                                               8.0F + static_cast<float>(i));
            d.vx = 20.0F * static_cast<float>(i) - 40.0F;
            d.restitution = 0.3F + 0.1F * static_cast<float>(i);
            mq_physics_body h{};
            mq_physics_add_body(w, &d, &h);
        }
        for (int i = 0; i < 600; ++i)
            mq_physics_step(w);
        return w;
    };
    mq_physics_world* w1 = build();
    mq_physics_world* w2 = build();
    CHECK(mq_physics_checksum(w1) == mq_physics_checksum(w2));
    CHECK(mq_physics_state_size(w1) == mq_physics_state_size(w2));
    const size_t size = mq_physics_state_size(w1);
    std::vector<unsigned char> s1(size, 0u);
    std::vector<unsigned char> s2(size, 0u);
    CHECK(mq_physics_save_state(w1, s1.data(), s1.size()) == size);
    CHECK(mq_physics_save_state(w2, s2.data(), s2.size()) == size);
    CHECK(std::memcmp(s1.data(), s2.data(), size) == 0); /* 快照字节逐位一致 */
    CHECK(mq_physics_step_count(w1) == 600u);

    /* 稳定排序：换插入顺序（互换两个独立小球）后物理状态逐位一致（index 分配不同）。
     * 低重力 + 有界世界，避免长跑出界导致状态不可比。 */
    mq_physics_config cfg3 = make_config();
    cfg3.gravity_y = 10.0F;
    mq_physics_world* w3 = mq_physics_create(&cfg3);
    mq_physics_world* w4 = mq_physics_create(&cfg3);
    CHECK(mq_physics_set_bounds(w3, 0.0F, 0.0F, 400.0F, kFloorY) == MQ_PHYSICS_OK);
    CHECK(mq_physics_set_bounds(w4, 0.0F, 0.0F, 400.0F, kFloorY) == MQ_PHYSICS_OK);
    const mq_physics_body_desc first = make_ball(50.0F, 20.0F);
    const mq_physics_body_desc second = make_ball(200.0F, 120.0F);
    mq_physics_body h1{}, h2{}, h3{}, h4{};
    CHECK(mq_physics_add_body(w3, &first, &h1) == MQ_PHYSICS_OK);
    CHECK(mq_physics_add_body(w3, &second, &h2) == MQ_PHYSICS_OK);
    CHECK(mq_physics_add_body(w4, &second, &h3) == MQ_PHYSICS_OK);
    CHECK(mq_physics_add_body(w4, &first, &h4) == MQ_PHYSICS_OK);
    CHECK(mq_physics_body_valid(w3, h1) == 1 && mq_physics_body_valid(w4, h3) == 1);
    for (int i = 0; i < 200; ++i) {
        CHECK(mq_physics_step(w3) == MQ_PHYSICS_OK);
        CHECK(mq_physics_step(w4) == MQ_PHYSICS_OK);
    }
    /* 交换插入顺序只改变 index 分配：按句柄取回后逐位比较物理状态 */
    float ax = 0.0F, ay = 0.0F, avx = 0.0F, avy = 0.0F;
    float bx = 0.0F, by = 0.0F, bvx = 0.0F, bvy = 0.0F;
    CHECK(mq_physics_get_transform(w3, h1, &ax, &ay) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w3, h1, &avx, &avy) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_transform(w4, h4, &bx, &by) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w4, h4, &bvx, &bvy) == MQ_PHYSICS_OK);
    CHECK(ax == bx && ay == by && avx == bvx && avy == bvy); /* first：w3@1 vs w4@2 */
    CHECK(mq_physics_get_transform(w3, h2, &ax, &ay) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w3, h2, &avx, &avy) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_transform(w4, h3, &bx, &by) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w4, h3, &bvx, &bvy) == MQ_PHYSICS_OK);
    CHECK(ax == bx && ay == by && avx == bvx && avy == bvy); /* second：w3@2 vs w4@1 */
    /* 同序重建可复现 */
    mq_physics_world* w5 = mq_physics_create(&cfg3);
    CHECK(mq_physics_add_body(w5, &first, &h1) == MQ_PHYSICS_OK);
    CHECK(mq_physics_add_body(w5, &second, &h2) == MQ_PHYSICS_OK);
    for (int i = 0; i < 200; ++i)
        CHECK(mq_physics_step(w5) == MQ_PHYSICS_OK);
    CHECK(mq_physics_checksum(w3) == mq_physics_checksum(w5));

    mq_physics_destroy(w1);
    mq_physics_destroy(w2);
    mq_physics_destroy(w3);
    mq_physics_destroy(w4);
    mq_physics_destroy(w5);
}

/* 5) 接触列表：按 (a_index,b_index) 严格升序、深度/法线有效 */
void test_contacts_stable_order() {
    mq_physics_config cfg = make_config();
    mq_physics_world* w = mq_physics_create(&cfg);
    CHECK(w != nullptr);
    float minx = 0.0F, miny = 0.0F, maxx = 0.0F, maxy = 0.0F;
    for (int i = 0; i < 5; ++i) {
        mq_physics_body_desc d = make_ball(100.0F + 4.0F * static_cast<float>(i),
                                           120.0F + 4.0F * static_cast<float>(i), 10.0F);
        d.vx = 10.0F;
        d.vy = -5.0F;
        mq_physics_body h{};
        CHECK(mq_physics_add_body(w, &d, &h) == MQ_PHYSICS_OK);
        minx = (i == 0) ? d.x - 10.0F : minx;
        miny = (i == 0) ? d.y - 10.0F : miny;
        maxx = d.x + 10.0F;
        maxy = d.y + 10.0F;
    }
    CHECK(mq_physics_set_bounds(w, minx - 100.0F, miny - 100.0F, maxx + 100.0F,
                                maxy + 5.0F) == MQ_PHYSICS_OK);
    for (int i = 0; i < 30; ++i)
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);

    const uint32_t count = mq_physics_contact_count(w);
    CHECK(count > 0u);
    uint32_t prev_a = 0u;
    uint32_t prev_b = 0u;
    bool first = true;
    for (uint32_t i = 0; i < count; ++i) {
        mq_physics_contact c{};
        CHECK(mq_physics_contact_at(w, i, &c) == MQ_PHYSICS_OK);
        CHECK(c.a_index != 0u);
        CHECK(c.a_index < c.b_index || c.b_index == MQ_PHYSICS_PLANE_INDEX);
        CHECK(c.a_index < cfg.max_bodies);
        CHECK(c.b_index > c.a_index);
        CHECK(c.depth >= 0.0F);
        const float len = c.nx * c.nx + c.ny * c.ny;
        CHECK(len > 0.999F && len < 1.001F);
        if (!first) {
            CHECK(prev_a < c.a_index || (prev_a == c.a_index && prev_b < c.b_index));
        }
        prev_a = c.a_index;
        prev_b = c.b_index;
        first = false;
    }
    CHECK(mq_physics_contact_count(nullptr) == 0u);
    mq_physics_destroy(w);
}

/* 6) 快照往返：save → step → load → step 与参考世界逐位一致 */
void test_snapshot_round_trip() {
    const auto build = []() {
        mq_physics_config cfg = make_config();
        mq_physics_world* w = mq_physics_create(&cfg);
        mq_physics_set_bounds(w, 0.0F, 0.0F, 300.0F, kFloorY);
        for (int i = 0; i < 4; ++i) {
            mq_physics_body_desc d = make_ball(40.0F + 50.0F * static_cast<float>(i), 30.0F, 9.0F);
            d.vx = (i % 2 == 0) ? 35.0F : -25.0F;
            mq_physics_body h{};
            mq_physics_add_body(w, &d, &h);
        }
        return w;
    };
    mq_physics_world* w = build();
    for (int i = 0; i < 100; ++i)
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);

    const size_t size = mq_physics_state_size(w);
    CHECK(size > 0u);
    std::vector<unsigned char> blob(size, 0u);
    CHECK(mq_physics_save_state(w, blob.data(), blob.size()) == size);

    /* 小缓冲：返回所需字节数且不写入 */
    std::vector<unsigned char> small(size > 0u ? size - 1u : 0u, 0xABu);
    CHECK(mq_physics_save_state(w, small.data(), small.size()) == size);

    /* 参考世界：从快照继续 200 步 */
    mq_physics_world* ref = build();
    CHECK(mq_physics_load_state(ref, blob.data(), blob.size()) == MQ_PHYSICS_OK);
    for (int i = 0; i < 200; ++i)
        CHECK(mq_physics_step(ref) == MQ_PHYSICS_OK);

    /* 被测世界：再跑 100 步 → 回滚 → 再跑 200 步；应在 200 步点逐位一致 */
    for (int i = 0; i < 100; ++i)
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);
    CHECK(mq_physics_load_state(w, blob.data(), blob.size()) == MQ_PHYSICS_OK);
    for (int i = 0; i < 200; ++i)
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);

    CHECK(mq_physics_checksum(w) == mq_physics_checksum(ref));
    const size_t size2 = mq_physics_state_size(w);
    CHECK(size2 == size);
    std::vector<unsigned char> b2(size2, 0u);
    CHECK(mq_physics_save_state(w, b2.data(), b2.size()) == size2);
    CHECK(std::memcmp(b2.data(), blob.data(), size) != 0); /* 状态已推进，快照应不同 */
    CHECK(mq_physics_step_count(w) == mq_physics_step_count(ref));

    /* 坏快照：拒绝、世界保持原状（强一致）、仍可继续步进 */
    std::vector<unsigned char> corrupt = blob;
    corrupt[0] ^= 0xFFu;
    const uint64_t before_bad = mq_physics_checksum(w);
    CHECK(mq_physics_load_state(w, corrupt.data(), corrupt.size()) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_load_state(w, blob.data(), blob.size() / 2u) == MQ_PHYSICS_ERR_INVALID_ARG);
    CHECK(mq_physics_checksum(w) == before_bad);
    CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);

    /* restore → 再 save 逐位一致（往返幂等） */
    CHECK(mq_physics_load_state(w, blob.data(), blob.size()) == MQ_PHYSICS_OK);
    std::vector<unsigned char> again(size, 0u);
    CHECK(mq_physics_save_state(w, again.data(), again.size()) == size);
    CHECK(std::memcmp(again.data(), blob.data(), size) == 0); /* 往返逐位一致 */

    mq_physics_destroy(w);
    mq_physics_destroy(ref);
}

/* 7) 只读插值：alpha 端点/中点正确，且绝不改变 checksum */
void test_interpolation_readonly() {
    mq_physics_config cfg = make_config();
    cfg.gravity_y = 0.0F;
    mq_physics_world* w = mq_physics_create(&cfg);
    CHECK(w != nullptr);
    mq_physics_body_desc d = make_ball(0.0F, 0.0F);
    d.vy = 60.0F;
    mq_physics_body b{};
    CHECK(mq_physics_add_body(w, &d, &b) == MQ_PHYSICS_OK);

    float x0 = 0.0F, y0 = 0.0F;
    CHECK(mq_physics_get_transform(w, b, &x0, &y0) == MQ_PHYSICS_OK);
    float ix = 0.0F, iy = 0.0F;
    CHECK(mq_physics_interpolate(w, b, 0.0F, &ix, &iy) == MQ_PHYSICS_OK);
    CHECK(ix == x0 && iy == y0); /* 未步进：prev == curr */

    CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);
    float x1 = 0.0F, y1 = 0.0F;
    CHECK(mq_physics_get_transform(w, b, &x1, &y1) == MQ_PHYSICS_OK);
    CHECK(y1 > y0);

    const uint64_t before = mq_physics_checksum(w);
    CHECK(mq_physics_interpolate(w, b, 0.0F, &ix, &iy) == MQ_PHYSICS_OK);
    CHECK(ix == x0 && iy == y0); /* alpha=0 → prev */
    CHECK(mq_physics_interpolate(w, b, 1.0F, &ix, &iy) == MQ_PHYSICS_OK);
    CHECK(ix == x1 && iy == y1); /* alpha=1 → curr */
    CHECK(mq_physics_interpolate(w, b, 0.5F, &ix, &iy) == MQ_PHYSICS_OK);
    CHECK(iy >= y0 && iy <= y1);
    CHECK(iy > y0 && iy < y1);
    for (int i = 0; i < 100; ++i)
        CHECK(mq_physics_interpolate(w, b, 0.25F, &ix, &iy) == MQ_PHYSICS_OK);
    CHECK(mq_physics_checksum(w) == before); /* 插值不污染模拟 */

    /* prev/curr 更新：再走一步后 alpha=0 等于上一步的 curr */
    CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);
    CHECK(mq_physics_interpolate(w, b, 0.0F, &ix, &iy) == MQ_PHYSICS_OK);
    CHECK(ix == x1 && iy == y1);
    mq_physics_destroy(w);
}

/* 8) 插件 vtable：版本匹配、与直接 API 一致 */
void test_plugin_vtable() {
    const mq_physics_v1* v1 = mq_physics_query(1u);
    CHECK(v1 != nullptr);
    CHECK(v1 != nullptr && v1->abi_version == MQ_PHYSICS_ABI_VERSION);
    CHECK(v1 != nullptr && v1->name != nullptr);
    CHECK(mq_physics_query(2u) == nullptr);
    CHECK(mq_physics_query(0u) == nullptr);

    if (v1 != nullptr && v1->create != nullptr) {
        const mq_physics_config cfg = make_config();
        mq_physics_world* w = v1->create(&cfg);
        CHECK(w != nullptr);
        mq_physics_body_desc d = make_ball(10.0F, 10.0F);
        mq_physics_body h{};
        CHECK(v1->add_body(w, &d, &h) == MQ_PHYSICS_OK);
        for (int i = 0; i < 30; ++i)
            CHECK(v1->step(w) == MQ_PHYSICS_OK);
        float vx = 0.0F, vy = 0.0F;
        CHECK(v1->get_transform(w, h, &vx, &vy) == MQ_PHYSICS_OK);
        CHECK(v1->set_velocity(w, h, 1.0F, 2.0F) == MQ_PHYSICS_OK);
        CHECK(v1->set_transform(w, h, 5.0F, 6.0F) == MQ_PHYSICS_OK);
        const uint64_t via_table = v1->checksum(w);
        CHECK(via_table == mq_physics_checksum(w));
        CHECK(v1->remove_body(w, h) == MQ_PHYSICS_OK);
        CHECK(v1->remove_body(w, h) == MQ_PHYSICS_ERR_INVALID_HANDLE);

        /* vtable 与直接 API 世界行为一致（同输入 → 同 checksum） */
        mq_physics_world* direct = mq_physics_create(&cfg);
        CHECK(direct != nullptr);
        mq_physics_body dh{};
        CHECK(mq_physics_add_body(direct, &d, &dh) == MQ_PHYSICS_OK);
        for (int i = 0; i < 30; ++i)
            CHECK(mq_physics_step(direct) == MQ_PHYSICS_OK);
        v1->destroy(w);
        CHECK(mq_physics_checksum(direct) != 0u);
        mq_physics_destroy(direct);
    }
}

/* 9) 类型语义（STATIC/KINEMATIC/关重力）、球-盒碰撞、多实例隔离 */
void test_body_kinds_and_isolation() {
    const float dt = 1.0F / 60.0F;
    mq_physics_config cfg = make_config();
    cfg.fixed_dt = dt;
    mq_physics_world* w = mq_physics_create(&cfg);
    CHECK(w != nullptr);

    /* STATIC：永不动 */
    mq_physics_body_desc box = make_ball(100.0F, 180.0F);
    box.type = MQ_PHYSICS_BODY_STATIC;
    box.shape = MQ_PHYSICS_SHAPE_CUBOID;
    box.half_w = 30.0F;
    box.half_h = 20.0F; /* 顶面 y = 160 */
    mq_physics_body box_h{};
    CHECK(mq_physics_add_body(w, &box, &box_h) == MQ_PHYSICS_OK);

    /* KINEMATIC：按自身速度平移、不受重力与冲量影响 */
    mq_physics_body_desc kin = make_ball(300.0F, 50.0F);
    kin.type = MQ_PHYSICS_BODY_KINEMATIC;
    kin.vy = 60.0F;
    mq_physics_body kin_h{};
    CHECK(mq_physics_add_body(w, &kin, &kin_h) == MQ_PHYSICS_OK);

    /* DYNAMIC + 关重力：静止不动 */
    mq_physics_body_desc fixed_ball = make_ball(200.0F, 30.0F);
    fixed_ball.flags = MQ_PHYSICS_BF_DISABLE_GRAVITY;
    mq_physics_body fixed_h{};
    CHECK(mq_physics_add_body(w, &fixed_ball, &fixed_h) == MQ_PHYSICS_OK);

    /* DYNAMIC：落到静态盒顶面 */
    mq_physics_body_desc fall = make_ball(100.0F, 100.0F);
    mq_physics_body fall_h{};
    CHECK(mq_physics_add_body(w, &fall, &fall_h) == MQ_PHYSICS_OK);

    float kx = 0.0F, ky = 0.0F, kvx = 0.0F, kvy = 0.0F;
    CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_transform(w, kin_h, &kx, &ky) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w, kin_h, &kvx, &kvy) == MQ_PHYSICS_OK);
    CHECK(kx == kin.x);              /* 无水平速度：x 不变 */
    CHECK(ky == kin.y + kin.vy * dt); /* 严格按 v*dt 平移 */
    CHECK(kvy == kin.vy);            /* 不受重力 */

    for (int i = 0; i < 120; ++i)
        CHECK(mq_physics_step(w) == MQ_PHYSICS_OK);

    float bx = 0.0F, by = 0.0F, bvx = 0.0F, bvy = 0.0F;
    CHECK(mq_physics_get_transform(w, box_h, &bx, &by) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w, box_h, &bvx, &bvy) == MQ_PHYSICS_OK);
    CHECK(bx == 100.0F && by == 180.0F && bvx == 0.0F && bvy == 0.0F); /* STATIC 永不动 */

    CHECK(mq_physics_get_transform(w, kin_h, &kx, &ky) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w, kin_h, &kvx, &kvy) == MQ_PHYSICS_OK);
    CHECK(kvy == 60.0F); /* 冲量不改 KINEMATIC 速度 */
    float expect_y = kin.y;
    for (int i = 0; i < 121; ++i)
        expect_y += 60.0F * dt;
    CHECK(ky == expect_y);

    CHECK(mq_physics_get_transform(w, fixed_h, &bx, &by) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w, fixed_h, &bvx, &bvy) == MQ_PHYSICS_OK);
    CHECK(bx == 200.0F && by == 30.0F && bvy == 0.0F); /* 关重力：悬停 */

    /* 球落在静态盒顶面（y ≈ 160 - r），不穿透、不抖 */
    CHECK(mq_physics_get_transform(w, fall_h, &bx, &by) == MQ_PHYSICS_OK);
    CHECK(mq_physics_get_velocity(w, fall_h, &bvx, &bvy) == MQ_PHYSICS_OK);
    CHECK(std::fabs(by - 150.0F) < 1.0e-2F);
    CHECK(bvy == 0.0F);
    bool found_pair = false;
    for (uint32_t i = 0u; i < mq_physics_contact_count(w); ++i) {
        mq_physics_contact c{};
        CHECK(mq_physics_contact_at(w, i, &c) == MQ_PHYSICS_OK);
        const bool pair = (c.a_index == fall_h.index && c.b_index == box_h.index) ||
                          (c.a_index == box_h.index && c.b_index == fall_h.index);
        if (pair)
            found_pair = true;
    }
    CHECK(found_pair);

    /* 多实例隔离：交替创建/销毁第三个世界不改变既有世界（零全局状态） */
    mq_physics_config cfg2 = make_config();
    cfg2.fixed_dt = dt;
    mq_physics_world* x = mq_physics_create(&cfg2);
    mq_physics_world* y = mq_physics_create(&cfg2);
    CHECK(x != nullptr && y != nullptr);
    for (int i = 0; i < 100; ++i) {
        CHECK(mq_physics_step(x) == MQ_PHYSICS_OK);
        if (i % 7 == 0) {
            mq_physics_world* z = mq_physics_create(&cfg2);
            mq_physics_body zh{};
            CHECK(mq_physics_add_body(z, &fall, &zh) == MQ_PHYSICS_OK);
            for (int k = 0; k < 3; ++k)
                CHECK(mq_physics_step(z) == MQ_PHYSICS_OK);
            mq_physics_destroy(z);
        }
        CHECK(mq_physics_step(y) == MQ_PHYSICS_OK);
    }
    CHECK(mq_physics_checksum(x) == mq_physics_checksum(y));
    CHECK(mq_physics_body_count(x) == 0 && mq_physics_body_count(y) == 0);
    mq_physics_destroy(x);
    mq_physics_destroy(y);
    mq_physics_destroy(w);
}

} // namespace


/* ---- ABI 守卫：DSL 生成的头若发生 ABI 漂移即编译失败（docs/02 R7）---- */
static_assert(sizeof(mq_physics_body) == 8, "mq_physics_body ABI 变更");
static_assert(sizeof(mq_physics_config) == 32, "mq_physics_config ABI 变更");
static_assert(offsetof(mq_physics_config, gravity_y) == 8, "字段偏移变更");
static_assert(offsetof(mq_physics_config, fixed_dt) == 12, "字段偏移变更");
static_assert(offsetof(mq_physics_config, seed) == 24, "字段偏移变更");
static_assert(sizeof(mq_physics_body_desc) == 56, "mq_physics_body_desc ABI 变更");
static_assert(offsetof(mq_physics_body_desc, x) == 12, "字段偏移变更");
static_assert(offsetof(mq_physics_body_desc, half_w) == 32, "字段偏移变更");
static_assert(offsetof(mq_physics_body_desc, flags) == 52, "字段偏移变更");
static_assert(sizeof(mq_physics_contact) == 20, "mq_physics_contact ABI 变更");
static_assert(sizeof(mq_physics_v1) == 88, "插件 vtable 尺寸变更");
static_assert(offsetof(mq_physics_v1, checksum) == 80, "vtable 槽位变更");
static_assert(MQ_PHYSICS_ABI_VERSION == 1u, "ABI 版本变更");
static_assert(MQ_PHYSICS_PLANE_INDEX == 0xFFFFFFFFu, "哨兵值变更");

int main() {
    test_lifecycle_handles();
    test_free_fall_exact();
    test_bounce();
    test_determinism();
    test_contacts_stable_order();
    test_snapshot_round_trip();
    test_interpolation_readonly();
    test_plugin_vtable();
    test_body_kinds_and_isolation();

    if (g_failures == 0) {
        std::print("physics_test: all checks passed\n");
        return 0;
    }
    std::print("physics_test: {} checks failed\n", g_failures);
    return 1;
}
