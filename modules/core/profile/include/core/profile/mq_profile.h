/* maque::profile —— **Linux 采样剖析（perf）**封装（docs/35）。
 *
 * 决策（docs/35 §1）：只支持 Linux + `perf(1)`，**不接 oprofile**：
 *   · perf 是内核自带的唯一标准采样器（oprofile 已进入维护模式，需要额外内核模块/root 常驻守护进程）；
 *   · `perf.data` + DWARF/帧指针可得到调用栈、源码行、内联函数；
 *   · 无守护进程、无后台线程：`perf record` 是引擎启动的**外部子进程**，不改变模拟行为。
 *
 * 与 Tracy（docs/29）的分工：
 *   · Tracy = **时间线/zone/内存域**（"这一帧里 render 占多少"）；
 *   · perf  = **统计采样火焰图/热点**（"这 600 帧里 CPU 花在哪些函数/源码行"）。
 *
 * 设计约束：
 *   · 只在 Linux + `MAQUE_PROFILE=ON` 构建里编译真实实现；其它平台/关闭时全部返回
 *     `unsupported`（调用方不需要 `#if`）。
 *   · **进程级单例**：perf 附加在整个进程上，同一时刻只能有一个录制（与 Tracy 客户端同理）。
 *   · 不做自动改代码：优化由 agent/人根据 `hotspots.json` 决定（docs/35 §7）。
 *
 * 典型用法（runtime 已封装，见 modules/runtime/src/engine/profile.cpp）：
 *   profile::Options o; o.output_dir = ".maque/profile/run1";
 *   if (!profile::start(o, &err)) ...;
 *   profile::mark(frame, "physics");                 // 可选：给报告打时间戳标记
 *   profile::stop(true, &err);                       // SIGINT → perf report → hotspots
 *   std::print("{}", profile::hotspots_json(20));
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* 构建期开关（CMake 的 maque::profile 目标 PUBLIC 定义；缺省 = 关闭）。
 * 运行时还要看 `supported()`（Linux 才为 true）。 */
#if !defined(MQ_PROFILE_ENABLED)
#  define MQ_PROFILE_ENABLED 0
#endif

namespace maque::profile {

enum class State : uint32_t {
    unsupported = 0,  /* 本构建/平台不支持 */
    idle        = 1,  /* 支持，当前未录制 */
    recording   = 2,  /* perf record 正在运行 */
    failed      = 3,  /* 最近一次启动/录制失败（reason 可读） */
};

const char* state_name(State s);

struct Options {
    std::string output_dir;          /* 必填：产物目录（perf.data / 日志 / hotspots.json） */
    std::string label;               /* 可选：人类可读标签（写入 meta.json） */
    std::string context_json;        /* 可选：调用方上下文 JSON（场景/帧号/构建），原样写入 meta.json */
    std::string perf_exe = "perf";   /* perf 可执行文件（MAQUE_PROFILE_PERF 覆盖） */
    int         frequency = 999;     /* 采样频率 Hz（perf -F；<=0 = 用 perf 默认） */
    std::string callgraph = "fp";    /* fp | dwarf | none（构建默认带 -fno-omit-frame-pointer） */
    bool        exclude_kernel = false; /* 不采内核侧样本（--exclude-kernel） */
    int         startup_timeout_ms = 600; /* 启动后等 "perf 仍存活" 的窗口；失败可快速报权限错误 */
    uint64_t    start_frame = 0;     /* 调用方的模拟帧号（只写元数据/标记） */
};

struct Hotspot {
    double      percent = 0.0;       /* 自身耗时占比（0..100；--no-children） */
    uint64_t    samples = 0;         /* 样本数（perf 不提供时为 0，percent 仍有效） */
    std::string dso;                 /* 共享对象/可执行文件 */
    std::string symbol;              /* 已符号化函数名（可能含 C++ 模板/参数） */
};

struct Status {
    State       state = State::unsupported;
    std::string reason;              /* unsupported / failed 的原因（可读） */
    int         pid = 0;             /* perf record 子进程 */
    uint64_t    start_frame = 0;
    double      elapsed_ms = 0.0;    /* 从 start 到 stop/现在 */
    uint64_t    samples = 0;         /* 最近一次 report 的样本总数 */
    bool        report_ok = false;
    std::string output_dir;
    std::string data_path;           /* perf.data */
    std::string log_path;            /* perf.log（record 的 stdout+stderr） */
    std::string report_path;         /* perf-report.txt（原始文本，保留给 agent） */
    std::string hotspots_path;       /* hotspots.json */
    std::string meta_path;           /* meta.json */
    std::string marks_path;          /* marks.jsonl */
    std::string perf_version;        /* 例如 "perf version 6.8.12"；探测失败为空 */
};

/* 本构建/本平台是否支持（Linux && MQ_PROFILE_ENABLED）。 */
bool supported();
/* 不支持时的可读原因（支持时返回空串）。 */
const char* support_reason();

/* 当前状态（进程级单例；线程安全只保证"同线程帧边界调用"——引擎就是这么用的）。
 * 注意：`status()` 会复制路径字符串；**热路径**（每帧自动停判断）用 `recording()`。 */
Status status();
bool recording();

/* 启动 perf record 附加到**本进程**。失败时 error 写入可操作原因（perf 不存在/权限不足/
 * 目录不可写…），并保持 state=failed。成功后 state=recording。 */
bool start(const Options& opts, std::string* error);

/* 停止（SIGINT → 等待 → 超时 SIGKILL），generate_report=true 时同步跑 `perf report`
 * 并写 hotspots.json。返回 false 仅表示"没有在录制/停止失败"；报告失败会写 error 但
 * 录制已被停止（调用方可以继续跑游戏）。
 * timeout_ms = 等 perf 正常退出的上限；窗口关闭路径应短（避免看起来"卡死"），
 * 显式 stop 可以长一点（默认 5s）。 */
bool stop(bool generate_report, std::string* error, int timeout_ms = 5000);

/* 对最近一次 perf.data 重新生成报告（agent 可以在 stop 后换 top 重跑解析）。 */
bool refresh_report(std::string* error);

/* 最近一次报告的热点（按 percent 降序）。 */
const std::vector<Hotspot>& hotspots();

/* 生成紧凑 JSON（含状态/路径/总数/top N 热点），供控制面/工具直接返回。 */
std::string hotspots_json(size_t top);

/* 给本次录制追加一个时间戳标记（frame + label）→ marks.jsonl。
 * 用于把"加载期 / 稳定期 / 关卡切换"从一次录制里切出来。未录制时返回 false。 */
bool mark(uint64_t frame, const std::string& label, std::string* error);

/* 解析 `perf report --stdio --no-children --sort dso,symbol [--show-nr-samples]` 的文本。
 * 公开给单测（不需要真的跑 perf；伪造文本即可验证百分比/样本/DSO/符号解析）。
 * 无法识别的行（表头、警告、空行）直接跳过，不抛异常。 */
std::vector<Hotspot> parse_report_text(const std::string& text);

}  // namespace maque::profile
