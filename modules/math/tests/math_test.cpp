// maque 数学库单元测试（独立：仅依赖 maque::math + 生成的类型）
#include "math/mq_math.h"
#include <print>

#include <cstdio>
#include <cmath>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
static bool near(float a, float b, float eps = 1e-4F) { return std::fabs(a - b) < eps; }
static bool vnear(const math::Vec3& a, const math::Vec3& b, float eps = 1e-4F) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

int main() {
    using namespace math;

    // Vec3 基本
    Vec3 a{1, 2, 3}, b{4, 5, 6};
    CHECK(dot(a, b) == 32.0F);
    CHECK(vnear(cross({1, 0, 0}, {0, 1, 0}), {0, 0, 1}));
    CHECK(near(length({3, 4, 0}), 5.0F));
    CHECK(vnear(normalize({0, 0, 5}), {0, 0, 1}));
    CHECK(vnear(lerp(Vec3{0, 0, 0}, Vec3{10, 0, 0}, 0.25F), {2.5F, 0, 0}));
    CHECK(near(distance({0, 0, 0}, {0, 3, 4}), 5.0F));

    // Quat：绕 Z 轴 90° 旋转 (1,0,0) → (0,1,0)
    Quat qz = quat_from_axis_angle({0, 0, 1}, kPi * 0.5F);
    CHECK(vnear(quat_rotate(qz, {1, 0, 0}), {0, 1, 0}));
    // 单位四元数不改变向量
    CHECK(vnear(quat_rotate(quat_identity(), a), a));
    // 乘法：q * q^-1 == identity（旋转向量的意义下）
    CHECK(vnear(quat_rotate(qz * quat_conjugate(qz), a), a));
    // slerp 端点
    Quat qx = quat_from_axis_angle({1, 0, 0}, kPi * 0.5F);
    CHECK(vnear(quat_rotate(quat_slerp(qz, qx, 0.0F), {1, 0, 0}),
                quat_rotate(qz, {1, 0, 0})));

    // Mat4：单位、平移、缩放
    CHECK(vnear(transform_point(mat4_identity(), {1, 2, 3}), {1, 2, 3}));
    CHECK(vnear(transform_point(mat4_translation({10, 0, 0}), {1, 2, 3}), {11, 2, 3}));
    CHECK(vnear(transform_point(mat4_scale({2, 2, 2}), {1, 2, 3}), {2, 4, 6}));
    // 向量不受平移影响
    CHECK(vnear(transform_vector(mat4_translation({10, 0, 0}), {1, 2, 3}), {1, 2, 3}));

    // 组合：T*R*S —— 缩放 2，绕 Z 转 90°，平移 (1,0,0)
    Mat4 m = mat4_translation({1, 0, 0}) * (mat4_from_quat(qz) * mat4_scale({2, 2, 2}));
    // 点 (1,0,0) -> 缩放(2,0,0) -> 旋转(0,2,0) -> 平移(1,2,0)
    CHECK(vnear(transform_point(m, {1, 0, 0}), {1, 2, 0}));

    // Transform3D 组合（无旋转）
    Transform3D t{};
    t.position = {5, 0, 0}; t.scale = {2, 2, 2};
    CHECK(vnear(transform_point(mat4_from_transform(t), {1, 0, 0}), {7, 0, 0}));

    // 确定性：同输入同输出
    CHECK(vnear(transform_point(m, {1, 0, 0}), transform_point(m, {1, 0, 0})));

    if (g_failures == 0) { std::print("math_test: all checks passed\n"); return 0; }
    std::print("math_test: {} failure(s)\n", g_failures);
    return 1;
}
