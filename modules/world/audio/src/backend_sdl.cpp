#include "audio/backend.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace maque::audio {
namespace {

/* SDL2 设备后端。这是**唯一** include <SDL.h> 的音频文件（与 ui/imgui/backend_sdl.cpp 同模式）；
 * 换 miniaudio 等第三方后端时不需要动 Mixer/解码器/ABI。 */
class SdlDevice final : public AudioDevice {
public:
    ~SdlDevice() override { close(); }

    bool open(const DeviceConfig& cfg, RenderCallback render, void* user) override {
        if (dev_) return false;
        if (!render) return false;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;

        const uint32_t rate = cfg.sample_rate ? cfg.sample_rate : 48000;
        const uint32_t buffer = cfg.buffer_frames ? cfg.buffer_frames : 1024;
        const int allowed = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
        const char* name = (cfg.device && *cfg.device) ? cfg.device : nullptr;

        SDL_AudioSpec want{};
        want.freq = static_cast<int>(rate);
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = static_cast<Uint16>(std::min<uint32_t>(buffer, 8192));
        want.callback = &SdlDevice::trampoline;
        want.userdata = this;

        SDL_AudioSpec have{};
        dev_ = SDL_OpenAudioDevice(name, 0, &want, &have, allowed);
        if (!dev_) {
            /* 少数老驱动不支持 F32 → 退 S16（回调内转换）。 */
            want.format = AUDIO_S16SYS;
            dev_ = SDL_OpenAudioDevice(name, 0, &want, &have, allowed);
        }
        if (!dev_) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        if (have.format != AUDIO_F32SYS && have.format != AUDIO_S16SYS) {
            close();
            return false;
        }

        have_ = have;
        render_ = render;
        user_ = user;
        rate_ = static_cast<uint32_t>(have.freq);
        channels_ = have.channels ? have.channels : 2;
        cap_frames_ = std::max<uint32_t>(have.samples ? have.samples : buffer, 256);
        scratch_.assign(static_cast<size_t>(cap_frames_) * channels_, 0.0F);
        return true;
    }

    void close() override {
        if (!dev_) return;
        SDL_PauseAudioDevice(dev_, 1);
        SDL_CloseAudioDevice(dev_);   /* SDL 会等当前回调返回 */
        dev_ = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    void start() override { if (dev_) SDL_PauseAudioDevice(dev_, 0); }
    void stop() override { if (dev_) SDL_PauseAudioDevice(dev_, 1); }

    void lock() override { if (dev_) SDL_LockAudioDevice(dev_); }
    void unlock() override { if (dev_) SDL_UnlockAudioDevice(dev_); }

    uint32_t sample_rate() const override { return rate_; }
    uint32_t channels() const override { return channels_; }
    uint64_t callbacks() const override { return callbacks_.load(std::memory_order_relaxed); }
    const char* name() const override { return "sdl"; }

private:
    static void trampoline(void* user, Uint8* stream, int len) {
        static_cast<SdlDevice*>(user)->on_render(stream, len);
    }

    void on_render(Uint8* stream, int len) {
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (!stream || len <= 0) return;
        const bool f32 = have_.format == AUDIO_F32SYS;
        const uint32_t bps = f32 ? 4u : 2u;
        const uint32_t frame_bytes = bps * channels_;
        uint32_t frames = static_cast<uint32_t>(len) / (frame_bytes ? frame_bytes : 1u);
        if (frames == 0) return;
        /* 回调内不分配：超出预分配的极端情况只混前 cap 帧，其余补静音。 */
        const uint32_t mix_frames = std::min(frames, cap_frames_);
        std::fill(scratch_.begin(), scratch_.begin() + static_cast<size_t>(mix_frames) * channels_,
                  0.0F);
        render_(user_, scratch_.data(), mix_frames);

        if (f32) {
            const size_t used = static_cast<size_t>(mix_frames) * frame_bytes;
            std::memcpy(stream, scratch_.data(), used);
            if (used < static_cast<size_t>(len)) std::memset(stream + used, 0, static_cast<size_t>(len) - used);
        } else {
            auto* out = reinterpret_cast<int16_t*>(stream);
            const size_t n = static_cast<size_t>(mix_frames) * channels_;
            for (size_t i = 0; i < n; ++i) {
                const float v = std::clamp(scratch_[i], -1.0F, 1.0F);
                out[i] = static_cast<int16_t>(v * 32767.0F);
            }
            const size_t used = n * sizeof(int16_t);
            if (used < static_cast<size_t>(len)) std::memset(reinterpret_cast<Uint8*>(out) + used, 0,
                                                              static_cast<size_t>(len) - used);
        }
    }

    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec have_{};
    RenderCallback render_ = nullptr;
    void* user_ = nullptr;
    uint32_t rate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t cap_frames_ = 1024;
    std::vector<float> scratch_;
    std::atomic<uint64_t> callbacks_{0};
};

} // namespace

bool sdl_audio_available() { return true; }

std::unique_ptr<AudioDevice> make_sdl_device() { return std::make_unique<SdlDevice>(); }

} // namespace maque::audio
