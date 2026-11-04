/* 资源 · 纹理/网格/材质（含 sRGB mip、sampler 路径、路径缓存；docs/13、plans/20）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S4，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 贴图路径 sampler 后缀（plans/20：glTF sampler 的 wrap/filter 落到 VFS 路径上）----
 * 路径形如 `textures/x.png#wrap=clamp,mirror&filter=nearest`：
 *   · '#' 之前是真正的 VFS 路径（读文件/解码用它）；
 *   · wrap=repeat|clamp|mirror → create_texture2 的 wrap_u/wrap_v（0/1/2）；可写一个（u=v）或 "u,v"；
 *   · filter=nearest|linear → create_texture2 的 filter（0/1）；
 *   · 无 '#'：文件贴图默认 repeat+linear（glTF sampler 缺省）；builtin 贴图由调用方默认 clamp。
 * 缓存 key 用**完整字符串**：同一张图配不同 sampler 是两个独立的 GPU 纹理对象。 */
static int wrap_mode_code(const std::string& s) {
    if (s == "repeat") return 0;
    if (s == "clamp") return 1;
    if (s == "mirror") return 2;
    return -1;                       /* 未知值 → 忽略该项（用默认） */
}

TexPathParams parse_tex_path(const std::string& ref) {
    TexPathParams p;
    p.vfs = ref;
    const size_t hash = ref.find('#');
    if (hash == std::string::npos) return p;
    p.vfs.assign(ref, 0, hash);
    const std::string query = ref.substr(hash + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string key = kv.substr(0, eq);
            const std::string val = kv.substr(eq + 1);
            if (key == "wrap") {
                const size_t comma = val.find(',');
                const int u = wrap_mode_code(comma == std::string::npos ? val : val.substr(0, comma));
                const int v = wrap_mode_code(comma == std::string::npos ? val : val.substr(comma + 1));
                if (u >= 0) { p.wrap_u = u; p.wrap_v = (v >= 0 ? v : u); p.wrap_set = true; }
            } else if (key == "filter") {
                if (val == "nearest") { p.filter = 0; p.filter_set = true; }
                else if (val == "linear") { p.filter = 1; p.filter_set = true; }
            }
        }
        pos = amp + 1;
    }
    return p;
}

std::string Engine::scoped_texture_ref(int entity, const std::string& ref) const {
    static constexpr const char* kRt = "rendertexture://";
    if (ref.rfind(kRt, 0) != 0) return ref;
    const std::string tail = ref.substr(std::strlen(kRt));
    if (tail.find('/') != std::string::npos) return ref;       /* 已是规范 URI */
    auto it = entity_instance.find(entity);
    const int owner = it == entity_instance.end() ? 0 : it->second;
    return std::string(kRt) + std::to_string(owner) + "/" + tail;
}

int Engine::load_texture(const std::string& path) {
    auto it = tex_cache.find(path);
    if (it != tex_cache.end()) return it->second;
    /* 旧 create_texture 没有 sampler 参数：这里只把 '#' 后缀剥掉再读文件（sampler 由 2D 全局采样器决定）；
     * PBR 贴图走 load_texture_srgb/mips → create_texture2 才真正生效。 */
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int id = rapi->create_texture(rself, rgba, (int)w, (int)h);
    mq_image_free(rgba);
    tex_cache[path] = id;
    return id;
}

/* plans/19：CPU box-filter mip 链（RGBA8，各层连续存放）。
 * 无 mip 时高频贴图（布纹/车漆细节）在远处会强烈闪烁/摩尔纹——PBR 正确性的重要一环。
 * srgb=true（M4g albedo/emissive）：RGB 必须**先解码到线性再平均、再编码回 sRGB**——
 * 直接在 sRGB 编码字节上平均会让明暗混合显著偏暗（黑白棋盘 2×2 的线性均值 0.5 →
 * 编码域平均只剩 ~0.214，线性亮度 -57%）。alpha 始终是线性数据，直接平均。
 * 线性数据贴图（MR/AO/normal）走 srgb=false，与旧实现逐字节一致。 */
static void srgb_mip_tables(float* decode, float* encode_thr) {
    for (int i = 0; i < 256; ++i) {
        const double c = (double)i / 255.0;
        decode[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    /* encode_thr[b] = 线性域中「编码值恰为 (b+0.5)/255」的门槛：值 >= 门槛即进位到 b+1，
     * 二分即得 round(255*srgb(linear))，避免逐像素 pow。 */
    for (int b = 0; b < 255; ++b) {
        const double s = ((double)b + 0.5) / 255.0;
        encode_thr[b] = (float)(s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4));
    }
}

static uint8_t srgb_encode_from_linear(float linear, const float* encode_thr) {
    if (linear <= 0.0F) return 0;
    if (linear >= 1.0F) return 255;
    int lo = 0, hi = 255;                       /* 找第一个 > linear 的门槛 → 输出 lo */
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (linear < encode_thr[mid]) hi = mid; else lo = mid + 1;
    }
    return (uint8_t)lo;
}

static std::vector<uint8_t> build_mip_chain(const uint8_t* src, int w0, int h0, int& levels,
                                            bool srgb) {
    levels = 0;
    std::vector<uint8_t> out;
    if (!src || w0 <= 0 || h0 <= 0) return out;
    out.assign(src, src + (size_t)w0 * (size_t)h0 * 4u);
    levels = 1;
    float decode[256];
    float encode_thr[255];
    if (srgb) srgb_mip_tables(decode, encode_thr);
    int w = w0, h = h0;
    size_t prev_off = 0;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const size_t dst_off = out.size();
        out.resize(dst_off + (size_t)nw * (size_t)nh * 4u);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                int acc[4] = { 0, 0, 0, 0 };
                float lin[3] = { 0.0F, 0.0F, 0.0F };
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(w - 1, x * 2 + dx);
                        const int sy = std::min(h - 1, y * 2 + dy);
                        const uint8_t* p = &out[prev_off + ((size_t)sy * (size_t)w + (size_t)sx) * 4u];
                        if (srgb) {
                            for (int c = 0; c < 3; ++c) lin[c] += decode[p[c]];
                            acc[3] += p[3];
                        } else {
                            for (int c = 0; c < 4; ++c) acc[c] += p[c];
                        }
                    }
                }
                uint8_t* q = &out[dst_off + ((size_t)y * (size_t)nw + (size_t)x) * 4u];
                if (srgb) {
                    for (int c = 0; c < 3; ++c) q[c] = srgb_encode_from_linear(lin[c] * 0.25F, encode_thr);
                    q[3] = (uint8_t)((acc[3] + 2) / 4);
                } else {
                    for (int c = 0; c < 4; ++c) q[c] = (uint8_t)((acc[c] + 2) / 4);
                }
            }
        }
        prev_off = dst_off;
        w = nw;
        h = nh;
        ++levels;
    }
    return out;
}

int Engine::load_texture_srgb(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    /* builtin: 是程序化纹理（checker 等），沿用既有 UNorm 路径，不做 sRGB 解码 */
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "srgb:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print("runtime: srgb texture not found '{}'\n", path);
        return -1;
    }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: srgb decode failed '{}': {}\n", path, err); return -1; }
    int32_t id = -1;
    if (rapi && rself && rapi->create_texture2 && w > 0 && h > 0) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, /*srgb=*/true);
        mq_renderer_texture_desc td{};
        td.width = (int32_t)w; td.height = (int32_t)h;
        td.pixel_format = 1;                  /* 1 = RGBA8_SRGB */
        td.kind = 0; td.mip_levels = levels;
        td.filter = tp.filter;                /* glTF sampler 后缀（缺省 linear） */
        td.wrap_u = tp.wrap_u; td.wrap_v = tp.wrap_v;   /* 缺省 repeat */
        td.data = mips.data(); td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    mq_image_free(rgba);
    if (id < 0) std::print("runtime: srgb texture upload failed '{}'\n", path);
    tex_cache[key] = id;
    return id;
}

/* plans/19：线性数据贴图（MR/AO/normal/clearcoat normal）+ CPU mip 链；采样 RGBA8Unorm。 */
int Engine::load_texture_mips(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "mip:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print("runtime: texture not found '{}'\n", path);
        return -1;
    }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int32_t id = -1;
    if (rapi && rself && rapi->create_texture2 && w > 0 && h > 0) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, /*srgb=*/false);
        mq_renderer_texture_desc td{};
        td.width = (int32_t)w; td.height = (int32_t)h;
        td.pixel_format = 0;                  /* 0 = RGBA8（线性数据） */
        td.kind = 0; td.mip_levels = levels;
        td.filter = tp.filter;                /* glTF sampler 后缀（缺省 linear） */
        td.wrap_u = tp.wrap_u; td.wrap_v = tp.wrap_v;   /* 缺省 repeat */
        td.data = mips.data(); td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    mq_image_free(rgba);
    if (id < 0) id = load_texture(path);      /* create_texture2 不可用时退回无 mip 路径（内部会剥后缀） */
    tex_cache[key] = id;
    return id;
}

int Engine::load_texture_ref(const std::string& ref) {
    const TexPathParams tp = parse_tex_path(ref);
    if (tp.vfs.rfind("builtin:", 0) == 0) {
        auto it = tex_cache.find(ref);
        if (it != tex_cache.end()) return it->second;
        int w = 1, h = 1;
        std::vector<uint8_t> px;
        int builtin_mips = 1;
        int builtin_format = 0;             /* 0=RGBA8Unorm 1=RGBA8UnormSrgb */
        int builtin_wrap_u = -1, builtin_wrap_v = -1;
        if (tp.vfs == "builtin:/textures/checker") {
            w = h = 16; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                bool alt = (((x / 4) + (y / 4)) & 1) != 0;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = alt ? 255 : 230; c[1] = alt ? 255 : 140; c[2] = alt ? 255 : 60; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/toon_ramp") {
            /* Toon 渐变：256×1 四档硬阶（与 toon_steps=4 对应；线性 clamp 采样） */
            w = 256; h = 1; px.resize((size_t)w * 4);
            static const uint8_t steps[4] = { 64, 132, 200, 255 };
            for (int x = 0; x < w; ++x) {
                const uint8_t v = steps[std::min(3, x * 4 / w)];
                uint8_t* c = &px[(size_t)x * 4]; c[0] = v; c[1] = v; c[2] = v; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/matcap_studio") {
            /* Matcap：256×256 冷色相影棚球（左上高光、右下暗），线性采样/clamp；用作 view-space 法线查表 */
            w = h = 256; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const float nx = (float(x) + 0.5F) / w * 2.0F - 1.0F;
                const float ny = (float(y) + 0.5F) / h * 2.0F - 1.0F;
                const float r2 = nx * nx + ny * ny;
                const float nz = std::sqrt(std::max(0.0F, 1.0F - r2));
                /* 视空间光：左上前方（与 view 法线约定一致；纹理 v 向下 → ny 取反） */
                const float lx = -0.42F, ly = -0.50F, lz = 0.76F;
                const float ndl = std::max(0.0F, nx * lx + ny * ly + nz * lz);
                const float spec = std::pow(std::max(0.0F, nx * (-0.20F) + ny * (-0.26F) + nz * 0.94F), 28.0F);
                const float diff = 0.12F + 0.88F * ndl;
                const float edge = std::clamp(1.0F - r2, 0.0F, 1.0F);   /* 球外渐暗 */
                const float r = (0.20F * diff + spec * 0.95F) * edge;
                const float g = (0.34F * diff + spec * 0.98F) * edge;
                const float b = (0.62F * diff + spec * 0.95F) * edge;
                const float a = 1.0F;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = (uint8_t)std::clamp((int)(r * 255.0F + 0.5F), 0, 255);
                c[1] = (uint8_t)std::clamp((int)(g * 255.0F + 0.5F), 0, 255);
                c[2] = (uint8_t)std::clamp((int)(b * 255.0F + 0.5F), 0, 255);
                c[3] = (uint8_t)(a * 255.0F);
            }
        } else { px = { 255, 255, 255, 255 }; }
        int id = -1;
        if (rapi && rself && rapi->create_texture2) {
            /* builtin 查表纹理默认 clamp（matcap 球/渐变在边界外不能 wrap）；路径后缀可覆盖。 */
            const int wrap_u = tp.wrap_set ? tp.wrap_u : (builtin_wrap_u >= 0 ? builtin_wrap_u : 1);
            const int wrap_v = tp.wrap_set ? tp.wrap_v : (builtin_wrap_v >= 0 ? builtin_wrap_v : 1);
            const int filter = tp.filter_set ? tp.filter : 1;
            mq_renderer_texture_desc td{};
            td.width = w; td.height = h; td.pixel_format = builtin_format; td.kind = 0;
            td.mip_levels = builtin_mips;
            td.filter = filter; td.wrap_u = wrap_u; td.wrap_v = wrap_v;
            td.data = px.data(); td.data_size = px.size();
            if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
        }
        if (id < 0) id = rapi->create_texture(rself, px.data(), w, h);
        tex_cache[ref] = id;
        return id;
    }
    return load_texture(ref);   // assets（load_texture 内部剥 sampler 后缀）
}

/* M4c：加载离线导入的 .mesh.json（tools/gltf_import.py 产出）→ 渲染器网格句柄。
 * 只读资产文件；按路径缓存（失败也缓存 -1，避免每帧重试/刷日志）。
 * plans/22 §6.1：vertex_layout 2/3 = 48/56B 基础 + joints u16x4 + weights f32x4（stride 72/80）；
 * morph 网格额外保存**打包后的基础顶点** + targets/default_weights，创建后立即应用默认权重。 */
int32_t Engine::load_mesh(const std::string& path) {
    auto it = mesh_cache.find(path);
    if (it != mesh_cache.end()) return it->second;
    if (!gltf_env_read) read_gltf_env();
    const auto t0 = std::chrono::steady_clock::now();
    int32_t id = -1;
    void* data = nullptr;
    size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, path.c_str(), &data, &n) != 0) {
        std::print(stderr, "runtime: mesh not found '{}'\n", path);
    } else {
        mq_json* dom = mq_json_parse((const char*)data, n);
        mq_vfs_free(data);
        if (!dom) {
            std::print(stderr, "runtime: mesh json 解析失败 '{}'\n", path);
        } else {
            std::vector<float> verts;
            std::vector<uint32_t> idx;
            /* 顶点布局（plans/22 §5.1）：
             *   0 = 旧 48B：pos3|normal3|tangent4|uv0 2（12 float/顶点）
             *   1 = 新 56B：pos3|normal3|color4|uv0 2|uv1 2（14 float/顶点）
             *   2 = 48B + joints u16x4 + weights f32x4（stride 72；JSON 行宽仍 12）
             *   3 = 56B + joints u16x4 + weights f32x4（stride 80；JSON 行宽仍 14）
             * 蒙皮数据在独立 joints/weights 数组 → 运行时按 GPU stride 打包。 */
            int32_t vertex_layout = 0;
            if (const mq_json* jlay = mq_json_get(dom, "vertex_layout"))
                vertex_layout = (int32_t)mq_json_num(jlay, 0.0);
            const bool base56 = vertex_layout == 1 || vertex_layout == 3;
            const uint32_t row_floats = base56 ? 14u : 12u;
            const uint32_t base_stride = base56 ? 56u : 48u;
            const bool skinned = vertex_layout >= 2 && vertex_layout <= 3;
            const uint32_t stride = base_stride + (skinned ? 24u : 0u);
            const mq_json* jv = mq_json_get(dom, "vertices");
            const uint32_t vn = mq_json_len(jv);
            verts.reserve((size_t)vn * row_floats);
            const mq_json* ji = mq_json_get(dom, "indices");
            const uint32_t in = mq_json_len(ji);
            idx.reserve(in);
            bool ok = vn > 0 && in > 0 && vertex_layout >= 0 && vertex_layout <= 3;
            if (!ok) std::print(stderr, "runtime: mesh '{}' 无效（vertices={} indices={} vertex_layout={}）\n",
                                path, vn, in, vertex_layout);
            for (uint32_t i = 0; ok && i < vn; ++i) {
                const mq_json* row = mq_json_at(jv, i);
                if (mq_json_len(row) != row_floats) { ok = false; break; }
                for (uint32_t k = 0; k < row_floats; ++k)
                    verts.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
            }
            for (uint32_t i = 0; ok && i < in; ++i)
                idx.push_back((uint32_t)mq_json_num(mq_json_at(ji, i), 0.0));
            /* 蒙皮属性打包（joints u16x4 + weights f32x4 追加在基础顶点之后） */
            std::vector<uint8_t> packed;
            const void* vptr = verts.data();
            uint64_t vbytes = (uint64_t)verts.size() * sizeof(float);
            if (ok && skinned) {
                const mq_json* jj = mq_json_get(dom, "joints");
                const mq_json* jw = mq_json_get(dom, "weights");
                ok = mq_json_len(jj) == vn && mq_json_len(jw) == vn;
                if (!ok) std::print(stderr, "runtime: mesh '{}' 蒙皮数据缺失/长度不符（joints={} weights={} vertices={}）\n",
                                    path, mq_json_len(jj), mq_json_len(jw), vn);
                if (ok) {
                    packed.resize((size_t)vn * stride);
                    for (uint32_t i = 0; i < vn; ++i) {
                        uint8_t* dst = packed.data() + (size_t)i * stride;
                        std::memcpy(dst, verts.data() + (size_t)i * row_floats, base_stride);
                        const mq_json* jr = mq_json_at(jj, i);
                        const mq_json* wr = mq_json_at(jw, i);
                        if (mq_json_len(jr) != 4 || mq_json_len(wr) != 4) { ok = false; break; }
                        uint16_t* jdst = (uint16_t*)(dst + base_stride);
                        for (uint32_t k = 0; k < 4; ++k) {
                            int v = (int)mq_json_num(mq_json_at(jr, k), 0.0);
                            if (v < 0) v = 0;
                            if (v > 65535) v = 65535;
                            jdst[k] = (uint16_t)v;
                        }
                        float* wdst = (float*)(dst + base_stride + 8);
                        for (uint32_t k = 0; k < 4; ++k)
                            wdst[k] = (float)mq_json_num(mq_json_at(wr, k), 0.0);
                    }
                    if (ok) { vptr = packed.data(); vbytes = packed.size(); }
                }
            }
            /* morph（可选）：default_weights + targets（逐顶点 pos/normal 增量；targets ≤ 8） */
            MorphMesh morph;
            if (ok) {
                if (const mq_json* jm = mq_json_get(dom, "morph")) {
                    const mq_json* jdw = mq_json_get(jm, "default_weights");
                    const uint32_t ndw = mq_json_len(jdw);
                    morph.default_weights.reserve(ndw);
                    for (uint32_t k = 0; k < ndw; ++k)
                        morph.default_weights.push_back((float)mq_json_num(mq_json_at(jdw, k), 0.0));
                    const mq_json* jts = mq_json_get(jm, "targets");
                    const uint32_t nts = mq_json_len(jts);
                    if (nts > 8)
                        std::print(stderr, "runtime: mesh '{}' morph targets={} 超出上限 8，后续忽略\n",
                                   path, nts);
                    for (uint32_t t = 0; t < nts && t < 8 && ok; ++t) {
                        const mq_json* jt = mq_json_at(jts, t);
                        MorphTarget tgt;
                        const mq_json* jp = mq_json_get(jt, "positions");
                        const mq_json* jn = mq_json_get(jt, "normals");
                        if (mq_json_len(jp) != vn) { ok = false; break; }
                        tgt.positions.reserve((size_t)vn * 3);
                        for (uint32_t i = 0; i < vn && ok; ++i) {
                            const mq_json* row = mq_json_at(jp, i);
                            if (mq_json_len(row) != 3) { ok = false; break; }
                            for (uint32_t k = 0; k < 3; ++k)
                                tgt.positions.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                        }
                        if (ok && mq_json_len(jn) == vn) {
                            tgt.normals.reserve((size_t)vn * 3);
                            for (uint32_t i = 0; i < vn && ok; ++i) {
                                const mq_json* row = mq_json_at(jn, i);
                                if (mq_json_len(row) != 3) { ok = false; break; }
                                for (uint32_t k = 0; k < 3; ++k)
                                    tgt.normals.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                            }
                        }
                        if (ok) morph.targets.push_back(std::move(tgt));
                    }
                    if (morph.default_weights.size() < morph.targets.size())
                        morph.default_weights.resize(morph.targets.size(), 0.0F);
                }
            }
            if (ok && rapi && rself && rapi->create_mesh) {
                mq_renderer_mesh_gpu md{};
                md.vertices = vptr;
                md.vertex_bytes = vbytes;
                md.vertex_stride = stride;
                md.vertex_layout = vertex_layout;
                md.indices = idx.data();
                md.index_count = (uint32_t)idx.size();
                const mq_json* bmin = mq_json_get(dom, "bounds_min");
                const mq_json* bmax = mq_json_get(dom, "bounds_max");
                for (uint32_t k = 0; k < 3 && k < mq_json_len(bmin); ++k)
                    md.bounds_min[k] = (float)mq_json_num(mq_json_at(bmin, k), 0.0);
                for (uint32_t k = 0; k < 3 && k < mq_json_len(bmax); ++k)
                    md.bounds_max[k] = (float)mq_json_num(mq_json_at(bmax, k), 0.0);
                if (rapi->create_mesh(rself, &md, &id) != 0) id = -1;
                if (id >= 0) mesh_stats[id] = { vn, (uint32_t)idx.size() };
            }
            /* morph：保存打包后的基础顶点（CPU 重算起点），创建后立即应用默认权重 */
            if (id >= 0 && !morph.targets.empty()) {
                morph.mesh = id;
                morph.vertex_count = vn;
                morph.stride = stride;
                morph.layout = vertex_layout;
                morph.base = std::move(packed);
                if (morph.base.empty()) {
                    morph.base.resize((size_t)vn * stride);
                    std::memcpy(morph.base.data(), verts.data(), (size_t)vn * base_stride);
                }
                morph.applied_weights = morph.default_weights;
                const std::string mp_key = path;
                morph_meshes[mp_key] = std::move(morph);
                bool any_w = false;
                for (float w : morph_meshes[mp_key].default_weights) if (w != 0.0F) { any_w = true; break; }
                if (any_w) (void)apply_morph(mp_key, morph_meshes[mp_key].default_weights);
                if (render3d_info)
                    std::print("runtime: morph mesh '{}' targets={} vertices={} stride={}\n",
                               path, (int)morph_meshes[mp_key].targets.size(),
                               (long long)morph_meshes[mp_key].vertex_count,
                               (long long)morph_meshes[mp_key].stride);
            }
            if (id < 0) std::print(stderr, "runtime: mesh 载入失败 '{}'\n", path);
            else if (render3d_info) {
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::print("runtime: mesh '{}' vertices={} indices={} ({:.1f} ms)\n",
                           path, vn, in, ms);
            }
            mesh_layout[path] = (id >= 0) ? vertex_layout : -1;
            mq_json_free(dom);
        }
    }
    mesh_cache[path] = id;
    return id;
}

/* ---- 运行时 glTF/GLB 直接加载（plans/23/24）----
 * mesh_path 指向 .gltf/.glb 时不必先跑 Python importer：装配期解析 + 上传一次，之后每帧只读缓存。
 * 资源（buffer/image）的相对 URI 按**模型文件所在目录**经 VFS 解析（远程挂载同样适用）。 */
bool Engine::read_vfs_file(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    if (!vfs) return false;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != 0 || !data) return false;
    out.assign((const uint8_t*)data, (const uint8_t*)data + n);
    mq_vfs_free(data);
    return true;
}

int32_t Engine::upload_texture_rgba(const uint8_t* rgba, int w, int h, bool srgb,
                                    int filter, int wrap_u, int wrap_v) {
    if (!rapi || !rself || !rgba || w <= 0 || h <= 0) return -1;
    int32_t id = -1;
    if (rapi->create_texture2) {
        int levels = 1;
        std::vector<uint8_t> mips = build_mip_chain(rgba, w, h, levels, srgb);
        mq_renderer_texture_desc td{};
        td.width = w;
        td.height = h;
        td.pixel_format = srgb ? 1 : 0;       /* 1 = RGBA8_SRGB，0 = RGBA8（线性数据） */
        td.kind = 0;
        td.mip_levels = levels;
        td.filter = filter;
        td.wrap_u = wrap_u;
        td.wrap_v = wrap_v;
        td.data = mips.data();
        td.data_size = (uint64_t)mips.size();
        if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
    }
    if (id < 0 && rapi->create_texture) {
        id = rapi->create_texture(rself, rgba, (int32_t)w, (int32_t)h);
    }
    return id;
}

/* 材质资源：builtin:/ 或 assets（JSON，反射反序列化 scn.SpriteMaterial）。 */
bool Engine::load_material(const std::string& ref, scn::SpriteMaterial& out) {
    if (ref.rfind("builtin:", 0) == 0) {
        out.base_color = math::Vec4{ 1, 1, 1, 1 };
        out.texture = (ref == "builtin:/materials/checker") ? "builtin:/textures/checker"
                                                            : "builtin:/textures/white";
        return true;
    }
    void* d = nullptr; size_t n = 0;
    std::string path = "/" + ref;
    if (mq_vfs_read(vfs, path.c_str(), &d, &n) != 0) { std::print("runtime: material not found '{}'\n", ref); return false; }
    mq_json* dom = mq_json_parse((const char*)d, n);
    mq_vfs_free(d);
    if (!dom) return false;
    const mq_type_desc* mt = mq_reflect_type_by_name(reflect, "scn.SpriteMaterial");
    int rc = mt ? mq_persist_apply_json(reflect, mt, dom, &out) : 1;
    mq_json_free(dom);
    return rc == 0;
}

}  // namespace maque::rt
