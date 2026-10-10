/* M1 · 下落弹跳球 —— 无头世界（docs/milestones/01-下落弹跳球.md）。
 * 只用真实模块：maque::math（Vec2）、maque::handle（实体表）。
 * 确定性：固定 dt、无系统熵；给定 点击序列+dt 逐帧可复现。 */
#pragma once

#include <cstdint>
#include <vector>

#include "core/handle/mq_handle.h"
#include "math/mq_math.h"

namespace ball_demo {

struct Config {
    float gravity        = 980.0F;   // px/s²
    float restitution    = 0.6F;     // 恢复系数
    float radius         = 8.0F;     // 球半径
    float ground_y       = 0.0F;     // 地面高度（球心最低 = ground_y + radius）
    float rest_speed_eps = 20.0F;    // 触地后 |vy| 小于此值视为静止
    float remove_delay   = 5.0F;     // 静止后延迟移除（秒）
    bool  auto_remove    = true;     // 关掉后由外部(脚本)决定移除
};

struct Sprite {
    float    x, y;      // 球心（世界坐标，y 向上）
    float    radius;
    uint32_t color;     // 0xRRGGBB
};

class BallWorld {
public:
    struct BallView { float x, y, vx, vy; bool resting; float rest_time; };

    explicit BallWorld(const Config& cfg = Config{});
    ~BallWorld();
    BallWorld(const BallWorld&) = delete;
    BallWorld& operator=(const BallWorld&) = delete;

    /* 点击屏幕 → 在该点生成一个球（初速度 0，自由落体）。返回实体句柄。 */
    mq_handle on_click(float x, float y);

    /* 固定步长推进（确定性）。 */
    void step(float dt);

    int                  ball_count() const;
    std::vector<Sprite>  sprites() const;            // 渲染数据（只读）
    bool                 get_ball(mq_handle h, BallView& out) const;
    mq_handle            handle_at(uint32_t index) const;   // 供脚本按 id 映射
    void                 remove(mq_handle h);                // 外部(脚本)移除

    const Config& config() const { return cfg_; }

private:
    struct Ball { float x, y, vx, vy; bool resting; float rest_time; };
    Config            cfg_;
    mq_handle_table*  table_;
};

} // namespace ball_demo
