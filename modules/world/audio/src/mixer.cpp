#include "audio/mixer.h"

#include <algorithm>
#include <cmath>

namespace maque::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;

/* 距离衰减（P0 三档；CustomCurve 在 P2 接入资产曲线）。
 * 单位：世界单位（m）。dist 已 >= 0。 */
float attenuation_gain(uint32_t model, float dist, float min_d, float max_d) {
    const float lo = std::max(min_d, 0.001F);
    const float hi = std::max(max_d, lo + 0.001F);
    switch (model) {
        case 1:   /* Linear */
            return std::clamp((hi - dist) / (hi - lo), 0.0F, 1.0F);
        case 2: { /* Exponential：按归一化距离平方衰减 */
            const float t = std::clamp((dist - lo) / (hi - lo), 0.0F, 1.0F);
            return (1.0F - t) * (1.0F - t);
        }
        case 0:   /* InverseDistance（默认） */
        default:
            return std::clamp(lo / std::max(dist, lo), 0.0F, 1.0F);
    }
}

} // namespace

void Mixer::init(uint32_t device_rate, uint32_t device_channels, uint32_t max_voices) {
    rate_ = device_rate ? device_rate : 48000;
    channels_ = (device_channels == 1) ? 1 : 2;   /* P0 只支持 mono/stereo 输出 */
    const uint32_t cap = std::max<uint32_t>(max_voices ? max_voices : 64, 1);
    voices_.assign(cap, Voice{});
    for (auto& v : voices_) v.generation = 1;
    buses_.clear();
    listener_ = Listener{};
    master_volume_ = 1.0F;
    master_mute_ = false;
    next_index_ = 0;
}

uint64_t Mixer::make_handle(uint32_t index) const {
    const auto& v = voices_[index];
    return (static_cast<uint64_t>(v.generation) << 32) | (static_cast<uint64_t>(index) + 1u);
}

Mixer::Voice* Mixer::find(uint64_t handle) {
    const uint32_t low = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    const uint32_t gen = static_cast<uint32_t>(handle >> 32);
    if (low == 0) return nullptr;
    const uint32_t index = low - 1u;
    if (index >= voices_.size()) return nullptr;
    Voice& v = voices_[index];
    if (v.generation != gen) return nullptr;
    return &v;
}

const Mixer::Voice* Mixer::find(uint64_t handle) const {
    return const_cast<Mixer*>(this)->find(handle);
}

uint64_t Mixer::play(std::shared_ptr<const Clip> clip, const VoiceParams& params) {
    if (!clip || clip->frames() == 0 || clip->channels == 0) return 0;
    if (clip->channels > 2) return 0;   /* P0：mono/stereo */
    Voice* slot = nullptr;
    const uint32_t n = static_cast<uint32_t>(voices_.size());
    for (uint32_t i = 0; i < n; ++i) {
        Voice& v = voices_[(next_index_ + i) % n];
        if (!v.active) { slot = &v; break; }
    }
    if (!slot) return 0;
    next_index_ = (static_cast<uint32_t>(slot - voices_.data()) + 1u) % n;
    ++slot->generation;
    if (slot->generation == 0) slot->generation = 1;   // 句柄高 32 位不得为 0
    slot->clip = std::move(clip);
    slot->params = params;
    slot->params.volume = std::clamp(slot->params.volume, 0.0F, 4.0F);
    slot->params.pitch = std::clamp(slot->params.pitch > 0.0F ? slot->params.pitch : 1.0F, 0.01F, 4.0F);
    slot->params.pan = std::clamp(slot->params.pan, -1.0F, 1.0F);
    slot->pos = 0.0;
    slot->logical = 0.0;
    slot->paused = false;
    slot->active = true;
    return make_handle(static_cast<uint32_t>(slot - voices_.data()));
}

bool Mixer::stop(uint64_t handle) {
    Voice* v = find(handle);
    if (!v || !v->active) return false;
    v->active = false;
    v->paused = false;
    v->clip.reset();
    return true;
}

bool Mixer::pause(uint64_t handle, bool paused) {
    Voice* v = find(handle);
    if (!v || !v->active) return false;
    v->paused = paused;
    return true;
}

bool Mixer::set_volume(uint64_t handle, float volume) {
    Voice* v = find(handle);
    if (!v || !v->active) return false;
    v->params.volume = std::clamp(volume, 0.0F, 4.0F);
    return true;
}

bool Mixer::set_pitch(uint64_t handle, float pitch) {
    Voice* v = find(handle);
    if (!v || !v->active) return false;
    v->params.pitch = std::clamp(pitch > 0.0F ? pitch : 1.0F, 0.01F, 4.0F);
    return true;
}

bool Mixer::set_spatial(uint64_t handle, float x, float y, float z, uint32_t panning,
                        uint32_t attenuation, float min_distance, float max_distance) {
    Voice* v = find(handle);
    if (!v || !v->active) return false;
    v->params.x = x;
    v->params.y = y;
    v->params.z = z;
    v->params.panning = panning;
    v->params.attenuation = attenuation;
    v->params.min_distance = min_distance > 0.0F ? min_distance : 1.0F;
    v->params.max_distance = max_distance > v->params.min_distance ? max_distance
                                                                   : v->params.min_distance + 1.0F;
    return true;
}

bool Mixer::set_params(uint64_t handle, float volume, float pitch, float pan) {
    Voice* v = find(handle);
    if (!v || !v->active) return false;
    v->params.volume = std::clamp(volume, 0.0F, 4.0F);
    v->params.pitch = std::clamp(pitch > 0.0F ? pitch : 1.0F, 0.01F, 4.0F);
    v->params.pan = std::clamp(pan, -1.0F, 1.0F);
    return true;
}

void Mixer::set_listener(const Listener& l) { listener_ = l; }

void Mixer::set_bus(const std::string& name, float volume, bool mute) {
    Bus& b = buses_[name];
    b.volume = std::clamp(volume, 0.0F, 4.0F);
    b.mute = mute;
}

void Mixer::set_master(float volume, bool mute) {
    master_volume_ = std::clamp(volume, 0.0F, 4.0F);
    master_mute_ = mute;
}

float Mixer::bus_gain(const std::string& name) const {
    const auto it = buses_.find(name);
    if (it == buses_.end()) return 1.0F;
    return it->second.mute ? 0.0F : it->second.volume;
}

void Mixer::mix(float* out, uint32_t frames) {
    if (!out || frames == 0) return;
    const uint32_t out_ch = channels_ ? channels_ : 2;
    std::fill(out, out + static_cast<size_t>(frames) * out_ch, 0.0F);
    if (!rate_) return;

    const float master = master_mute_ ? 0.0F : master_volume_;
    if (master <= 0.0F) return;

    for (Voice& v : voices_) {
        if (!v.active || v.paused || !v.clip) continue;
        const Clip& c = *v.clip;
        const uint32_t cframes = c.frames();
        const uint32_t cch = c.channels;
        if (!cframes || !cch) continue;

        const double step = static_cast<double>(v.params.pitch) *
                            static_cast<double>(c.sample_rate) / static_cast<double>(rate_);
        if (!(step > 0.0)) continue;

        float gain = v.params.volume * bus_gain(v.params.bus) * master;
        float pan = v.params.pan;
        if (v.params.panning != 0 && cch >= 1) {
            const float dx = v.params.x - listener_.x;
            const float dy = v.params.y - listener_.y;
            const float dz = v.params.z - listener_.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            gain *= attenuation_gain(v.params.attenuation, dist, v.params.min_distance,
                                     v.params.max_distance);
            /* 听者右向量：朝向 (dir_x, dir_y) 在 2D 平面内旋转 -90° → (-dir_y, dir_x)。
             * 投影到右向量并按水平距离归一化 → 方位 pan（3D 的 HRTF 留 P2）。 */
            /* 朝向 dir 的"右"：y 向上的 2D 里 facing +x 时右手边是 -y → (dir_y, -dir_x) */
            const float right_x = listener_.dir_y;
            const float right_y = -listener_.dir_x;
            const float horiz = std::sqrt(dx * dx + dy * dy);
            const float proj = dx * right_x + dy * right_y;
            pan = std::clamp(proj / std::max(horiz, 1e-3F), -1.0F, 1.0F);
        }
        if (channels_ == 2) {
            /* 等功率 pan：pan=0 时两侧各 1/√2。 */
            const float a = (pan + 1.0F) * 0.25F * static_cast<float>(kPi);
            const float gl = std::cos(a) * gain;
            const float gr = std::sin(a) * gain;
            for (uint32_t i = 0; i < frames; ++i) {
                const uint32_t i0 = static_cast<uint32_t>(v.pos);
                if (i0 >= cframes) break;
                const uint32_t i1 = (i0 + 1u < cframes) ? (i0 + 1u) : (v.params.loop ? 0u : i0);
                const float frac = static_cast<float>(v.pos - static_cast<double>(i0));
                float sl = 0.0F, sr = 0.0F;
                if (cch == 1) {
                    const float s0 = c.samples[i0];
                    const float s1 = c.samples[i1];
                    sl = sr = s0 + (s1 - s0) * frac;
                } else {
                    const float l0 = c.samples[i0 * 2 + 0];
                    const float l1 = c.samples[i1 * 2 + 0];
                    const float r0 = c.samples[i0 * 2 + 1];
                    const float r1 = c.samples[i1 * 2 + 1];
                    sl = l0 + (l1 - l0) * frac;
                    sr = r0 + (r1 - r0) * frac;
                }
                out[i * 2 + 0] += sl * gl;
                out[i * 2 + 1] += sr * gr;
                v.pos += step;
                if (v.pos >= static_cast<double>(cframes)) {
                    if (v.params.loop) {
                        v.pos = std::fmod(v.pos, static_cast<double>(cframes));
                    } else {
                        v.active = false;   // 不 reset clip：音频线程禁止 free（等 slot 复用/stop）
                        break;
                    }
                }
            }
        } else { /* mono 设备输出 */
            for (uint32_t i = 0; i < frames; ++i) {
                const uint32_t i0 = static_cast<uint32_t>(v.pos);
                if (i0 >= cframes) break;
                const uint32_t i1 = (i0 + 1u < cframes) ? (i0 + 1u) : (v.params.loop ? 0u : i0);
                const float frac = static_cast<float>(v.pos - static_cast<double>(i0));
                float s0 = c.samples[i0 * cch];
                float s1 = c.samples[i1 * cch];
                out[i] += (s0 + (s1 - s0) * frac) * gain;
                v.pos += step;
                if (v.pos >= static_cast<double>(cframes)) {
                    if (v.params.loop) {
                        v.pos = std::fmod(v.pos, static_cast<double>(cframes));
                    } else {
                        v.active = false;   // 不 reset clip：音频线程禁止 free（等 slot 复用/stop）
                        break;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < static_cast<size_t>(frames) * out_ch; ++i)
        out[i] = std::clamp(out[i], -1.0F, 1.0F);
}

void Mixer::update(float dt) {
    if (!(dt > 0.0F)) return;
    for (Voice& v : voices_) {
        if (!v.active || v.paused || !v.clip) continue;
        v.logical += static_cast<double>(dt) * static_cast<double>(v.params.pitch);
        const double len = v.clip->seconds();
        if (len <= 0.0) { v.active = false; v.clip.reset(); continue; }
        if (v.logical >= len) {
            if (v.params.loop) {
                v.logical = std::fmod(v.logical, len);
            } else {
                v.logical = len;
                v.active = false;
                v.clip.reset();
            }
        }
    }
}

bool Mixer::voice_state(uint64_t handle, uint32_t* state, float* playhead) const {
    const Voice* v = find(handle);
    if (!v) return false;
    if (state) *state = v->active ? (v->paused ? 2u : 1u) : 0u;
    if (playhead) *playhead = static_cast<float>(v->logical);
    return true;
}

uint32_t Mixer::voice_at(uint32_t index, uint64_t* handle, uint32_t* state,
                         float* playhead) const {
    uint32_t seen = 0;
    for (size_t i = 0; i < voices_.size(); ++i) {
        const Voice& v = voices_[i];
        if (!v.active) continue;
        if (seen++ != index) continue;
        if (handle) *handle = make_handle((uint32_t)i);
        if (state) *state = v.paused ? 2u : 1u;
        if (playhead) *playhead = (float)v.logical;
        return 1;
    }
    return 0;
}

uint32_t Mixer::active_voices() const {
    uint32_t n = 0;
    for (const auto& v : voices_) if (v.active) ++n;
    return n;
}

} // namespace maque::audio
