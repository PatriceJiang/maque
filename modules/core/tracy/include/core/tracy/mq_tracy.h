/* maque::tracy —— Tracy 性能分析的**统一封装**（docs/29）。
 *
 * 为什么先封装再埋点：
 *   ① 各模块不直接 include 第三方 tracy 头 —— 命名/颜色/开关只有一处；
 *   ② 本引擎按**模块**给 zone 上色（`Module` 枚举 → 固定颜色）：在 Tracy 时间线里
 *      一眼看出耗时落在 runtime / script / render / physics / ... 哪个模块；
 *   ③ 构建开关 `-DMAQUE_TRACY=OFF` 时所有宏展开为 no-op（无第三方头、无链接依赖）；
 *   ④ Tracy 客户端**手动启动**（F3 / `MAQUE_TRACY=1`）：未启动时 zone 立即返回，
 *      普通运行/CI 不监听端口、不额外占线程。
 *
 * 用法：
 *   #include "core/tracy/mq_tracy.h"            // 模块里只需要这个头
 *   void foo(const char* name) {
 *       MQ_TRACY_ZONE(runtime);                  // 函数级 zone（runtime 颜色）
 *       MQ_TRACY_ZONE_N(script, "eval");         // 命名 zone
 *       MQ_TRACY_ZONE_NAMED(z, script, "call");  // 需要动态文本时命名变量
 *       MQ_TRACY_ZONE_TEXT(z, name);
 *   }
 * 数据 / 一次性标记：
 *   MQ_TRACY_FRAME();  MQ_TRACY_PLOT("fps", v);  MQ_TRACY_MESSAGE("加载完成");
 */
#pragma once

#include <cstdint>

/* CMake（`maque::tracy` 目标）在 ON 时 PUBLIC 定义 MQ_TRACY_ENABLED=1；缺省 = 关闭。 */
#if !defined(MQ_TRACY_ENABLED)
#  define MQ_TRACY_ENABLED 0
#endif

#if MQ_TRACY_ENABLED
#  include <cstring>
#  include <optional>
#  include "tracy/Tracy.hpp"
#endif

namespace maque::tracy {

/* 引擎模块 —— 每个模块一个固定颜色（Tracy zone 色条）。
 * 新增模块：枚举加一项 + `color()` 里给颜色（0xRRGGBB）。 */
enum class Module : uint32_t {
    runtime, script, input, platform, physics, nav, anim, ui, render, text, asset, vfs, imgui, audio, count
};

constexpr uint32_t color(Module m) {
    switch (m) {
        case Module::runtime:  return 0x4E79A7;   // 蓝
        case Module::script:   return 0x59A14F;   // 绿
        case Module::input:    return 0xF28E2B;   // 橙
        case Module::platform: return 0xB07AA1;   // 紫
        case Module::physics:  return 0xE15759;   // 红
        case Module::nav:      return 0x76B7B2;   // 青
        case Module::anim:     return 0xFF9DA7;   // 粉
        case Module::ui:       return 0xEDC948;   // 黄
        case Module::render:   return 0x9C755F;   // 棕
        case Module::text:     return 0xBAB0AC;   // 灰
        case Module::asset:    return 0x8CD17D;   // 浅绿
        case Module::vfs:      return 0x499894;   // 深青
        case Module::imgui:    return 0xD372AD;   // 品红
        case Module::audio:    return 0x86BCB6;   // 蓝绿
        default:               return 0x9E9E9E;   // 未知
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

/* ---- 生命周期（手动启动；幂等；线程安全）----
 * `startup()` 启动 Tracy client（默认监听本机 8086，等待 profiler GUI 连接）；
 * `shutdown()` 停止并冲刷数据。普通游戏运行不调用 → 零开销。 */
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
void message(const char* text);                           // 时间线标记
void message_color(const char* text, uint32_t text_color);
void thread_name(const char* thread);
#else
inline void frame_mark() {}
inline void plot(const char*, double) {}
inline void plot_config(const char*, PlotType, bool, bool, uint32_t) {}
inline void message(const char*) {}
inline void message_color(const char*, uint32_t) {}
inline void thread_name(const char*) {}
#endif

/* RAII zone：模块颜色 + 可选名称（函数名 / 动态文本见 `text()`）。 */
class ScopedZone {
public:
#if MQ_TRACY_ENABLED
    ScopedZone(Module m, const char* zone_name, const char* file, uint32_t line, const char* function) {
        if (!started()) return;                            // 未启动：不碰 Tracy（避免其全局未初始化）
        const size_t fl = file ? std::strlen(file) : 0;
        const size_t nl = zone_name ? std::strlen(zone_name) : 0;
        const size_t fnn = function ? std::strlen(function) : 0;
        m_zone.emplace(line, file ? file : "", fl, function ? function : "", fnn,
                       zone_name, nl, color(m), true);
    }
    ~ScopedZone() = default;
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
    /* 动态 zone 名（在 zone 内调用一次；如脚本函数名 / 事件名）。 */
    void text(const char* txt) {
        if (m_zone && txt) m_zone->Text(txt, std::strlen(txt));
    }
private:
    /* ⚠️ 必须写全局 `::tracy::`：本类在 namespace maque::tracy 内，
     * 裸写 `tracy::ScopedZone` 会解析成 maque::tracy::ScopedZone（自己，未完成类型）。 */
    std::optional<::tracy::ScopedZone> m_zone;
#else
    ScopedZone(Module, const char*, const char*, uint32_t, const char*) {}
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
    void text(const char*) {}
#endif
};

} // namespace maque::tracy

/* ---- 宏（调用方不接触 Tracy）---- */
#define MQ_TRACY_CONCAT_INNER(a, b) a##b
#define MQ_TRACY_CONCAT(a, b) MQ_TRACY_CONCAT_INNER(a, b)

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
#  define MQ_TRACY_FRAME() ::maque::tracy::frame_mark()
#  define MQ_TRACY_PLOT(pname, value) ::maque::tracy::plot((pname), (double)(value))
#  define MQ_TRACY_PLOT_CONFIG(pname, ptype, pstep, pfill, pcolor) \
       ::maque::tracy::plot_config((pname), (::maque::tracy::PlotType)(ptype), (pstep), (pfill), (pcolor))
#  define MQ_TRACY_MESSAGE(txt) ::maque::tracy::message(txt)
#  define MQ_TRACY_MESSAGE_COLOR(txt, tcolor) ::maque::tracy::message_color((txt), (tcolor))
#else
/* 关闭时全部 no-op（Named 版本仍是空对象，保证调用处语法不变、可被编译器优化掉） */
#  define MQ_TRACY_ZONE(mod) ((void)0)
#  define MQ_TRACY_ZONE_N(mod, zname) ((void)0)
#  define MQ_TRACY_ZONE_NAMED(var, mod, zname) \
       ::maque::tracy::ScopedZone var(::maque::tracy::Module::mod, (zname), __FILE__, (uint32_t)__LINE__, __func__)
#  define MQ_TRACY_ZONE_TEXT(var, txt) ((void)0)
#  define MQ_TRACY_FRAME() ((void)0)
#  define MQ_TRACY_PLOT(pname, value) ((void)0)
#  define MQ_TRACY_PLOT_CONFIG(pname, ptype, pstep, pfill, pcolor) ((void)0)
#  define MQ_TRACY_MESSAGE(txt) ((void)0)
#  define MQ_TRACY_MESSAGE_COLOR(txt, tcolor) ((void)0)
#endif
