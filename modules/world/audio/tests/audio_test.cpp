#include "abi/audio.h"

#include "audio/decoder.h"
#include "audio/mixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <print>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

/* ---- ABI 守卫（DSL 生成头漂移即编译失败，docs/02 R7）---- */
static_assert(MQ_AUDIO_ABI_VERSION == 1u, "ABI 版本变更");
static_assert(MQ_AUDIO_OK == 0 && MQ_AUDIO_ERR_FORMAT == 2 && MQ_AUDIO_ERR_HANDLE == 6,
              "错误码变更");
static_assert(MQ_AUDIO_BACKEND_NULL == 1 && MQ_AUDIO_BACKEND_SDL == 2, "后端枚举变更");
static_assert(offsetof(mq_audio_desc, struct_size) == 0, "字段偏移变更");
static_assert(offsetof(mq_audio_desc, sample_rate) == 4, "字段偏移变更");
/* 指针字段的偏移/尺寸依赖位宽（WASM 是 32-bit），只在 64-bit 上断言。 */
#if defined(__LP64__) || defined(_WIN64)
static_assert(offsetof(mq_audio_desc, backend) == 20, "字段偏移变更");
static_assert(offsetof(mq_audio_desc, device) == 24, "字段偏移变更");
static_assert(offsetof(mq_audio_voice_desc, clip) == 8, "字段偏移变更");
static_assert(offsetof(mq_audio_voice_desc, volume) == 16, "字段偏移变更");
static_assert(offsetof(mq_audio_voice_desc, bus) == 32, "字段偏移变更");
static_assert(offsetof(mq_audio_voice_desc, x) == 40, "字段偏移变更");
static_assert(offsetof(mq_audio_voice_desc, doppler) == 68, "字段偏移变更");
static_assert(sizeof(mq_audio_desc) == 32, "mq_audio_desc ABI 变更");
static_assert(sizeof(mq_audio_voice_desc) == 72, "mq_audio_voice_desc ABI 变更");
static_assert(sizeof(mq_audio_stats) == 32, "mq_audio_stats ABI 变更");
#endif

namespace {

constexpr uint32_t kRate = 48000;

/* 生成一段正弦 WAV（PCM16 或 float32），供解码/混音测试。 */
std::vector<uint8_t> make_wav(uint16_t channels, uint16_t bits, const std::vector<float>& samples,
                              uint32_t rate = kRate) {
    const bool f32 = (bits == 32);
    const uint32_t bytes_per = bits / 8u;
    const uint32_t data_size = static_cast<uint32_t>(samples.size() * bytes_per);
    std::vector<uint8_t> out(44 + data_size, 0);
    auto w32 = [&](size_t off, uint32_t v) { std::memcpy(out.data() + off, &v, 4); };
    auto w16 = [&](size_t off, uint16_t v) { std::memcpy(out.data() + off, &v, 2); };
    std::memcpy(out.data(), "RIFF", 4);
    w32(4, 36 + data_size);
    std::memcpy(out.data() + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, f32 ? 3 : 1);
    w16(22, channels);
    w32(24, rate);
    w32(28, rate * channels * bytes_per);
    w16(32, static_cast<uint16_t>(channels * bytes_per));
    w16(34, bits);
    std::memcpy(out.data() + 36, "data", 4);
    w32(40, data_size);
    uint8_t* dst = out.data() + 44;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (f32) {
            std::memcpy(dst + i * 4, &samples[i], 4);
        } else {
            const float v = std::clamp(samples[i], -1.0F, 1.0F);
            const int16_t s = static_cast<int16_t>(std::lround(v * 32767.0F));
            std::memcpy(dst + i * 2, &s, 2);
        }
    }
    return out;
}

std::vector<float> sine(uint32_t frames, float freq, float amp, uint16_t channels = 1) {
    std::vector<float> s(static_cast<size_t>(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = amp * std::sin(2.0F * 3.14159265358979F * freq * static_cast<float>(i) /
                                       static_cast<float>(kRate));
        for (uint16_t c = 0; c < channels; ++c) s[static_cast<size_t>(i) * channels + c] = v;
    }
    return s;
}

struct Rms {
    double left = 0.0;
    double right = 0.0;
};

Rms mix_rms(mq_audio* a, uint32_t frames) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0F);
    const int rc = mq_audio_mix(a, buf.data(), frames);
    Rms r;
    if (rc != MQ_AUDIO_OK) return r;
    for (uint32_t i = 0; i < frames; ++i) {
        r.left += static_cast<double>(buf[i * 2 + 0]) * buf[i * 2 + 0];
        r.right += static_cast<double>(buf[i * 2 + 1]) * buf[i * 2 + 1];
    }
    r.left = std::sqrt(r.left / frames);
    r.right = std::sqrt(r.right / frames);
    return r;
}

mq_audio* make_audio(uint32_t backend = MQ_AUDIO_BACKEND_NULL, uint32_t max_voices = 8) {
    mq_audio_desc d{};
    d.struct_size = sizeof d;
    d.sample_rate = kRate;
    d.channels = 2;
    d.buffer_frames = 512;
    d.max_voices = max_voices;
    d.backend = backend;
    return mq_audio_create(&d);
}

mq_audio_clip* load_clip(mq_audio* a, const std::vector<uint8_t>& wav, const char* hint = ".wav") {
    mq_audio_clip* clip = nullptr;
    const int rc = mq_audio_load_memory(a, wav.data(), wav.size(), hint, &clip);
    CHECK(rc == MQ_AUDIO_OK);
    CHECK(clip != nullptr);
    return clip;
}

mq_audio_voice_desc voice_desc(const mq_audio_clip* clip, float volume = 1.0F, float pan = 0.0F,
                               uint32_t loop = 0) {
    mq_audio_voice_desc d{};
    d.struct_size = sizeof d;
    d.clip = clip;
    d.volume = volume;
    d.pitch = 1.0F;
    d.pan = pan;
    d.loop = loop;
    d.bus = "sfx";
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    return d;
}

void test_decode() {
    const auto wav = make_wav(1, 16, sine(2400, 440.0F, 0.5F));
    std::shared_ptr<maque::audio::Clip> clip;
    std::string err;
    CHECK(maque::audio::decode_wav(wav.data(), wav.size(), clip, err));
    CHECK(clip && clip->frames() == 2400);
    CHECK(clip->sample_rate == kRate);
    CHECK(clip->channels == 1);
    CHECK(std::abs(clip->seconds() - 0.05) < 1e-6);

    const auto fwav = make_wav(2, 32, sine(480, 220.0F, 0.25F, 2));
    CHECK(maque::audio::decode_audio(fwav.data(), fwav.size(), ".wav", clip, err));
    CHECK(clip && clip->channels == 2 && clip->frames() == 480);

    /* 坏数据必须报 FORMAT，而不是崩。 */
    const uint8_t junk[16] = {0};
    CHECK(!maque::audio::decode_wav(junk, sizeof junk, clip, err));
    CHECK(!err.empty());
}

std::vector<uint8_t> read_fixture(const char* name) {
    std::ifstream f(std::string(MAQUE_AUDIO_TEST_DATA) + "/" + name, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_decode_lossy() {
    for (const char* name : { "beep.ogg", "beep.mp3" }) {
        const auto bytes = read_fixture(name);
        CHECK(!bytes.empty());
        if (bytes.empty()) continue;
        std::shared_ptr<maque::audio::Clip> clip;
        std::string err;
        CHECK(maque::audio::decode_audio(bytes.data(), bytes.size(), name, clip, err));
        CHECK(clip != nullptr);
        if (!clip) continue;
        CHECK(clip->sample_rate == 48000);
        CHECK(clip->channels == 1);
        CHECK(clip->frames() > 8000);   /* 0.2s（MP3 可能带 encoder delay） */
        double rms = 0.0;
        for (float v : clip->samples) rms += (double)v * v;
        rms = std::sqrt(rms / (double)clip->samples.size());
        CHECK(rms > 0.05 && rms < 0.2);  /* ffmpeg sine 默认振幅 0.125 → RMS ≈ 0.088 */
    }
}

void test_offline_mix() {
    mq_audio* a = make_audio();
    CHECK(a != nullptr);
    const auto wav = make_wav(1, 16, sine(4800, 440.0F, 0.5F));
    mq_audio_clip* clip = load_clip(a, wav);
    CHECK(clip != nullptr);
    uint32_t frames = 0, rate = 0, ch = 0;
    CHECK(mq_audio_clip_info(clip, &frames, &rate, &ch) == MQ_AUDIO_OK);
    CHECK(frames == 4800 && rate == kRate && ch == 1);

    const auto d = voice_desc(clip, 1.0F, 0.0F);
    const uint64_t v = mq_audio_play(a, &d);
    CHECK(v != 0);
    const Rms mid = mix_rms(a, 2400);
    /* 正弦振幅 0.5 → RMS=0.3536；等功率中心 pan → 每声道 ×0.7071 ≈ 0.25 */
    CHECK(std::abs(mid.left - 0.25) < 0.02);
    CHECK(std::abs(mid.right - 0.25) < 0.02);
    CHECK(std::abs(mid.left - mid.right) < 0.01);

    CHECK(mq_audio_stop(a, v) == MQ_AUDIO_OK);

    /* 全左 pan：右声道应接近 0。 */
    const auto dl = voice_desc(clip, 1.0F, -1.0F);
    CHECK(mq_audio_play(a, &dl) != 0);
    const Rms left = mix_rms(a, 2400);
    CHECK(left.left > 0.3);
    CHECK(left.right < 0.005);

    /* master=0 → 静音 */
    CHECK(mq_audio_set_master(a, 0.0F, 0) == MQ_AUDIO_OK);
    const Rms muted = mix_rms(a, 256);
    CHECK(muted.left < 1e-6 && muted.right < 1e-6);

    mq_audio_stats st{};
    st.struct_size = sizeof st;
    CHECK(mq_audio_get_stats(a, &st) == MQ_AUDIO_OK);
    CHECK(st.voices == 1);   /* 左 pan 的 voice 仍在播（只混了 4800 帧） */
    mq_audio_clip_release(clip);
    mq_audio_destroy(a);
}

void test_spatial() {
    mq_audio* a = make_audio();
    CHECK(a != nullptr);
    const auto wav = make_wav(1, 16, sine(4800, 440.0F, 0.5F));
    mq_audio_clip* clip = load_clip(a, wav);

    mq_audio_listener l{};
    l.struct_size = sizeof l;
    l.dir_x = 1.0F;
    l.dir_y = 0.0F;
    CHECK(mq_audio_set_listener(a, &l) == MQ_AUDIO_OK);

    /* 听者在原点朝 +x；声源在右手边 (0,-10)：inverse 衰减 = 1/10，pan = +1（左≈0）。 */
    auto d = voice_desc(clip, 1.0F, 0.0F);
    d.panning = MQ_AUDIO_PANNING_SPATIAL;
    d.attenuation = MQ_AUDIO_ATTEN_INVERSE;
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    d.x = 0.0F;
    d.y = -10.0F;
    const uint64_t vnear = mq_audio_play(a, &d);
    CHECK(vnear != 0);
    const Rms near = mix_rms(a, 2400);
    CHECK(near.right > 0.03 && near.right < 0.04);   /* 0.3536 * 0.1 */
    CHECK(near.left < 0.005);                        /* 全右 pan */
    CHECK(mq_audio_stop(a, vnear) == MQ_AUDIO_OK);   /* 别把近处声音混进远场断言 */

    /* 同理放到 1000m：衰减 1/1000 → 基本静音 */
    auto far = d;
    far.y = -1000.0F;
    CHECK(mq_audio_play(a, &far) != 0);
    const Rms distant = mix_rms(a, 2400);
    CHECK(distant.right < 0.001);
    mq_audio_clip_release(clip);
    mq_audio_destroy(a);
}

void test_logical_clock() {
    mq_audio* a = make_audio();
    CHECK(a != nullptr);
    const auto wav = make_wav(1, 16, sine(4800, 440.0F, 0.5F));   /* 0.1s */
    mq_audio_clip* clip = load_clip(a, wav);
    const auto d = voice_desc(clip, 1.0F, 0.0F);
    const uint64_t v = mq_audio_play(a, &d);
    CHECK(v != 0);

    uint32_t state = 99;
    float head = 0.0F;
    CHECK(mq_audio_update(a, 0.05F) == MQ_AUDIO_OK);
    CHECK(mq_audio_voice_state(a, v, &state, &head) == MQ_AUDIO_OK);
    CHECK(state == MQ_AUDIO_PLAYING);
    CHECK(std::abs(head - 0.05F) < 1e-4F);

    CHECK(mq_audio_update(a, 0.06F) == MQ_AUDIO_OK);
    CHECK(mq_audio_voice_state(a, v, &state, &head) == MQ_AUDIO_OK);
    CHECK(state == MQ_AUDIO_STOPPED);
    CHECK(head >= 0.099F);   /* 夹到 clip 长度 */

    /* loop：超过长度后回绕，仍在播。 */
    const auto dl = voice_desc(clip, 1.0F, 0.0F, 1u);
    const uint64_t vl = mq_audio_play(a, &dl);
    CHECK(vl != 0);
    CHECK(mq_audio_update(a, 0.25F) == MQ_AUDIO_OK);
    CHECK(mq_audio_voice_state(a, vl, &state, &head) == MQ_AUDIO_OK);
    CHECK(state == MQ_AUDIO_PLAYING);
    CHECK(std::abs(head - 0.05F) < 1e-3F);

    /* 过期句柄 */
    CHECK(mq_audio_stop(a, vl) == MQ_AUDIO_OK);
    CHECK(mq_audio_stop(a, vl) == MQ_AUDIO_ERR_HANDLE);
    mq_audio_clip_release(clip);
    mq_audio_destroy(a);
}

void test_capacity() {
    mq_audio* a = make_audio(MQ_AUDIO_BACKEND_NULL, 2);
    CHECK(a != nullptr);
    const auto wav = make_wav(1, 16, sine(480, 440.0F, 0.2F));
    mq_audio_clip* clip = load_clip(a, wav);
    const auto d = voice_desc(clip, 1.0F, 0.0F);
    CHECK(mq_audio_play(a, &d) != 0);
    CHECK(mq_audio_play(a, &d) != 0);
    CHECK(mq_audio_play(a, &d) == 0);   /* 池满 */
    mq_audio_stats st{};
    CHECK(mq_audio_get_stats(a, &st) == MQ_AUDIO_OK);
    CHECK(st.voices == 2);
    CHECK(st.dropped == 1);
    mq_audio_clip_release(clip);
    mq_audio_destroy(a);
}

void test_device_sdl() {
#ifdef MAQUE_HAVE_SDL_AUDIO
    /* dummy 驱动：无真实设备也能跑回调（CI 用）；必须在 SDL 初始化之前设。 */
#if defined(_WIN32)
    _putenv_s("SDL_AUDIODRIVER", "dummy");
#else
    setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
    mq_audio* a = make_audio(MQ_AUDIO_BACKEND_SDL);
    CHECK(a != nullptr);
    const auto wav = make_wav(1, 16, sine(96000, 440.0F, 0.3F));   /* 2s：150ms 内不会播完 */
    mq_audio_clip* clip = load_clip(a, wav);
    const auto d = voice_desc(clip, 0.5F, 0.0F);
    CHECK(mq_audio_play(a, &d) != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    mq_audio_stats st{};
    st.struct_size = sizeof st;
    CHECK(mq_audio_get_stats(a, &st) == MQ_AUDIO_OK);
    CHECK(st.callbacks > 0);          /* 回调真的在跑 */
    CHECK(st.sample_rate == kRate);   /* dummy 驱动按请求的采样率 */
    CHECK(st.voices == 1);
    mq_audio_clip_release(clip);
    mq_audio_destroy(a);
#else
    std::print("audio: SDL 未编译，跳过设备用例\n");
#endif
}

} // namespace

int main() {
    test_decode();
    test_decode_lossy();
    test_offline_mix();
    test_spatial();
    test_logical_clock();
    test_capacity();
    test_device_sdl();

    if (g_failures) {
        std::print("audio_test: {} check(s) failed\n", g_failures);
        return 1;
    }
    std::print("audio_test: ok\n");
    return 0;
}
