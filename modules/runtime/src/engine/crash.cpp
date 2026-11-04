/* 终止信号与崩溃落盘（docs/09 §5）：处理器只置位/写预渲染缓冲并恢复默认行为，
 * 真正的上下文预渲染在帧边界（engine/run.cpp 的 refresh_crash_buffer）。
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 终止信号（docs/09 §5）：处理器只置位并恢复默认行为，写盘在帧边界 ---- */
volatile sig_atomic_t g_signal_hit = 0;
const char* g_signal_name = nullptr;
/* 信号安全崩溃落盘（docs/09 §5）：处理器内不得分配/加锁/调用 iostream。
 * 因此引擎在每个帧边界把"崩溃上下文"预渲染进 g_crash_buf，并预打开报告 fd；
 * 处理器只做 write(fd, buf, len) 后恢复默认行为再 raise。 */
int  g_crash_fd = -1;
char g_crash_buf[16384];
volatile sig_atomic_t g_crash_len = 0;
/* 优雅退出标志（宿主 kill / CI 超时清理用）。
 * 注意：SDL2 会**接管 SIGTERM/SIGINT 并只投递 SDL_QUIT 事件** —— 无窗口（headless）时没人处理该事件，
 * 进程就杀不掉了。这里自己装处理器置标志，主循环据此走正常退出路径（teardown/端点清理）。 */
volatile sig_atomic_t g_quit_requested = 0;
void on_quit_signal(int) { g_quit_requested = 1; }

void on_fatal_signal(int sig) {
    g_signal_hit = sig;
    g_signal_name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT"
                  : sig == SIGFPE ? "SIGFPE" : sig == SIGINT ? "SIGINT" : "signal";
    if (g_crash_fd >= 0 && g_crash_len > 0) {
        const maque::compat::ssize_t ignored =
            maque::compat::write_fd(g_crash_fd, g_crash_buf, (size_t)g_crash_len);
        (void)ignored;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace maque::rt
