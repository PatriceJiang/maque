/* 控制面 · 音频（docs/30；voice 用字符串句柄）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

/* ---- 音频控制面（docs/30）：只读查询 + 变更类（D2 由 control_mutating 统一限制）---- */
extern "C" int mq_control_Audio_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->audio) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"audio unavailable\"}"); return 0; }
    mq_audio_stats st{};
    st.struct_size = sizeof st;
    mq_audio_get_stats(e->audio, &st);
    std::snprintf(out, cap,
                  "{\"ok\":true,\"sample_rate\":%u,\"channels\":%u,\"voices\":%u,"
                  "\"callbacks\":%llu,\"dropped\":%llu}",
                  st.sample_rate, st.channels, st.voices,
                  (unsigned long long)st.callbacks, (unsigned long long)st.dropped);
    return 0;
}

extern "C" int mq_control_Audio_voices(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->audio) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"audio unavailable\"}"); return 0; }
    std::string json = "{\"ok\":true,\"voices\":[";
    uint64_t voice = 0;
    uint32_t state = 0;
    float head = 0.0F;
    for (uint32_t i = 0; i < 256; ++i) {
        if (!mq_audio_voice_at(e->audio, i, &voice, &state, &head)) break;
        if (i) json += ',';
        char b[192];
        std::snprintf(b, sizeof b,
                      "{\"voice\":\"%llu\",\"state\":%u,\"playhead\":%.6f}",
                      (unsigned long long)voice, state, (double)head);
        json += b;
    }
    json += "]}";
    std::snprintf(out, cap, "%s", json.c_str());
    return 0;
}

extern "C" int mq_control_Audio_play(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!e->audio || !path || !*path) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path/audio\"}");
        return 0;
    }
    mq_audio_clip* clip = e->audio_clip(path);
    if (!clip) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"load failed\"}"); return 0; }
    mq_audio_voice_desc d{};
    d.struct_size = sizeof d;
    d.clip = clip;
    d.volume = (float)mq_json_num(mq_json_get(params, "volume"), 1.0);
    d.pitch = 1.0F;
    d.loop = mq_json_num(mq_json_get(params, "loop"), 0.0) != 0.0 ? 1u : 0u;
    const char* bus = mq_json_str(mq_json_get(params, "bus"), "sfx");
    d.bus = (bus && *bus) ? bus : "sfx";
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    const uint64_t voice = mq_audio_play(e->audio, &d);
    if (!voice) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"voice pool full\"}"); return 0; }
    std::snprintf(out, cap, "{\"ok\":true,\"voice\":\"%llu\"}", (unsigned long long)voice);
    return 0;
}

extern "C" int mq_control_Audio_stop(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* vs = mq_json_str(mq_json_get(params, "voice"), nullptr);
    const uint64_t voice = (vs && *vs) ? std::strtoull(vs, nullptr, 10) : 0;
    if (!e->audio || !voice) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing voice\"}"); return 0; }
    const int rc = mq_audio_stop(e->audio, voice);
    std::snprintf(out, cap, "{\"ok\":%s}", rc == MQ_AUDIO_OK ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Audio_set_master(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->audio) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"audio unavailable\"}"); return 0; }
    const float volume = (float)mq_json_num(mq_json_get(params, "volume"), 1.0);
    const bool mute = mq_json_num(mq_json_get(params, "mute"), 0.0) != 0.0;
    const int rc = mq_audio_set_master(e->audio, volume, mute ? 1u : 0u);
    std::snprintf(out, cap, "{\"ok\":%s}", rc == MQ_AUDIO_OK ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Audio_set_bus(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* bus = mq_json_str(mq_json_get(params, "bus"), nullptr);
    if (!e->audio || !bus || !*bus) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing bus\"}");
        return 0;
    }
    const float volume = (float)mq_json_num(mq_json_get(params, "volume"), 1.0);
    const bool mute = mq_json_num(mq_json_get(params, "mute"), 0.0) != 0.0;
    const int rc = mq_audio_set_bus(e->audio, bus, volume, mute ? 1u : 0u);
    std::snprintf(out, cap, "{\"ok\":%s}", rc == MQ_AUDIO_OK ? "true" : "false");
    return 0;
}
