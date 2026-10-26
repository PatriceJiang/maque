/* maque.audio 的 C ABI 门面：把 Mixer / 解码器 / 设备后端 组装成 `mq_audio_*`。
 *
 * 分层边界：
 *   - Mixer：纯 C++（无 SDL/VFS）；
 *   - decoder：字节 → Clip（WAV 现在；OGG/MP3 后续加）；
 *   - backend：AudioDevice 接口（SDL / null；第三方后端只实现接口）；
 *   - 本文件只做参数校验、生命周期、环境变量、锁的包裹。
 */
#include "abi/audio.h"

#include "audio/backend.h"
#include "audio/decoder.h"
#include "audio/mixer.h"
#include "core/tracy/mq_tracy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>

using maque::audio::AudioDevice;
using maque::audio::Clip;
using maque::audio::DeviceConfig;
using maque::audio::Listener;
using maque::audio::Mixer;
using maque::audio::RenderCallback;
using maque::audio::VoiceParams;

struct mq_audio_clip {
    std::shared_ptr<const Clip> clip;
};

struct mq_audio {
    Mixer mixer;
    std::unique_ptr<AudioDevice> device;
    std::string device_name;
    uint64_t dropped = 0;
    bool sdl_fallback_logged = false;
};

namespace {

void render_cb(void* user, float* out, uint32_t frames) {
    MQ_TRACY_ZONE_N(audio, "mix");   // 音频线程的外层入口（docs/29：只埋外层）
    static_cast<mq_audio*>(user)->mixer.mix(out, frames);
}

uint32_t env_backend_override() {
    const char* e = std::getenv("MAQUE_AUDIO");
    if (!e || !*e) return 0;   // AUTO
    if (std::strcmp(e, "off") == 0 || std::strcmp(e, "null") == 0 || std::strcmp(e, "0") == 0)
        return MQ_AUDIO_BACKEND_NULL;
    if (std::strcmp(e, "sdl") == 0) return MQ_AUDIO_BACKEND_SDL;
    return MQ_AUDIO_BACKEND_AUTO;
}

bool open_device(mq_audio* a, uint32_t backend, const DeviceConfig& cfg, bool log_fallback) {
    const uint32_t chosen = backend;
    if (chosen == MQ_AUDIO_BACKEND_NULL) {
        auto dev = maque::audio::make_null_device();
        if (!dev->open(cfg, &render_cb, a)) return false;
        a->device_name = dev->name();
        a->device = std::move(dev);
        return true;
    }
#ifdef MAQUE_HAVE_SDL_AUDIO
    if (chosen == MQ_AUDIO_BACKEND_SDL || chosen == MQ_AUDIO_BACKEND_AUTO) {
        auto dev = maque::audio::make_sdl_device();
        if (dev->open(cfg, &render_cb, a)) {
            a->device_name = dev->name();
            a->device = std::move(dev);
            return true;
        }
        if (log_fallback && !a->sdl_fallback_logged) {
            a->sdl_fallback_logged = true;
            std::print(stderr, "audio: SDL 设备打开失败，回退 null 后端（逻辑播放照常）\n");
        }
        if (chosen == MQ_AUDIO_BACKEND_SDL) {
            auto nul = maque::audio::make_null_device();
            if (nul->open(cfg, &render_cb, a)) {
                a->device_name = nul->name();
                a->device = std::move(nul);
                return true;
            }
        }
    }
#endif
    /* AUTO 无 SDL / SDL 失败：null */
    auto nul = maque::audio::make_null_device();
    if (!nul->open(cfg, &render_cb, a)) return false;
    a->device_name = nul->name();
    a->device = std::move(nul);
    return true;
}

} // namespace

extern "C" {

mq_audio* mq_audio_create(const mq_audio_desc* desc) {
    MQ_TRACY_ZONE_N(audio, "create");
    uint32_t rate = MQ_AUDIO_DEFAULT_RATE;
    uint32_t channels = MQ_AUDIO_DEFAULT_CHANNELS;
    uint32_t buffer = MQ_AUDIO_DEFAULT_BUFFER;
    uint32_t voices = MQ_AUDIO_DEFAULT_VOICES;
    uint32_t backend = MQ_AUDIO_BACKEND_AUTO;
    const char* device_name = nullptr;
    if (desc) {
        if (desc->struct_size != 0) {
            if (desc->sample_rate) rate = desc->sample_rate;
            if (desc->channels) channels = desc->channels;
            if (desc->buffer_frames) buffer = desc->buffer_frames;
            if (desc->max_voices) voices = desc->max_voices;
            backend = desc->backend;
            device_name = desc->device;
        }
    }
    const uint32_t env = env_backend_override();
    if (env != 0) backend = env;
    channels = (channels == 1) ? 1 : 2;

    auto* a = new mq_audio();
    a->mixer.init(rate, channels, voices);
    DeviceConfig cfg{rate, channels, buffer, device_name};
    if (!open_device(a, backend, cfg, true)) {
        delete a;
        return nullptr;
    }
    /* 设备可能协商出不同采样率/声道 → 混音器以设备为准。 */
    a->mixer.init(a->device->sample_rate(), a->device->channels(), voices);
    a->device->start();
    return a;
}

void mq_audio_destroy(mq_audio* a) {
    if (!a) return;
    if (a->device) {
        a->device->stop();
        a->device->close();
        a->device.reset();
    }
    delete a;
}

int mq_audio_load_memory(mq_audio* a, const void* data, size_t size, const char* hint,
                         mq_audio_clip** out) {
    if (!a || !data || size == 0 || !out) return MQ_AUDIO_ERR_ARG;
    *out = nullptr;
    std::shared_ptr<Clip> clip;
    std::string error;
    if (!maque::audio::decode_audio(data, size, hint, clip, error) || !clip) {
        std::print(stderr, "audio: decode '{}' failed: {}\n", hint ? hint : "(memory)",
                   error.empty() ? "unknown" : error);
        return MQ_AUDIO_ERR_FORMAT;
    }
    auto* w = new mq_audio_clip();
    w->clip = std::move(clip);
    *out = w;
    return MQ_AUDIO_OK;
}

void mq_audio_clip_release(mq_audio_clip* clip) { delete clip; }

int mq_audio_clip_info(const mq_audio_clip* clip, uint32_t* frames, uint32_t* rate,
                       uint32_t* channels) {
    if (!clip || !clip->clip) return MQ_AUDIO_ERR_ARG;
    if (frames) *frames = clip->clip->frames();
    if (rate) *rate = clip->clip->sample_rate;
    if (channels) *channels = clip->clip->channels;
    return MQ_AUDIO_OK;
}

uint64_t mq_audio_play(mq_audio* a, const mq_audio_voice_desc* d) {
    if (!a || !d || !d->clip || !d->clip->clip) return 0;
    VoiceParams p;
    p.volume = std::clamp(d->volume, 0.0F, 4.0F);
    p.pitch = std::clamp(d->pitch > 0.0F ? d->pitch : 1.0F, 0.01F, 4.0F);
    p.pan = std::clamp(d->pan, -1.0F, 1.0F);
    p.loop = d->loop != 0;
    p.panning = d->panning;
    p.attenuation = d->attenuation;
    p.x = d->x; p.y = d->y; p.z = d->z;
    p.min_distance = d->min_distance > 0.0F ? d->min_distance : 1.0F;
    p.max_distance = d->max_distance > p.min_distance ? d->max_distance : p.min_distance + 1.0F;
    p.doppler = d->doppler;
    if (d->bus && *d->bus) p.bus = d->bus;

    a->device->lock();
    const uint64_t h = a->mixer.play(d->clip->clip, p);
    a->device->unlock();
    if (h == 0) ++a->dropped;
    return h;
}

int mq_audio_stop(mq_audio* a, uint64_t voice) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.stop(voice);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

int mq_audio_pause(mq_audio* a, uint64_t voice, uint32_t paused) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.pause(voice, paused != 0);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

int mq_audio_set_voice(mq_audio* a, uint64_t voice, float volume, float pitch, float pan) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.set_params(voice, volume, pitch, pan);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

int mq_audio_set_volume(mq_audio* a, uint64_t voice, float volume) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.set_volume(voice, volume);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

int mq_audio_set_pitch(mq_audio* a, uint64_t voice, float pitch) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.set_pitch(voice, pitch);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

int mq_audio_set_spatial(mq_audio* a, uint64_t voice, float x, float y, float z,
                         uint32_t panning, uint32_t attenuation, float min_distance,
                         float max_distance) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.set_spatial(voice, x, y, z, panning, attenuation,
                                         min_distance, max_distance);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

int mq_audio_set_listener(mq_audio* a, const mq_audio_listener* l) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    Listener out;
    if (l) {
        out.x = l->x; out.y = l->y; out.z = l->z;
        out.dir_x = l->dir_x; out.dir_y = l->dir_y; out.dir_z = l->dir_z;
    }
    a->device->lock();
    a->mixer.set_listener(out);
    a->device->unlock();
    return MQ_AUDIO_OK;
}

int mq_audio_set_bus(mq_audio* a, const char* bus, float volume, uint32_t mute) {
    if (!a || !bus) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    a->mixer.set_bus(bus, volume, mute != 0);
    a->device->unlock();
    return MQ_AUDIO_OK;
}

int mq_audio_set_master(mq_audio* a, float volume, uint32_t mute) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    a->mixer.set_master(volume, mute != 0);
    a->device->unlock();
    return MQ_AUDIO_OK;
}

int mq_audio_update(mq_audio* a, float dt) {
    if (!a || !(dt >= 0.0F) || !std::isfinite(dt)) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    a->mixer.update(dt);
    a->device->unlock();
    return MQ_AUDIO_OK;
}

int mq_audio_mix(mq_audio* a, float* out, uint32_t frames) {
    if (!a || !out || frames == 0) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    a->mixer.mix(out, frames);
    a->device->unlock();
    return MQ_AUDIO_OK;
}

int mq_audio_voice_state(mq_audio* a, uint64_t voice, uint32_t* state, float* playhead) {
    if (!a) return MQ_AUDIO_ERR_ARG;
    a->device->lock();
    const bool ok = a->mixer.voice_state(voice, state, playhead);
    a->device->unlock();
    return ok ? MQ_AUDIO_OK : MQ_AUDIO_ERR_HANDLE;
}

uint32_t mq_audio_voice_at(mq_audio* a, uint32_t index, uint64_t* voice, uint32_t* state,
                           float* playhead) {
    if (!a) return 0;
    a->device->lock();
    const uint32_t ok = a->mixer.voice_at(index, voice, state, playhead);
    a->device->unlock();
    return ok;
}

int mq_audio_get_stats(mq_audio* a, mq_audio_stats* out) {
    if (!a || !out) return MQ_AUDIO_ERR_ARG;
    out->struct_size = sizeof(mq_audio_stats);
    a->device->lock();   // 与音频线程的 voice 状态串行（active_voices 不是原子）
    out->sample_rate = a->mixer.device_rate();
    out->channels = a->mixer.device_channels();
    out->voices = a->mixer.active_voices();
    out->dropped = a->dropped;
    a->device->unlock();
    out->callbacks = a->device ? a->device->callbacks() : 0;
    return MQ_AUDIO_OK;
}

} // extern "C"
