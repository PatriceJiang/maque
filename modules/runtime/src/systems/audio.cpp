/* 系统 · 音频步进（docs/30 §6）：组件同步 → 空间化 → playhead 按模拟 dt 推进
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S7，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

void Engine::audio_step(float dt) {
    (void)dt;
    if (!audio || !scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 听者：每 instance 取稳定排序后的第一个 active */
    const mq_type_desc* listener_desc = nullptr;
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        auto* lb = comp_by_role(*this, id, MQ_ROLE_AUDIO_LISTENER);
        if (!lb) continue;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(reflect, tid);
            if (d && (d->role & MQ_ROLE_AUDIO_LISTENER)) { listener_desc = d; break; }
        }
        const auto* lis = listener_desc ? as_component<audio::AudioListener>(listener_desc, lb->data()) : nullptr;
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
        auto* sb = comp_by_role(*this, id, MQ_ROLE_AUDIO_SOURCE);
        if (!sb) continue;
        const mq_type_desc* d = nullptr;
        for (auto& [tid, b] : e->comps) {
            const mq_type_desc* t = mq_reflect_type_by_id(reflect, tid);
            if (t && (t->role & MQ_ROLE_AUDIO_SOURCE)) { d = t; break; }
        }
        auto* src = d ? as_component_mut<audio::AudioSource>(d, sb->data()) : nullptr;
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
                std::print("[audio] start id={} type={} clip='{}'\n", id, d->name, *clip_path);
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

}  // namespace maque::rt
