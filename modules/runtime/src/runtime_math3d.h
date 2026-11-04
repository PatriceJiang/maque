/* 3D 数学助手（原 runtime.cpp 的 static m4_*）：跨 TU 复用 → 头内 inline。
 * 列主序 mat4；右手系、Y 向上、相机看 -Z；WebGPU NDC z∈[0,1]（plans/18 §M2）。 */
#pragma once

#include "runtime_common.h"

namespace maque::rt {

inline void m4_identity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0F;
}

inline void m4_from_trs(const math::Transform3D& t, float* m) {
    const float cx = std::cos(t.rotation.x), sx = std::sin(t.rotation.x);
    const float cy = std::cos(t.rotation.y), sy = std::sin(t.rotation.y);
    const float cz = std::cos(t.rotation.z), sz = std::sin(t.rotation.z);
    const float r00 = cy * cz, r10 = cy * sz, r20 = -sy;
    const float r01 = sx * sy * cz - cx * sz, r11 = sx * sy * sz + cx * cz, r21 = sx * cy;
    const float r02 = cx * sy * cz + sx * sz, r12 = cx * sy * sz - sx * cz, r22 = cx * cy;
    m[0] = r00 * t.scale.x; m[1] = r10 * t.scale.x; m[2] = r20 * t.scale.x; m[3] = 0.0F;
    m[4] = r01 * t.scale.y; m[5] = r11 * t.scale.y; m[6] = r21 * t.scale.y; m[7] = 0.0F;
    m[8] = r02 * t.scale.z; m[9] = r12 * t.scale.z; m[10] = r22 * t.scale.z; m[11] = 0.0F;
    m[12] = t.position.x; m[13] = t.position.y; m[14] = t.position.z; m[15] = 1.0F;
}

inline void m4_perspective(float fovy_rad, float aspect, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    const float f = 1.0F / std::tan(fovy_rad * 0.5F);
    m[0] = f / aspect; m[5] = f;
    m[10] = zf / (zn - zf); m[11] = -1.0F; m[14] = (zn * zf) / (zn - zf);
}

inline void m4_orthographic(float height, float aspect, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    const float hh = std::max(height * 0.5F, 1e-5F);
    const float hw = std::max(hh * aspect, 1e-5F);
    m[0] = 1.0F / hw;
    m[5] = 1.0F / hh;
    m[10] = 1.0F / (zn - zf);
    m[14] = zn / (zn - zf);
    m[15] = 1.0F;
}

inline void m4_axis(const float* m, int c, float out[3]) {
    float x = m[c * 4], y = m[c * 4 + 1], z = m[c * 4 + 2];
    const float len2 = x * x + y * y + z * z;
    if (std::fabs(std::sqrt(len2) - 1.0F) > 1.0e-4F) {
        const float inv = len2 > 0.0F ? 1.0F / std::sqrt(len2) : 1.0F;
        x *= inv; y *= inv; z *= inv;
    }
    out[0] = x; out[1] = y; out[2] = z;
}

inline void m4_view_from_m(const float* wm, float* m) {
    float R[16];
    for (int c = 0; c < 3; ++c) m4_axis(wm, c, R + c * 4);
    R[3] = R[7] = R[11] = 0.0F;
    R[12] = R[13] = R[14] = 0.0F; R[15] = 1.0F;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m[c * 4 + r] = R[r * 4 + c];
    m[3] = 0.0F; m[7] = 0.0F; m[11] = 0.0F; m[15] = 1.0F;
    const float px = wm[12], py = wm[13], pz = wm[14];
    m[12] = -(m[0] * px + m[4] * py + m[8] * pz);
    m[13] = -(m[1] * px + m[5] * py + m[9] * pz);
    m[14] = -(m[2] * px + m[6] * py + m[10] * pz);
}

inline void m4_from_quat_trs(const math::Vec3& pos, const float q_in[4], const math::Vec3& scale, float* m) {
    float q[4] = { q_in[0], q_in[1], q_in[2], q_in[3] };
    const float qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (qlen > 0.0F) { const float inv = 1.0F / qlen; for (int i = 0; i < 4; ++i) q[i] *= inv; }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1.0F - 2.0F * (yy + zz)) * scale.x; m[1] = (2.0F * (xy + wz)) * scale.x;
    m[2] = (2.0F * (xz - wy)) * scale.x; m[3] = 0.0F;
    m[4] = (2.0F * (xy - wz)) * scale.y; m[5] = (1.0F - 2.0F * (xx + zz)) * scale.y;
    m[6] = (2.0F * (yz + wx)) * scale.y; m[7] = 0.0F;
    m[8] = (2.0F * (xz + wy)) * scale.z; m[9] = (2.0F * (yz - wx)) * scale.z;
    m[10] = (1.0F - 2.0F * (xx + yy)) * scale.z; m[11] = 0.0F;
    m[12] = pos.x; m[13] = pos.y; m[14] = pos.z; m[15] = 1.0F;
}

inline void m4_mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                           a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

inline void m4_from_qtrs(const float t[3], const float q[4], const float s[3], float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1.0F - 2.0F * (yy + zz)) * s[0];
    m[1] = (2.0F * (xy + wz)) * s[0];
    m[2] = (2.0F * (xz - wy)) * s[0];
    m[3] = 0.0F;
    m[4] = (2.0F * (xy - wz)) * s[1];
    m[5] = (1.0F - 2.0F * (xx + zz)) * s[1];
    m[6] = (2.0F * (yz + wx)) * s[1];
    m[7] = 0.0F;
    m[8] = (2.0F * (xz + wy)) * s[2];
    m[9] = (2.0F * (yz - wx)) * s[2];
    m[10] = (1.0F - 2.0F * (xx + yy)) * s[2];
    m[11] = 0.0F;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0F;
}

}  // namespace maque::rt
