// hello.cpp —— native 脚本最小样例（plans/26-cpp-scripts.md）。
// 同一套 API 名（host.log / host.quit）与 JS/Lua 一致；事件名来自 schema callbacks。
#include "maque/script.hpp"

struct Hello : maque::Script {
    int frames = 0;

    void on_start() override { host.log("native: hello on_start"); }

    void update(double /*dt*/) override {
        ++frames;
        if (frames == 1) host.log("native: first update");
        if (frames >= 5) {
            host.log("native: bye");
            host.quit();
        }
    }
};

MAQUE_SCRIPT(Hello)
