/* maque::tracy —— Tracy 性能分析的**统一封装**（docs/29）。
 *
 * 为什么先封装再埋点：
 *   ① 各模块不直接 include 第三方 tracy 头 —— 命名/颜色/开关只有一处；
 *   ② 本引擎按**分区（Area）+ 模块**给 zone 上色：UI / 3D **暖色**、System / Script **冷色**
 *      （`Area` + `warm()`），每个模块在所属色系里取一个色阶 → 时间线一眼分域；
 *   ③ 构建开关 `-DMAQUE_TRACY=OFF` 时所有宏展开为 no-op（无第三方头、无链接依赖）；
 *   ④ Tracy 客户端**手动启动**（F3 / `MAQUE_TRACY=1`）：未启动时 zone 立即返回，
 *      普通运行/CI 不监听端口、不额外占线程。
 *
 * 宏分几类（全部在文件末尾）：
 *   · 时间轴 zone      MQ_TRACY_ZONE / _N / _NAMED / _TEXT / _VALUE / _AREA* / _MEM*
 *   · 帧与数据          MQ_TRACY_FRAME / PLOT / PLOT_CONFIG / PLOT_MEM / MESSAGE*
 *   · 内存记账          MQ_TRACY_MEM_ADD / _SUB / _SET / _DELTA* / _REPORT
 *   · 分配事件          MQ_TRACY_ALLOC_EVENT / FREE_EVENT（Tracy 的 Memory 视图）
 *   · 锁                MQ_TRACY_LOCKABLE / LOCK_MARK
 *
 * 用法：
 *   #include "core/tracy/mq_tracy.h"            // 模块里只需要这个头
 *   void foo(const char* name) {
 *       MQ_TRACY_ZONE(runtime);                  // 函数级 zone（runtime 颜色）
 *       MQ_TRACY_ZONE_N(script, "eval");         // 命名 zone
 *       MQ_TRACY_ZONE_NAMED(z, script, "call");  // 需要动态文本时命名变量
 *       MQ_TRACY_ZONE_TEXT(z, name);
 *       MQ_TRACY_ZONE_VALUE(z, bytes);
 *       MQ_TRACY_ZONE_AREA_N(world3d, "shadow"); // 按分区（暖/冷）上色
 *       MQ_TRACY_MEM_ZONE_N(texture, "upload");  // 按内存域上色
 *   }
 *
 * 内存域（docs/29 §10）：script / node / texture / gpu / font / heap。每个域只有**一个写入者**
 * （见 docs/29 表格），避免重复计数；`heap` 是可选的全量 C++ 堆（`-DMAQUE_TRACY_ALLOC=ON`）。
 */
#pragma once

#include <cstddef>
#include <cstdint>

/* ---- 编译开关（CMake 的 maque::tracy 目标 PUBLIC 定义；缺省 = 全关）----
 *   MQ_TRACY_ENABLED        总开关（MAQUE_TRACY）
 *   MQ_TRACY_MEMORY_ENABLED 内存记账（MAQUE_TRACY_MEMORY，默认 ON）
 *   MQ_TRACY_ALLOC_ENABLED  全局 new/delete 拦截（MAQUE_TRACY_ALLOC，默认 OFF：每块 +16B）
 *   MQ_TRACY_LOCKS_ENABLED  锁追踪（MAQUE_TRACY_LOCKS，默认 ON）
 * 运行时开关（环境变量）：MAQUE_TRACY / MAQUE_TRACY_MEM / MAQUE_TRACY_MEM_REPORT /
 *   MAQUE_TRACY_MEM_INTERVAL。 */
#if !defined(MQ_TRACY_ENABLED)
#  define MQ_TRACY_ENABLED 0
#endif
#if !defined(MQ_TRACY_MEMORY_ENABLED)
#  define MQ_TRACY_MEMORY_ENABLED (MQ_TRACY_ENABLED)
#endif
#if !defined(MQ_TRACY_ALLOC_ENABLED)
#  define MQ_TRACY_ALLOC_ENABLED 0
#endif
#if !defined(MQ_TRACY_LOCKS_ENABLED)
#  define MQ_TRACY_LOCKS_ENABLED (MQ_TRACY_ENABLED)
#endif
/* 依赖关系：内存记账/分配拦截/锁都要求总开关打开（防止单独 define 出半开状态）。 */
#if MQ_TRACY_MEMORY_ENABLED && !MQ_TRACY_ENABLED
#  undef MQ_TRACY_MEMORY_ENABLED
#  define MQ_TRACY_MEMORY_ENABLED 0
#endif
#if MQ_TRACY_ALLOC_ENABLED && !MQ_TRACY_MEMORY_ENABLED
#  undef MQ_TRACY_ALLOC_ENABLED
#  define MQ_TRACY_ALLOC_ENABLED 0
#endif
#if MQ_TRACY_LOCKS_ENABLED && !MQ_TRACY_ENABLED
#  undef MQ_TRACY_LOCKS_ENABLED
#  define MQ_TRACY_LOCKS_ENABLED 0
#endif

#if MQ_TRACY_ENABLED
#  include <cstring>
#  include <optional>
#  include "tracy/Tracy.hpp"
#endif

namespace maque::tracy {

/* ============================ 分区（冷暖色） ============================
 * 时间线里最粗的一层分类：**暖色 = 玩家能看到的东西（UI / 3D），冷色 = 基础设施
 * （System / Script）**。模块颜色一律从所属分区的色系里取，所以"这一坨是 UI 还是物理"
 * 不用看名字、看色温就能判断。 */
enum class Area : uint32_t {
    ui,       /* 暖：UI/文本/ImGui/输入 */
    world3d,  /* 暖：3D 渲染/动画/物理/导航 */
    system,   /* 冷：平台/运行时/资源/VFS/音频 */
    script,   /* 冷：脚本 VM / 事件总线 */
    count
};

constexpr bool warm(Area a) {
    return a == Area::ui || a == Area::world3d;
}

/* 分区基色（色系锚点）：UI 琥珀 / 3D 珊瑚红（暖）；System 蓝 / Script 绿（冷）。 */
constexpr uint32_t area_color(Area a) {
    switch (a) {
        case Area::ui:      return 0xE8A33D;
        case Area::world3d: return 0xE15759;
        case Area::system:  return 0x4E79A7;
        case Area::script:  return 0x59A14F;
        default:            return 0x9E9E9E;
    }
}

constexpr const char* area_name(Area a) {
    switch (a) {
        case Area::ui:      return "ui";
        case Area::world3d: return "3d";
        case Area::system:  return "system";
        case Area::script:  return "script";
        default:            return "?";
    }
}

/* zone 颜色统一入口：Area 直接用分区基色。 */
constexpr uint32_t color(Area a) { return area_color(a); }

/* ============================ 模块 ============================ */
/* 引擎模块 —— 每个模块一个固定颜色（Tracy zone 色条）。
 * 新增模块：枚举加一项 + `area()` 里归区 + `color()`/`name()` 里各加一项
 * （颜色 0xRRGGBB，必须互不相同；单测会断言唯一性）。 */
enum class Module : uint32_t {
    runtime, script, input, platform, physics, nav, anim, ui, render, text, asset, vfs, imgui, audio, count
};

/* 模块 → 分区：决定冷暖色系（Area 是给"不知道具体模块"的调用方用的粗粒度）。 */
constexpr Area area(Module m) {
    switch (m) {
        case Module::ui:
        case Module::text:
        case Module::imgui:
        case Module::input:    return Area::ui;
        case Module::render:
        case Module::anim:
        case Module::physics:
        case Module::nav:      return Area::world3d;
        case Module::script:   return Area::script;
        default:               return Area::system;
    }
}

constexpr uint32_t color(Module m) {
    switch (m) {
        /* ---- 暖：UI（琥珀/金）---- */
        case Module::ui:       return 0xE8B84B;
        case Module::text:     return 0xD9A441;
        case Module::imgui:    return 0xC98A3C;
        case Module::input:    return 0xF0C96B;
        /* ---- 暖：3D（珊瑚/粉）---- */
        case Module::render:   return 0xE15759;
        case Module::anim:     return 0xFF9DA7;
        case Module::physics:  return 0xC4484A;
        case Module::nav:      return 0xEE8A72;
        /* ---- 冷：System（蓝）---- */
        case Module::runtime:  return 0x4E79A7;
        case Module::platform: return 0x6C8FC4;
        case Module::asset:    return 0x3F6E9B;
        case Module::vfs:      return 0x5BA3B5;
        case Module::audio:    return 0x7FA8D0;
        /* ---- 冷：Script（绿）---- */
        case Module::script:   return 0x59A14F;
        default:               return 0x9E9E9E;
    }
}

constexpr const char* name(Module m) {
    switch (m) {
        case Module::runtime:  return "runtime";
        case Module::script:   return "script";
        case Module::input:    return "input";
        case Module::platform: return "platform";
        case Module::physics:  return "physics";
        case Module::nav:      return "nav";
        case Module::anim:     return "anim";
        case Module::ui:       return "ui";
        case Module::render:   return "render";
        case Module::text:     return "text";
        case Module::asset:    return "asset";
        case Module::vfs:      return "vfs";
        case Module::imgui:    return "imgui";
        case Module::audio:    return "audio";
        default:               return "?";
    }
}

/* ============================ 内存域 ============================ */
/* 每个域只有**一个写入者**（docs/29 §10.2）：
 *   script  ← QuickJS/Lua 的自定义分配器（delta，精确）
 *   node    ← 场景节点树采样（set，估算：实体 + 组件 blob + 树/实例元数据）
 *   texture ← 渲染器纹理字节（set；含字形/ImGui 图集、RenderTexture、env cube）
 *   gpu     ← 渲染器缓冲字节（set；网格顶点/索引、morph 上传）
 *   font    ← 文本模块字形图集（set；CPU 侧 RGBA + 字形表）
 *   heap    ← **可选**全局 C++ 堆（delta；`-DMAQUE_TRACY_ALLOC=ON`）
 * 前 5 个互不重叠（GPU 侧纹理/缓冲分开统计），可以相加；heap 是"除此之外的全部 C++ 堆"，
 * **不要**和明细相加（它包含这些域里的引擎侧对象）。 */
enum class Mem : uint32_t {
    script,   /* 脚本 VM 堆（QuickJS + Lua） */
    node,     /* 场景节点树 */
    texture,  /* GPU 纹理 */
    gpu,      /* GPU 缓冲 */
    font,     /* CPU 字体/字形图集 */
    heap,     /* 全局 C++ 堆（可选） */
    count
};

/* 内存域 → 分区：script=冷绿、node=冷蓝、texture/gpu=暖红、font=暖金。 */
constexpr Area area(Mem m) {
    switch (m) {
        case Mem::font:    return Area::ui;
        case Mem::texture:
        case Mem::gpu:     return Area::world3d;
        case Mem::script:  return Area::script;
        default:           return Area::system;
    }
}

constexpr uint32_t color(Mem m) {
    switch (m) {
        case Mem::script:  return 0x6BC97A;   /* 冷：亮绿 */
        case Mem::node:    return 0x8FA8C8;   /* 冷：钢蓝 */
        case Mem::texture: return 0xE08A6B;   /* 暖：陶土 */
        case Mem::gpu:     return 0xF0625D;   /* 暖：亮红 */
        case Mem::font:    return 0xF5C542;   /* 暖：金 */
        case Mem::heap:    return 0x9AA5B1;   /* 灰蓝：全量堆 */
        default:           return 0x9E9E9E;
    }
}

constexpr const char* name(Mem m) {
    switch (m) {
        case Mem::script:  return "script";
        case Mem::node:    return "node";
        case Mem::texture: return "texture";
        case Mem::gpu:     return "gpu";
        case Mem::font:    return "font";
        case Mem::heap:    return "heap";
        default:           return "?";
    }
}

/* ---- 生命周期（手动启动；幂等；线程安全）----
 * `startup()` 启动 Tracy client（默认监听本机 8086，等待 profiler GUI 连接）；
 * `shutdown()` 停止并冲刷数据（若 MAQUE_TRACY_MEM_REPORT 打开，会先打印一次内存报告）。 */
#if MQ_TRACY_ENABLED
bool started();
bool startup();
void shutdown();
#else
/* MAQUE_TRACY=OFF：头文件级 no-op（否则 runtime 里的直接调用会链接失败）。 */
inline bool started() { return false; }
inline bool startup() { return false; }
inline void shutdown() {}
#endif

/* ---- 帧 / 数据（未启动时 no-op）---- */
enum class PlotType : int { number = 0, memory = 1, percentage = 2 };
#if MQ_TRACY_ENABLED
void frame_mark();                                        // 标记一次"显示帧"结束
void plot(const char* plot_name, double value);           // 曲线点
void plot_config(const char* plot_name, PlotType type, bool step, bool fill, uint32_t plot_color);
void plot_memory(const char* plot_name, uint64_t bytes, uint32_t plot_color = 0x9E9E9E);  // 字节曲线（自动按 memory 格式配置一次）
void message(const char* text);                           // 时间线标记
void message_color(const char* text, uint32_t text_color);
void thread_name(const char* thread);
#else
inline void frame_mark() {}
inline void plot(const char*, double) {}
inline void plot_config(const char*, PlotType, bool, bool, uint32_t) {}
inline void plot_memory(const char*, uint64_t, uint32_t = 0x9E9E9E) {}
inline void message(const char*) {}
inline void message_color(const char*, uint32_t) {}
inline void thread_name(const char*) {}
#endif

/* ============================ 内存记账 ============================ */
struct MemInfo {
    uint64_t bytes = 0;    /* 当前存活字节 */
    uint64_t peak = 0;     /* 历史峰值（set/alloc 时更新） */
    uint64_t allocs = 0;   /* 记账的分配次数（采样型域恒 0） */
    uint64_t frees = 0;    /* 记账的释放次数（采样型域恒 0） */
};

#if MQ_TRACY_MEMORY_ENABLED
constexpr bool memory_available() { return true; }
/* 运行时总开关：启动时读 MAQUE_TRACY_MEM（默认 1；0/off/false 关闭）。
 * 关闭后所有记账函数立即返回（保留已记录值；需要干净基线请调用 memory_reset）。 */
bool memory_enabled();
void memory_set_enabled(bool on);

MemInfo memory_info(Mem m);
uint64_t memory_bytes(Mem m);
uint64_t memory_peak(Mem m);
/* 已分类总量 = script + node + texture + gpu + font（**不含 heap**）。 */
uint64_t memory_total();
/* 分区总量 = 该分区下的内存域之和（heap 不计入）。 */
uint64_t memory_area_total(Area a);

/* 记账：`memory_add/sub` 用于 delta 型分配器；`memory_set` 用于采样型域（绝对覆盖）。
 * 全部线程安全；memory_sub 有下溢保护（不会出现负值）。 */
void memory_add(Mem m, uint64_t bytes);
void memory_sub(Mem m, uint64_t bytes);
void memory_set(Mem m, uint64_t bytes);
void memory_reset();

/* 采样节流：累计 dt，达到间隔（MAQUE_TRACY_MEM_INTERVAL，默认 1s）返回 true。
 * 调用方在 true 时刷新采样型域（节点/GPU/字体），然后调用 memory_publish()。 */
bool memory_due(double dt_seconds);
/* 把当前计数推成 Tracy 曲线（每域 + 分区 + 总量 + heap）；到间隔且 MAQUE_TRACY_MEM_REPORT
 * 打开时还打印一行文本报告。Tracy 未启动时仍可打印文本报告（CI 不用 GUI）。 */
void memory_publish();
/* 立即打印内存表（tag 用于区分 tick/shutdown/manual）。 */
void memory_report(const char* tag);
/* 压成一行状态文本：`script 12.0 KiB | node ... | heap ...`；返回写入字节数。 */
int memory_format(char* out, size_t cap);
/* 压成一行分区文本：`ui 8.5 MiB | 3d 9.2 MiB | system 3.4 MiB | script 12.0 KiB`。 */
int memory_area_format(char* out, size_t cap);

/* 分配事件（Tracy Memory 视图；按指针配对）。显式分配器在 alloc/free 处调用；
 * 全局 new/delete 拦截（MAQUE_TRACY_ALLOC）内部也走这两个。 */
void memory_note_alloc(const void* ptr, size_t bytes, Mem m);
void memory_note_alloc_callstack(const void* ptr, size_t bytes, Mem m, uint32_t depth);
void memory_note_free(const void* ptr, Mem m);
#else
constexpr bool memory_available() { return false; }
inline bool memory_enabled() { return false; }
inline void memory_set_enabled(bool) {}
inline MemInfo memory_info(Mem) { return {}; }
inline uint64_t memory_bytes(Mem) { return 0; }
inline uint64_t memory_peak(Mem) { return 0; }
inline uint64_t memory_total() { return 0; }
inline uint64_t memory_area_total(Area) { return 0; }
inline void memory_add(Mem, uint64_t) {}
inline void memory_sub(Mem, uint64_t) {}
inline void memory_set(Mem, uint64_t) {}
inline void memory_reset() {}
inline bool memory_due(double) { return false; }
inline void memory_publish() {}
inline void memory_report(const char*) {}
inline int memory_format(char* out, size_t cap) { if (out && cap) out[0] = '\0'; return 0; }
inline int memory_area_format(char* out, size_t cap) { if (out && cap) out[0] = '\0'; return 0; }
inline void memory_note_alloc(const void*, size_t, Mem) {}
inline void memory_note_alloc_callstack(const void*, size_t, Mem, uint32_t) {}
inline void memory_note_free(const void*, Mem) {}
#endif

/* RAII zone：颜色 = 模块色 / 分区色 / 内存域色；可选名称、动态文本、数值。 */
class ScopedZone {
public:
#if MQ_TRACY_ENABLED
    ScopedZone(Module m, const char* zone_name, const char* file, uint32_t line, const char* function)
        : ScopedZone(color(m), zone_name, file, line, function) {}
    ScopedZone(Area a, const char* zone_name, const char* file, uint32_t line, const char* function)
        : ScopedZone(color(a), zone_name, file, line, function) {}
    ScopedZone(Mem m, const char* zone_name, const char* file, uint32_t line, const char* function)
        : ScopedZone(color(m), zone_name, file, line, function) {}
    ~ScopedZone() = default;
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
    /* 动态 zone 名（在 zone 内调用一次；如脚本函数名 / 事件名）。 */
    void text(const char* txt) {
        if (m_zone && txt) m_zone->Text(txt, std::strlen(txt));
    }
    /* 数值（Tracy 会在 zone 上显示；内存 delta / 计数用）。 */
    void value(uint64_t v) {
        if (m_zone) m_zone->Value(v);
    }
private:
    /* 先按 uint32 颜色构造；三个 enum 重载都委托到这里。
     * ⚠️ 必须写全局 `::tracy::`：本类在 namespace maque::tracy 内，
     * 裸写 `tracy::ScopedZone` 会解析成 maque::tracy::ScopedZone（自己，未完成类型）。 */
    ScopedZone(uint32_t zone_color, const char* zone_name, const char* file, uint32_t line, const char* function) {
        if (!started()) return;                            // 未启动：不碰 Tracy（避免其全局未初始化）
        const size_t fl = file ? std::strlen(file) : 0;
        const size_t nl = zone_name ? std::strlen(zone_name) : 0;
        const size_t fnn = function ? std::strlen(function) : 0;
        m_zone.emplace(line, file ? file : "", fl, function ? function : "", fnn,
                       zone_name, nl, zone_color, true);
    }
    std::optional<::tracy::ScopedZone> m_zone;
#else
    ScopedZone(Module, const char*, const char*, uint32_t, const char*) {}
    ScopedZone(Area, const char*, const char*, uint32_t, const char*) {}
    ScopedZone(Mem, const char*, const char*, uint32_t, const char*) {}
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
    void text(const char*) {}
    void value(uint64_t) {}
#endif
};

/* 内存增量 zone：构造时记下该域当前字节，析构时把（now-before）写进 zone value。
 * 用途：看一次加载/一次图集增长到底分配了多少（无需人工算差值）。 */
class MemDeltaScope {
public:
    MemDeltaScope(Mem m, const char* zone_name, const char* file, uint32_t line, const char* function)
        : m_mem(m), m_before(memory_bytes(m)), m_zone(m, zone_name, file, line, function) {}
    ~MemDeltaScope() {
        const uint64_t now = memory_bytes(m_mem);
        m_zone.value(now >= m_before ? now - m_before : 0);
    }
    MemDeltaScope(const MemDeltaScope&) = delete;
    MemDeltaScope& operator=(const MemDeltaScope&) = delete;
private:
    Mem m_mem;
    uint64_t m_before;
    ScopedZone m_zone;
};

#if MQ_TRACY_LOCKS_ENABLED && MQ_TRACY_ENABLED
/* 锁包装（用于 MQ_TRACY_LOCKABLE）：Tracy 的 `tracy::Lockable` 在**构造时**就要向 profiler
 * 发 LockAnnounce，client 未启动时构造会断言 s_profilerData（普通运行必崩）。
 * 这里**懒创建**：第一次加锁时若 client 已启动才建 Tracy 跟踪对象，否则永远走普通锁。
 * 于是锁对象可以安全地存在于任何全局/成员对象里（启动前、关闭后都不崩）。 */
template <class T>
class Lockable {
public:
    explicit Lockable(const ::tracy::SourceLocationData* srcloc) : m_srcloc(srcloc) {}
    Lockable(const Lockable&) = delete;
    Lockable& operator=(const Lockable&) = delete;
    void lock() {
        ensure();
        if (m_traced && started()) m_traced->lock(); else m_impl.lock();
    }
    void unlock() {
        if (m_traced && started()) m_traced->unlock(); else m_impl.unlock();
    }
    bool try_lock() {
        ensure();
        return (m_traced && started()) ? m_traced->try_lock() : m_impl.try_lock();
    }
    /* 标记"持锁点"（Tracy 的 LockMark）：未启动/已关闭/未创建跟踪对象时 no-op。 */
    void mark() {
        if (m_traced && started()) m_traced->Mark(m_srcloc);
    }
private:
    void ensure() {
        if (!m_traced && started()) m_traced.emplace(m_srcloc);
    }
    T m_impl{};
    const ::tracy::SourceLocationData* m_srcloc = nullptr;
    std::optional<::tracy::Lockable<T>> m_traced;
};
#endif

} // namespace maque::tracy

/* ---- 宏（调用方不接触 Tracy）---- */
#define MQ_TRACY_CONCAT_INNER(a, b) a##b
#define MQ_TRACY_CONCAT(a, b) MQ_TRACY_CONCAT_INNER(a, b)

/* 时间轴 zone ---------------------------------------------------------------- */
#if MQ_TRACY_ENABLED
#  define MQ_TRACY_ZONE_VAR(var, mod, zname) \
       ::maque::tracy::ScopedZone var(::maque::tracy::Module::mod, (zname), __FILE__, (uint32_t)__LINE__, __func__)
/* 函数级 zone（颜色 = 模块色） */
#  define MQ_TRACY_ZONE(mod) \
       MQ_TRACY_ZONE_VAR(MQ_TRACY_CONCAT(__mq_tracy_zone_, __LINE__), mod, nullptr)
/* 命名 zone（静态字符串） */
#  define MQ_TRACY_ZONE_N(mod, zname) \
       MQ_TRACY_ZONE_VAR(MQ_TRACY_CONCAT(__mq_tracy_zone_, __LINE__), mod, (zname))
/* 命名变量（需要后续 MQ_TRACY_ZONE_TEXT 动态改名的场景） */
#  define MQ_TRACY_ZONE_NAMED(var, mod, zname) MQ_TRACY_ZONE_VAR(var, mod, (zname))
#  define MQ_TRACY_ZONE_TEXT(var, txt) (var).text(txt)
#  define MQ_TRACY_ZONE_VALUE(var, val) (var).value((uint64_t)(val))
/* 分区色 zone（暖：UI/3D；冷：System/Script） */
#  define MQ_TRACY_ZONE_AREA_VAR(var, ar, zname) \
       ::maque::tracy::ScopedZone var(::maque::tracy::Area::ar, (zname), __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_ZONE_AREA(ar) \
       MQ_TRACY_ZONE_AREA_VAR(MQ_TRACY_CONCAT(__mq_tracy_area_, __LINE__), ar, nullptr)
#  define MQ_TRACY_ZONE_AREA_N(ar, zname) \
       MQ_TRACY_ZONE_AREA_VAR(MQ_TRACY_CONCAT(__mq_tracy_area_, __LINE__), ar, (zname))
#  define MQ_TRACY_ZONE_AREA_NAMED(var, ar, zname) MQ_TRACY_ZONE_AREA_VAR(var, ar, (zname))
#  define MQ_TRACY_FRAME() ::maque::tracy::frame_mark()
#  define MQ_TRACY_PLOT(pname, value) ::maque::tracy::plot((pname), (double)(value))
#  define MQ_TRACY_PLOT_CONFIG(pname, ptype, pstep, pfill, pcolor) \
       ::maque::tracy::plot_config((pname), (::maque::tracy::PlotType)(ptype), (pstep), (pfill), (pcolor))
#  define MQ_TRACY_PLOT_MEM(pname, bytes) ::maque::tracy::plot_memory((pname), (uint64_t)(bytes))
#  define MQ_TRACY_PLOT_MEM_C(pname, bytes, pcolor) \
       ::maque::tracy::plot_memory((pname), (uint64_t)(bytes), (pcolor))
#  define MQ_TRACY_MESSAGE(txt) ::maque::tracy::message(txt)
#  define MQ_TRACY_MESSAGE_COLOR(txt, tcolor) ::maque::tracy::message_color((txt), (tcolor))
#else
/* 关闭时全部 no-op（Named 版本仍是空对象，保证调用处语法不变、可被编译器优化掉） */
#  define MQ_TRACY_ZONE(mod) ((void)0)
#  define MQ_TRACY_ZONE_N(mod, zname) ((void)0)
#  define MQ_TRACY_ZONE_NAMED(var, mod, zname) \
       ::maque::tracy::ScopedZone var(::maque::tracy::Module::mod, (zname), __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_ZONE_TEXT(var, txt) ((void)0)
#  define MQ_TRACY_ZONE_VALUE(var, val) ((void)0)
#  define MQ_TRACY_ZONE_AREA(ar) ((void)0)
#  define MQ_TRACY_ZONE_AREA_N(ar, zname) ((void)0)
#  define MQ_TRACY_ZONE_AREA_NAMED(var, ar, zname) \
       ::maque::tracy::ScopedZone var(::maque::tracy::Area::ar, (zname), __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_FRAME() ((void)0)
#  define MQ_TRACY_PLOT(pname, value) ((void)0)
#  define MQ_TRACY_PLOT_CONFIG(pname, ptype, pstep, pfill, pcolor) ((void)0)
#  define MQ_TRACY_PLOT_MEM(pname, bytes) ((void)0)
#  define MQ_TRACY_PLOT_MEM_C(pname, bytes, pcolor) ((void)0)
#  define MQ_TRACY_MESSAGE(txt) ((void)0)
#  define MQ_TRACY_MESSAGE_COLOR(txt, tcolor) ((void)0)
#endif

/* 内存记账 ------------------------------------------------------------------ */
#if MQ_TRACY_MEMORY_ENABLED
#  define MQ_TRACY_MEM_ADD(mem, n) ::maque::tracy::memory_add(::maque::tracy::Mem::mem, (uint64_t)(n))
#  define MQ_TRACY_MEM_SUB(mem, n) ::maque::tracy::memory_sub(::maque::tracy::Mem::mem, (uint64_t)(n))
#  define MQ_TRACY_MEM_SET(mem, n) ::maque::tracy::memory_set(::maque::tracy::Mem::mem, (uint64_t)(n))
#  define MQ_TRACY_MEM_ZONE(mem) \
       ::maque::tracy::ScopedZone MQ_TRACY_CONCAT(__mq_tracy_mem_, __LINE__)( \
           ::maque::tracy::Mem::mem, nullptr, __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_MEM_ZONE_N(mem, zname) \
       ::maque::tracy::ScopedZone MQ_TRACY_CONCAT(__mq_tracy_mem_, __LINE__)( \
           ::maque::tracy::Mem::mem, (zname), __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_MEM_DELTA(mem) \
       ::maque::tracy::MemDeltaScope MQ_TRACY_CONCAT(__mq_tracy_memd_, __LINE__)( \
           ::maque::tracy::Mem::mem, nullptr, __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_MEM_DELTA_N(mem, zname) \
       ::maque::tracy::MemDeltaScope MQ_TRACY_CONCAT(__mq_tracy_memd_, __LINE__)( \
           ::maque::tracy::Mem::mem, (zname), __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_MEM_REPORT() ::maque::tracy::memory_report("manual")
#  define MQ_TRACY_ALLOC_EVENT(ptr, size, mem) \
       ::maque::tracy::memory_note_alloc((ptr), (size_t)(size), ::maque::tracy::Mem::mem)
#  define MQ_TRACY_FREE_EVENT(ptr, mem) \
       ::maque::tracy::memory_note_free((ptr), ::maque::tracy::Mem::mem)
#  define MQ_TRACY_ALLOC_CALLSTACK(ptr, size, mem, depth) \
       ::maque::tracy::memory_note_alloc_callstack((ptr), (size_t)(size), ::maque::tracy::Mem::mem, (uint32_t)(depth))
#else
#  define MQ_TRACY_MEM_ADD(mem, n) ((void)0)
#  define MQ_TRACY_MEM_SUB(mem, n) ((void)0)
#  define MQ_TRACY_MEM_SET(mem, n) ((void)0)
#  define MQ_TRACY_MEM_ZONE(mem) ((void)0)
#  define MQ_TRACY_MEM_ZONE_N(mem, zname) ((void)0)
#  define MQ_TRACY_MEM_DELTA(mem) ((void)0)
#  define MQ_TRACY_MEM_DELTA_N(mem, zname) ((void)0)
#  define MQ_TRACY_MEM_REPORT() ((void)0)
#  define MQ_TRACY_ALLOC_EVENT(ptr, size, mem) ((void)0)
#  define MQ_TRACY_FREE_EVENT(ptr, mem) ((void)0)
#  define MQ_TRACY_ALLOC_CALLSTACK(ptr, size, mem, depth) ((void)0)
#endif

/* 锁 ------------------------------------------------------------------------ */
/* 用法：struct Foo { MQ_TRACY_LOCKABLE(std::mutex, mu); ... };
 *      持锁处 MQ_TRACY_LOCK_MARK(mu)（标记该锁当前持有；未启动 Tracy 时 no-op）。
 * 未开启锁追踪时退化为普通成员（零行为变化）；开启时是 maque::tracy::Lockable
 *（懒创建 Tracy 跟踪对象，启动前的构造/加锁不会断言）。 */
#if MQ_TRACY_LOCKS_ENABLED && MQ_TRACY_ENABLED
#  define MQ_TRACY_LOCKABLE(type, var) \
       ::maque::tracy::Lockable<type> var{ []() -> const ::tracy::SourceLocationData* { \
           static constexpr ::tracy::SourceLocationData srcloc{ nullptr, #type " " #var, TracyFile, TracyLine, 0 }; \
           return &srcloc; }() }
#  define MQ_TRACY_LOCKABLE_N(type, var, desc) \
       ::maque::tracy::Lockable<type> var{ []() -> const ::tracy::SourceLocationData* { \
           static constexpr ::tracy::SourceLocationData srcloc{ nullptr, desc, TracyFile, TracyLine, 0 }; \
           return &srcloc; }() }
#  define MQ_TRACY_LOCK_MARK(var) ((var).mark())
#else
#  define MQ_TRACY_LOCKABLE(type, var) type var
#  define MQ_TRACY_LOCKABLE_N(type, var, desc) type var
#  define MQ_TRACY_LOCK_MARK(var) ((void)0)
#endif
