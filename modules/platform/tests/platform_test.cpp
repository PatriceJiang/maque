// platform(SDL2) 单元测试（headless：dummy 视频驱动）。
// 覆盖：采集归一化 / 事件顺序 / 注入路径（AI/CI/回放与真机同队列）/ 容量截断 / 退出。
#include "core/platform/mq_platform.h"
#include "core/compat/mq_env.h"   // setenv/unsetenv 的跨平台封装（Windows 无 setenv）

#include <print>
#include <vector>
#include <cstdlib>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

struct Frame {
    std::vector<mq_platform_pointer_event> ptrs;
    std::vector<mq_platform_key_event>     keys;
    mq_input_frame f{};

    void reset(uint32_t pcap = 16, uint32_t kcap = 16) {
        ptrs.assign(pcap, {});
        keys.assign(kcap, {});
        f = mq_input_frame{};
        f.pointers = ptrs.data(); f.pointer_cap = pcap;
        f.keys = keys.data();     f.key_cap = kcap;
    }
};

} // namespace

int main() {
    // 单测必须走 dummy 路径（有显示环境也确定）。Windows 无 setenv，走兼容封装。
    maque::compat::set_env("MAQUE_HEADLESS", "1");
    mq_platform* p = mq_platform_create("maque-test", 320, 240);
    CHECK(p != nullptr);
    CHECK(mq_platform_has_window(p) == 0);        // headless / dummy

    Frame fr;
    fr.reset();
    uint32_t n = mq_platform_poll(p, &fr.f);
    CHECK(n == 0);
    CHECK(fr.f.quit == 0);
    CHECK(fr.f.width == 320 && fr.f.height == 240);

    // ---- 注入指针事件：顺序确定、delta 由平台统一计算 ----
    mq_platform_pointer_event mv{};
    mv.type = MQ_PLATFORM_POINTER_MOVE; mv.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
    mv.x = 10; mv.y = 20;
    mq_platform_inject_pointer(p, &mv);
    mv.x = 13; mv.y = 24;
    mq_platform_inject_pointer(p, &mv);

    mq_platform_pointer_event dn{};
    dn.type = MQ_PLATFORM_POINTER_DOWN; dn.pointer_type = MQ_PLATFORM_POINTER_MOUSE;
    dn.button = 0; dn.x = 13; dn.y = 24; dn.pressure = 1.0F;
    mq_platform_inject_pointer(p, &dn);

    mq_platform_key_event kd{};
    kd.key = MQ_KEY_SPACE; kd.modifiers = MQ_PLATFORM_MOD_SHIFT;
    mq_platform_inject_key(p, &kd);
    mq_platform_key_event txt{};
    txt.key = MQ_KEY_TEXT; txt.text[0] = 'a'; txt.text[1] = 0;
    mq_platform_inject_key(p, &txt);

    fr.reset();
    n = mq_platform_poll(p, &fr.f);
    CHECK(n == 5);
    CHECK(fr.f.pointer_count == 3);
    CHECK(fr.f.key_count == 2);
    CHECK(fr.f.mouse_x == 13.0F && fr.f.mouse_y == 24.0F);
    CHECK(fr.f.mouse_down == 1);
    // 顺序：先设备/注入到达顺序
    CHECK(fr.ptrs[0].type == MQ_PLATFORM_POINTER_MOVE);
    CHECK(fr.ptrs[0].dx == 0.0F);                    // 首帧无前值 → delta 0（确定）
    CHECK(fr.ptrs[1].dx == 3.0F && fr.ptrs[1].dy == 4.0F);
    CHECK(fr.ptrs[2].type == MQ_PLATFORM_POINTER_DOWN);
    CHECK(fr.keys[0].key == MQ_KEY_SPACE);
    CHECK(fr.keys[0].modifiers == MQ_PLATFORM_MOD_SHIFT);
    CHECK(fr.keys[1].key == MQ_KEY_TEXT);
    CHECK(fr.keys[1].text[0] == 'a');

    // 消费后应清空
    fr.reset();
    n = mq_platform_poll(p, &fr.f);
    CHECK(n == 0);
    CHECK(fr.f.pointer_count == 0 && fr.f.key_count == 0);

    // ---- 触摸注入：pointer_id 规范为 ≥1 ----
    mq_platform_pointer_event tc{};
    tc.type = MQ_PLATFORM_POINTER_DOWN; tc.pointer_type = MQ_PLATFORM_POINTER_TOUCH;
    tc.x = 5; tc.y = 6; tc.pressure = 0.5F;
    mq_platform_inject_pointer(p, &tc);
    fr.reset();
    mq_platform_poll(p, &fr.f);
    CHECK(fr.f.pointer_count == 1);
    CHECK(fr.f.pointers[0].pointer_id == 1);
    CHECK(fr.f.pointers[0].pointer_type == MQ_PLATFORM_POINTER_TOUCH);

    // ---- 容量截断：超出容量的部分丢弃，不越界 ----
    for (int i = 0; i < 5; ++i) mq_platform_inject_pointer(p, &mv);
    fr.reset(2, 1);
    n = mq_platform_poll(p, &fr.f);
    CHECK(n == 5);                       // 消费总数
    CHECK(fr.f.pointer_count == 2);      // 写入受容量限制

    // ---- 退出请求 ----
    mq_platform_request_quit(p);
    fr.reset();
    mq_platform_poll(p, &fr.f);
    CHECK(fr.f.quit == 1);

    mq_platform_destroy(p);

    if (g_failures == 0) { std::print("platform_test: all checks passed\n"); return 0; }
    std::print("platform_test: {} failure(s)\n", g_failures);
    return 1;
}
