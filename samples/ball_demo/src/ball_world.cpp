#include "ball_demo/ball_world.h"

#include <cmath>

namespace ball_demo {

BallWorld::BallWorld(const Config& cfg) : cfg_(cfg) {
    table_ = mq_handle_table_create(sizeof(Ball), alignof(Ball));
}

BallWorld::~BallWorld() {
    if (table_) mq_handle_table_destroy(table_);
}

mq_handle BallWorld::on_click(float x, float y) {
    void* p = nullptr;
    mq_handle h = mq_handle_table_alloc(table_, &p);
    if (h.index == MQ_HANDLE_INDEX_NONE) return MQ_HANDLE_INVALID;
    auto* b = static_cast<Ball*>(p);
    b->x = x; b->y = y; b->vx = 0.0F; b->vy = 0.0F;
    b->resting = false; b->rest_time = 0.0F;
    return h;
}

void BallWorld::step(float dt) {
    if (!table_) return;
    const float floor_center = cfg_.ground_y + cfg_.radius;
    const uint32_t cap = mq_handle_table_capacity(table_);

    std::vector<mq_handle> to_remove;

    for (uint32_t i = 0; i < cap; ++i) {
        mq_handle h = mq_handle_table_handle_at(table_, i);
        if (h.index == MQ_HANDLE_INDEX_NONE) continue;
        auto* b = static_cast<Ball*>(mq_handle_table_get(table_, h));
        if (!b) continue;

        if (b->resting) {
            b->rest_time += dt;
            if (cfg_.auto_remove && b->rest_time >= cfg_.remove_delay) to_remove.push_back(h);
            continue;
        }

        /* 半隐式欧拉：先积分速度再积分位置（确定、稳定）。 */
        b->vy -= cfg_.gravity * dt;
        b->y  += b->vy * dt;
        b->x  += b->vx * dt;

        if (b->y <= floor_center) {
            b->y = floor_center;
            b->vy = -b->vy * cfg_.restitution;
            if (std::fabs(b->vy) < cfg_.rest_speed_eps) {
                b->vy = 0.0F;
                b->resting = true;       /* 高度 ≈ 0，进入静止计时 */
                b->rest_time = 0.0F;
            }
        }
    }

    for (mq_handle h : to_remove) mq_handle_table_free(table_, h);
}

int BallWorld::ball_count() const {
    return table_ ? (int)mq_handle_table_count(table_) : 0;
}

std::vector<Sprite> BallWorld::sprites() const {
    std::vector<Sprite> out;
    if (!table_) return out;
    const uint32_t cap = mq_handle_table_capacity(table_);
    for (uint32_t i = 0; i < cap; ++i) {
        mq_handle h = mq_handle_table_handle_at(table_, i);
        if (h.index == MQ_HANDLE_INDEX_NONE) continue;
        auto* b = static_cast<const Ball*>(mq_handle_table_get(table_, h));
        if (!b) continue;
        out.push_back(Sprite{ b->x, b->y, cfg_.radius, 0x66CCFFu });
    }
    return out;
}

mq_handle BallWorld::handle_at(uint32_t index) const {
    return table_ ? mq_handle_table_handle_at(table_, index) : MQ_HANDLE_INVALID;
}

void BallWorld::remove(mq_handle h) {
    if (table_) mq_handle_table_free(table_, h);
}

bool BallWorld::get_ball(mq_handle h, BallView& out) const {
    auto* b = static_cast<const Ball*>(mq_handle_table_get(table_, h));
    if (!b) return false;
    out = BallView{ b->x, b->y, b->vx, b->vy, b->resting, b->rest_time };
    return true;
}

} // namespace ball_demo
