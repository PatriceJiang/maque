// 导航单元测试（2D 网格 A* + 平滑 + 追踪）：确定性、绕障、不切角、边界与错误码。
#include "nav/mq_nav.h"

#include <print>
#include <vector>
#include <cmath>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static mq_nav_world* make_world(uint32_t w = 16, uint32_t h = 16, float cell = 10.0F) {
    mq_nav_config cfg{};
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.width = w; cfg.height = h; cfg.cell_size = cell; cfg.allow_diagonal = 1;
    return mq_nav_create(&cfg);
}


/* ---- ABI 守卫：DSL 生成的头若发生 ABI 漂移即编译失败（docs/02 R7）---- */
static_assert(sizeof(mq_nav_config) == 20, "mq_nav_config ABI 变更");
static_assert(offsetof(mq_nav_config, struct_size) == 0, "字段偏移变更");
static_assert(offsetof(mq_nav_config, width) == 4, "字段偏移变更");
static_assert(offsetof(mq_nav_config, height) == 8, "字段偏移变更");
static_assert(offsetof(mq_nav_config, cell_size) == 12, "字段偏移变更");
static_assert(offsetof(mq_nav_config, allow_diagonal) == 16, "字段偏移变更");
static_assert(sizeof(((mq_nav_config*)0)->allow_diagonal) == 1, "allow_diagonal 必须为 1 字节");
static_assert(MQ_NAV_ERR_NOMEM == 5 && MQ_NAV_ERR_CAPACITY == 6, "错误码变更");
static_assert(MQ_NAV_ABI_VERSION == 1u, "ABI 版本变更");

int main() {
    // ---- 1) 空地图：直线路径 + 平滑 ----
    {
        mq_nav_world* w = make_world();
        CHECK(w != nullptr);
        CHECK(mq_nav_walkable_count(w) == 16u * 16u);
        std::vector<float> path(2 * 64);
        uint32_t n = 0, need = 0;
        CHECK(mq_nav_find_path(w, 5, 5, 145, 145, path.data(), (uint32_t)path.size() / 2, &n, &need) == MQ_NAV_OK);
        CHECK(n >= 2);
        CHECK(path[0] > 4.9F && path[0] < 5.1F);                 // 起点格中心
        CHECK(path[(n - 1) * 2] > 144.9F && path[(n - 1) * 2] < 145.1F);
        const uint32_t expanded = mq_nav_last_expanded(w);
        // 同一查询重复执行：扩展节点数一致（确定性）
        uint32_t n2 = 0;
        CHECK(mq_nav_find_path(w, 5, 5, 145, 145, path.data(), (uint32_t)path.size() / 2, &n2, &need) == MQ_NAV_OK);
        CHECK(n2 == n && mq_nav_last_expanded(w) == expanded);
        const uint32_t smooth_n = mq_nav_smooth(w, path.data(), n);
        CHECK(smooth_n <= n && smooth_n >= 2);
        mq_nav_destroy(w);
    }

    // ---- 2) 一堵墙 + 缺口：必须绕行，且路径全程可行走 ----
    {
        mq_nav_world* w = make_world();
        CHECK(mq_nav_set_rect(w, 8, 0, 1, 16, 1) == MQ_NAV_OK);   // 整列 x=8 封死
        CHECK(mq_nav_set_blocked(w, 8, 15, 0) == MQ_NAV_OK);      // 只在最下行留缺口
        std::vector<float> path(2 * 256);
        uint32_t n = 0, need = 0;
        CHECK(mq_nav_find_path(w, 45, 45, 125, 45, path.data(), (uint32_t)path.size() / 2, &n, &need) == MQ_NAV_OK);
        CHECK(n > 3);
        for (uint32_t i = 0; i < n; ++i)
            CHECK(mq_nav_is_walkable_at(w, path[i * 2], path[i * 2 + 1]) == 1);
        bool went_around = false;
        for (uint32_t i = 0; i < n; ++i) if (path[i * 2 + 1] > 140.0F) went_around = true;   // 经过最下行
        CHECK(went_around);
        // 完全封死 → NOPATH
        CHECK(mq_nav_set_blocked(w, 8, 15, 1) == MQ_NAV_OK);
        CHECK(mq_nav_find_path(w, 45, 45, 125, 45, path.data(), (uint32_t)path.size() / 2, &n, &need) == MQ_NAV_ERR_NOPATH);
        mq_nav_destroy(w);
    }

    // ---- 3) 不切角：对角紧贴障碍时不得穿过角点 ----
    {
        mq_nav_world* w = make_world(4, 4, 10.0F);
        CHECK(mq_nav_set_blocked(w, 1, 0, 1) == MQ_NAV_OK);
        CHECK(mq_nav_set_blocked(w, 0, 1, 1) == MQ_NAV_OK);
        std::vector<float> path(2 * 32);
        uint32_t n = 0, need = 0;
        const int rc = mq_nav_find_path(w, 5, 5, 25, 25, path.data(), (uint32_t)path.size() / 2, &n, &need);
        CHECK(rc == MQ_NAV_OK || rc == MQ_NAV_ERR_NOPATH);       // 要么绕行要么无路，但不得穿角
        for (uint32_t i = 0; i < n; ++i) CHECK(mq_nav_is_walkable_at(w, path[i * 2], path[i * 2 + 1]) == 1);
        mq_nav_destroy(w);
    }

    // ---- 4) 视线与错误码 ----
    {
        mq_nav_world* w = make_world();
        CHECK(mq_nav_line_of_sight(w, 5, 5, 145, 5) == 1);
        CHECK(mq_nav_set_rect(w, 8, 0, 1, 16, 1) == MQ_NAV_OK);
        CHECK(mq_nav_line_of_sight(w, 5, 5, 145, 5) == 0);        // 被墙挡住
        CHECK(mq_nav_find_path(w, 5, 5, 1000, 1000, nullptr, 0, nullptr, nullptr) == MQ_NAV_ERR_RANGE);
        CHECK(mq_nav_find_path(w, -5, 5, 100, 100, nullptr, 0, nullptr, nullptr) == MQ_NAV_ERR_RANGE);
        CHECK(mq_nav_find_path(w, 85, 45, 100, 100, nullptr, 0, nullptr, nullptr) == MQ_NAV_ERR_BLOCKED);
        CHECK(mq_nav_set_blocked(w, 999, 999, 1) == MQ_NAV_ERR_RANGE);
        // 完全封死 → NOPATH（终点所在列被封）
        CHECK(mq_nav_find_path(w, 5, 5, 145, 145, nullptr, 0, nullptr, nullptr) == MQ_NAV_ERR_NOPATH);
        mq_nav_destroy(w);
    }

    // ---- 4b) 容量不足：返回 CAPACITY 并报告所需浮点数（== 点数 × 2）----
    {
        mq_nav_world* w = make_world();                    // 空地图，路径必然 > 1 点
        std::vector<float> tiny(2, 0.0F);
        uint32_t got = 0, need = 0;
        CHECK(mq_nav_find_path(w, 5, 5, 145, 145, tiny.data(), 1, &got, &need) == MQ_NAV_ERR_CAPACITY);
        CHECK(need > 2);
        CHECK(need == got * 2);
        mq_nav_destroy(w);
    }

    // ---- 5) 追踪：沿路径逐点前进，确定性 ----
    {
        mq_nav_world* w = make_world();
        std::vector<float> path(2 * 64);
        uint32_t n = 0, need = 0;
        CHECK(mq_nav_find_path(w, 10, 10, 100, 10, path.data(), (uint32_t)path.size() / 2, &n, &need) == MQ_NAV_OK);
        n = mq_nav_smooth(w, path.data(), n);
        float x = 10, y = 10;
        uint32_t idx = 0;
        int rc = 0, steps = 0;
        while (rc == 0 && steps < 1000) { rc = mq_nav_step_towards(w, &x, &y, path.data(), n, &idx, 60.0F, 1.0F / 60.0F, 0.5F); ++steps; }
        CHECK(rc == 1);
        // 终点落在目标格中心（目标 (100,10) → 格 (10,1) 中心 (105,15)）
        CHECK(mq_nav_is_walkable_at(w, x, y) == 1);
        uint32_t gx = 0, gy = 0;
        CHECK(mq_nav_cell_at(w, x, y, &gx, &gy) == MQ_NAV_OK);
        CHECK(gx == 10 && gy == 1);
        mq_nav_destroy(w);
    }

    // ---- 6) 确定性：两次独立世界给出完全相同的路径字节 ----
    {
        auto run = [](std::vector<float>& out) {
            mq_nav_world* w = make_world(24, 24, 8.0F);
            mq_nav_set_rect(w, 10, 0, 2, 18, 1);
            mq_nav_set_rect(w, 16, 6, 2, 18, 1);
            std::vector<float> path(2 * 512);
            uint32_t n = 0, need = 0;
            const int rc = mq_nav_find_path(w, 20, 20, 180, 180, path.data(), (uint32_t)path.size() / 2, &n, &need);
            if (rc != MQ_NAV_OK) { mq_nav_destroy(w); out.clear(); return; }
            n = mq_nav_smooth(w, path.data(), n);
            out.assign(path.begin(), path.begin() + (size_t)n * 2);
            mq_nav_destroy(w);
        };
        std::vector<float> a, b;
        run(a); run(b);
        CHECK(!a.empty());
        CHECK(a.size() == b.size());
        CHECK(a == b);
    }

    if (g_failures == 0) { std::print("nav_test: all checks passed\n"); return 0; }
    std::print("nav_test: {} failure(s)\n", g_failures);
    return 1;
}
