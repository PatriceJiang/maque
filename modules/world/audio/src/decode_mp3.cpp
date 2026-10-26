/* MP3 解码（dr_mp3，public domain / MIT-0）。
 *
 * 整段解码（P1 音效路径；长音乐 P2 改流式）：按 2048 帧块读取，避免依赖
 * Xing/VBR 头给出的总帧数（有些流没有）。超过 2 分钟拒绝，防止误加载长文件吃内存。
 */
#include "audio/decoder.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#include <memory>
#include <vector>

namespace maque::audio {

bool decode_mp3(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out, std::string& error) {
    out.reset();
    error.clear();
    if (!data || size < 4) { error = "mp3: too small"; return false; }
    if (size > (size_t)UINT32_MAX) { error = "mp3: file too large"; return false; }

    drmp3 dec{};
    if (!drmp3_init_memory(&dec, data, size, nullptr)) {
        error = "mp3: init failed";
        return false;
    }
    const uint32_t channels = dec.channels ? dec.channels : 2;
    const uint32_t rate = dec.sampleRate;
    if (channels < 1 || channels > 2 || rate == 0) {
        drmp3_uninit(&dec);
        error = "mp3: unsupported channels/rate";
        return false;
    }

    constexpr uint64_t kMaxFrames = 48000ull * 60ull * 2ull;   // 2 分钟
    std::vector<float> samples;
    std::vector<float> chunk((size_t)2048 * channels);
    for (;;) {
        const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&dec, 2048, chunk.data());
        if (got == 0) break;
        if (samples.size() + (size_t)got * channels > (size_t)kMaxFrames * channels) {
            drmp3_uninit(&dec);
            error = "mp3: too long (max 2 min)";
            return false;
        }
        samples.insert(samples.end(), chunk.begin(), chunk.begin() + (size_t)got * channels);
    }
    drmp3_uninit(&dec);
    if (samples.empty()) { error = "mp3: no frames"; return false; }

    auto clip = std::make_shared<Clip>();
    clip->sample_rate = rate;
    clip->channels = (uint16_t)channels;
    clip->samples = std::move(samples);
    out = std::move(clip);
    return true;
}

} // namespace maque::audio
