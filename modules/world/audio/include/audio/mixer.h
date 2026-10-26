/* maque::audio::Mixer —— 平台无关的混音核心（**不依赖 SDL / VFS / 引擎**）。
 *
 * 分层：
 *   - 本文件只有 C++ 标准库：可离线渲染（设备回调与单元测试走同一条 mix 路径）；
 *   - 设备后端（SDL / null / 未来的 miniaudio…）只负责"按周期调 render 回调"；
 *   - 解码器（WAV / OGG…）只负责"字节 → Clip"。
 *
 * 线程模型：Mixer 自身**不加锁**。由设备后端提供 lock()/unlock()：
 *   - 音频线程：mix()（不取锁）；
 *   - 模拟线程：play/stop/set_xxx/update()，调用方先 lock 再调用；
 *   - SDL 的 SDL_LockAudioDevice 会等当前回调返回 → 两侧互斥，回调里不阻塞。
 * 未来要换成无锁命令环时，只需替换这一层，不影响解码/后端接口。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace maque::audio {

/* 解码后的 PCM：交错 float32，采样率/声道为源文件原始值。 */
struct Clip {
    std::vector<float> samples;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;

    uint32_t frames() const {
        return channels ? static_cast<uint32_t>(samples.size() / channels) : 0u;
    }
    double seconds() const {
        return sample_rate ? static_cast<double>(frames()) / static_cast<double>(sample_rate) : 0.0;
    }
};

struct VoiceParams {
    float volume = 1.0F;
    float pitch = 1.0F;          /* 0.01..4；同时改变播放速度 */
    float pan = 0.0F;            /* -1 左 .. +1 右（Stereo2D） */
    bool loop = false;
    uint32_t panning = 0;        /* mq_audio_panning */
    uint32_t attenuation = 0;    /* mq_audio_attenuation */
    float x = 0.0F, y = 0.0F, z = 0.0F;
    float min_distance = 1.0F;
    float max_distance = 50.0F;
    float doppler = 0.0F;        /* P0 只记录 */
    std::string bus = "sfx";
};

struct Listener {
    float x = 0.0F, y = 0.0F, z = 0.0F;
    float dir_x = 1.0F, dir_y = 0.0F, dir_z = 0.0F;
    float master_volume = 1.0F;
};

class Mixer {
public:
    /* 初始化/重置（设备协商后的实际采样率、声道；P0 仅 stereo）。 */
    void init(uint32_t device_rate, uint32_t device_channels, uint32_t max_voices);

    /* 返回 voice 句柄（0 = 失败）。clip 由 voice 持有 shared_ptr → 调用方 release 安全。 */
    uint64_t play(std::shared_ptr<const Clip> clip, const VoiceParams& params);
    bool stop(uint64_t handle);
    bool pause(uint64_t handle, bool paused);
    bool set_params(uint64_t handle, float volume, float pitch, float pan);
    bool set_volume(uint64_t handle, float volume);
    bool set_pitch(uint64_t handle, float pitch);
    bool set_spatial(uint64_t handle, float x, float y, float z, uint32_t panning,
                     uint32_t attenuation, float min_distance, float max_distance);

    void set_listener(const Listener& l);
    void set_bus(const std::string& name, float volume, bool mute);
    void set_master(float volume, bool mute);

    /* 音频线程/离线：清空 out 并混音（frames = 每声道帧数；out 大小 = frames*channels）。 */
    void mix(float* out, uint32_t frames);

    /* 模拟线程：**逻辑时钟**推进（playhead/状态），与设备是否出声无关。 */
    void update(float dt);

    bool voice_state(uint64_t handle, uint32_t* state, float* playhead) const;
    uint32_t active_voices() const;
    /* 调试/控制面：按 slot 顺序枚举 active voice；找到返回 1 并填 handle/state/playhead。 */
    uint32_t voice_at(uint32_t index, uint64_t* handle, uint32_t* state, float* playhead) const;
    uint32_t device_rate() const { return rate_; }
    uint32_t device_channels() const { return channels_; }
    uint32_t max_voices() const { return static_cast<uint32_t>(voices_.size()); }

private:
    struct Voice {
        std::shared_ptr<const Clip> clip;
        VoiceParams params;
        double pos = 0.0;       /* 音频线程的采样位置（源采样率下的帧） */
        double logical = 0.0;   /* 模拟线程的逻辑时间（秒） */
        uint32_t generation = 0;
        bool active = false;
        bool paused = false;
    };
    struct Bus {
        float volume = 1.0F;
        bool mute = false;
    };

    uint64_t make_handle(uint32_t index) const;
    Voice* find(uint64_t handle);
    const Voice* find(uint64_t handle) const;
    float bus_gain(const std::string& name) const;

    std::vector<Voice> voices_;
    std::unordered_map<std::string, Bus> buses_;
    Listener listener_{};
    uint32_t rate_ = 0;
    uint32_t channels_ = 0;
    float master_volume_ = 1.0F;
    bool master_mute_ = false;
    uint32_t next_index_ = 0;
};

} // namespace maque::audio
