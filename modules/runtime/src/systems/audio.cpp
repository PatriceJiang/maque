/* 系统 · 音频步进（docs/30 §6）：组件同步 → 空间化 → playhead 按模拟 dt 推进
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S7，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

void Engine::audio_step(float dt) {
    (void)dt;
    if (!audio || !scene || !has_audio_components) return;   // 无音源/听者：跳过两次全树遍历
    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 听者：每 instance 取稳定排序后的第一个 active */
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        const auto* lis = component<audio::AudioListener>(*e);
        if (!lis || !lis->active) continue;
        const float rad = e->angle * 3.14159265358979F / 180.0F;
        mq_audio_listener l{};
        l.struct_size = sizeof l;
        l.x = e->world_x;
        l.y = e->world_y;
        l.z = 0.0F;
        l.dir_x = std::cos(rad);
        l.dir_y = std::sin(rad);
        l.dir_z = 0.0F;
        mq_audio_set_listener(audio, &l);
        const float master = lis->master_volume;
        if (master != last_listener_master) {
            mq_audio_set_master(audio, master, 0);
            last_listener_master = master;
        }
        break;
    }

    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        auto* src = component<audio::AudioSource>(*e);
        if (!src) continue;
        uint64_t voice = src->voice;
        const std::string* clip_path = &src->clip;
        if (clip_path->empty()) {
            if (voice) {
                mq_audio_stop(audio, voice);
                src->voice = 0;
            }
            continue;
        }
        const bool autoplay = src->autoplay;
        const bool loop = src->loop;
        const float volume = src->volume;
        const float pitch = src->pitch;
        const float min_d = src->min_distance;
        const float max_d = src->max_distance;
        const uint32_t panning = (uint32_t)src->panning;
        const uint32_t attenuation = (uint32_t)src->attenuation;
        const std::string* bus = &src->bus;

        if (voice == 0) {
            if (!autoplay) continue;
            if (std::getenv("MAQUE_TRACE_AUDIO"))
                std::print("[audio] start id={} type={} clip='{}'\n", id, "audio.AudioSource", *clip_path);
            mq_audio_clip* clip = audio_clip(*clip_path);
            if (!clip) continue;
            mq_audio_voice_desc vd{};
            vd.struct_size = sizeof vd;
            vd.clip = clip;
            vd.volume = volume;
            vd.pitch = pitch;
            vd.loop = loop ? 1u : 0u;
            vd.bus = (bus && !bus->empty()) ? bus->c_str() : "sfx";
            vd.panning = panning;
            vd.attenuation = attenuation;
            vd.min_distance = min_d;
            vd.max_distance = max_d;
            vd.x = e->world_x;
            vd.y = e->world_y;
            vd.z = 0.0F;
            voice = mq_audio_play(audio, &vd);
            if (std::getenv("MAQUE_TRACE_AUDIO")) {
                uint32_t fr = 0, rt = 0, chn = 0;
                mq_audio_clip_info(clip, &fr, &rt, &chn);
                std::print("[audio] start voice={} frames={} rate={} ch={}\n", voice, fr, rt, chn);
            }
            if (!voice) continue;
            src->voice = voice;
        }
        /* 每帧同步可调参数（脚本可用 node_set_number 改 volume 等） */
        mq_audio_set_volume(audio, voice, volume);
        mq_audio_set_pitch(audio, voice, pitch);
        mq_audio_set_spatial(audio, voice, e->world_x, e->world_y, 0.0F, panning, attenuation,
                             min_d, max_d);
        uint32_t st = 0;
        float head = 0.0F;
        if (mq_audio_voice_state(audio, voice, &st, &head) == MQ_AUDIO_OK) {
            src->state = (audio::AudioPlayState)st;
            src->playhead = head;
        }
    }
}

/* ---- 音频生命周期（docs/30）---- */
void Engine::audio_setup() {
    mq_audio_desc d{};
    d.struct_size = sizeof d;
    d.sample_rate = 48000;
    d.channels = 2;
    d.buffer_frames = 1024;
    d.max_voices = 64;
    d.backend = MQ_AUDIO_BACKEND_NULL;   // 默认静音：逻辑/脚本可用，CI 不弹设备
    bool want_device = false;
    if (const char* env = std::getenv("MAQUE_AUDIO"); env && *env) {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0 ||
            std::strcmp(env, "null") == 0) {
            d.backend = MQ_AUDIO_BACKEND_NULL;
        } else if (std::strcmp(env, "sdl") == 0) {
            d.backend = MQ_AUDIO_BACKEND_SDL;
            want_device = true;
        } else {   // 1 / on / auto
            d.backend = MQ_AUDIO_BACKEND_AUTO;
            want_device = true;
        }
    }
    if (const char* dev = std::getenv("MAQUE_AUDIO_DEVICE"); dev && *dev) d.device = dev;
    if (const char* buf = std::getenv("MAQUE_AUDIO_BUFFER"); buf && *buf) {
        const int v = std::atoi(buf);
        if (v > 0) d.buffer_frames = (uint32_t)v;
    }
    const char* cap_path = std::getenv("MAQUE_AUDIO_CAPTURE");
    const bool capture = cap_path && *cap_path;
    if (capture) {
        /* 抓音必须独占混音推进（否则设备回调 + 抓音各 mix 一次 → playhead 双倍消耗） */
        d.backend = MQ_AUDIO_BACKEND_NULL;
        want_device = false;
    }
    audio = mq_audio_create(&d);
    if (audio) {
        mq_audio_stats st{};
        st.struct_size = sizeof st;
        mq_audio_get_stats(audio, &st);
        audio_cap_rate = st.sample_rate;
        audio_cap_channels = st.channels;
        std::print("runtime: audio {} backend rate={} ch={}\n",
                   capture ? "capture(null pump)"
                           : (want_device ? "device requested" : "null (MAQUE_AUDIO=off/null)"),
                   st.sample_rate, st.channels);
        if (capture) audio_capture_start(cap_path);
    } else {
        std::print(stderr, "runtime: audio init failed（脚本 audio_* 将返回失败）\n");
    }
}

void Engine::audio_teardown() {
    audio_capture_finish();
    if (audio) {
        mq_audio_destroy(audio);   // 内含 stop/close（join 音频线程）
        audio = nullptr;
    }
    for (auto& kv : audio_clips) mq_audio_clip_release(kv.second);
    audio_clips.clear();
}

void Engine::audio_capture_start(const char* path) {
    audio_cap = std::fopen(path, "wb");
    if (!audio_cap) {
        std::print(stderr, "runtime: audio capture open failed '{}'\n", path);
        return;
    }
    std::array<uint8_t, 44> header{};   // 先占位，退出时回填
    std::fwrite(header.data(), 1, header.size(), audio_cap);
    audio_cap_path = path;
    audio_cap_frames = 0;
    audio_cap_frac = 0.0;
    std::print("runtime: audio capture -> {}\n", path);
}

void Engine::audio_capture_step(float dt) {
    if (!audio_cap || !audio) return;
    audio_cap_frac += (double)dt * (double)audio_cap_rate;
    uint32_t frames = (uint32_t)audio_cap_frac;
    if (frames == 0) return;
    audio_cap_frac -= (double)frames;
    if (frames > 4800) frames = 4800;                 // 单帧上限（防 dt 异常）
    audio_cap_buf.assign((size_t)frames * 2, 0.0F);
    if (mq_audio_mix(audio, audio_cap_buf.data(), frames) != MQ_AUDIO_OK) return;
    std::vector<int16_t> pcm((size_t)frames * 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float v = std::clamp(audio_cap_buf[i], -1.0F, 1.0F);
        pcm[i] = (int16_t)std::lround(v * 32767.0F);
    }
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), audio_cap);
    audio_cap_frames += frames;
}

void Engine::audio_capture_finish() {
    if (!audio_cap) return;
    const uint32_t channels = audio_cap_channels ? audio_cap_channels : 2;
    const uint32_t data_bytes = (uint32_t)(audio_cap_frames * channels * 2u);
    const uint32_t byte_rate = audio_cap_rate * channels * 2u;
    std::array<uint8_t, 44> h{};
    auto w16 = [&](size_t off, uint16_t v) { h[off] = (uint8_t)(v & 0xFF); h[off + 1] = (uint8_t)(v >> 8); };
    auto w32 = [&](size_t off, uint32_t v) {
        h[off] = (uint8_t)(v & 0xFF); h[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        h[off + 2] = (uint8_t)((v >> 16) & 0xFF); h[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    };
    std::memcpy(h.data(), "RIFF", 4);
    w32(4, 36u + data_bytes);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);                       // PCM
    w16(22, (uint16_t)channels);
    w32(24, audio_cap_rate);
    w32(28, byte_rate);
    w16(32, (uint16_t)(channels * 2u));
    w16(34, 16);
    std::memcpy(h.data() + 36, "data", 4);
    w32(40, data_bytes);
    std::fseek(audio_cap, 0, SEEK_SET);
    std::fwrite(h.data(), 1, h.size(), audio_cap);
    std::fclose(audio_cap);
    audio_cap = nullptr;
    std::print("runtime: audio capture done '{}' frames={}\n", audio_cap_path,
               (unsigned long long)audio_cap_frames);
}

mq_audio_clip* Engine::audio_clip(const std::string& path) {
    if (!audio || path.empty()) return nullptr;
    auto it = audio_clips.find(path);
    if (it != audio_clips.end()) return it->second;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != MQ_VFS_OK || !data || n == 0) {
        std::print(stderr, "runtime: audio clip not found '{}'\n", path);
        return nullptr;
    }
    mq_audio_clip* clip = nullptr;
    const int rc = mq_audio_load_memory(audio, data, n, path.c_str(), &clip);
    mq_vfs_free(data);
    if (rc != MQ_AUDIO_OK || !clip) return nullptr;
    audio_clips[path] = clip;
    return clip;
}

}  // namespace maque::rt
