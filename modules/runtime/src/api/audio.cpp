/* 宿主 API · 音频（docs/30；一次性的音效/总线控制）
 *
 * 从 runtime.cpp 的 `namespace host` 按前缀拆出（plans/25 第 1 阶段 S2，纯搬运）：
 * 实现与顺序不变；声明仍由 schema 的 api_list 生成（maque_api.h）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace host {

/* ---- 音频（docs/30）：一次性音效；场景常驻声源用 audio.AudioSource 组件 ---- */
double audio_play(void* ctx, const char* path, float volume, float loop, const char* bus) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio || !path || !*path) return 0.0;
    mq_audio_clip* clip = eng->audio_clip(path);
    if (!clip) return 0.0;
    mq_audio_voice_desc d{};
    d.struct_size = sizeof d;
    d.clip = clip;
    d.volume = volume;
    d.pitch = 1.0F;
    d.loop = (loop != 0.0F) ? 1u : 0u;
    d.bus = (bus && *bus) ? bus : "sfx";
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    return (double)mq_audio_play(eng->audio, &d);
}

void audio_stop(void* ctx, double voice) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_stop(eng->audio, (uint64_t)voice);
}

void audio_pause(void* ctx, double voice, float paused) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_pause(eng->audio, (uint64_t)voice, paused != 0.0F);
}

void audio_set_volume(void* ctx, double voice, float volume) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_set_volume(eng->audio, (uint64_t)voice, volume);
}

void audio_set_pitch(void* ctx, double voice, float pitch) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && voice > 0.0) mq_audio_set_pitch(eng->audio, (uint64_t)voice, pitch);
}

float audio_state(void* ctx, double voice) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio || voice <= 0.0) return -1.0F;
    uint32_t state = 0;
    if (mq_audio_voice_state(eng->audio, (uint64_t)voice, &state, nullptr) != MQ_AUDIO_OK)
        return -1.0F;
    return (float)state;
}

float audio_playhead(void* ctx, double voice) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio || voice <= 0.0) return -1.0F;
    float head = 0.0F;
    if (mq_audio_voice_state(eng->audio, (uint64_t)voice, nullptr, &head) != MQ_AUDIO_OK)
        return -1.0F;
    return head;
}

float audio_enabled(void* ctx) {
    auto* eng = (Engine*)ctx;
    return (eng && eng->audio) ? 1.0F : 0.0F;
}

void audio_set_listener(void* ctx, float x, float y, float dir_x, float dir_y) {
    auto* eng = (Engine*)ctx;
    if (!eng || !eng->audio) return;
    mq_audio_listener l{};
    l.struct_size = sizeof l;
    l.x = x; l.y = y; l.z = 0.0F;
    l.dir_x = dir_x; l.dir_y = dir_y; l.dir_z = 0.0F;
    mq_audio_set_listener(eng->audio, &l);
}

void audio_master_volume(void* ctx, float volume) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio) mq_audio_set_master(eng->audio, volume, 0);
}

void audio_bus_volume(void* ctx, const char* bus, float volume) {
    auto* eng = (Engine*)ctx;
    if (eng && eng->audio && bus && *bus) mq_audio_set_bus(eng->audio, bus, volume, 0);
}

}  // namespace host
