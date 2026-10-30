/* maque::tracy 单测：封装层在任何构建形态下都安全（不崩、可显式启停）。
 *
 * 覆盖：
 *   ① 编译形态：MAQUE_TRACY=ON/OFF 的宏都能编译（zone 可在未启动时调用）；
 *   ② 手动生命周期：started→startup→started→shutdown→!started（幂等）；
 *   ③ 数据接口：未启动时 plot/message/frame/thread 全部安全 no-op；
 *   ④ 颜色/分区：模块色唯一；UI/3D 暖色、System/Script 冷色；内存域色唯一；
 *   ⑤ 内存记账：add/sub/set/peak/下溢保护/分区汇总/状态行；
 *   ⑥ 全局 new/delete 拦截（仅 -DMAQUE_TRACY_ALLOC=ON）：heap 域与对齐分配；
 *   ⑦ CI 钩子：MAQUE_TRACY_SELFTEST=<迭代数> —— 启动后持续产 zone/frame/plot，
 *      供 tests/tracy_e2e.sh 用 tracy-capture 断言"数据真的发出去了"。
 */
#include "core/tracy/mq_tracy.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <new>
#include <print>
#include <set>
#include <string>
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
    MQ_TRACY_ZONE_VALUE(z, 123);
    MQ_TRACY_ZONE_AREA_N(world3d, "before-start-area");
    MQ_TRACY_MEM_ZONE_N(texture, "before-start-mem");
    MQ_TRACY_MEM_DELTA(script);
}

/* 锁追踪宏必须可声明/使用（MAQUE_TRACY_LOCKS=ON 时是 Tracy 的 Lockable，OFF 时退化为普通成员）。 */
struct LockedBox {
    MQ_TRACY_LOCKABLE(std::mutex, mu);
    int value = 0;
};

static void test_areas_and_colors() {
    /* ④ 模块色唯一（时间线里能区分模块） */
    std::set<uint32_t> colors;
    for (uint32_t i = 0; i < (uint32_t)Module::count; ++i) colors.insert(color((Module)i));
    CHECK(colors.size() == (size_t)Module::count);

    /* 内存域色唯一 */
    std::set<uint32_t> mem_colors;
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) mem_colors.insert(color((Mem)i));
    CHECK(mem_colors.size() == (size_t)Mem::count);

    /* 冷暖分类：UI / 3D = 暖；System / Script = 冷 */
    CHECK(warm(Area::ui));
    CHECK(warm(Area::world3d));
    CHECK(!warm(Area::system));
    CHECK(!warm(Area::script));

    /* 模块 → 分区映射（颜色按分区色系派生，映射错会让冷暖失真） */
    CHECK(area(Module::ui) == Area::ui);
    CHECK(area(Module::text) == Area::ui);
    CHECK(area(Module::imgui) == Area::ui);
    CHECK(area(Module::input) == Area::ui);
    CHECK(area(Module::render) == Area::world3d);
    CHECK(area(Module::anim) == Area::world3d);
    CHECK(area(Module::physics) == Area::world3d);
    CHECK(area(Module::nav) == Area::world3d);
    CHECK(area(Module::script) == Area::script);
    CHECK(area(Module::runtime) == Area::system);
    CHECK(area(Module::vfs) == Area::system);

    /* 内存域 → 分区（font=UI 暖、texture/gpu=3D 暖、node=System 冷、script=Script 冷） */
    CHECK(area(Mem::font) == Area::ui);
    CHECK(area(Mem::texture) == Area::world3d);
    CHECK(area(Mem::gpu) == Area::world3d);
    CHECK(area(Mem::node) == Area::system);
    CHECK(area(Mem::script) == Area::script);
    CHECK(area(Mem::heap) == Area::system);

    /* 名字非空且互不相同 */
    std::set<std::string> names;
    for (uint32_t i = 0; i < (uint32_t)Module::count; ++i) names.insert(name((Module)i));
    CHECK(names.size() == (size_t)Module::count);
    std::set<std::string> mem_names;
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) mem_names.insert(name((Mem)i));
    CHECK(mem_names.size() == (size_t)Mem::count);
    CHECK(area_color(Area::ui) == color(Area::ui));
}

static void test_memory() {
    CHECK(memory_available() == (MQ_TRACY_MEMORY_ENABLED != 0));
    if (!memory_available()) {
        /* 未编入内存记账：API 全部是 inline no-op（调用点不用改），安全且零状态。 */
        CHECK(!memory_enabled());
        CHECK(memory_bytes(Mem::script) == 0);
        CHECK(memory_total() == 0);
        CHECK(memory_area_total(Area::ui) == 0);
        memory_add(Mem::script, 100);              // no-op
        memory_set(Mem::gpu, 4096);                // no-op
        CHECK(memory_bytes(Mem::script) == 0);
        CHECK(memory_bytes(Mem::gpu) == 0);
        char line[16] = { 'x' };
        CHECK(memory_format(line, sizeof line) == 0);
        CHECK(line[0] == '\0');
        CHECK(!memory_due(100.0));
        char dummy[8];
        memory_note_alloc(dummy, sizeof dummy, Mem::font);
        memory_note_free(dummy, Mem::font);
        memory_reset();
        memory_publish();
        memory_report("disabled");
        return;
    }
    memory_set_enabled(true);
    CHECK(memory_enabled());
    memory_reset();

    CHECK(memory_bytes(Mem::script) == 0);
    CHECK(memory_total() == 0);

    /* delta 记账 + 峰值 */
    memory_add(Mem::script, 100);
    CHECK(memory_bytes(Mem::script) == 100);
    CHECK(memory_peak(Mem::script) == 100);
    memory_add(Mem::script, 50);
    CHECK(memory_bytes(Mem::script) == 150);
    memory_sub(Mem::script, 120);
    CHECK(memory_bytes(Mem::script) == 30);
    CHECK(memory_peak(Mem::script) == 150);           // 峰值不回退
    memory_sub(Mem::script, 9999);                    // 下溢保护：不会变负数/天文数字
    CHECK(memory_bytes(Mem::script) == 0);

    /* 采样型域：绝对值覆盖 */
    memory_set(Mem::gpu, 4096);
    CHECK(memory_bytes(Mem::gpu) == 4096);
    memory_set(Mem::gpu, 1024);
    CHECK(memory_bytes(Mem::gpu) == 1024);
    CHECK(memory_peak(Mem::gpu) == 4096);

    /* 分类总量 / 分区总量（不含 heap） */
    memory_reset();
    memory_add(Mem::script, 10);
    memory_add(Mem::node, 20);
    memory_set(Mem::texture, 30);
    memory_set(Mem::gpu, 40);
    memory_set(Mem::font, 50);
    memory_set(Mem::heap, 999);
    CHECK(memory_total() == 150);
    CHECK(memory_area_total(Area::script) == 10);     // script
    CHECK(memory_area_total(Area::system) == 20);     // node
    CHECK(memory_area_total(Area::world3d) == 70);    // texture + gpu
    CHECK(memory_area_total(Area::ui) == 50);         // font

    /* 状态行（ImGui / 报告共用）非空且包含各域名字 */
    char line[512];
    memory_format(line, sizeof line);
    CHECK(line[0] != '\0');
    CHECK(std::string(line).find("script") != std::string::npos);
    CHECK(std::string(line).find("gpu") != std::string::npos);
    memory_area_format(line, sizeof line);
    CHECK(std::string(line).find("ui") != std::string::npos);
    CHECK(std::string(line).find("tracked") != std::string::npos);

    /* 采样节流：一次超大 dt 必然到期（间隔默认 1s；环境变量只可能调小/调大，100s 足够） */
    CHECK(memory_due(100.0));

    /* 显式分配事件：未启动 Tracy 时安全 no-op；启动后也不崩（Memory 视图按指针配对） */
    char dummy[16];
    memory_note_alloc(dummy, sizeof dummy, Mem::script);
    memory_note_free(dummy, Mem::script);

    memory_reset();
    CHECK(memory_total() == 0);
}

#if MQ_TRACY_ALLOC_ENABLED
static void test_global_alloc() {
    /* ⑥ 全局 new/delete 拦截：请求字节精确进 heap 域，释放回冲；对齐分配单独路径。 */
    const uint64_t before = memory_bytes(Mem::heap);
    void* p = ::operator new(1000);
    CHECK(memory_bytes(Mem::heap) == before + 1000);
    static_cast<volatile char*>(p)[0] = 1;
    ::operator delete(p);
    CHECK(memory_bytes(Mem::heap) == before);

    const uint64_t a0 = memory_bytes(Mem::heap);
    void* q = ::operator new(100, std::align_val_t(64));
    CHECK(reinterpret_cast<uintptr_t>(q) % 64 == 0);
    CHECK(memory_bytes(Mem::heap) == a0 + 100);
    ::operator delete(q, std::align_val_t(64));
    CHECK(memory_bytes(Mem::heap) == a0);

    const uint64_t n0 = memory_bytes(Mem::heap);
    void* r = ::operator new(64, std::nothrow);
    CHECK(r != nullptr);
    ::operator delete(r, std::nothrow);
    CHECK(memory_bytes(Mem::heap) == n0);
}
#endif

int main() {
    test_areas_and_colors();
    test_memory();

    /* ② 未启动：宏/数据接口安全 */
    CHECK(!started());
    zone_before_start();
    plot("tracy.test.plot", 1.0);
    plot_config("tracy.test.plot", PlotType::number, false, false, 0x4E79A7);
    plot_memory("tracy.test.mem", 1234, color(Mem::script));
    message("before start");
    frame_mark();
    thread_name("main");
    CHECK(!started());

    /* 锁追踪（未启动也不崩；启动后 Tracy 会画锁动画） */
    {
        LockedBox box;
        auto& mu = box.mu;
        mu.lock();
        MQ_TRACY_LOCK_MARK(mu);
        box.value = 42;
        mu.unlock();
        CHECK(box.value == 42);
    }

#if MQ_TRACY_ALLOC_ENABLED
    test_global_alloc();
#endif

#if MQ_TRACY_ENABLED
    /* ③ 启动 → zone/数据 → 关闭（幂等） */
    CHECK(startup());
    CHECK(started());
    CHECK(startup());                        // 幂等
    {
        MQ_TRACY_ZONE_N(runtime, "tracy unit test zone");
        MQ_TRACY_ZONE_NAMED(z2, script, "call");
        MQ_TRACY_ZONE_TEXT(z2, "on_update");
        MQ_TRACY_ZONE_VALUE(z2, 7);
        MQ_TRACY_ZONE_AREA_N(ui, "unit-area");
        MQ_TRACY_MEM_ZONE_N(font, "unit-mem-zone");
        {
            MQ_TRACY_MEM_DELTA_N(font, "unit-mem-delta");
            memory_add(Mem::font, 4096);
        }
        plot("tracy.test.plot", 2.0);
        plot_memory("tracy.test.mem", (double)memory_bytes(Mem::font));
        message_color("started", 0x59A14F);
        frame_mark();
    }
    memory_set(Mem::node, 2048);
    memory_publish();                        // 曲线 + 报告路径（未设 MAQUE_TRACY_MEM_REPORT 时只推曲线）
    if (const char* sel = std::getenv("MAQUE_TRACY_SELFTEST"); sel && *sel) {
        const int iters = std::atoi(sel);
        for (int i = 0; i < iters; ++i) {
            {
                MQ_TRACY_ZONE_NAMED(zs, runtime, "selftest");
                MQ_TRACY_ZONE_TEXT(zs, "loop");
                MQ_TRACY_PLOT("selftest.value", (double)i);
                MQ_TRACY_MEM_ZONE(script);
                memory_add(Mem::script, 256);
                memory_sub(Mem::script, 256);
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

    std::print("tracy: {} checks, {} failed (MQ_TRACY_ENABLED={} MEMORY={} ALLOC={} LOCKS={})\n",
               g_checks, g_fail, MQ_TRACY_ENABLED, MQ_TRACY_MEMORY_ENABLED, MQ_TRACY_ALLOC_ENABLED,
               MQ_TRACY_LOCKS_ENABLED);
    return g_fail == 0 ? 0 : 1;
}
