/* maque::tracy 实现：Tracy client 生命周期 + 数据上报 + 内存记账（宏在 core/tracy/mq_tracy.h）。
 *
 * 生命周期是**手动**的（TracyClient 以 TRACY_DELAYED_INIT + TRACY_MANUAL_LIFETIME 编译）：
 *   · 普通运行 / CI：一次都不 startup → 不监听端口、不占线程、zone 全部 no-op；
 *   · F3 或 MAQUE_TRACY=1 → startup() → 监听 8086，等 profiler GUI 连接；
 *   · 退出前 shutdown()（否则 Tracy 的堆分配会被 ASan 报泄漏）。
 *
 * 内存记账（docs/29 §10）与 client 生命周期**解耦**：计数器永远可用（关掉 Tracy 也能用
 * MAQUE_TRACY_MEM_REPORT=1 打印文本表），只有曲线/Memory 视图需要 client 启动。
 * 每个内存域只有一个写入者（见 mq_tracy.h 的 Mem 注释），因此不同域之间可以相加。
 *
 * 注意：本文件在 namespace maque::tracy 内，引用 Tracy 全局命名空间必须写 `::tracy::`
 *（裸 `tracy::` 会解析成 maque::tracy 自己，实测编译报 "no member named ..."）。
 */
#include "core/tracy/mq_tracy.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

#if MQ_TRACY_ENABLED
#  include "common/TracySystem.hpp"
#  include "tracy/Tracy.hpp"
#endif

#if MQ_TRACY_ALLOC_ENABLED
/* 链接锚点：让静态库一定把 mem_alloc.cpp（全局 new/delete 替换）拉进可执行文件。
 * 没有这个引用时，链接器看不到任何符号需求 → operator new 不会覆盖 CRT。 */
namespace maque::tracy {
void mem_alloc_link_anchor();
}
#endif

namespace maque::tracy {
namespace {

/* 与 client 内部状态同步的本地标志：避免 IsProfilerStarted 与启动/关闭之间的竞态窗口。 */
std::atomic<bool> g_started{ false };

/* ---- 内存域计数器（POD/常量初始化 → 静态初始化顺序安全：operator new 拦截可能在 main 前跑）---- */
#if MQ_TRACY_MEMORY_ENABLED
struct MemCounter {
    std::atomic<uint64_t> bytes{ 0 };
    std::atomic<uint64_t> peak{ 0 };
    std::atomic<uint64_t> allocs{ 0 };
    std::atomic<uint64_t> frees{ 0 };
};
MemCounter g_mem[(size_t)Mem::count];

std::atomic<bool> g_mem_enabled{ true };
std::atomic<bool> g_mem_env_read{ false };
#endif

/* 环境变量布尔解析：0/false/off/no = 关；其余非空 = 开；缺省 = def。 */
bool env_flag(const char* env_name, bool def) {
    const char* e = std::getenv(env_name);
    if (!e || !*e) return def;
    if (std::strcmp(e, "0") == 0 || std::strcmp(e, "false") == 0 || std::strcmp(e, "off") == 0 ||
        std::strcmp(e, "no") == 0)
        return false;
    return true;
}

#if MQ_TRACY_MEMORY_ENABLED
/* 计量单位换算（报告/状态行共用）。 */
void fmt_bytes(char* out, size_t cap, uint64_t b) {
    if (b < 1024ull)
        std::snprintf(out, cap, "%llu B", (unsigned long long)b);
    else if (b < 1024ull * 1024ull)
        std::snprintf(out, cap, "%.1f KiB", (double)b / 1024.0);
    else if (b < 1024ull * 1024ull * 1024ull)
        std::snprintf(out, cap, "%.2f MiB", (double)b / (1024.0 * 1024.0));
    else
        std::snprintf(out, cap, "%.2f GiB", (double)b / (1024.0 * 1024.0 * 1024.0));
}

/* 曲线名（静态字符串：Tracy 只保存指针）。 */
constexpr const char* kMemPlot[(size_t)Mem::count] = {
    "mem/script", "mem/node", "mem/texture", "mem/gpu", "mem/font", "mem/heap"
};
constexpr const char* kAreaPlot[(size_t)Area::count] = {
    "mem/area/ui", "mem/area/3d", "mem/area/system", "mem/area/script"
};
constexpr const char* kTotalPlot = "mem/tracked";

/* 内存域曲线首次上报时配置格式（Tracy 的 plot 默认是 number；memory 会显示成字节单位）。 */
void configure_memory_plots() {
    static bool done = false;
    if (done) return;
    done = true;
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) {
#if !MQ_TRACY_ALLOC_ENABLED
        if ((Mem)i == Mem::heap) continue;      /* 未编入全量堆：不配置 heap 曲线 */
#endif
        plot_config(kMemPlot[i], PlotType::memory, false, true, color((Mem)i));
    }
    for (uint32_t i = 0; i < (uint32_t)Area::count; ++i)
        plot_config(kAreaPlot[i], PlotType::memory, false, true, area_color((Area)i));
    plot_config(kTotalPlot, PlotType::memory, false, true, 0xD372AD);
}

/* memory_note_alloc/free 的重入护栏：Tracy 底层可能用 new 分配自己的队列 → 拦截器再进来。
 * 这里直接拒绝嵌套的 Memory 事件（计数仍由各自域完成，不会重复/悬挂）。 */
thread_local bool g_in_mem_event = false;
#endif  /* MQ_TRACY_MEMORY_ENABLED */

} // namespace

bool started() {
#if MQ_TRACY_ENABLED
    return g_started.load(std::memory_order_relaxed) && ::tracy::IsProfilerStarted();
#else
    return false;
#endif
}

bool startup() {
#if MQ_TRACY_ENABLED
    if (started()) return true;
#if MQ_TRACY_ALLOC_ENABLED
    mem_alloc_link_anchor();          // 只用于强制链接；运行期无副作用
#endif
    if (!::tracy::IsProfilerStarted()) ::tracy::StartupProfiler();
    g_started.store(true, std::memory_order_release);
    return true;
#else
    return false;
#endif
}

void shutdown() {
#if MQ_TRACY_ENABLED
    /* 退出快照：**与 client 是否启动无关**（文本报告路径，CI/无 GUI 也能看结论）。只打一次。 */
    {
        static bool reported = false;
        if (!reported && env_flag("MAQUE_TRACY_MEM_REPORT", false)) {
            reported = true;
            memory_report("shutdown");
        }
    }
    if (!g_started.load(std::memory_order_relaxed)) return;
    g_started.store(false, std::memory_order_release);
    if (::tracy::IsProfilerStarted()) ::tracy::ShutdownProfiler();   // 冲刷未发完的数据
#endif
}

void frame_mark() {
#if MQ_TRACY_ENABLED
    if (!started()) return;
    /* 不用 FrameMark 宏：宏体里的 `tracy::` 会被解析成 maque::tracy（见文件头注释）。 */
    ::tracy::Profiler::SendFrameMark(nullptr);
#endif
}

void plot(const char* plot_name, double value) {
#if MQ_TRACY_ENABLED
    if (!started() || !plot_name) return;
    ::tracy::Profiler::PlotData(plot_name, value);
#else
    (void)plot_name;
    (void)value;
#endif
}

void plot_config(const char* plot_name, PlotType type, bool step, bool fill, uint32_t plot_color) {
#if MQ_TRACY_ENABLED
    if (!started() || !plot_name) return;
    ::tracy::Profiler::ConfigurePlot(plot_name, (::tracy::PlotFormatType)type, step, fill, plot_color);
#else
    (void)plot_name;
    (void)type;
    (void)step;
    (void)fill;
    (void)plot_color;
#endif
}

void plot_memory(const char* plot_name, uint64_t bytes, uint32_t plot_color) {
#if MQ_TRACY_ENABLED
    if (!started() || !plot_name) return;
    /* 同一曲线名只配置一次（Tracy 的 ConfigurePlot 是消息，不会自动去重）。
     * 名字来自调用方的静态字面量 → 只存指针；固定 32 条上限（超出的曲线按 number 格式显示）。 */
    static const char* seen[32] = {};
    static size_t seen_count = 0;
    bool known = false;
    for (size_t i = 0; i < seen_count; ++i) {
        if (seen[i] == plot_name) { known = true; break; }
    }
    if (!known && seen_count < 32) {
        seen[seen_count++] = plot_name;
        plot_config(plot_name, PlotType::memory, false, true, plot_color);
    }
    ::tracy::Profiler::PlotData(plot_name, (double)bytes);
#else
    (void)plot_name;
    (void)bytes;
    (void)plot_color;
#endif
}

void message(const char* text) {
#if MQ_TRACY_ENABLED
    if (!started() || !text) return;
    ::tracy::Profiler::Message(text, std::strlen(text), 0);
#else
    (void)text;
#endif
}

void message_color(const char* text, uint32_t text_color) {
#if MQ_TRACY_ENABLED
    if (!started() || !text) return;
    ::tracy::Profiler::MessageColor(text, std::strlen(text), text_color, 0);
#else
    (void)text;
    (void)text_color;
#endif
}

void thread_name(const char* thread) {
#if MQ_TRACY_ENABLED
    if (!started() || !thread) return;
    ::tracy::SetThreadName(thread);
#else
    (void)thread;
#endif
}

/* ============================ 内存记账 ============================ */

#if MQ_TRACY_MEMORY_ENABLED

bool memory_enabled() {
    if (!g_mem_env_read.load(std::memory_order_acquire)) {
        /* MAQUE_TRACY_MEM：运行时总开关（默认开）。只读一次；memory_set_enabled 可覆盖。 */
        g_mem_enabled.store(env_flag("MAQUE_TRACY_MEM", true), std::memory_order_relaxed);
        g_mem_env_read.store(true, std::memory_order_release);
    }
    return g_mem_enabled.load(std::memory_order_relaxed);
}

void memory_set_enabled(bool on) {
    g_mem_env_read.store(true, std::memory_order_release);
    g_mem_enabled.store(on, std::memory_order_relaxed);
}

MemInfo memory_info(Mem m) {
    MemInfo out;
    const size_t i = (size_t)m;
    if (i >= (size_t)Mem::count) return out;
    const MemCounter& c = g_mem[i];
    out.bytes = c.bytes.load(std::memory_order_relaxed);
    out.peak = c.peak.load(std::memory_order_relaxed);
    out.allocs = c.allocs.load(std::memory_order_relaxed);
    out.frees = c.frees.load(std::memory_order_relaxed);
    return out;
}

uint64_t memory_bytes(Mem m) {
    const size_t i = (size_t)m;
    return i < (size_t)Mem::count ? g_mem[i].bytes.load(std::memory_order_relaxed) : 0;
}

uint64_t memory_peak(Mem m) {
    const size_t i = (size_t)m;
    return i < (size_t)Mem::count ? g_mem[i].peak.load(std::memory_order_relaxed) : 0;
}

uint64_t memory_total() {
    /* 已分类总量：不含 heap（heap 是"其余全部 C++ 堆"，与明细有交叠）。 */
    uint64_t n = 0;
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) {
        if ((Mem)i == Mem::heap) continue;
        n += g_mem[i].bytes.load(std::memory_order_relaxed);
    }
    return n;
}

uint64_t memory_area_total(Area a) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) {
        if ((Mem)i == Mem::heap) continue;
        if (area((Mem)i) == a) n += g_mem[i].bytes.load(std::memory_order_relaxed);
    }
    return n;
}

void memory_add(Mem m, uint64_t bytes) {
    if (bytes == 0 || !memory_enabled()) return;
    const size_t i = (size_t)m;
    if (i >= (size_t)Mem::count) return;
    MemCounter& c = g_mem[i];
    const uint64_t cur = c.bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    c.allocs.fetch_add(1, std::memory_order_relaxed);
    uint64_t peak = c.peak.load(std::memory_order_relaxed);
    while (cur > peak &&
           !c.peak.compare_exchange_weak(peak, cur, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void memory_sub(Mem m, uint64_t bytes) {
    if (bytes == 0 || !memory_enabled()) return;
    const size_t i = (size_t)m;
    if (i >= (size_t)Mem::count) return;
    MemCounter& c = g_mem[i];
    c.frees.fetch_add(1, std::memory_order_relaxed);
    uint64_t cur = c.bytes.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t next = cur > bytes ? cur - bytes : 0;   /* 下溢保护：陈旧指针不把计数打成天文数字 */
        if (c.bytes.compare_exchange_weak(cur, next, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }
}

void memory_set(Mem m, uint64_t bytes) {
    if (!memory_enabled()) return;
    const size_t i = (size_t)m;
    if (i >= (size_t)Mem::count) return;
    MemCounter& c = g_mem[i];
    c.bytes.store(bytes, std::memory_order_relaxed);
    uint64_t peak = c.peak.load(std::memory_order_relaxed);
    while (bytes > peak &&
           !c.peak.compare_exchange_weak(peak, bytes, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void memory_reset() {
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) {
        g_mem[i].bytes.store(0, std::memory_order_relaxed);
        g_mem[i].peak.store(0, std::memory_order_relaxed);
        g_mem[i].allocs.store(0, std::memory_order_relaxed);
        g_mem[i].frees.store(0, std::memory_order_relaxed);
    }
}

bool memory_due(double dt_seconds) {
    if (!memory_enabled()) return false;
    static double interval = 0.0;
    static double accum = 0.0;
    if (interval <= 0.0) {
        const char* e = std::getenv("MAQUE_TRACY_MEM_INTERVAL");
        interval = e && *e ? std::atof(e) : 1.0;
        if (interval < 0.05) interval = 0.05;     /* 防止 CI 里 1e-9 把曲线刷爆 */
    }
    accum += dt_seconds > 0.0 ? dt_seconds : 0.0;
    if (accum < interval) return false;
    accum = 0.0;
    return true;
}

void memory_publish() {
    if (!memory_enabled()) return;
    if (started()) {
        configure_memory_plots();
        for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) {
#if !MQ_TRACY_ALLOC_ENABLED
            if ((Mem)i == Mem::heap) continue;
#endif
            ::tracy::Profiler::PlotData(kMemPlot[i], (double)g_mem[i].bytes.load(std::memory_order_relaxed));
        }
        for (uint32_t i = 0; i < (uint32_t)Area::count; ++i)
            ::tracy::Profiler::PlotData(kAreaPlot[i], (double)memory_area_total((Area)i));
        ::tracy::Profiler::PlotData(kTotalPlot, (double)memory_total());
    }
    if (env_flag("MAQUE_TRACY_MEM_REPORT", false)) memory_report("tick");
}

void memory_report(const char* tag) {
    if (!memory_enabled()) return;
    char b[(size_t)Mem::count][24];
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) fmt_bytes(b[i], sizeof b[i], memory_bytes((Mem)i));
    /* 一行明细（近似的计量单位），一行分区总量。 */
    std::print("[mem] {}: script {} | node {} | texture {} | gpu {} | font {} | heap {}\n",
               tag ? tag : "tick", b[(size_t)Mem::script], b[(size_t)Mem::node], b[(size_t)Mem::texture],
               b[(size_t)Mem::gpu], b[(size_t)Mem::font], b[(size_t)Mem::heap]);
    char a[(size_t)Area::count][24];
    for (uint32_t i = 0; i < (uint32_t)Area::count; ++i)
        fmt_bytes(a[i], sizeof a[i], memory_area_total((Area)i));
    char total[24];
    fmt_bytes(total, sizeof total, memory_total());
    std::print("[mem] {}: area ui {} | 3d {} | system {} | script {} | tracked {}\n",
               tag ? tag : "tick", a[(size_t)Area::ui], a[(size_t)Area::world3d], a[(size_t)Area::system],
               a[(size_t)Area::script], total);
    /* 退出快照的 current 可能已被 teardown 清零 → 额外给一行**峰值**（高水位线才是"这次跑过多少"）。 */
    if (tag && std::strcmp(tag, "shutdown") == 0) {
        for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i)
            fmt_bytes(b[i], sizeof b[i], memory_peak((Mem)i));
        std::print("[mem] {}: peak script {} | node {} | texture {} | gpu {} | font {} | heap {}\n",
                   tag, b[(size_t)Mem::script], b[(size_t)Mem::node], b[(size_t)Mem::texture],
                   b[(size_t)Mem::gpu], b[(size_t)Mem::font], b[(size_t)Mem::heap]);
    }
}

int memory_format(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    char b[(size_t)Mem::count][24];
    for (uint32_t i = 0; i < (uint32_t)Mem::count; ++i) fmt_bytes(b[i], sizeof b[i], memory_bytes((Mem)i));
    const int n = std::snprintf(out, cap,
                                "script %s | node %s | texture %s | gpu %s | font %s | heap %s",
                                b[(size_t)Mem::script], b[(size_t)Mem::node], b[(size_t)Mem::texture],
                                b[(size_t)Mem::gpu], b[(size_t)Mem::font], b[(size_t)Mem::heap]);
    return n < 0 ? 0 : n;
}

int memory_area_format(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    char a[(size_t)Area::count][24];
    for (uint32_t i = 0; i < (uint32_t)Area::count; ++i)
        fmt_bytes(a[i], sizeof a[i], memory_area_total((Area)i));
    char total[24];
    fmt_bytes(total, sizeof total, memory_total());
    const int n = std::snprintf(out, cap, "ui %s | 3d %s | system %s | script %s | tracked %s",
                                a[(size_t)Area::ui], a[(size_t)Area::world3d], a[(size_t)Area::system],
                                a[(size_t)Area::script], total);
    return n < 0 ? 0 : n;
}

void memory_note_alloc(const void* ptr, size_t bytes, Mem m) {
#if MQ_TRACY_ENABLED
    if (!ptr || !memory_enabled() || !started() || g_in_mem_event) return;
    g_in_mem_event = true;
    ::tracy::Profiler::MemAllocNamed(ptr, bytes, false, name(m));
    g_in_mem_event = false;
#else
    (void)ptr;
    (void)bytes;
    (void)m;
#endif
}

void memory_note_alloc_callstack(const void* ptr, size_t bytes, Mem m, uint32_t depth) {
#if MQ_TRACY_ENABLED
    if (!ptr || !memory_enabled() || !started() || g_in_mem_event) return;
    g_in_mem_event = true;
    ::tracy::Profiler::MemAllocCallstackNamed(ptr, bytes, (int)depth, false, name(m));
    g_in_mem_event = false;
#else
    (void)ptr;
    (void)bytes;
    (void)m;
    (void)depth;
#endif
}

void memory_note_free(const void* ptr, Mem m) {
#if MQ_TRACY_ENABLED
    if (!ptr || !memory_enabled() || !started() || g_in_mem_event) return;
    g_in_mem_event = true;
    ::tracy::Profiler::MemFreeNamed(ptr, false, name(m));
    g_in_mem_event = false;
#else
    (void)ptr;
    (void)m;
#endif
}

#endif  /* MQ_TRACY_MEMORY_ENABLED：关闭时头文件里的 inline no-op 就是全部实现 */

} // namespace maque::tracy
