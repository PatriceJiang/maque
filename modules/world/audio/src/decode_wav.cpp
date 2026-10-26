#include "audio/decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace maque::audio {
namespace {

uint16_t rd_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
bool tag_eq(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

bool ends_with_ci(const char* s, const char* ext) {
    if (!s || !ext) return false;
    const size_t n = std::strlen(s), m = std::strlen(ext);
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i) {
        char a = s[n - m + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

struct WavFormat {
    uint16_t format = 0;      /* 1=PCM, 3=float, 0xFFFE=extensible */
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    uint16_t block_align = 0;
};

} // namespace

bool decode_wav(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out, std::string& error) {
    out.reset();
    error.clear();
    if (!data || size < 12) { error = "wav: too small"; return false; }
    if (!tag_eq(data, "RIFF") || !tag_eq(data + 8, "WAVE")) {
        error = "wav: not RIFF/WAVE";
        return false;
    }

    WavFormat fmt;
    const uint8_t* pcm = nullptr;
    size_t pcm_size = 0;
    size_t off = 12;
    while (off + 8 <= size) {
        const uint8_t* id = data + off;
        uint32_t len = rd_u32(data + off + 4);
        off += 8;
        if (len > size - off) len = static_cast<uint32_t>(size - off);   /* 截断的尾块 */
        if (tag_eq(id, "fmt ") && len >= 16) {
            fmt.format = rd_u16(data + off);
            fmt.channels = rd_u16(data + off + 2);
            fmt.rate = rd_u32(data + off + 4);
            fmt.block_align = rd_u16(data + off + 12);
            fmt.bits = rd_u16(data + off + 14);
            if (fmt.format == 0xFFFE && len >= 40) fmt.format = rd_u16(data + off + 24);
        } else if (tag_eq(id, "data")) {
            pcm = data + off;
            pcm_size = len;
        }
        off += len + (len & 1u);   /* chunk 按偶数字节对齐 */
    }

    if (!pcm || pcm_size == 0) { error = "wav: missing data chunk"; return false; }
    if (fmt.channels < 1 || fmt.channels > 2) { error = "wav: only mono/stereo supported"; return false; }
    if (fmt.rate == 0) { error = "wav: invalid sample rate"; return false; }
    if (fmt.format != 1 && fmt.format != 3) { error = "wav: unsupported format tag"; return false; }
    if (fmt.bits != 8 && fmt.bits != 16 && fmt.bits != 24 && fmt.bits != 32) {
        error = "wav: unsupported bit depth";
        return false;
    }
    const uint32_t bytes_per_sample = fmt.bits / 8u;
    const uint32_t block = fmt.block_align ? fmt.block_align : fmt.channels * bytes_per_sample;
    if (block == 0) { error = "wav: invalid block align"; return false; }
    const size_t frames = pcm_size / block;
    if (frames == 0) { error = "wav: empty data"; return false; }

    auto clip = std::make_shared<Clip>();
    clip->sample_rate = fmt.rate;
    clip->channels = fmt.channels;
    clip->samples.resize(frames * fmt.channels);

    const auto read_sample = [&](const uint8_t* p) -> float {
        if (fmt.format == 3) {
            float f = 0.0F;
            std::memcpy(&f, p, sizeof f);
            return std::isfinite(f) ? f : 0.0F;
        }
        switch (fmt.bits) {
            case 8:  return (static_cast<float>(*p) - 128.0F) / 128.0F;
            case 16: return static_cast<float>(static_cast<int16_t>(rd_u16(p))) / 32768.0F;
            case 24: {
                int32_t s = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
                if (s & 0x800000) s |= ~0xFFFFFF;
                return static_cast<float>(s) / 8388608.0F;
            }
            case 32: {
                const uint32_t u = rd_u32(p);
                int32_t s = 0;
                std::memcpy(&s, &u, sizeof s);
                return static_cast<float>(static_cast<double>(s) / 2147483648.0);
            }
            default: return 0.0F;
        }
    };

    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* frame = pcm + f * block;
        for (uint16_t c = 0; c < fmt.channels; ++c)
            clip->samples[f * fmt.channels + c] = read_sample(frame + c * bytes_per_sample);
    }

    out = std::move(clip);
    return true;
}

bool decode_audio(const void* data, size_t size, const char* hint, std::shared_ptr<Clip>& out,
                  std::string& error) {
    static const struct {
        const char* ext;
        DecodeFn fn;
    } kDecoders[] = {
        { ".wav", &decode_wav },
        { ".ogg", &decode_ogg },
        { ".mp3", &decode_mp3 },
    };
    if (!data || size == 0) { error = "audio: empty buffer"; return false; }

    for (const auto& d : kDecoders) {
        if (!ends_with_ci(hint, d.ext)) continue;
        return d.fn(static_cast<const uint8_t*>(data), size, out, error);
    }
    /* 未知扩展名：依次尝试（各自校验文件头），全部失败时返回最后一个原因。 */
    std::string last = "audio: unsupported format";
    for (const auto& d : kDecoders) {
        if (d.fn(static_cast<const uint8_t*>(data), size, out, error)) return true;
        if (!error.empty()) last = error;
    }
    error = last;
    out.reset();
    return false;
}

} // namespace maque::audio
