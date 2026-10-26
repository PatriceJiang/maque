/* maque::tracy 单测：封装层在任何构建形态下都安全（不崩、可显式启停）。
 *
 * 覆盖：
 *   ① 编译形态：MAQUE_TRACY=ON/OFF 的宏都能编译（zone 可在未启动时调用）；
 *   ② 手动生命周期：started→startup→started→shutdown→!started（幂等）；
 *   ③ 数据接口：未启动时 plot/message/frame/thread 全部安全 no-op；
 *   ④ 模块颜色表：每个模块颜色唯一（时间线里能区分模块）。
 *   ⑤ CI 钩子：MAQUE_TRACY_SELFTEST=<迭代数> —— 启动后持续产 zone/frame/plot，
 *      供 tests/tracy_e2e.sh 用 tracy-capture 断言"数据真的发出去了"。
 */
#include "core/tracy/mq_tracy.h"

#include <chrono>
#include <cstdlib>
#include <print>
#include <set>
#include <thread>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::print("FAIL {}:{}: {}\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

using namespace maque::tracy;

static void zone_before_start() {
    MQ_TRACY_ZONE(runtime);
    MQ_TRACY_ZONE_N(script, "before-start");
    MQ_TRACY_ZONE_NAMED(z, script, "dynamic");
    MQ_TRACY_ZONE_TEXT(z, "x");
}

int main() {
    /* ① 模块颜色唯一（否则时间线分不清模块） */
    std::set<uint32_t> colors;
    for (uint32_t i = 0; i < (uint32_t)Module::count; ++i) colors.insert(color((Module)i));
    CHECK(colors.size() == (size_t)Module::count);

    /* ② 未启动：宏/数据接口安全 */
    CHECK(!started());
    zone_before_start();
    plot("tracy.test.plot", 1.0);
    plot_config("tracy.test.plot", PlotType::number, false, false, 0x4E79A7);
    message("before start");
    frame_mark();
    thread_name("main");
    CHECK(!started());

#if MQ_TRACY_ENABLED
    /* ③ 启动 → zone/数据 → 关闭（幂等） */
    CHECK(startup());
    CHECK(started());
    CHECK(startup());                        // 幂等
    {
        MQ_TRACY_ZONE_N(runtime, "tracy unit test zone");
        MQ_TRACY_ZONE_NAMED(z2, script, "call");
        MQ_TRACY_ZONE_TEXT(z2, "on_update");
        plot("tracy.test.plot", 2.0);
        message_color("started", 0x59A14F);
        frame_mark();
    }
    if (const char* sel = std::getenv("MAQUE_TRACY_SELFTEST"); sel && *sel) {
        const int iters = std::atoi(sel);
        for (int i = 0; i < iters; ++i) {
            {
                MQ_TRACY_ZONE_NAMED(zs, runtime, "selftest");
                MQ_TRACY_ZONE_TEXT(zs, "loop");
                MQ_TRACY_PLOT("selftest.value", (double)i);
            }
            MQ_TRACY_FRAME();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    shutdown();
    CHECK(!started());
    shutdown();                              // 幂等
    CHECK(!started());
    /* 关闭后宏仍然安全 */
    zone_before_start();
    plot("tracy.test.plot", 3.0);
#else
    CHECK(!startup());                       // OFF：启动是 no-op
    CHECK(!started());
    shutdown();
#endif

    std::print("tracy: {} checks, {} failed (MQ_TRACY_ENABLED={})\n", g_checks, g_fail, MQ_TRACY_ENABLED);
    return g_fail == 0 ? 0 : 1;
}
