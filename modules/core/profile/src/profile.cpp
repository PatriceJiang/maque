/* Linux 采样剖析（perf）实现 —— docs/35。
 *
 * 关键设计（改这块前先读 docs/35 §8）：
 *   · perf 是**外部子进程**（`perf record -p <本进程>`），引擎侧没有采样线程、没有信号处理器；
 *   · 启动时等一小段（默认 600ms）确认 perf 还活着：权限不足/事件不可用会立刻退出，于是能在
 *     同一帧里给出可操作错误，而不是"录了 600 帧才发现 perf.data 是空的"；
 *   · 停止用 **SIGINT**（perf 收到后才会冲刷 perf.data；SIGTERM 可能丢尾部），超时才 SIGKILL；
 *   · 报告解析只依赖 `perf report --stdio` 的文本（单测用伪造文本覆盖），不链接 libperf。
 */
#include "core/profile/mq_profile.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if MQ_PROFILE_ENABLED && defined(__linux__)
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <poll.h>
extern char** environ;
#endif

namespace maque::profile {
namespace {

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

[[maybe_unused]] std::string json_num(double v, int decimals = 2) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

[[maybe_unused]] std::string now_iso8601() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}

[[maybe_unused]] bool write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << text;
    return (bool)f;
}

[[maybe_unused]] std::string read_file(const std::string& path, size_t max_bytes = 64 * 1024) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > max_bytes) s = s.substr(s.size() - max_bytes);   // 保留尾部（错误通常在最后）
    return s;
}

[[maybe_unused]] std::string first_line(const std::string& s) {
    const size_t p = s.find('\n');
    return trim(p == std::string::npos ? s : s.substr(0, p));
}

/* ---- 供单测/状态查询的进程级状态（stub 构建也会实例化） ---- */
struct Global {
    State       state = State::unsupported;
    std::string reason;
    int         pid = 0;
    uint64_t    start_frame = 0;
    std::chrono::steady_clock::time_point started{};
    std::string output_dir;
    std::string data_path;
    std::string log_path;
    std::string report_path;
    std::string hotspots_path;
    std::string meta_path;
    std::string marks_path;
    std::string perf_version;
    std::string perf_exe = "perf";
    double      elapsed_ms = 0.0;
    std::string label;
    std::string context_json;
    std::vector<Hotspot> hotspots;
    uint64_t samples = 0;
    bool report_ok = false;
};

Global& global() {
    static Global g = [] {
        Global r;
        r.state = supported() ? State::idle : State::unsupported;
        r.reason = support_reason();
        return r;
    }();
    return g;
}

}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::unsupported: return "unsupported";
        case State::idle:        return "idle";
        case State::recording:   return "recording";
        case State::failed:      return "failed";
        default:                 return "?";
    }
}

bool supported() {
#if MQ_PROFILE_ENABLED && defined(__linux__)
    return true;
#else
    return false;
#endif
}

const char* support_reason() {
#if MQ_PROFILE_ENABLED && defined(__linux__)
    return "";
#elif MQ_PROFILE_ENABLED
    return "perf profiling is Linux-only (this platform is not Linux)";
#elif defined(__linux__)
    return "this build was configured with -DMAQUE_PROFILE=OFF";
#else
    return "perf profiling is Linux-only; this build has no profiler support";
#endif
}

/* ====================================================================== */
/* 报告文本解析 —— 与平台无关（单测覆盖；stub 构建也能用）                  */
/* ====================================================================== */

std::vector<Hotspot> parse_report_text(const std::string& text) {
    /* 目标行（perf report --stdio --no-children --sort dso,symbol [--show-nr-samples]）：
     *     12.34%  123  maque_player  maque_player  [.] maque::rt::Engine::step_frame()
     * 兼容没有 Samples 列、命令名/DSO 含空格、符号含括号/空格/模板参数的形态。
     * 解析策略：从左取第一个 `%` 作为占比；若下一个 token 全数字则当样本数；
     * `[.]`/`[k]`/`[u]` 之前最后一个 token 当 DSO；标记之后全部当符号。 */
    std::map<std::pair<std::string, std::string>, Hotspot> agg;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        /* 表头/警告/进度条不是数据行：数据行必须以数字开头。 */
        if (!std::isdigit((unsigned char)line[0])) continue;

        const size_t pct_pos = line.find('%');
        if (pct_pos == std::string::npos) continue;
        char* endp = nullptr;
        const double pct = std::strtod(line.c_str(), &endp);
        if (endp != line.c_str() + pct_pos) continue;

        /* 找符号类型标记 `[.]` / `[k]` / `[u]` / `[g]` / `[j]`。 */
        size_t mark = std::string::npos;
        for (size_t i = pct_pos + 1; i + 2 < line.size(); ++i) {
            if (line[i] == '[' && line[i + 2] == ']' &&
                (line[i + 1] == '.' || line[i + 1] == 'k' || line[i + 1] == 'u' ||
                 line[i + 1] == 'g' || line[i + 1] == 'j')) {
                mark = i;
                break;
            }
        }
        if (mark == std::string::npos) continue;
        const std::string symbol = trim(line.substr(mark + 3));
        if (symbol.empty()) continue;

        const std::string left = trim(line.substr(0, mark));
        std::vector<std::string> tok;
        {
            size_t i = 0;
            while (i < left.size()) {
                while (i < left.size() && std::isspace((unsigned char)left[i])) ++i;
                size_t b = i;
                while (i < left.size() && !std::isspace((unsigned char)left[i])) ++i;
                if (i > b) tok.push_back(left.substr(b, i - b));
            }
        }
        if (tok.size() < 2) continue;
        /* tok[0] = 占比；tok[1] 若为纯数字 = Samples；最后一个 token = DSO。 */
        std::string dso = tok.back();
        uint64_t samples = 0;
        if (tok.size() >= 3) {
            bool digits = !tok[1].empty();
            for (char c : tok[1]) if (!std::isdigit((unsigned char)c)) { digits = false; break; }
            if (digits) samples = std::strtoull(tok[1].c_str(), nullptr, 10);
        }
        if (dso.empty()) dso = "?";

        auto& h = agg[{dso, symbol}];
        h.dso = dso;
        h.symbol = symbol;
        h.percent += pct;
        h.samples += samples;
    }

    std::vector<Hotspot> out;
    out.reserve(agg.size());
    for (auto& [key, h] : agg) { (void)key; out.push_back(std::move(h)); }
    std::sort(out.begin(), out.end(), [](const Hotspot& a, const Hotspot& b) {
        if (a.percent != b.percent) return a.percent > b.percent;
        if (a.samples != b.samples) return a.samples > b.samples;
        if (a.dso != b.dso) return a.dso < b.dso;
        return a.symbol < b.symbol;
    });
    return out;
}

#if MQ_PROFILE_ENABLED && defined(__linux__)
/* ====================================================================== */
/* 真实实现（Linux）                                                       */
/* ====================================================================== */

namespace {

struct Capture {
    int         rc = -1;
    bool        timed_out = false;
    std::string out;
};

std::string resolve_exe(const std::string& exe) {
    if (exe.find('/') != std::string::npos)
        return ::access(exe.c_str(), X_OK) == 0 ? exe : std::string();
    const char* path = std::getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p(path);
    size_t i = 0;
    while (i <= p.size()) {
        const size_t c = p.find(':', i);
        const std::string dir = p.substr(i, c == std::string::npos ? std::string::npos : c - i);
        const std::string cand = (dir.empty() ? std::string(".") : dir) + "/" + exe;
        if (::access(cand.c_str(), X_OK) == 0) return cand;
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return {};
}

/* 父进程侧构造 execve 环境（`LC_ALL=C` 保证 perf 输出可解析）。
 * 过滤已有的 LC_ALL/LANG，避免重复项行为不确定；不改父进程 environ。 */
std::vector<std::string> build_env() {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        if (std::strncmp(*e, "LC_ALL=", 7) == 0 || std::strncmp(*e, "LANG=", 5) == 0) continue;
        env.emplace_back(*e);
    }
    env.emplace_back("LC_ALL=C");
    return env;
}

Capture run_capture(const std::string& exe, const std::vector<std::string>& args,
                    int timeout_ms = 30000) {
    Capture cap;
    const std::string path = resolve_exe(exe);
    if (path.empty()) { cap.out = "executable not found: " + exe; return cap; }

    std::vector<std::string> argv_storage;
    argv_storage.push_back(exe);
    for (const auto& a : args) argv_storage.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argv_storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = build_env();
    std::vector<char*> envp;
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) { cap.out = std::string("pipe: ") + std::strerror(errno); return cap; }
    const pid_t pid = ::fork();
    if (pid < 0) { cap.out = std::string("fork: ") + std::strerror(errno); ::close(fds[0]); ::close(fds[1]); return cap; }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        if (fds[1] > STDERR_FILENO) ::close(fds[1]);
        ::execve(path.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    const auto t0 = std::chrono::steady_clock::now();
    bool eof = false;
    int status = 0;
    bool reaped = false;
    while (!eof) {
        struct pollfd pfd{ fds[0], POLLIN | POLLHUP | POLLERR, 0 };
        const int pr = ::poll(&pfd, 1, 100);
        if (pr > 0) {
            char buf[65536];
            for (;;) {
                const ssize_t n = ::read(fds[0], buf, sizeof buf);
                if (n > 0) cap.out.append(buf, (size_t)n);
                else if (n == 0) { eof = true; break; }
                else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else { eof = true; break; }
            }
        }
        if (!reaped) {
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) reaped = true;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > (double)timeout_ms) {
            cap.timed_out = true;
            ::kill(pid, SIGKILL);
            break;
        }
    }
    ::close(fds[0]);
    if (!reaped) {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (WIFEXITED(status)) cap.rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) cap.rc = 128 + WTERMSIG(status);
    else cap.rc = -1;
    return cap;
}

void write_meta(const Global& g, int record_rc, const std::string& extra_reason) {
    if (g.meta_path.empty()) return;
    const double elapsed = g.started.time_since_epoch().count()
        ? (g.state == State::recording
               ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.started).count()
               : g.elapsed_ms)
        : 0.0;
    std::string o;
    o += "{\n";
    o += "  \"backend\": \"perf\",\n";
    o += "  \"state\": \"" + std::string(state_name(g.state)) + "\",\n";
    o += "  \"reason\": \"" + json_escape(extra_reason.empty() ? g.reason : extra_reason) + "\",\n";
    o += "  \"perf_version\": \"" + json_escape(g.perf_version) + "\",\n";
    o += "  \"pid\": " + std::to_string(g.pid) + ",\n";
    o += "  \"record_rc\": " + std::to_string(record_rc) + ",\n";
    o += "  \"label\": \"" + json_escape(g.label) + "\",\n";
    o += "  \"start_frame\": " + std::to_string(g.start_frame) + ",\n";
    o += "  \"elapsed_ms\": " + json_num(elapsed, 1) + ",\n";
    o += "  \"samples\": " + std::to_string(g.samples) + ",\n";
    o += "  \"hotspots\": " + std::to_string(g.hotspots.size()) + ",\n";
    o += "  \"report_ok\": " + std::string(g.report_ok ? "true" : "false") + ",\n";
    o += "  \"finished_at\": \"" + now_iso8601() + "\",\n";
    if (!g.context_json.empty()) o += "  \"context\": " + g.context_json + ",\n";
    o += "  \"artifacts\": {\n";
    o += "    \"data\": \"" + json_escape(g.data_path) + "\",\n";
    o += "    \"log\": \"" + json_escape(g.log_path) + "\",\n";
    o += "    \"report\": \"" + json_escape(g.report_path) + "\",\n";
    o += "    \"hotspots\": \"" + json_escape(g.hotspots_path) + "\",\n";
    o += "    \"marks\": \"" + json_escape(g.marks_path) + "\"\n";
    o += "  }\n";
    o += "}\n";
    write_file(g.meta_path, o);
}

std::string hotspots_json_impl(const Global& g, size_t top) {
    std::string o;
    o += "{\"ok\":" + std::string(g.state == State::failed ? "false" : "true") +
         ",\"backend\":\"perf\",\"state\":\"" + std::string(state_name(g.state)) + "\"";
    o += ",\"supported\":true,\"report_ok\":" + std::string(g.report_ok ? "true" : "false");
    o += ",\"pid\":" + std::to_string(g.pid);
    o += ",\"samples\":" + std::to_string(g.samples);
    o += ",\"hotspot_count\":" + std::to_string(g.hotspots.size());
    o += ",\"perf_version\":\"" + json_escape(g.perf_version) + "\"";
    o += ",\"start_frame\":" + std::to_string(g.start_frame);
    o += ",\"elapsed_ms\":" + json_num(g.started.time_since_epoch().count()
        ? (g.state == State::recording
               ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.started).count()
               : g.elapsed_ms)
        : 0.0, 1);
    o += ",\"reason\":\"" + json_escape(g.reason) + "\"";
    o += ",\"data\":\"" + json_escape(g.data_path) + "\"";
    o += ",\"log\":\"" + json_escape(g.log_path) + "\"";
    o += ",\"report\":\"" + json_escape(g.report_path) + "\"";
    o += ",\"hotspots_file\":\"" + json_escape(g.hotspots_path) + "\"";
    o += ",\"meta\":\"" + json_escape(g.meta_path) + "\"";
    o += ",\"hotspots\":[";
    const size_t n = std::min(top, g.hotspots.size());
    for (size_t i = 0; i < n; ++i) {
        if (i) o += ",";
        o += "{\"percent\":" + json_num(g.hotspots[i].percent, 2) +
             ",\"samples\":" + std::to_string(g.hotspots[i].samples) +
             ",\"dso\":\"" + json_escape(g.hotspots[i].dso) +
             "\",\"symbol\":\"" + json_escape(g.hotspots[i].symbol) + "\"}";
    }
    o += "]}";
    return o;
}

bool start_impl(const Options& opts, std::string* error) {
    Global& g = global();
    auto fail = [&](const std::string& msg) {
        g.state = State::failed;
        g.reason = msg;
        if (error) *error = msg;
        if (!g.meta_path.empty()) write_meta(g, -1, msg);
        return false;
    };
    if (!supported()) return fail(support_reason());
    /* 正在录制：只报错，**不要**覆盖当前录制产物的 meta.json。 */
    if (g.state == State::recording) {
        if (error) *error = "already recording";
        return false;
    }
    if (opts.output_dir.empty()) return fail("output_dir is empty");

    std::error_code ec;
    std::filesystem::create_directories(opts.output_dir, ec);
    if (ec) return fail("cannot create output dir '" + opts.output_dir + "': " + ec.message());

    g = Global{};
    g.state = State::idle;
    g.label = opts.label;
    g.context_json = opts.context_json;
    g.start_frame = opts.start_frame;
    g.perf_exe = opts.perf_exe.empty() ? "perf" : opts.perf_exe;
    g.output_dir = opts.output_dir;
    g.data_path = opts.output_dir + "/perf.data";
    g.log_path = opts.output_dir + "/perf.log";
    g.report_path = opts.output_dir + "/perf-report.txt";
    g.hotspots_path = opts.output_dir + "/hotspots.json";
    g.meta_path = opts.output_dir + "/meta.json";
    g.marks_path = opts.output_dir + "/marks.jsonl";
    /* 同一目录重复 start：清掉上一轮产物，避免旧热点/旧 meta 被误读。 */
    std::filesystem::remove(g.marks_path, ec);
    std::filesystem::remove(g.report_path, ec);
    std::filesystem::remove(g.hotspots_path, ec);
    std::filesystem::remove(g.meta_path, ec);
    std::filesystem::remove(g.data_path, ec);

    const std::string perf_path = resolve_exe(opts.perf_exe.empty() ? "perf" : opts.perf_exe);
    if (perf_path.empty())
        return fail("perf not found (looked for '" + (opts.perf_exe.empty() ? "perf" : opts.perf_exe) +
                    "'); install perf or set MAQUE_PROFILE_PERF=<path>");
    {
        Capture v = run_capture(perf_path, { "--version" }, 5000);
        if (v.rc != 0) {
            std::string why = "cannot run perf --version";
            if (v.rc == 127) why += " (not executable)";
            if (!v.out.empty()) why += ": " + first_line(v.out);
            return fail(why);
        }
        g.perf_version = first_line(v.out);
    }

    std::vector<std::string> base{ "record", "-o", g.data_path, "-p", std::to_string((long)::getpid()) };
    if (opts.frequency > 0) {
        base.push_back("-F");
        base.push_back(std::to_string(opts.frequency));
    }
    if (!opts.callgraph.empty() && opts.callgraph != "none") {
        base.push_back("--call-graph");
        base.push_back(opts.callgraph);
    }
    if (opts.exclude_kernel) base.push_back("--exclude-kernel");

    /* `--no-buildid-mmap` 抑制某些内核/容器里的 build-id 警告；老 perf 不认该选项时回退。 */
    auto spawn_try = [&](bool no_buildid) -> std::pair<int, int> {
        std::vector<std::string> args = base;
        if (no_buildid) args.push_back("--no-buildid-mmap");
        const int fd = ::open(g.log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) return { -1, 0 };
        std::vector<std::string> argv_storage{ perf_path };
        for (auto& a : args) argv_storage.push_back(a);
        std::vector<char*> argv;
        for (auto& s : argv_storage) argv.push_back(s.data());
        argv.push_back(nullptr);
        std::vector<std::string> env_storage = build_env();
        std::vector<char*> envp;
        for (auto& s : env_storage) envp.push_back(s.data());
        envp.push_back(nullptr);

        const pid_t pid = ::fork();
        if (pid < 0) { ::close(fd); return { -1, 0 }; }
        if (pid == 0) {
            /* 不 setsid：让 Ctrl-C/进程组信号同时到达 perf（它会 SIGINT 冲刷 perf.data），
             * 崩溃/中断时尽量留下可分析的 trace；正常停止仍由显式 SIGINT 完成。 */
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) ::close(fd);
            ::execve(perf_path.c_str(), argv.data(), envp.data());
            ::_exit(127);
        }
        ::close(fd);
        return { (int)pid, 0 };
    };

    auto wait_startup = [&](int pid, int timeout_ms, int* out_status) -> bool {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            const pid_t w = ::waitpid(pid, out_status, WNOHANG);
            if (w == pid) return false;              // 已退出 = 启动失败
            if (w < 0 && errno != EINTR) return false;
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= (double)timeout_ms) return true;  // 仍活着 = 成功
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    auto attempt = [&](bool no_buildid) -> bool {
        auto [pid, spawn_err] = spawn_try(no_buildid);
        (void)spawn_err;
        if (pid <= 0) {
            fail("cannot spawn perf (fork/open failed); see perf.log");
            return false;
        }
        int st = 0;
        if (wait_startup(pid, std::max(50, opts.startup_timeout_ms), &st)) {
            g.pid = pid;
            g.state = State::recording;
            g.started = std::chrono::steady_clock::now();
            g.reason.clear();
            write_meta(g, 0, {});
            if (error) error->clear();
            return true;
        }
        const std::string log = read_file(g.log_path, 16 * 1024);
        const std::string tail = trim(log.size() > 2000 ? log.substr(log.size() - 2000) : log);
        const bool unknown_opt = no_buildid &&
            (tail.find("unrecognized option") != std::string::npos ||
             tail.find("unknown option") != std::string::npos);
        if (unknown_opt) return false;               // 交给外层重试（不带该 flag）
        const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        std::string msg = "perf record exited during startup (rc=" + std::to_string(rc) + ")";
        if (!tail.empty()) msg += ": " + tail;
        if (tail.find("paranoid") != std::string::npos ||
            tail.find("permission") != std::string::npos ||
            tail.find("Access to performance monitoring") != std::string::npos) {
            msg += "\n  hint: sudo sysctl kernel.perf_event_paranoid=1  (or grant CAP_PERFMON), "
                   "then rerun; see docs/35 §8";
        }
        fail(msg);
        return false;
    };

    if (attempt(true)) return true;
    if (g.state == State::failed) {
        /* 失败可能是老 perf 不认 --no-buildid-mmap；用不带该 flag 的命令再试一次。
         * （只有"未知选项"的失败会走到这里；权限错误已经直接 fail。） */
        const std::string why = g.reason;
        if (why.find("unknown option") == std::string::npos &&
            why.find("unrecognized option") == std::string::npos) return false;
    }
    g.state = State::idle;
    g.reason.clear();
    return attempt(false);
}

bool stop_impl(bool generate_report, std::string* error, int timeout_ms) {
    Global& g = global();
    if (g.state != State::recording) {
        if (error) *error = g.reason.empty() ? "not recording" : g.reason;
        return false;
    }
    const int pid = g.pid;
    ::kill(pid, SIGINT);

    int status = 0;
    bool exited = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { exited = true; break; }
        if (w < 0 && errno != EINTR) break;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms > (double)std::max(100, timeout_ms)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) {
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    g.pid = 0;
    g.state = State::idle;
    g.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.started).count();
    g.reason.clear();
    if (rc != 0 && rc != 130 && !exited) g.reason = "perf record did not exit cleanly";
    write_meta(g, rc, {});
    if (generate_report) {
        std::string rerr;
        if (!refresh_report(&rerr) && error) *error = rerr;
    }
    return true;
}

bool refresh_report_impl(std::string* error) {
    Global& g = global();
    auto fail = [&](const std::string& msg) {
        g.report_ok = false;
        g.reason = msg;
        if (error) *error = msg;
        write_meta(g, 0, msg);
        return false;
    };
    if (!supported()) return fail(support_reason());
    std::error_code ec;
    if (!std::filesystem::exists(g.data_path, ec) || std::filesystem::file_size(g.data_path, ec) == 0)
        return fail("perf.data is missing or empty (perf recorded no samples; see docs/35 §8)");

    auto run_report = [&](bool show_samples) {
        std::vector<std::string> args{ "report", "--stdio", "-i", g.data_path,
                                       "--no-children", "--sort", "dso,symbol", "--percent-limit", "0" };
        if (show_samples) args.push_back("--show-nr-samples");
        return run_capture(g.perf_exe, args, 60000);
    };
    Capture cap = run_report(true);
    if (cap.rc != 0 && (cap.out.find("unknown option") != std::string::npos ||
                        cap.out.find("unrecognized option") != std::string::npos))
        cap = run_report(false);

    write_file(g.report_path, cap.out);
    if (cap.rc != 0)
        return fail("perf report failed (rc=" + std::to_string(cap.rc) + "): " + first_line(cap.out));

    g.hotspots = parse_report_text(cap.out);
    g.samples = 0;
    for (const auto& h : g.hotspots) g.samples += h.samples;
    g.report_ok = !g.hotspots.empty();
    if (!g.report_ok)
        return fail("perf report produced no hotspot rows (missing symbols? see docs/35 §11)");
    g.reason.clear();
    write_file(g.hotspots_path, hotspots_json_impl(g, 100));
    write_meta(g, 0, {});
    return true;
}

bool mark_impl(uint64_t frame, const std::string& label, std::string* error) {
    Global& g = global();
    if (g.state != State::recording) {
        if (error) *error = "not recording";
        return false;
    }
    std::ofstream f(g.marks_path, std::ios::app);
    if (!f) {
        if (error) *error = "cannot append " + g.marks_path;
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g.started).count();
    f << "{\"frame\":" << frame << ",\"t_ms\":" << json_num(ms, 1)
      << ",\"label\":\"" << json_escape(label) << "\"}\n";
    return true;
}

}  // namespace

bool start(const Options& opts, std::string* error) { return start_impl(opts, error); }
bool stop(bool generate_report, std::string* error, int timeout_ms) { return stop_impl(generate_report, error, timeout_ms); }
bool refresh_report(std::string* error) { return refresh_report_impl(error); }
bool mark(uint64_t frame, const std::string& label, std::string* error) { return mark_impl(frame, label, error); }

bool recording() { return global().state == State::recording; }

Status status() {
    Global& g = global();
    Status s;
    s.state = g.state;
    s.reason = g.reason;
    s.pid = g.pid;
    s.start_frame = g.start_frame;
    s.samples = g.samples;
    s.report_ok = g.report_ok;
    s.output_dir = g.output_dir;
    s.data_path = g.data_path;
    s.log_path = g.log_path;
    s.report_path = g.report_path;
    s.hotspots_path = g.hotspots_path;
    s.meta_path = g.meta_path;
    s.marks_path = g.marks_path;
    s.perf_version = g.perf_version;
    if (g.started.time_since_epoch().count())
        s.elapsed_ms = (g.state == State::recording)
            ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.started).count()
            : g.elapsed_ms;
    return s;
}

const std::vector<Hotspot>& hotspots() { return global().hotspots; }

std::string hotspots_json(size_t top) { return hotspots_json_impl(global(), top); }

#else  /* 非 Linux / MAQUE_PROFILE=OFF：接口保持可用，调用方不需要 #if */

bool start(const Options&, std::string* error) {
    if (error) *error = support_reason();
    return false;
}
bool stop(bool, std::string* error, int) {
    if (error) *error = support_reason();
    return false;
}
bool refresh_report(std::string* error) {
    if (error) *error = support_reason();
    return false;
}
bool mark(uint64_t, const std::string&, std::string* error) {
    if (error) *error = support_reason();
    return false;
}
bool recording() { return false; }

Status status() {
    Global& g = global();
    Status s;
    s.state = State::unsupported;
    s.reason = support_reason();
    s.perf_version = g.perf_version;
    return s;
}
const std::vector<Hotspot>& hotspots() { return global().hotspots; }
std::string hotspots_json(size_t) {
    return std::string("{\"ok\":false,\"backend\":\"perf\",\"state\":\"unsupported\",\"supported\":false,"
                       "\"report_ok\":false,\"reason\":\"") +
           json_escape(support_reason()) + "\",\"hotspots\":[]}";
}

#endif

}  // namespace maque::profile
