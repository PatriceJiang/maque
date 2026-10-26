/* 设备后端接口：可替换的"谁来周期性地要音频帧"。
 *
 * 将来接第三方（miniaudio / SoLoud / 平台原生）只需要：
 *   1) 新文件实现 AudioDevice（open/close/start/stop/lock/unlock + stats）；
 *   2) 在 audio.cpp 的后端选择处加一个分支；
 *   3) 不动 Mixer、解码器、C ABI、runtime。
 *
 * P0 提供：
 *   - null：无声，只用于离线混音/CI/无设备环境；
 *   - sdl：SDL2 audio（Windows WASAPI / Linux Pulse-PipeWire-ALSA / macOS CoreAudio / WASM WebAudio）。
 */
#pragma once

#include <cstdint>
#include <memory>

namespace maque::audio {

struct DeviceConfig {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t buffer_frames = 1024;
    const char* device = nullptr;   /* 仅 open() 期间借用；nullptr/空 = 默认设备 */
};

/* 音频线程回调：把 frames 帧交错 float 写进 out（out 由后端提供，已按 channels 分配）。 */
using RenderCallback = void (*)(void* user, float* out, uint32_t frames);

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual bool open(const DeviceConfig& cfg, RenderCallback render, void* user) = 0;
    virtual void close() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

    /* 互斥：后端保证"lock 期间回调不会运行"（SDL：SDL_LockAudioDevice）。
     * 模拟线程改 voice 参数时用它；回调内**不得**调用 lock。 */
    virtual void lock() = 0;
    virtual void unlock() = 0;

    virtual uint32_t sample_rate() const = 0;
    virtual uint32_t channels() const = 0;
    virtual uint64_t callbacks() const = 0;
    virtual const char* name() const = 0;
};

std::unique_ptr<AudioDevice> make_null_device();

bool sdl_audio_available();
#ifdef MAQUE_HAVE_SDL_AUDIO
std::unique_ptr<AudioDevice> make_sdl_device();
#endif

} // namespace maque::audio
