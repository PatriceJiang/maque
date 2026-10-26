/* maque::tracy 实现：Tracy client 生命周期 + 数据上报（宏在 core/tracy/mq_tracy.h）。
 *
 * 生命周期是**手动**的（TracyClient 以 TRACY_DELAYED_INIT + TRACY_MANUAL_LIFETIME 编译）：
 *   · 普通运行 / CI：一次都不 startup → 不监听端口、不占线程、zone 全部 no-op；
 *   · F3 或 MAQUE_TRACY=1 → startup() → 监听 8086，等 profiler GUI 连接；
 *   · 退出前 shutdown()（否则 Tracy 的堆分配会被 ASan 报泄漏）。
 *
 * 注意：本文件在 namespace maque::tracy 内，引用 Tracy 全局命名空间必须写 `::tracy::`
 *（裸 `tracy::` 会解析成 maque::tracy 自己，实测编译报 "no member named ..."）。
 */
#include "core/tracy/mq_tracy.h"

#include <atomic>
#include <cstring>

#if MQ_TRACY_ENABLED
#  include "common/TracySystem.hpp"
#  include "tracy/Tracy.hpp"
#endif

namespace maque::tracy {
namespace {

/* 与 client 内部状态同步的本地标志：避免 IsProfilerStarted 与启动/关闭之间的竞态窗口。 */
std::atomic<bool> g_started{ false };

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
    if (!::tracy::IsProfilerStarted()) ::tracy::StartupProfiler();
    g_started.store(true, std::memory_order_release);
    return true;
#else
    return false;
#endif
}

void shutdown() {
#if MQ_TRACY_ENABLED
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

} // namespace maque::tracy
