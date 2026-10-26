#include "audio/backend.h"

namespace maque::audio {
namespace {

class NullDevice final : public AudioDevice {
public:
    bool open(const DeviceConfig& cfg, RenderCallback, void*) override {
        rate_ = cfg.sample_rate ? cfg.sample_rate : 48000;
        channels_ = (cfg.channels == 1) ? 1 : 2;
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    void start() override {}
    void stop() override {}
    void lock() override {}
    void unlock() override {}
    uint32_t sample_rate() const override { return rate_; }
    uint32_t channels() const override { return channels_; }
    uint64_t callbacks() const override { return 0; }
    const char* name() const override { return "null"; }

private:
    uint32_t rate_ = 48000;
    uint32_t channels_ = 2;
    bool open_ = false;
};

} // namespace

std::unique_ptr<AudioDevice> make_null_device() { return std::make_unique<NullDevice>(); }

} // namespace maque::audio
