// M1 无头球世界单元测试（验收 1-5）
#include "ball_demo/ball_world.h"

#include <cstdio>
#include <cmath>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

using namespace ball_demo;

int main() {
    const float dt = 1.0F / 60.0F;

    // 1. 点击生成
    {
        BallWorld w;
        mq_handle h = w.on_click(160.0F, 200.0F);
        CHECK(h.index != MQ_HANDLE_INDEX_NONE);
        CHECK(w.ball_count() == 1);
        BallWorld::BallView bv{};
        CHECK(w.get_ball(h, bv));
        CHECK(std::fabs(bv.x - 160.0F) < 1e-3F && std::fabs(bv.y - 200.0F) < 1e-3F);
    }

    // 2/3/4. 落体 → 弹跳 → 静止
    {
        BallWorld w;
        mq_handle h = w.on_click(100.0F, 200.0F);
        bool fell = false, bounced = false, rested = false;
        float prev_y = 200.0F;
        for (int f = 0; f < 60 * 10; ++f) {
            w.step(dt);
            BallWorld::BallView bv{};
            if (!w.get_ball(h, bv)) break;   // 已移除
            if (bv.y < prev_y) fell = true;
            if (bv.vy > 0.0F) bounced = true; // 触地后向上 = 弹跳
            if (bv.resting) rested = true;
            prev_y = bv.y;
        }
        CHECK(fell);
        CHECK(bounced);
        CHECK(rested);
    }

    // 移除时机：静止后 5s 内移除
    {
        BallWorld w;
        mq_handle h = w.on_click(100.0F, 200.0F);
        int rest_frame = -1, removed_frame = -1;
        for (int f = 0; f < 60 * 12; ++f) {
            w.step(dt);
            BallWorld::BallView bv{};
            if (w.get_ball(h, bv)) {
                if (bv.resting && rest_frame < 0) rest_frame = f;
            } else if (removed_frame < 0) {
                removed_frame = f;
            }
        }
        CHECK(rest_frame >= 0);
        CHECK(removed_frame >= 0);
        if (rest_frame >= 0 && removed_frame >= 0) {
            float removed_after = (removed_frame - rest_frame) * dt;
            std::printf("rest_frame=%d removed_frame=%d -> %.2fs after rest\n",
                        rest_frame, removed_frame, removed_after);
            CHECK(removed_after >= 5.0F - 0.05F);   // 至少约 5 秒
            CHECK(removed_after < 5.2F);
        }
    }

    // 5. 确定性：同输入同轨迹
    {
        BallWorld a, b;
        mq_handle ha = a.on_click(50.0F, 180.0F);
        mq_handle hb = b.on_click(50.0F, 180.0F);
        bool same = true;
        for (int f = 0; f < 300; ++f) {
            a.step(dt); b.step(dt);
            BallWorld::BallView va{}, vb{};
            bool ga = a.get_ball(ha, va), gb = b.get_ball(hb, vb);
            if (ga != gb) { same = false; break; }
            if (ga && (va.x != vb.x || va.y != vb.y || va.vy != vb.vy)) { same = false; break; }
        }
        CHECK(same);
    }

    // 多球独立
    {
        BallWorld w;
        mq_handle h1 = w.on_click(50.0F, 200.0F);
        mq_handle h2 = w.on_click(150.0F, 100.0F);
        CHECK(w.ball_count() == 2);
        for (int f = 0; f < 60; ++f) w.step(dt);
        BallWorld::BallView b1{}, b2{};
        CHECK(w.get_ball(h1, b1) && w.get_ball(h2, b2));
        CHECK(b1.y < 200.0F && b2.y < 100.0F);   // 两球都在下落
    }

    if (g_failures == 0) { std::printf("ball_world_test: all checks passed\n"); return 0; }
    std::printf("ball_world_test: %d failure(s)\n", g_failures);
    return 1;
}
