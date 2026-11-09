/* 程序化内建几何（plans/27-threejs-feature-migration.md M1-1）。
 *
 * three.js 的 Box/Sphere/Plane/Cylinder/Cone/Torus/CapsuleGeometry 迁移为
 * `render.MeshRenderer.geometry`（枚举）+ geometry_size/geometry_segments。
 * 输出与 build_cube 同一套 48B 规范顶点（pos3|normal3|tangent4|uv2），
 * 因此可以直接走 gfx 现有 create_mesh / 阴影 / 拾取链，不需要改渲染 ABI。
 *
 * 约定：
 *  - 右手系、y 向上；Cube 与旧内建 cube 同尺寸（size=1.2 → ±0.6）；
 *  - 三角绕序 CCW（外表面），与 build_cube 一致；
 *  - 切线固定 (1,0,0,1)（与 cube 一致；PBR 的 normal map 用屏幕导数构造切线基）；
 *  - UV 的 v 轴按引擎既有约定（Cube/Plane v=0 在底边；球/柱/胶囊取 1-纬度）。
 *  - geometry_segments.x = 径向/宽分段，y = 高/管向分段；Cube 固定 1×1/面。 */
#include "../runtime_engine.h"

namespace maque::rt {

namespace {

constexpr float kPi = 3.14159265358979323846F;

float sane(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
int sane_seg(float v, int lo, int hi) {
    const float f = std::isfinite(v) ? v : (float)lo;
    return std::clamp((int)std::lround(f), lo, hi);
}
float span(float v) { return std::max(std::fabs(sane(v, 1.0F)), 1e-4F); }

struct Builder {
    std::vector<float>& verts;
    std::vector<uint32_t>& idx;

    uint32_t add(float px, float py, float pz, float nx, float ny, float nz, float u, float v) {
        const uint32_t i = (uint32_t)(verts.size() / 12);
        verts.insert(verts.end(), { px, py, pz, nx, ny, nz, 1.0F, 0.0F, 0.0F, 1.0F, u, v });
        return i;
    }
    void tri(uint32_t a, uint32_t b, uint32_t c) { idx.insert(idx.end(), { a, b, c }); }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        idx.insert(idx.end(), { a, b, c, a, c, d });
    }
};

/* 平面网格：origin + du*i/su + dv*j/sv；要求 cross(du,dv) == n。 */
void add_grid(Builder& b, const float o[3], const float du[3], const float dv[3],
              const float n[3], int su, int sv) {
    su = std::max(1, su);
    sv = std::max(1, sv);
    const uint32_t base = (uint32_t)(b.verts.size() / 12);
    for (int j = 0; j <= sv; ++j) {
        const float bv = (float)j / (float)sv;
        for (int i = 0; i <= su; ++i) {
            const float au = (float)i / (float)su;
            b.add(o[0] + du[0] * au + dv[0] * bv,
                  o[1] + du[1] * au + dv[1] * bv,
                  o[2] + du[2] * au + dv[2] * bv,
                  n[0], n[1], n[2], au, bv);
        }
    }
    for (int j = 0; j < sv; ++j) {
        for (int i = 0; i < su; ++i) {
            const uint32_t p0 = base + (uint32_t)(j * (su + 1) + i);
            const uint32_t p1 = p0 + 1;
            const uint32_t p2 = p0 + (uint32_t)(su + 1);
            const uint32_t p3 = p2 + 1;
            b.quad(p0, p1, p3, p2);
        }
    }
}

void build_box(Builder& b, float hx, float hy, float hz) {
    /* 面顺序与 build_cube 一致：+Z,-Z,+X,-X,+Y,-Y；每面 du×dv = 外法线。 */
    struct Face { float n[3]; float o[3]; float du[3]; float dv[3]; };
    const Face faces[6] = {
        { { 0, 0, 1 }, { -hx, -hy, hz }, { 2 * hx, 0, 0 }, { 0, 2 * hy, 0 } },
        { { 0, 0, -1 }, { hx, -hy, -hz }, { -2 * hx, 0, 0 }, { 0, 2 * hy, 0 } },
        { { 1, 0, 0 }, { hx, -hy, hz }, { 0, 0, -2 * hz }, { 0, 2 * hy, 0 } },
        { { -1, 0, 0 }, { -hx, -hy, -hz }, { 0, 0, 2 * hz }, { 0, 2 * hy, 0 } },
        { { 0, 1, 0 }, { -hx, hy, hz }, { 2 * hx, 0, 0 }, { 0, 0, -2 * hz } },
        { { 0, -1, 0 }, { -hx, -hy, -hz }, { 2 * hx, 0, 0 }, { 0, 0, 2 * hz } },
    };
    for (const Face& f : faces) add_grid(b, f.o, f.du, f.dv, f.n, 1, 1);
}

void build_sphere(Builder& b, float r, int slices, int stacks) {
    const uint32_t base = (uint32_t)(b.verts.size() / 12);
    for (int i = 0; i <= stacks; ++i) {
        const float v = (float)i / (float)stacks;
        const float phi = kPi * v;
        const float y = std::cos(phi) * r;
        const float rr = std::sin(phi) * r;
        for (int j = 0; j <= slices; ++j) {
            const float u = (float)j / (float)slices;
            const float th = 2.0F * kPi * u;
            const float x = rr * std::cos(th);
            const float z = rr * std::sin(th);
            b.add(x, y, z, x / r, y / r, z / r, u, 1.0F - v);
        }
    }
    const uint32_t row = (uint32_t)(slices + 1);
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = base + (uint32_t)i * row + (uint32_t)j;
            const uint32_t bb = a + 1;
            const uint32_t c = a + row;
            const uint32_t d = c + 1;
            if (i != 0) b.tri(a, bb, c);
            if (i != stacks - 1) b.tri(bb, d, c);
        }
    }
}

void build_plane(Builder& b, float w, float h, int su, int sv) {
    const float o[3] = { -w * 0.5F, -h * 0.5F, 0.0F };
    const float du[3] = { w, 0.0F, 0.0F };
    const float dv[3] = { 0.0F, h, 0.0F };
    const float n[3] = { 0.0F, 0.0F, 1.0F };
    add_grid(b, o, du, dv, n, su, sv);
}

void build_cylinder(Builder& b, float r, float h, int slices) {
    const float hy = h * 0.5F;
    const uint32_t side = (uint32_t)(b.verts.size() / 12);
    for (int j = 0; j <= slices; ++j) {
        const float u = (float)j / (float)slices;
        const float th = 2.0F * kPi * u;
        const float c = std::cos(th), s = std::sin(th);
        b.add(r * c, hy, r * s, c, 0.0F, s, u, 1.0F);      /* 上环 */
    }
    for (int j = 0; j <= slices; ++j) {
        const float u = (float)j / (float)slices;
        const float th = 2.0F * kPi * u;
        const float c = std::cos(th), s = std::sin(th);
        b.add(r * c, -hy, r * s, c, 0.0F, s, u, 0.0F);     /* 下环 */
    }
    const uint32_t row = (uint32_t)(slices + 1);
    for (int j = 0; j < slices; ++j) {
        const uint32_t a = side + (uint32_t)j;
        const uint32_t bb = a + 1;
        const uint32_t c = a + row;
        const uint32_t d = c + 1;
        b.tri(a, bb, c);
        b.tri(bb, d, c);
    }
    /* 顶盖（+Y）：中心 + 环；三角形 center, θ+1, θ（法线 +Y）。 */
    const uint32_t tc = b.add(0.0F, hy, 0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 0.5F);
    const uint32_t tr = (uint32_t)(b.verts.size() / 12);
    for (int j = 0; j < slices; ++j) {
        const float th = 2.0F * kPi * (float)j / (float)slices;
        const float c = std::cos(th), s = std::sin(th);
        b.add(r * c, hy, r * s, 0.0F, 1.0F, 0.0F, 0.5F + 0.5F * c, 0.5F + 0.5F * s);
    }
    for (int j = 0; j < slices; ++j)
        b.tri(tc, tr + (uint32_t)((j + 1) % slices), tr + (uint32_t)j);
    /* 底盖（-Y）：center, θ, θ+1。 */
    const uint32_t bc = b.add(0.0F, -hy, 0.0F, 0.0F, -1.0F, 0.0F, 0.5F, 0.5F);
    const uint32_t br = (uint32_t)(b.verts.size() / 12);
    for (int j = 0; j < slices; ++j) {
        const float th = 2.0F * kPi * (float)j / (float)slices;
        const float c = std::cos(th), s = std::sin(th);
        b.add(r * c, -hy, r * s, 0.0F, -1.0F, 0.0F, 0.5F + 0.5F * c, 0.5F - 0.5F * s);
    }
    for (int j = 0; j < slices; ++j)
        b.tri(bc, br + (uint32_t)j, br + (uint32_t)((j + 1) % slices));
}

void build_cone(Builder& b, float r, float h, int slices) {
    const float hy = h * 0.5F;
    const float len = std::sqrt(h * h + r * r);
    const float nr = r / len;          /* 法线径向分量 */
    const float ny = h / len;          /* 法线 y 分量 */
    const uint32_t base = (uint32_t)(b.verts.size() / 12);
    for (int j = 0; j < slices; ++j) {
        const float u = (float)j / (float)slices;
        const float u2 = (float)(j + 1) / (float)slices;
        const float th = 2.0F * kPi * u;
        const float th2 = 2.0F * kPi * u2;
        const float c = std::cos(th), s = std::sin(th);
        b.add(r * c, -hy, r * s, nr * c, ny, nr * s, u, 0.0F);
        const uint32_t p1 = b.add(r * std::cos(th2), -hy, r * std::sin(th2),
                                  nr * std::cos(th2), ny, nr * std::sin(th2), u2, 0.0F);
        const float cm = std::cos(0.5F * (th + th2)), sm = std::sin(0.5F * (th + th2));
        const uint32_t ap = b.add(0.0F, hy, 0.0F, nr * cm, ny, nr * sm, u, 1.0F);
        b.tri(base + (uint32_t)(j * 3), ap, p1);
    }
    /* 底盖（-Y）：center, θ, θ+1。 */
    const uint32_t bc = b.add(0.0F, -hy, 0.0F, 0.0F, -1.0F, 0.0F, 0.5F, 0.5F);
    const uint32_t br = (uint32_t)(b.verts.size() / 12);
    for (int j = 0; j < slices; ++j) {
        const float th = 2.0F * kPi * (float)j / (float)slices;
        const float c = std::cos(th), s = std::sin(th);
        b.add(r * c, -hy, r * s, 0.0F, -1.0F, 0.0F, 0.5F + 0.5F * c, 0.5F - 0.5F * s);
    }
    for (int j = 0; j < slices; ++j)
        b.tri(bc, br + (uint32_t)j, br + (uint32_t)((j + 1) % slices));
}

void build_torus(Builder& b, float R, float r, int slices, int tubular) {
    const uint32_t base = (uint32_t)(b.verts.size() / 12);
    for (int i = 0; i <= slices; ++i) {
        const float u = (float)i / (float)slices;
        const float th = 2.0F * kPi * u;
        const float cu = std::cos(th), su = std::sin(th);
        for (int j = 0; j <= tubular; ++j) {
            const float v = (float)j / (float)tubular;
            const float ph = 2.0F * kPi * v;
            const float cv = std::cos(ph), sv = std::sin(ph);
            const float rr = R + r * cv;
            b.add(rr * cu, rr * su, r * sv, cv * cu, cv * su, sv, u, v);
        }
    }
    const uint32_t row = (uint32_t)(tubular + 1);
    for (int i = 0; i < slices; ++i) {
        for (int j = 0; j < tubular; ++j) {
            const uint32_t a = base + (uint32_t)i * row + (uint32_t)j;
            const uint32_t bb = a + 1;          /* v+ */
            const uint32_t c = a + row;         /* u+ */
            const uint32_t d = c + 1;
            /* cross(u+, v+) = 外法线 → quad(a, c, d, b)。 */
            b.quad(a, c, d, bb);
        }
    }
}

void build_capsule(Builder& b, float r, float half_len, int slices, int caps) {
    struct Ring { float y, rad, nrad, ny; };
    std::vector<Ring> rings;
    rings.reserve((size_t)(2 * caps + 2));
    for (int i = 0; i <= caps; ++i) {
        const float phi = 0.5F * kPi * (float)i / (float)caps;
        rings.push_back({ half_len + r * std::cos(phi), r * std::sin(phi),
                          std::sin(phi), std::cos(phi) });
    }
    for (int i = 0; i <= caps; ++i) {
        const float phi = 0.5F * kPi + 0.5F * kPi * (float)i / (float)caps;
        rings.push_back({ -half_len + r * std::cos(phi), r * std::sin(phi),
                          std::sin(phi), std::cos(phi) });
    }
    const uint32_t base = (uint32_t)(b.verts.size() / 12);
    const int nr = (int)rings.size();
    for (int i = 0; i < nr; ++i) {
        const Ring& rg = rings[(size_t)i];
        const float v = (float)i / (float)(nr - 1);
        for (int j = 0; j <= slices; ++j) {
            const float u = (float)j / (float)slices;
            const float th = 2.0F * kPi * u;
            const float c = std::cos(th), s = std::sin(th);
            b.add(rg.rad * c, rg.y, rg.rad * s,
                  rg.nrad * c, rg.ny, rg.nrad * s, u, 1.0F - v);
        }
    }
    const uint32_t row = (uint32_t)(slices + 1);
    for (int i = 0; i < nr - 1; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = base + (uint32_t)i * row + (uint32_t)j;
            const uint32_t bb = a + 1;
            const uint32_t c = a + row;
            const uint32_t d = c + 1;
            if (i != 0) b.tri(a, bb, c);
            if (i != nr - 2) b.tri(bb, d, c);
        }
    }
}

}  // namespace

void build_geometry(int32_t kind, const float size[3], const float seg[3],
                    std::vector<float>& verts, std::vector<uint32_t>& indices) {
    verts.clear();
    indices.clear();
    Builder b{ verts, indices };
    const float sx = span(size ? size[0] : 1.0F);
    const float sy = span(size ? size[1] : 1.0F);
    const float sz = span(size ? size[2] : 1.0F);
    const float seg_x = seg ? seg[0] : 32.0F;
    const float seg_y = seg ? seg[1] : 16.0F;
    switch ((render::MeshGeometry)kind) {
        case render::MeshGeometry::Cube:
            build_box(b, sx * 0.5F, sy * 0.5F, sz * 0.5F);
            break;
        case render::MeshGeometry::Sphere:
            build_sphere(b, sx, sane_seg(seg_x, 3, 256), sane_seg(seg_y, 2, 128));
            break;
        case render::MeshGeometry::Plane:
            build_plane(b, sx, sy, sane_seg(seg_x, 1, 256), sane_seg(seg_y, 1, 256));
            break;
        case render::MeshGeometry::Cylinder:
            build_cylinder(b, sx, sy, sane_seg(seg_x, 3, 256));
            break;
        case render::MeshGeometry::Cone:
            build_cone(b, sx, sy, sane_seg(seg_x, 3, 256));
            break;
        case render::MeshGeometry::Torus:
            build_torus(b, sx, std::max(sy, 1e-4F), sane_seg(seg_x, 3, 256),
                        sane_seg(seg_y, 3, 128));
            break;
        case render::MeshGeometry::Capsule:
            build_capsule(b, sx,
                          0.5F * std::fabs(sane(size ? size[1] : 0.0F, 0.0F)),
                          sane_seg(seg_x, 3, 256), sane_seg(seg_y, 2, 64));
            break;
        default:
            build_box(b, 0.6F, 0.6F, 0.6F);
            break;
    }
}

void primitive_bounds(int32_t kind, const float size[3], float out_min[3], float out_max[3]) {
    const float sx = span(size ? size[0] : 1.0F);
    const float sy = span(size ? size[1] : 1.0F);
    const float sz = span(size ? size[2] : 1.0F);
    float mn[3] = { -sx * 0.5F, -sy * 0.5F, -sz * 0.5F };
    float mx[3] = { sx * 0.5F, sy * 0.5F, sz * 0.5F };
    switch ((render::MeshGeometry)kind) {
        case render::MeshGeometry::Cube:
            break;
        case render::MeshGeometry::Sphere:
            mn[0] = mn[1] = mn[2] = -sx;
            mx[0] = mx[1] = mx[2] = sx;
            break;
        case render::MeshGeometry::Plane:
            mn[0] = -sx * 0.5F; mx[0] = sx * 0.5F;
            mn[1] = -sy * 0.5F; mx[1] = sy * 0.5F;
            mn[2] = mx[2] = 0.0F;
            break;
        case render::MeshGeometry::Cylinder:
        case render::MeshGeometry::Cone:
            mn[0] = mn[2] = -sx; mx[0] = mx[2] = sx;
            mn[1] = -sy * 0.5F; mx[1] = sy * 0.5F;
            break;
        case render::MeshGeometry::Torus:
            mn[0] = mn[1] = -(sx + sy);
            mx[0] = mx[1] = (sx + sy);
            mn[2] = -sy; mx[2] = sy;
            break;
        case render::MeshGeometry::Capsule: {
            const float hl = std::fabs(sane(size ? size[1] : 0.0F, 0.0F)) * 0.5F;
            mn[0] = mn[2] = -sx; mx[0] = mx[2] = sx;
            mn[1] = -hl - sx; mx[1] = hl + sx;
            break;
        }
        default:
            break;
    }
    for (int i = 0; i < 3; ++i) {
        out_min[i] = mn[i];
        out_max[i] = mx[i];
    }
}

int32_t Engine::load_primitive(const render::MeshRenderer& mesh) {
    if (!rapi || !rself || !rapi->create_mesh) return -1;
    PrimitiveKey key;
    key.kind = (int32_t)mesh.geometry;
    /* NaN/Inf 先归一（否则 key 永远 find 不到，脚本写入坏值会逐帧创建网格直到缓存上限）。 */
    key.size[0] = sane(mesh.geometry_size.x, 1.2F);
    key.size[1] = sane(mesh.geometry_size.y, 1.2F);
    key.size[2] = sane(mesh.geometry_size.z, 1.2F);
    key.seg[0] = sane(mesh.geometry_segments.x, 32.0F);
    key.seg[1] = sane(mesh.geometry_segments.y, 16.0F);
    key.seg[2] = sane(mesh.geometry_segments.z, 8.0F);
    auto it = primitive_meshes.find(key);
    if (it != primitive_meshes.end()) return it->second;
    /* 参数组合上限（正常场景远小于此；512 = 8 车 × 多种图元 + 场景道具仍有富余）。
     * 超限回退内建 cube，防止脚本逐帧改参导致句柄无限增长。 */
    if (primitive_meshes.size() >= 512) {
        if (!primitive_cap_warned) {
            primitive_cap_warned = true;
            std::print(stderr, "runtime: 程序化图元缓存已达上限 512，后续组合回退内建 cube\n");
        }
        return -1;
    }

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    build_geometry(key.kind, key.size, key.seg, verts, indices);
    if (verts.empty() || indices.empty()) {
        primitive_meshes[key] = -1;
        return -1;
    }
    mq_renderer_mesh_gpu md{};
    md.vertices = verts.data();
    md.vertex_bytes = (uint64_t)verts.size() * sizeof(float);
    md.vertex_stride = 48;
    md.vertex_layout = 0;
    md.indices = indices.data();
    md.index_count = (uint32_t)indices.size();
    primitive_bounds(key.kind, key.size, md.bounds_min, md.bounds_max);
    int32_t id = -1;
    if (rapi->create_mesh(rself, &md, &id) != 0 || id < 0) {
        primitive_meshes[key] = -1;
        std::print(stderr, "runtime: primitive geometry={} 创建失败（size={:.3f},{:.3f},{:.3f}）\n",
                   key.kind, (double)key.size[0], (double)key.size[1], (double)key.size[2]);
        return -1;
    }
    primitive_meshes[key] = id;
    mesh_stats[id] = { (uint32_t)(verts.size() / 12), (uint32_t)indices.size() };
    std::print("runtime: primitive geometry={} size=({:.3f},{:.3f},{:.3f}) "
               "seg=({:.0f},{:.0f},{:.0f}) verts={} tris={} -> mesh={}\n",
               key.kind, (double)key.size[0], (double)key.size[1], (double)key.size[2],
               (double)key.seg[0], (double)key.seg[1], (double)key.seg[2],
               (long long)(verts.size() / 12), (long long)(indices.size() / 3), (long long)id);
    return id;
}

}  // namespace maque::rt
