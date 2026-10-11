/* maque 数学库 —— 对 codegen 生成的 POD 类型 (maque_types.h) 的确定性操作。
 * 纯函数、无状态、可内联；布局即 ABI（docs/06/07）。
 * 约定：Mat4 列主序 m[col*4+row]；欧拉角 XYZ 弧度。
 *
 * ⚠️ **函数声明由 DSL 生成**（docs/21 §1）：单一真相 = schema.yaml 的 `module_abi`
 *   （math 段，language: cxx）；生成物 = build/modules/core/data/generated/abi/math.h。
 *   本文件只保留**运算符与 inline 实现**。 */
#pragma once

#include "abi/math.h"     /* namespace math 的声明 + kPi 常量（生成） */

namespace math {

/* ---------------- Vec2 ---------------- */
inline Vec2 operator+(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(const Vec2& a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, const Vec2& a) { return {a.x * s, a.y * s}; }
inline Vec2& operator+=(Vec2& a, const Vec2& b) { a.x += b.x; a.y += b.y; return a; }
inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float length_sq(const Vec2& a) { return dot(a, a); }
inline float length(const Vec2& a) { return std::sqrt(length_sq(a)); }
inline Vec2 lerp(const Vec2& a, const Vec2& b, float t) { return a + (b - a) * t; }
inline Vec2 normalize(const Vec2& a) { float l = length(a); return l > 0.0F ? a * (1.0F / l) : Vec2{}; }

/* ---------------- Vec3 ---------------- */
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, const Vec3& a) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length_sq(const Vec3& a) { return dot(a, a); }
inline float length(const Vec3& a) { return std::sqrt(length_sq(a)); }
inline float distance(const Vec3& a, const Vec3& b) { return length(b - a); }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 normalize(const Vec3& a) { float l = length(a); return l > 0.0F ? a * (1.0F / l) : Vec3{}; }

/* ---------------- Vec4 ---------------- */
inline Vec4 operator+(const Vec4& a, const Vec4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 operator-(const Vec4& a, const Vec4& b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 operator*(const Vec4& a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float dot(const Vec4& a, const Vec4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Vec4 lerp(const Vec4& a, const Vec4& b, float t) { return a + (b - a) * t; }

/* ---------------- Quat (x,y,z,w) ---------------- */
inline Quat quat_identity() { return {0.0F, 0.0F, 0.0F, 1.0F}; }
inline Quat operator*(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}
inline Quat quat_conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat quat_normalize(const Quat& q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l <= 0.0F) return quat_identity();
    float inv = 1.0F / l;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}
inline Quat quat_from_axis_angle(const Vec3& axis, float radians) {
    Vec3 a = normalize(axis);
    float h = radians * 0.5F, s = std::sin(h);
    return {a.x * s, a.y * s, a.z * s, std::cos(h)};
}
inline Vec3 quat_rotate(const Quat& q, const Vec3& v) {
    /* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v) */
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = cross(u, v) + v * q.w;
    return v + cross(u, t) * 2.0F;
}
inline Quat quat_slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0F) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995F) {
        return quat_normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                               a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    float theta0 = std::acos(d);
    float theta = theta0 * t;
    float st = std::sin(theta), st0 = std::sin(theta0);
    float wa = std::cos(theta) - d * st / st0;
    float wb = st / st0;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

/* ---------------- Mat4 (列主序 m[col*4+row]) ---------------- */
inline Mat4 mat4_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0F;
    return r;
}
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0F;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
inline Vec4 operator*(const Mat4& a, const Vec4& v) {
    Vec4 r{};
    float in[4] = {v.x, v.y, v.z, v.w};
    float out[4] = {0, 0, 0, 0};
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) out[row] += a.m[k * 4 + row] * in[k];
    r = {out[0], out[1], out[2], out[3]};
    return r;
}
inline Vec3 transform_point(const Mat4& a, const Vec3& p) {
    Vec4 r = a * Vec4{p.x, p.y, p.z, 1.0F};
    if (r.w != 0.0F && r.w != 1.0F) { float iw = 1.0F / r.w; r.x *= iw; r.y *= iw; r.z *= iw; }
    return {r.x, r.y, r.z};
}
inline Vec3 transform_vector(const Mat4& a, const Vec3& v) {
    Vec4 r = a * Vec4{v.x, v.y, v.z, 0.0F};
    return {r.x, r.y, r.z};
}
inline Mat4 mat4_translation(const Vec3& t) {
    Mat4 r = mat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
inline Mat4 mat4_scale(const Vec3& s) {
    Mat4 r = mat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}
inline Mat4 mat4_from_quat(const Quat& q) {
    Quat n = quat_normalize(q);
    float x = n.x, y = n.y, z = n.z, w = n.w;
    Mat4 r = mat4_identity();
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + z * w); r.m[2]  = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);     r.m[5] = 1 - 2 * (x * x + z * z); r.m[6]  = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);     r.m[9] = 2 * (y * z - x * w);     r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}
inline Mat4 mat4_from_transform(const Transform3D& t) {
    /* M = T * Ry * Rx * Rz * S  (欧拉 XYZ，弧度) */
    Quat qx = quat_from_axis_angle({1, 0, 0}, t.rotation.x);
    Quat qy = quat_from_axis_angle({0, 1, 0}, t.rotation.y);
    Quat qz = quat_from_axis_angle({0, 0, 1}, t.rotation.z);
    Quat q = quat_normalize(qy * (qx * qz));
    return mat4_translation(t.position) * (mat4_from_quat(q) * mat4_scale(t.scale));
}

/* ---------------- Transform3D 便利 ---------------- */
inline Vec3 to_euler_xyz(const Quat& q) {
    /* 返回 XYZ 欧拉角（弧度），适配 mat4_from_transform 的约定。 */
    float sinp = 2.0F * (q.w * q.x + q.y * q.z);
    float x = std::asin(sinp > 1.0F ? 1.0F : (sinp < -1.0F ? -1.0F : sinp));
    float y = std::atan2(2.0F * (q.w * q.y - q.z * q.x), 1.0F - 2.0F * (q.x * q.x + q.y * q.y));
    float z = std::atan2(2.0F * (q.w * q.z - q.x * q.y), 1.0F - 2.0F * (q.x * q.x + q.z * q.z));
    return {x, y, z};
}

} // namespace math
