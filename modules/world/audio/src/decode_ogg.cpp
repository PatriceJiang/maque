/* OGG Vorbis 解码（stb_vorbis，public domain）。
 *
 * 与 WAV 解码器同接口：整段解码为交错 float32；stb 分配的内存由这里 free。
 * 分层不变：本文件不碰 VFS/设备；加/换解码库只改 decode_audio() 的表。
 */
#include "audio/decoder.h"

#include <climits>
#include <cstdlib>
#include <memory>

extern "C" {
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                             short** output);
}

namespace maque::audio {

bool decode_ogg(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out, std::string& error) {
    out.reset();
    error.clear();
    if (!data || size < 4) { error = "ogg: too small"; return false; }
    if (size > (size_t)INT_MAX) { error = "ogg: file too large"; return false; }
    int channels = 0, rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(data, (int)size, &channels, &rate, &pcm);
    if (frames <= 0 || !pcm) {
        std::free(pcm);
        error = "ogg: decode failed";
        return false;
    }
    if (channels < 1 || channels > 2) {
        std::free(pcm);
        error = "ogg: only mono/stereo supported";
        return false;
    }
    if (rate <= 0) {
        std::free(pcm);
        error = "ogg: invalid sample rate";
        return false;
    }
    auto clip = std::make_shared<Clip>();
    clip->sample_rate = (uint32_t)rate;
    clip->channels = (uint16_t)channels;
    clip->samples.resize((size_t)frames * (size_t)channels);
    const size_t n = clip->samples.size();
    for (size_t i = 0; i < n; ++i) clip->samples[i] = (float)pcm[i] / 32768.0F;
    std::free(pcm);
    out = std::move(clip);
    return true;
}

} // namespace maque::audio
