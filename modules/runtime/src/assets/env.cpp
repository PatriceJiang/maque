/* 资源 · 环境贴图（RGBE/EXR 解码 + CPU PMREM + cubemap 预过滤；plans/19 §环境光照）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S4，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* M4g：PBR albedo 贴图必须按 sRGB 采样（文件存 sRGB、shader 需要线性）——
 * create_texture2 的 RGBA8_SRGB；MR/AO/normal 是线性数据，仍走 load_texture_mips（RGBA8Unorm）。 */

/* HDR env 解码 helper 的提前声明（定义在 load_texture_ref 之前）。 */
static bool parse_radiance_hdr(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba);
/* OpenEXR scanline（NONE/RLE/ZIP/ZIPS；half/float 通道）→ 线性 RGBA float。 */
static bool parse_exr_env(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba);
static void make_studio_env_base(std::vector<float>& out, int& w, int& h);
/* PMREM：GGX 粗糙度预过滤（specular mip 链）+ 余弦卷积（diffuse irradiance）。 */
static void build_pmrem(const std::vector<float>& base, int w, int h,
                        std::vector<uint8_t>& spec_chain, int& levels,
                        std::vector<uint8_t>& irr, int& irr_w, int& irr_h);
static void build_pmrem_cube(const std::vector<std::vector<float>>& base, int w,
                             std::vector<uint8_t>& spec_chain, int& levels,
                             std::vector<uint8_t>& irr, int& irr_w);

/* envmap 加载（PMREM）：.hdr（RGBE）/ EXR / PNG/JPEG / builtin studio → RGBA16F 预过滤 mip 链 + irradiance。
 * 全部在装配期 CPU 预计算（确定性；与 GPU/后端无关），失败时 spec/irr 均为 -1 → shader 回退 analytic。 */
void Engine::load_env_textures(const std::string& path, int32_t& spec_id, int32_t& irr_id) {
    spec_id = irr_id = -1;
    auto cached = env_tex_cache.find(path);
    if (cached != env_tex_cache.end()) { spec_id = cached->second.first; irr_id = cached->second.second; return; }
    const TexPathParams tp = parse_tex_path(path);
    int w = 0, h = 0;
    std::vector<float> base;
    if (tp.vfs == "builtin:/textures/studio_env") {
        make_studio_env_base(base, w, h);
    } else if (!load_env_image_linear(path, w, h, base)) {
        env_tex_cache[path] = { -1, -1 };
        return;
    }
    if (w <= 0 || h <= 0 || base.empty()) {
        env_tex_cache[path] = { -1, -1 };
        return;
    }
    std::vector<uint8_t> spec_chain, irr;
    int levels = 1, irr_w = 0, irr_h = 0;
    const auto t0 = std::chrono::steady_clock::now();
    build_pmrem(base, w, h, spec_chain, levels, irr, irr_w, irr_h);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rapi && rself && rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = w; td.height = h;
        td.pixel_format = 2;                       /* 2 = RGBA16F（线性 HDR / 预过滤结果） */
        td.kind = 0; td.mip_levels = levels;
        td.filter = 1;                             /* linear */
        td.wrap_u = 0; td.wrap_v = 1;              /* 经度 repeat、纬度 clamp */
        td.data = spec_chain.data(); td.data_size = (uint64_t)spec_chain.size();
        if (rapi->create_texture2(rself, &td, &spec_id) != 0) spec_id = -1;
        mq_renderer_texture_desc id_ = {};
        id_.width = irr_w; id_.height = irr_h;
        id_.pixel_format = 2; id_.kind = 0; id_.mip_levels = 1;
        id_.filter = 1; id_.wrap_u = 0; id_.wrap_v = 1;
        id_.data = irr.data(); id_.data_size = (uint64_t)irr.size();
        if (rapi->create_texture2(rself, &id_, &irr_id) != 0) irr_id = -1;
    }
    if (spec_id < 0) std::print(stderr, "runtime: env_texture '{}' PMREM 上传失败\n", path);
    else
        std::print("runtime: env PMREM '{}' {}x{} → {} mips (GGX) + irradiance {}x{} ({:.1f} ms)\n",
                   path, w, h, levels, irr_w, irr_h, ms);
    env_tex_cache[path] = { spec_id, irr_id };
}

/* env 图片解码（.hdr RGBE / EXR / PNG/JPEG）→ 线性 RGBA float；供 equirect 与 6 面 cube 复用。 */
bool Engine::load_env_image_linear(const std::string& path, int& w, int& h, std::vector<float>& base) {
    w = 0; h = 0; base.clear();
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (!vfs || mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) {
        std::print(stderr, "runtime: env_texture not found '{}'\n", path);
        return false;
    }
    const bool is_hdr = (n >= 4 && data && ((const uint8_t*)data)[0] == '#' &&
                         ((const uint8_t*)data)[1] == '?') ||
                        (tp.vfs.size() >= 4 &&
                         (tp.vfs.compare(tp.vfs.size() - 4, 4, ".hdr") == 0 ||
                          tp.vfs.compare(tp.vfs.size() - 4, 4, ".HDR") == 0));
    const bool is_exr = n >= 4 && data && ((const uint8_t*)data)[0] == 0x76 &&
                        ((const uint8_t*)data)[1] == 0x2f && ((const uint8_t*)data)[2] == 0x31 &&
                        ((const uint8_t*)data)[3] == 0x01;
    if (is_hdr) {
        const bool ok = parse_radiance_hdr((const uint8_t*)data, n, w, h, base);
        mq_vfs_free(data);
        if (!ok) {
            std::print(stderr, "runtime: env_texture '{}' HDR(RGBE) 解析失败，回退 analytic\n", path);
            return false;
        }
        if (const char* info = std::getenv("MAQUE_RENDER3D_INFO"); info && info[0] == '1') {
            const size_t c = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            std::print(stderr, "runtime: env HDR {}x{} p0=({:.4f},{:.4f},{:.4f}) "
                               "pc=({:.4f},{:.4f},{:.4f})\n",
                       w, h, base[0], base[1], base[2], base[c], base[c + 1], base[c + 2]);
        }
        return true;
    }
    if (is_exr) {
        const bool ok = parse_exr_env((const uint8_t*)data, n, w, h, base);
        mq_vfs_free(data);
        if (!ok) {
            std::print(stderr, "runtime: env_texture '{}' EXR 解析失败（仅支持 scanline NONE/RLE/ZIP/ZIPS，"
                               "half/float 通道），回退 analytic\n", path);
            return false;
        }
        if (const char* info = std::getenv("MAQUE_RENDER3D_INFO"); info && info[0] == '1') {
            float mx = 0.0F;
            for (float v : base) mx = std::max(mx, v);
            const size_t c = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            std::print(stderr, "runtime: env EXR {}x{} p0=({:.4f},{:.4f},{:.4f}) "
                               "pc=({:.4f},{:.4f},{:.4f}) max={:.3f}\n",
                       w, h, base[0], base[1], base[2], base[c], base[c + 1], base[c + 2], mx);
        }
        return true;
    }
    uint8_t* rgba = nullptr; uint32_t iw = 0, ih = 0;
    char err[128] = { 0 };
    const int rc = mq_image_decode(data, n, &rgba, &iw, &ih, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) {
        std::print(stderr, "runtime: env_texture '{}' 解码失败: {}\n", path, err);
        return false;
    }
    /* PNG/JPEG 存的是 sRGB 编码值：先解码到线性域再做 PMREM（否则高光/平均亮度错误）。 */
    w = (int)iw; h = (int)ih;
    base.assign((size_t)w * h * 4, 1.0f);
    for (size_t i = 0, np = (size_t)w * h; i < np; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float sv = (float)rgba[i * 4 + (size_t)c] / 255.0f;
            base[i * 4 + (size_t)c] = (sv <= 0.04045f) ? sv / 12.92f
                                                       : std::pow((sv + 0.055f) / 1.055f, 2.4f);
        }
        base[i * 4 + 3] = 1.0f;
    }
    mq_image_free(rgba);
    return true;
}

/* 立方体 env 加载（第 26 轮）：基名 + 6 面后缀 → GGX 预过滤 cube（mip 链）+ irradiance cube。
 * 失败时 spec/irr 均为 -1 → shader 回退 equirect/analytic（不崩）。 */
void Engine::load_env_cubemap(const std::string& prefix, int32_t& spec_id, int32_t& irr_id) {
    spec_id = irr_id = -1;
    auto cached = env_cube_cache.find(prefix);
    if (cached != env_cube_cache.end()) { spec_id = cached->second.first; irr_id = cached->second.second; return; }
    static const char* kSuffix[6] = { "_px", "_nx", "_py", "_ny", "_pz", "_nz" };
    static const char* kExts[5] = { ".hdr", ".exr", ".png", ".jpg", ".jpeg" };
    std::vector<std::vector<float>> faces(6);
    int w = 0;
    bool ok = false;
    for (const char* ext : kExts) {
        int fw = 0, fh = 0;
        bool all = true;
        for (int f = 0; f < 6; ++f) {
            const std::string p = prefix + kSuffix[f] + std::string(ext);
            int iw = 0, ih = 0;
            std::vector<float> img;
            if (!load_env_image_linear(p, iw, ih, img)) { all = false; break; }
            if (f == 0) { fw = iw; fh = ih; }
            else if (iw != fw || ih != fh) { all = false; break; }
            faces[(size_t)f] = std::move(img);
        }
        if (all && fw > 0 && fw == fh) { w = fw; ok = true; break; }
    }
    if (!ok) {
        std::print(stderr, "runtime: env_cubemap '{}' 6 面加载失败（<基名>_px/_nx/_py/_ny/_pz/_nz + "
                           ".hdr/.exr/.png/.jpg），回退 equirect/analytic\n", prefix);
        env_cube_cache[prefix] = { -1, -1 };
        return;
    }
    std::vector<uint8_t> spec_chain, irr;
    int levels = 1, irr_w = 0;
    const auto t0 = std::chrono::steady_clock::now();
    build_pmrem_cube(faces, w, spec_chain, levels, irr, irr_w);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rapi && rself && rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = w; td.height = w;
        td.pixel_format = 2;                        /* RGBA16F */
        td.kind = 1;                                /* cube（6 面 × mip 链） */
        td.mip_levels = levels;
        td.filter = 1; td.wrap_u = 1; td.wrap_v = 1;
        td.data = spec_chain.data(); td.data_size = (uint64_t)spec_chain.size();
        if (rapi->create_texture2(rself, &td, &spec_id) != 0) spec_id = -1;
        mq_renderer_texture_desc id_{};
        id_.width = irr_w; id_.height = irr_w;
        id_.pixel_format = 2; id_.kind = 1; id_.mip_levels = 1;
        id_.filter = 1; id_.wrap_u = 1; id_.wrap_v = 1;
        id_.data = irr.data(); id_.data_size = (uint64_t)irr.size();
        if (rapi->create_texture2(rself, &id_, &irr_id) != 0) irr_id = -1;
    }
    if (spec_id < 0) std::print(stderr, "runtime: env_cubemap '{}' PMREM 上传失败\n", prefix);
    else
        std::print("runtime: env CUBEMAP '{}' {}x{} ×6 → {} mips (GGX) + irradiance {}x{}×6 ({:.1f} ms)\n",
                   prefix, w, w, levels, irr_w, irr_w, ms);
    env_cube_cache[prefix] = { spec_id, irr_id };
}

/* builtin:/ → 引擎内建（此处程序化生成）；否则 assets（VFS）。
 * ref 可带 sampler 后缀（`...` 见 parse_tex_path）；缓存 key 用完整字符串。 */

/* 内建 equirect studio 环境（512x256，**线性浮点**）：天空-地面渐变 + 4 个软光箱
 *（方向与 analytic studio IBL 一致）；作为 PMREM 的源 radiance，与 .hdr 走同一条预过滤路径。 */
static void make_studio_env_base(std::vector<float>& out, int& w, int& h) {
    w = 512; h = 256;
    constexpr float kPi = 3.14159265f;
    struct Box { float dx, dy, dz; float r, g, b; float amp; float sharp; };
    static const Box boxes[4] = {
        { -0.55f, 0.78f, 0.55f, 1.00f, 0.97f, 0.92f, 4.2f, 18.0f },  /* key softbox */
        { -0.30f, 0.84f, 0.62f, 1.00f, 0.98f, 0.95f, 2.0f, 22.0f },  /* fill */
        {  0.80f, 0.30f, 0.45f, 0.85f, 0.92f, 1.00f, 1.3f, 10.0f },  /* cool rim */
        {  0.05f, 0.25f, -0.95f,0.92f, 0.95f, 1.00f, 0.8f, 12.0f }, /* back */
    };
    auto norm = [](float x, float y, float z, float& ox, float& oy, float& oz) {
        const float l = std::sqrt(x * x + y * y + z * z) + 1e-8f;
        ox = x / l; oy = y / l; oz = z / l;
    };
    out.assign((size_t)w * h * 4, 1.0f);
    for (int y = 0; y < h; ++y) {
        const float lat = kPi * (0.5f - (float(y) + 0.5f) / (float)h);
        const float cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < w; ++x) {
            const float lon = kPi * 2.0f * ((float(x) + 0.5f) / (float)w) - kPi;
            const float dx = cl * std::cos(lon), dy = sl, dz = cl * std::sin(lon);
            /* 半球渐变：地面暖灰、天空冷灰、地平线略亮 */
            const float t = std::clamp(dy * 0.5f + 0.5f, 0.0f, 1.0f);
            float r = 0.16f + (0.48f - 0.16f) * t;
            float g = 0.15f + (0.52f - 0.15f) * t;
            float b = 0.14f + (0.60f - 0.14f) * t;
            const float horizon = std::exp(-std::fabs(dy) * 8.0f);
            r += horizon * 0.10f; g += horizon * 0.10f; b += horizon * 0.11f;
            /* 软光箱（方向余弦的锐利幂） */
            for (const Box& bx : boxes) {
                float nx, ny, nz; norm(bx.dx, bx.dy, bx.dz, nx, ny, nz);
                const float c = std::max(0.0f, dx * nx + dy * ny + dz * nz);
                const float spot = std::pow(c, bx.sharp) * bx.amp;
                r += bx.r * spot; g += bx.g * spot; b += bx.b * spot;
            }
            float* px = &out[((size_t)y * w + x) * 4];
            px[0] = r; px[1] = g; px[2] = b; px[3] = 1.0f;
        }
    }
}

/* ---- envmap：Radiance .hdr（RGBE）解码 + 线性 mip（PMREM 式预过滤近似） ---- */
static uint16_t f32_to_f16(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = (int32_t)((x >> 23) & 0xffu) - 127;
    uint32_t mantissa = x & 0x7fffffu;
    if (exponent == 128) return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent > 15) return (uint16_t)(sign | 0x7c00u);
    if (exponent < -14) {
        if (exponent < -24) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(-exponent - 14);
        return (uint16_t)(sign | (mantissa >> (13 + shift)));
    }
    return (uint16_t)(sign | ((uint32_t)(exponent + 15) << 10) | (mantissa >> 13));
}

static void append_half_rgba(std::vector<uint8_t>& out, float r, float g, float b) {
    const uint16_t h[4] = { f32_to_f16(r), f32_to_f16(g), f32_to_f16(b), f32_to_f16(1.0f) };
    const uint8_t* p = (const uint8_t*)h;
    out.insert(out.end(), p, p + sizeof h);
}

/* 解析 Radiance RGBE（new RLE 扫描线；老格式按平铺 RGBE 兼容）。
 * 头部按行读，**容忍 CRLF**：Radiance 规范只要求 \n 结尾，但不少工具（以及 core.autocrlf=true
 * 的检出）会把头部写成 \r\n —— 不剥掉 \r 的话"空行结束头部"判定失效，会把扫描线数据当分辨率行
 * 去 sscanf → 整张 env 静默回退 analytic（实测 studio_cube 的 _py/_ny 两面就是这样废掉的）。
 * 注意这只救头部：数据区若被换行转换插过字节（含 0x0A 的扫描线）无从恢复，
 * 二进制资产必须在 .gitattributes 里标 -text。 */
static bool parse_radiance_hdr(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba) {
    size_t pos = 0;
    auto get_line = [&](std::string& line) {
        line.clear();
        while (pos < n && d[pos] != '\n') line.push_back((char)d[pos++]);
        if (pos < n) ++pos;
        if (!line.empty() && line.back() == '\r') line.pop_back();
    };
    std::string line;
    get_line(line);
    if (line.rfind("#?", 0) != 0) return false;
    bool rgbe = false;
    for (;;) {
        get_line(line);
        if (line.empty() || pos >= n) break;
        if (line.rfind("FORMAT=", 0) == 0) {
            if (line.find("32-bit_rle_rgbe") != std::string::npos) rgbe = true;
            else return false;
        }
    }
    if (!rgbe) return false;
    get_line(line);                       /* 分辨率行：-Y <h> +X <w>（也支持 +Y 等简单变体） */
    int h = 0, w = 0;
    char s1 = 0, s2 = 0;
    if (std::sscanf(line.c_str(), "%c%c %d %c%c %d", &s1, &s2, &h, &s1, &s2, &w) != 6) return false;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 64u * 1024u * 1024u) return false;
    W = w; H = h;
    rgba.assign((size_t)w * h * 4, 0.0f);
    std::vector<uint8_t> scan((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        if (pos + 4 > n) return false;
        const uint8_t b0 = d[pos], b1 = d[pos + 1], b2 = d[pos + 2], b3 = d[pos + 3];
        bool new_rle = (b0 == 2 && b1 == 2 && (((int)b2 << 8) | b3) == w);
        if (new_rle) {
            pos += 4;
            for (int ch = 0; ch < 4; ++ch) {
                int x = 0;
                while (x < w) {
                    if (pos >= n) return false;
                    const int cnt = d[pos++];
                    if (cnt > 128) {
                        const int run = cnt - 128;
                        if (pos >= n) return false;
                        const uint8_t v = d[pos++];
                        for (int i = 0; i < run && x < w; ++i) scan[(size_t)(x++) * 4 + ch] = v;
                    } else {
                        for (int i = 0; i < cnt && x < w; ++i) {
                            if (pos >= n) return false;
                            scan[(size_t)(x++) * 4 + ch] = d[pos++];
                        }
                    }
                }
            }
        } else {
            /* 老格式/平铺 RGBE：直接读 w*4 字节 */
            if (pos + (size_t)w * 4 > n) return false;
            std::memcpy(scan.data(), d + pos, (size_t)w * 4);
            pos += (size_t)w * 4;
        }
        for (int x = 0; x < w; ++x) {
            const uint8_t* p4 = &scan[(size_t)x * 4];
            float* out = &rgba[((size_t)y * w + x) * 4];
            if (p4[3] == 0) { out[0] = out[1] = out[2] = 0.0f; }
            else {
                const float f = std::ldexp(1.0f, (int)p4[3] - 136);
                out[0] = p4[0] * f; out[1] = p4[1] * f; out[2] = p4[2] * f;
            }
            out[3] = 1.0f;
        }
    }
    return true;
}

/* ---- OpenEXR scanline 解码（envmap 用）----
 * 支持：单 part scanline、compression NONE/RLE/ZIP(16 行)/ZIPS(1 行)、通道类型 HALF/FLOAT/UINT、
 * 通道名 R/G/B(/A) 或 Y（灰度）。PIZ/DWAA/B44/深数据/平铺图不支持（返回 false → 调用方回退 analytic）。
 * 输出线性 RGBA float（EXR 本身是线性域，无需 sRGB 转换）。 */
struct ExrChannel { std::string name; int type = 2; int bytes = 4; };
static float exr_half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;                       /* ±0 */
        else {                                           /* subnormal */
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);         /* inf/NaN */
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f = 0.0F;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

/* OpenEXR 的 ZIP/RLE 压缩在 deflate/RLE 之前做了「predictor + 字节交错」两步变换（ImfZip/ImfRle），
 * 解压后必须逆变换：先按字节累加还原（b[i] = b[i-1] + b[i] - 128），再把数据从中间劈成两半交错。
 * 少这一步会得到量级离谱的浮点垃圾（EXR 刚接入时踩过）。 */
static void exr_unpredict_reorder(std::vector<uint8_t>& buf) {
    const size_t n = buf.size();
    if (n < 2) return;
    for (size_t i = 1; i < n; ++i)
        buf[i] = (uint8_t)((int)buf[i - 1] + (int)buf[i] - 128);
    std::vector<uint8_t> out(n);
    const size_t half = (n + 1) / 2;
    size_t p1 = 0, p2 = half;
    for (size_t i = 0; i < n; i += 2) {
        out[i] = buf[p1++];
        if (i + 1 < n) out[i + 1] = buf[p2++];
    }
    buf.swap(out);
}

static bool exr_rle_decode(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
    out.clear(); out.reserve(expected);
    size_t i = 0;
    while (out.size() < expected && i < n) {
        const int cnt = (int)(int8_t)src[i++];
        if (cnt < 0) {
            const size_t lit = (size_t)(-cnt);
            if (i + lit > n) return false;
            out.insert(out.end(), src + i, src + i + lit);
            i += lit;
        } else {
            if (i >= n) return false;
            out.insert(out.end(), (size_t)cnt + 1, src[i++]);
        }
    }
    return out.size() == expected;
}

static bool exr_unzip(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
#ifdef MQ_RUNTIME_HAVE_ZLIB
    out.resize(expected);
    uLongf dst = (uLongf)expected;
    if (uncompress(out.data(), &dst, src, (uLong)n) != Z_OK) return false;
    out.resize((size_t)dst);
    return dst == (uLongf)expected;
#else
    (void)src; (void)n; (void)expected; (void)out;
    return false;
#endif
}

static bool parse_exr_env(const uint8_t* d, size_t n, int& W, int& H, std::vector<float>& rgba) {
    if (!d || n < 8) return false;
    if (d[0] != 0x76 || d[1] != 0x2f || d[2] != 0x31 || d[3] != 0x01) return false;
    uint32_t version = 0;
    std::memcpy(&version, d + 4, 4);
    if ((version & 0xFFu) != 2u) return false;            /* version 2 */
    if (version & 0xFFFFFE00u) return false;              /* tiled/long-names/deep/multipart 不支持 */
    size_t pos = 8;
    std::vector<ExrChannel> channels;
    int compression = -1;
    int line_order = 0;
    int xmin = 0, ymin = 0, xmax = -1, ymax = -1;
    auto need = [&](size_t k) { return pos + k <= n; };
    while (pos < n) {
        if (d[pos] == 0) { ++pos; break; }                /* header 结束 */
        const size_t name_beg = pos;
        while (pos < n && d[pos] != 0) ++pos;
        if (pos >= n) return false;
        std::string name((const char*)d + name_beg, pos - name_beg);
        ++pos;
        const size_t type_beg = pos;
        while (pos < n && d[pos] != 0) ++pos;
        if (pos >= n) return false;
        std::string type((const char*)d + type_beg, pos - type_beg);
        ++pos;
        if (!need(4)) return false;
        uint32_t size = 0;
        std::memcpy(&size, d + pos, 4); pos += 4;
        if (!need(size)) return false;
        const uint8_t* v = d + pos;
        if (name == "channels" && type == "chlist") {
            size_t q = 0;
            while (q < size && v[q] != 0) {
                const size_t nb = q;
                while (q < size && v[q] != 0) ++q;
                if (q >= size) return false;
                ExrChannel ch;
                ch.name.assign((const char*)v + nb, q - nb);
                ++q;
                if (q + 16 > size) return false;           /* type4 + pLinear/reserved4 + x/ySampling8 */
                int ptype = 0;
                std::memcpy(&ptype, v + q, 4); q += 4;
                q += 4;                                    /* pLinear + 3 reserved */
                int32_t xs = 0, ys = 0;
                std::memcpy(&xs, v + q, 4); q += 4;
                std::memcpy(&ys, v + q, 4); q += 4;
                if (xs != 1 || ys != 1) return false;      /* 子采样通道不支持 */
                if (ptype < 0 || ptype > 2) return false;
                ch.type = ptype;
                ch.bytes = (ptype == 1) ? 2 : 4;
                channels.push_back(std::move(ch));
            }
        } else if (name == "compression") {
            if (size < 1) return false;
            compression = v[0];
        } else if (name == "lineOrder") {
            if (size < 1) return false;
            line_order = v[0];
        } else if (name == "dataWindow") {
            if (size < 16) return false;
            std::memcpy(&xmin, v, 4); std::memcpy(&ymin, v + 4, 4);
            std::memcpy(&xmax, v + 8, 4); std::memcpy(&ymax, v + 12, 4);
        }
        pos += size;
    }
    if (channels.empty()) return false;
    if (compression < 0) compression = 0;
    if (line_order != 0) return false;                    /* 只支持 increasing Y（HDRI 常态） */
    const int w = xmax - xmin + 1, h = ymax - ymin + 1;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 64u * 1024u * 1024u) return false;
    int lines_per_block = 1;
    if (compression == 3) lines_per_block = 16;           /* ZIP；ZIPS(2)/RLE(1)/NONE(1) 均为 1 行 */
    else if (compression > 3) return false;
    const int blocks = (h + lines_per_block - 1) / lines_per_block;
    if (!need((size_t)blocks * 8)) return false;
    std::vector<uint64_t> offsets((size_t)blocks);
    std::memcpy(offsets.data(), d + pos, (size_t)blocks * 8);
    size_t bytes_per_pixel = 0;
    for (const auto& ch : channels) bytes_per_pixel += (size_t)ch.bytes;
    int ri = -1, gi = -1, bi = -1, ai = -1, yi = -1;
    for (int i = 0; i < (int)channels.size(); ++i) {
        if (channels[(size_t)i].name == "R") ri = i;
        else if (channels[(size_t)i].name == "G") gi = i;
        else if (channels[(size_t)i].name == "B") bi = i;
        else if (channels[(size_t)i].name == "A") ai = i;
        else if (channels[(size_t)i].name == "Y") yi = i;
    }
    const bool rgb = ri >= 0 && gi >= 0 && bi >= 0;
    if (!rgb && yi < 0) return false;
    auto channel_offset = [&](int c) {
        size_t off = 0;
        for (int i = 0; i < c; ++i) off += (size_t)channels[(size_t)i].bytes;
        return off * (size_t)w;      /* 同一扫描线内按通道整行排列（不是逐像素交错） */
    };
    auto sample = [](const uint8_t* p, int type) -> float {
        if (type == 1) { uint16_t hv = 0; std::memcpy(&hv, p, 2); return exr_half_to_float(hv); }
        if (type == 2) { float f = 0.0F; std::memcpy(&f, p, 4); return f; }
        uint32_t u = 0; std::memcpy(&u, p, 4); return (float)u;
    };
    W = w; H = h;
    rgba.assign((size_t)w * h * 4, 0.0f);
    for (size_t i = 0; i < (size_t)w * h; ++i) rgba[i * 4 + 3] = 1.0f;
    std::vector<uint8_t> raw, dec;
    for (int b = 0; b < blocks; ++b) {
        const uint64_t off = offsets[(size_t)b];
        if (off > (uint64_t)n || (uint64_t)n - off < 8) return false;
        int32_t by = 0, dsize = 0;
        std::memcpy(&by, d + (size_t)off, 4); std::memcpy(&dsize, d + (size_t)off + 4, 4);
        if (dsize < 0 || (uint64_t)dsize > (uint64_t)n - off - 8) return false;
        const int nrows = std::min(lines_per_block, h - (by - ymin));
        if (nrows <= 0 || by < ymin || by > ymax) return false;
        const size_t expected = (size_t)nrows * w * bytes_per_pixel;
        const uint8_t* src = d + off + 8;
        const uint8_t* rows = nullptr;
        if (compression == 0) {
            if ((size_t)dsize != expected) return false;
            rows = src;
        } else if (compression == 1) {
            if (!exr_rle_decode(src, (size_t)dsize, expected, dec)) return false;
            exr_unpredict_reorder(dec);
            rows = dec.data();
        } else {
            if (!exr_unzip(src, (size_t)dsize, expected, dec)) return false;
            exr_unpredict_reorder(dec);
            rows = dec.data();
        }
        for (int r = 0; r < nrows; ++r) {
            const uint8_t* row = rows + (size_t)r * w * bytes_per_pixel;
            for (int x = 0; x < w; ++x) {
                float* out = &rgba[((size_t)(by - ymin + r) * w + x) * 4];
                if (rgb) {
                    out[0] = sample(row + channel_offset(ri) + (size_t)x * channels[(size_t)ri].bytes,
                                    channels[(size_t)ri].type);
                    out[1] = sample(row + channel_offset(gi) + (size_t)x * channels[(size_t)gi].bytes,
                                    channels[(size_t)gi].type);
                    out[2] = sample(row + channel_offset(bi) + (size_t)x * channels[(size_t)bi].bytes,
                                    channels[(size_t)bi].type);
                } else {
                    const float y = sample(row + channel_offset(yi) + (size_t)x * channels[(size_t)yi].bytes,
                                           channels[(size_t)yi].type);
                    out[0] = out[1] = out[2] = y;
                }
                if (ai >= 0)
                    out[3] = sample(row + channel_offset(ai) + (size_t)x * channels[(size_t)ai].bytes,
                                    channels[(size_t)ai].type);
            }
        }
    }
    return true;
}

/* ---- 真正 PMREM（IBL 预过滤，CPU、确定性）----
 * three.js / UE 的 split-sum IBL：镜面 = GGX importance-sample 预过滤的粗糙度 mip 链
 *（V=N=R 假设下按 NdotL 加权归一），漫反射 = 余弦加权半球卷积的 irradiance 贴图。
 * 之前实现是 2×2 box mip 近似：粗糙度增大时只是"更糊"，不是 GGX 能量分布，
 * 且漫反射仍用 analytic —— 这两点在此替换。EXR 已支持；cubemap kind=1 仍列后续。 */
static constexpr float kEnvPi = 3.14159265358979f;

static void env_dir_from_uv(float u, float v, float out[3]) {
    const float phi = (u - 0.5f) * 2.0f * kEnvPi;
    const float theta = v * kEnvPi;
    const float st = std::sin(theta);
    out[0] = st * std::cos(phi);
    out[1] = std::cos(theta);
    out[2] = st * std::sin(phi);
}

static void env_uv_from_dir(const float d[3], float& u, float& v) {
    u = std::atan2(d[2], d[0]) / (2.0f * kEnvPi) + 0.5f;
    v = std::acos(std::clamp(d[1], -1.0f, 1.0f)) / kEnvPi;
}

static void env_normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-20f;
    v[0] /= l; v[1] /= l; v[2] /= l;
}

static float env_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* 以 n 为 z 轴的切线基（避免极点退化）。 */
static void env_basis(const float n[3], float t[3], float b[3]) {
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (std::fabs(n[1]) > 0.999f) { up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f; }
    t[0] = up[1] * n[2] - up[2] * n[1];
    t[1] = up[2] * n[0] - up[0] * n[2];
    t[2] = up[0] * n[1] - up[1] * n[0];
    env_normalize(t);
    b[0] = n[1] * t[2] - n[2] * t[1];
    b[1] = n[2] * t[0] - n[0] * t[2];
    b[2] = n[0] * t[1] - n[1] * t[0];
}

/* equirect 双线性采样：经度 wrap、纬度 clamp（与 GPU 侧 sampler 语义一致）。 */
static void env_sample_bilinear(const std::vector<float>& img, int w, int h,
                                const float d[3], float out[3]) {
    float u = 0.0f, v = 0.0f;
    env_uv_from_dir(d, u, v);
    const float fx = u * (float)w - 0.5f;
    const float fy = v * (float)h - 0.5f;
    const int x0 = (int)std::floor(fx);
    const int y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto at = [&](int x, int y) -> const float* {
        x %= w; if (x < 0) x += w;
        y = std::clamp(y, 0, h - 1);
        return &img[((size_t)y * (size_t)w + (size_t)x) * 4];
    };
    const float* p00 = at(x0, y0);
    const float* p10 = at(x0 + 1, y0);
    const float* p01 = at(x0, y0 + 1);
    const float* p11 = at(x0 + 1, y0 + 1);
    for (int c = 0; c < 3; ++c) {
        const float a = p00[c] + (p10[c] - p00[c]) * tx;
        const float b = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = a + (b - a) * ty;
    }
}

/* Hammersley 低差异序列（radical inverse base 2）。 */
static float radical_inverse_vdc(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

/* GGX/Trowbridge-Reitz 重要性采样：返回围绕 n 的半程向量 H。 */
static void env_importance_sample_ggx(float xi1, float xi2, const float n[3], float rough, float h[3]) {
    const float a = rough * rough;
    const float a2 = a * a;
    const float phi = 2.0f * kEnvPi * xi1;
    const float cos_theta = std::sqrt((1.0f - xi2) / (1.0f + (a2 - 1.0f) * xi2));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    float t[3], b[3];
    env_basis(n, t, b);
    const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
    for (int i = 0; i < 3; ++i) h[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
}

/* ---- cubemap env 辅助（第 26 轮）----
 * 面序：0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z；面内 u 向右、v 向下（图片行 0 = 顶部），
 * 与 tools/equirect_to_cube.py 的 face_dir 逐式一致；env_face_uv_from_dir 是其逆映射。 */
static void env_face_dir(int face, float u, float v, float d[3]) {
    const float s = 2.0f * u - 1.0f, t = 2.0f * v - 1.0f;
    switch (face) {
    case 0: d[0] = 1.0f;  d[1] = -t;    d[2] = -s;    break;   /* +X */
    case 1: d[0] = -1.0f; d[1] = -t;    d[2] = s;     break;   /* -X */
    case 2: d[0] = s;     d[1] = 1.0f;  d[2] = t;     break;   /* +Y */
    case 3: d[0] = s;     d[1] = -1.0f; d[2] = -t;    break;   /* -Y */
    case 4: d[0] = s;     d[1] = -t;    d[2] = 1.0f;  break;   /* +Z */
    default: d[0] = -s;   d[1] = -t;    d[2] = -1.0f; break;   /* -Z */
    }
}

static void env_face_uv_from_dir(const float d[3], int& face, float& u, float& v) {
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    const float ma = std::max(ax, std::max(ay, az));
    if (ma <= 1e-8f) { face = 0; u = 0.5f; v = 0.5f; return; }
    if (ax >= ay && ax >= az) {
        if (d[0] >= 0.0f) {
            face = 0; u = 0.5f * (1.0f - d[2] / ma); v = 0.5f * (1.0f - d[1] / ma);
        } else {
            face = 1; u = 0.5f * (1.0f - d[2] / ma); v = 0.5f * (1.0f + d[1] / ma);
        }
    } else if (ay >= az) {
        if (d[1] >= 0.0f) {
            face = 2; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f + d[2] / ma);
        } else {
            face = 3; u = 0.5f * (1.0f - d[0] / ma); v = 0.5f * (1.0f + d[2] / ma);
        }
    } else {
        if (d[2] >= 0.0f) {
            face = 4; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f - d[1] / ma);
        } else {
            face = 5; u = 0.5f * (1.0f + d[0] / ma); v = 0.5f * (1.0f + d[1] / ma);
        }
    }
}

static void env_sample_cube(const std::vector<std::vector<float>>& faces, int w,
                            const float dir[3], float out[3]) {
    float n[3] = { dir[0], dir[1], dir[2] };
    env_normalize(n);
    int face = 0;
    float u = 0.5f, v = 0.5f;
    env_face_uv_from_dir(n, face, u, v);
    const std::vector<float>& img = faces[(size_t)face];
    const float x = u * (float)w - 0.5f, y = v * (float)w - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - (float)x0, fy = y - (float)y0;
    auto px = [&](int xx, int yy, float c[3]) {
        xx = std::clamp(xx, 0, w - 1);
        yy = std::clamp(yy, 0, w - 1);
        const float* p = &img[((size_t)yy * (size_t)w + (size_t)xx) * 4];
        c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
    };
    float c00[3], c10[3], c01[3], c11[3];
    px(x0, y0, c00); px(x0 + 1, y0, c10); px(x0, y0 + 1, c01); px(x0 + 1, y0 + 1, c11);
    for (int i = 0; i < 3; ++i)
        out[i] = (c00[i] * (1.0f - fx) + c10[i] * fx) * (1.0f - fy) +
                 (c01[i] * (1.0f - fx) + c11[i] * fx) * fy;
}

/* cube PMREM：mip0 = 原始 6 面（镜面），mip L = GGX 预过滤（rough = L/(levels-1)），
 * 另外生成 16×16×6 的余弦卷积 irradiance cube。输出布局 = “每个 mip 依次 6 面”（gfx create_texture2 约定）。 */
static void build_pmrem_cube(const std::vector<std::vector<float>>& base, int w,
                             std::vector<uint8_t>& spec_chain, int& levels,
                             std::vector<uint8_t>& irr, int& irr_w) {
    levels = 1;
    for (int sz = w; sz > 1; sz >>= 1) ++levels;
    spec_chain.clear();
    for (int f = 0; f < 6; ++f) {
        for (size_t i = 0, np = (size_t)w * (size_t)w; i < np; ++i) {
            append_half_rgba(spec_chain, base[(size_t)f][i * 4], base[(size_t)f][i * 4 + 1],
                             base[(size_t)f][i * 4 + 2]);
        }
    }
    for (int lvl = 1; lvl < levels; ++lvl) {
        const int ow = std::max(1, w >> lvl);
        const float rough = (levels > 1) ? (float)lvl / (float)(levels - 1) : 0.0f;
        const int samples = std::clamp(32 + (int)(rough * 96.0f), 32, 128);
        for (int f = 0; f < 6; ++f) {
            for (int y = 0; y < ow; ++y) {
                for (int x = 0; x < ow; ++x) {
                    float n[3];
                    env_face_dir(f, ((float)x + 0.5f) / (float)ow, ((float)y + 0.5f) / (float)ow, n);
                    env_normalize(n);
                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    float wsum = 0.0f;
                    for (int s = 0; s < samples; ++s) {
                        const float xi1 = ((float)s + 0.5f) / (float)samples;
                        const float xi2 = radical_inverse_vdc((uint32_t)s);
                        float hv[3], ldir[3];
                        env_importance_sample_ggx(xi1, xi2, n, std::max(rough, 1e-3f), hv);
                        const float ndh = env_dot(n, hv);
                        for (int i = 0; i < 3; ++i) ldir[i] = 2.0f * ndh * hv[i] - n[i];
                        env_normalize(ldir);
                        const float ndl = env_dot(n, ldir);
                        if (ndl <= 0.0f) continue;
                        float c[3];
                        env_sample_cube(base, w, ldir, c);
                        for (int i = 0; i < 3; ++i) acc[i] += c[i] * ndl;
                        wsum += ndl;
                    }
                    if (wsum > 0.0f) { for (int i = 0; i < 3; ++i) acc[i] /= wsum; }
                    append_half_rgba(spec_chain, acc[0], acc[1], acc[2]);
                }
            }
        }
    }
    irr_w = 16;
    const int irr_samples = 128;
    irr.clear();
    for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < irr_w; ++y) {
            for (int x = 0; x < irr_w; ++x) {
                float n[3];
                env_face_dir(f, ((float)x + 0.5f) / (float)irr_w, ((float)y + 0.5f) / (float)irr_w, n);
                env_normalize(n);
                float t[3], b[3];
                env_basis(n, t, b);
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                for (int s = 0; s < irr_samples; ++s) {
                    const float xi1 = radical_inverse_vdc((uint32_t)s);
                    const float xi2 = ((float)s + 0.5f) / (float)irr_samples;
                    const float phi = 2.0f * kEnvPi * xi1;
                    const float cos_theta = std::sqrt(std::max(0.0f, 1.0f - xi2));
                    const float sin_theta = std::sqrt(xi2);
                    const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
                    float ldir[3];
                    for (int i = 0; i < 3; ++i) ldir[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
                    env_normalize(ldir);
                    float c[3];
                    env_sample_cube(base, w, ldir, c);
                    for (int i = 0; i < 3; ++i) acc[i] += c[i];
                }
                for (int i = 0; i < 3; ++i) acc[i] /= (float)irr_samples;
                append_half_rgba(irr, acc[0], acc[1], acc[2]);
            }
        }
    }
}

static void build_pmrem(const std::vector<float>& base, int w, int h,
                        std::vector<uint8_t>& spec_chain, int& levels,
                        std::vector<uint8_t>& irr, int& irr_w, int& irr_h) {
    levels = 1;
    for (int s = std::max(w, h); s > 1; s >>= 1) ++levels;
    spec_chain.clear();
    /* mip0 = 原始 radiance（roughness 0 = 镜面，shader 采样它必须保持锐利） */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float* p = &base[((size_t)y * (size_t)w + (size_t)x) * 4];
            append_half_rgba(spec_chain, p[0], p[1], p[2]);
        }
    }
    for (int lvl = 1; lvl < levels; ++lvl) {
        const int ow = std::max(1, w >> lvl), oh = std::max(1, h >> lvl);
        const float rough = (levels > 1) ? (float)lvl / (float)(levels - 1) : 0.0f;
        const int samples = std::clamp(32 + (int)(rough * 96.0f), 32, 128);
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                float n[3];
                env_dir_from_uv(((float)x + 0.5f) / (float)ow, ((float)y + 0.5f) / (float)oh, n);
                float acc[3] = { 0.0f, 0.0f, 0.0f };
                float wsum = 0.0f;
                for (int s = 0; s < samples; ++s) {
                    const float xi1 = ((float)s + 0.5f) / (float)samples;
                    const float xi2 = radical_inverse_vdc((uint32_t)s);
                    float hv[3], ldir[3];
                    env_importance_sample_ggx(xi1, xi2, n, std::max(rough, 1e-3f), hv);
                    const float ndh = env_dot(n, hv);
                    /* V=N=R 假设：L = reflect(-V, H) = 2*(V·H)*H - V */
                    for (int i = 0; i < 3; ++i) ldir[i] = 2.0f * ndh * hv[i] - n[i];
                    env_normalize(ldir);
                    const float ndl = env_dot(n, ldir);
                    if (ndl <= 0.0f) continue;
                    float c[3];
                    env_sample_bilinear(base, w, h, ldir, c);
                    for (int i = 0; i < 3; ++i) acc[i] += c[i] * ndl;
                    wsum += ndl;
                }
                if (wsum > 0.0f) { for (int i = 0; i < 3; ++i) acc[i] /= wsum; }
                append_half_rgba(spec_chain, acc[0], acc[1], acc[2]);
            }
        }
    }
    /* 漫反射 irradiance：余弦加权半球采样 → E/π（= 与 Lambert albedo/π 相乘后的等效辐亮度）。
     * 低分辨率即可（irradiance 在方向域很平滑）：64×32，256 样本。 */
    irr_w = 64; irr_h = 32;
    const int irr_samples = 256;
    irr.clear();
    for (int y = 0; y < irr_h; ++y) {
        for (int x = 0; x < irr_w; ++x) {
            float n[3];
            env_dir_from_uv(((float)x + 0.5f) / (float)irr_w, ((float)y + 0.5f) / (float)irr_h, n);
            float t[3], b[3];
            env_basis(n, t, b);
            float acc[3] = { 0.0f, 0.0f, 0.0f };
            for (int s = 0; s < irr_samples; ++s) {
                const float xi1 = radical_inverse_vdc((uint32_t)s);
                const float xi2 = ((float)s + 0.5f) / (float)irr_samples;
                const float phi = 2.0f * kEnvPi * xi1;
                const float cos_theta = std::sqrt(std::max(0.0f, 1.0f - xi2));
                const float sin_theta = std::sqrt(xi2);
                const float sc = sin_theta * std::cos(phi), ss = sin_theta * std::sin(phi);
                float ldir[3];
                for (int i = 0; i < 3; ++i) ldir[i] = t[i] * sc + b[i] * ss + n[i] * cos_theta;
                env_normalize(ldir);
                float c[3];
                env_sample_bilinear(base, w, h, ldir, c);
                for (int i = 0; i < 3; ++i) acc[i] += c[i];
            }
            for (int i = 0; i < 3; ++i) acc[i] /= (float)irr_samples;
            append_half_rgba(irr, acc[0], acc[1], acc[2]);
        }
    }
}

}  // namespace maque::rt
