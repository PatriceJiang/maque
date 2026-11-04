/* 3D 数学与内建几何（列主序 mat4 / 右手系、WebGPU NDC z∈[0,1]；plans/18 §M2）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S6，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ============================================================================
 * M2 3D 基础（plans/18-pbr-3d.md M2）：render.* 组件 → mq_renderer_frame3d
 *   · 矩阵列主序；右手系、Y 向上、相机看 -Z；WebGPU NDC z∈[0,1]
 *   · M2 光照 = 方向光 Lambert + 环境色（PBR 在 M3）；所有 mesh 共用内建 cube（M4 换资产）
 *   · 提取只读（渲染不回写模拟，ADR-0005）
 * ==========================================================================*/

/* 局部 TRS（Euler XYZ 弧度）→ 列主序 mat4；R = Rz * Ry * Rx（与 docs/components/02 约定一致）。 */

/* 透视投影（右手系，WebGPU NDC z∈[0,1]）。 */

/* 正交投影（ortho_height = 垂直可见范围；右手系，WebGPU z∈[0,1]）。 */

/* 世界矩阵的旋转轴（列 c）：单位化后返回。列长≈1 时**不做除法** ——
 * 保证平级场景（world_m = m4_from_trs(local)，scale=1）与旧的 Euler 旋转逐字节一致；
 * 真正带缩放的节点（列长明显偏离 1）才归一化（相机/灯光忽略缩放）。 */

/* 相机世界矩阵 → view（R^T * T(-pos)）；平级场景（scale=1）与旧 m4_view(trs) 逐字节一致。 */

/* 四元数 (x,y,z,w) + 平移/缩放 → 列主序 mat4（glTF rotation 轨道；无万向锁）。 */

/* glTF 节点局部 TRS（四元数）→ 列主序 mat4；T * R * S（glTF 规范）。 */

void read_color(const mq_type_desc* d, const void* base, const char* field, math::Color* out) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (f) std::memcpy(out, field_ptr(f, (const uint8_t*)base), sizeof *out);
}

/* 内建 cube：24 顶点（每面 4 个，法线独立）/ 36 索引；48B 规范布局（pos3|normal3|tangent4|uv2）。
 * M2 用它验证 depth/相机/提取/直呈链路；M4 由 glTF mesh 资产取代。 */
void build_cube(std::vector<float>& verts, std::vector<uint32_t>& indices) {
    const float s = 0.6F;
    struct Face { float n[3]; float p[4][3]; };
    static const Face faces[6] = {
        { { 0, 0, 1 }, { { -s, -s, s }, { s, -s, s }, { s, s, s }, { -s, s, s } } },
        { { 0, 0, -1 }, { { s, -s, -s }, { -s, -s, -s }, { -s, s, -s }, { s, s, -s } } },
        { { 1, 0, 0 }, { { s, -s, s }, { s, -s, -s }, { s, s, -s }, { s, s, s } } },
        { { -1, 0, 0 }, { { -s, -s, -s }, { -s, -s, s }, { -s, s, s }, { -s, s, -s } } },
        { { 0, 1, 0 }, { { -s, s, s }, { s, s, s }, { s, s, -s }, { -s, s, -s } } },
        { { 0, -1, 0 }, { { -s, -s, -s }, { s, -s, -s }, { s, -s, s }, { -s, -s, s } } },
    };
    verts.clear();
    indices.clear();
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = (uint32_t)verts.size() / 12;
        for (int i = 0; i < 4; ++i) {
            /* 每面 4 角 UV：0=(-s,-s) 1=(s,-s) 2=(s,s) 3=(-s,s)（贴图 PBR 用；M5 法线贴图同 UV） */
            const float u = (i == 0 || i == 3) ? 0.0F : 1.0F;
            const float v = (i >= 2) ? 1.0F : 0.0F;
            verts.insert(verts.end(), { faces[f].p[i][0], faces[f].p[i][1], faces[f].p[i][2],
                                        faces[f].n[0], faces[f].n[1], faces[f].n[2],
                                        1.0F, 0.0F, 0.0F, 1.0F, u, v });
        }
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

}  // namespace maque::rt
