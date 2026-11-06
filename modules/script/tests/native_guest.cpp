// native 后端测试 guest：同一份 C++ SDK 编成 .so/.dll，由 native_test 加载。
#include "maque/script.hpp"

#include <string>

namespace {

struct TestScript : maque::Script {
    int updates = 0;
    std::string last_text;

    void on_start() override { host.log("on_start"); }

    void update(double /*dt*/) override {
        ++updates;
        const float c = math.clamp(1.5F, 0.0F, 1.0F);
        (void)c;
        host.log("update");
    }

    void on_text(double /*target*/, std::string_view text) override {
        last_text.assign(text);
        host.log(last_text.c_str());
    }

    void on_destroy() override { host.log("on_destroy"); }
};

} // namespace

MAQUE_SCRIPT(TestScript)
