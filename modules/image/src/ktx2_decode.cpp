/* KTX2 / Basis Universal 解码（image 模块的 C++ 扩展）：
 * - ETC1S（BasisLZ）与 UASTC（含 Zstd 超压缩）→ 统一转成 RGBA8；
 * - 只取二维纹理的 first layer / first face（glTF 贴图路径）；cubemap/array 暂不支持。
 * basis_universal 的 transcoder 直接支持 KTX2 容器（含 level index/DFD/KVD/Zstd）。 */
#include "basisu_transcoder.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" int mq_ktx2_decode(const void* data, size_t size, uint8_t** out_rgba, uint32_t* out_w,
                              uint32_t* out_h, char* err, size_t err_cap);

namespace
{
void set_err(char* err, size_t cap, const char* msg)
{
    if (!err || cap == 0)
        return;
    std::strncpy(err, msg ? msg : "", cap - 1);
    err[cap - 1] = '\0';
}

uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t rd64(const uint8_t* p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* KTX2 未压缩格式（RGBA8/RGB8/BGRA8，UNORM/SRGB）→ RGBA8。
 * 只取 mip 0；压缩块格式（BC/ASTC/ETC）不做（wgpu 上传路径只收 RGBA8）。 */
int decode_uncompressed(const uint8_t* d, size_t size, uint8_t** out_rgba, uint32_t* out_w,
                        uint32_t* out_h, char* err, size_t err_cap)
{
    const uint32_t vk = rd32(d + 12);
    const uint32_t w = rd32(d + 20);
    const uint32_t h = rd32(d + 24);
    const uint32_t depth = rd32(d + 28);
    const uint32_t layers = rd32(d + 32);
    const uint32_t faces = rd32(d + 36);
    const uint32_t levels = rd32(d + 40);
    const uint32_t scheme = rd32(d + 44);
    int bpp = 0;
    bool rgb = false;
    bool bgra = false;
    switch (vk)
    {
        case 37: /* R8G8B8A8_UNORM */
        case 43: /* R8G8B8A8_SRGB */
            bpp = 4;
            break;
        case 23: /* R8G8B8_UNORM */
        case 29: /* R8G8B8_SRGB */
            bpp = 3;
            rgb = true;
            break;
        case 44: /* B8G8R8A8_UNORM */
        case 50: /* B8G8R8A8_SRGB */
            bpp = 4;
            bgra = true;
            break;
        default:
            return 2; /* 非本函数处理的格式 */
    }
    if (w == 0 || h == 0 || depth != 0 || faces > 1 || layers > 1 || levels == 0 || scheme != 0)
    {
        set_err(err, err_cap, "ktx2: unsupported layout (3D/array/cubemap/supercompressed)");
        return 1;
    }
    if ((uint64_t)w * h > (uint64_t)16384 * 16384)
    {
        set_err(err, err_cap, "ktx2: image too large");
        return 1;
    }
    const size_t lvl_off = 80; /* KTX2 header 后紧跟 level index */
    if (lvl_off + 24 > size)
    {
        set_err(err, err_cap, "ktx2: truncated level index");
        return 1;
    }
    const uint64_t data_off = rd64(d + lvl_off);
    const uint64_t data_len = rd64(d + lvl_off + 8);
    const size_t row = (size_t)w * (size_t)bpp;
    if (data_off + (uint64_t)row * h > size || data_len < (uint64_t)row * h)
    {
        set_err(err, err_cap, "ktx2: level data out of range");
        return 1;
    }
    const size_t bytes = (size_t)w * h * 4u;
    uint8_t* out = (uint8_t*)std::malloc(bytes);
    if (!out)
    {
        set_err(err, err_cap, "ktx2: oom");
        return 1;
    }
    const uint8_t* src = d + (size_t)data_off;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* s = src + (size_t)y * row;
        uint8_t* o = out + (size_t)y * w * 4u;
        for (uint32_t x = 0; x < w; ++x)
        {
            if (rgb)
            {
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
                o[3] = 255;
                s += 3;
            }
            else if (bgra)
            {
                o[0] = s[2];
                o[1] = s[1];
                o[2] = s[0];
                o[3] = s[3];
                s += 4;
            }
            else
            {
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
                o[3] = s[3];
                s += 4;
            }
            o += 4;
        }
    }
    *out_rgba = out;
    *out_w = w;
    *out_h = h;
    return 0;
}
} // namespace

extern "C" int mq_ktx2_decode(const void* data, size_t size, uint8_t** out_rgba, uint32_t* out_w,
                              uint32_t* out_h, char* err, size_t err_cap)
{
    if (err && err_cap)
        err[0] = '\0';
    if (out_rgba)
        *out_rgba = nullptr;
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!data || size < 12 || size > 0xFFFFFFFFull)
    {
        set_err(err, err_cap, "ktx2: bad input");
        return 1;
    }
    static const uint8_t kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (std::memcmp(data, kId, sizeof kId) != 0)
    {
        set_err(err, err_cap, "ktx2: bad identifier");
        return 1;
    }
    if (size < 80)
    {
        set_err(err, err_cap, "ktx2: truncated header");
        return 1;
    }
    /* 未压缩 RGBA/RGB/BGRA 的常见 KTX2 直接转 RGBA；BasisU 走 transcoder。 */
    const int urc = decode_uncompressed((const uint8_t*)data, size, out_rgba, out_w, out_h,
                                        err, err_cap);
    if (urc != 2)
        return urc;

    basist::ktx2_transcoder dec;
    static std::once_flag init_flag;
    std::call_once(init_flag, []
                   { basist::basisu_transcoder_init(); });
    if (!dec.init(data, (uint32_t)size))
    {
        set_err(err, err_cap, "ktx2: init failed (invalid/truncated container)");
        return 1;
    }
    if (!dec.is_etc1s() && !dec.is_uastc())
    {
        set_err(err, err_cap, "ktx2: only Basis Universal ETC1S/UASTC is supported");
        return 1;
    }
    if (dec.get_faces() > 1 || dec.get_layers() > 1)
    {
        set_err(err, err_cap, "ktx2: cubemap/texture array not supported by image decode");
        return 1;
    }
    if (!dec.start_transcoding())
    {
        set_err(err, err_cap, "ktx2: start_transcoding failed");
        return 1;
    }

    const uint32_t w = dec.get_width();
    const uint32_t h = dec.get_height();
    if (w == 0 || h == 0 || (uint64_t)w * h > (uint64_t)16384 * 16384)
    {
        set_err(err, err_cap, "ktx2: image too large");
        return 1;
    }
    const size_t bytes = (size_t)w * h * 4u;
    std::vector<uint8_t> rgba(bytes);
    if (!dec.transcode_image_level(0, 0, 0, rgba.data(), (uint32_t)(w * h),
                                   basist::transcoder_texture_format::cTFRGBA32, 0, w, h))
    {
        set_err(err, err_cap, "ktx2: transcode failed");
        return 1;
    }

    uint8_t* out = (uint8_t*)std::malloc(bytes);
    if (!out)
    {
        set_err(err, err_cap, "ktx2: oom");
        return 1;
    }
    std::memcpy(out, rgba.data(), bytes);
    *out_rgba = out;
    *out_w = w;
    *out_h = h;
    return 0;
}
