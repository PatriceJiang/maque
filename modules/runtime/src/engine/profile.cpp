/* 诊断 · Linux 采样剖析（perf，docs/35）：环境启动 / F5 开关 / 控制面支撑 / 退出兜底
 *
 * 实现边界：
 *   · 真正的 perf 子进程与报告解析在 maque::profile（modules/core/profile）；
 *   · 这里只负责**引擎策略**：什么时候录、产物放哪、把场景/帧号写进 meta、更新 obs 指标；
 *   · 不在帧内做 IO（start 只在首帧/控制面/F5 发生一次；stop 可能同步跑 perf report，
 *     只在到达 MAQUE_PROFILE_FRAMES 或退出时发生，属于可接受的显式操作）。
 */
#include "../runtime_engine.h"

#include <cerrno>

namespace maque::rt {
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

uint64_t env_u64(const char* name, uint64_t def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    char* end = nullptr;
    errno = 0;
    const unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v || errno != 0) return def;
    return (uint64_t)n;
}

std::string env_str(const char* name, const std::string& def) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return def;
}

bool valid_callgraph(const std::string& v) {
    return v == "fp" || v == "dwarf" || v == "none" || v == "lbr";
}

std::string default_profile_dir() {
    /* `.maque/` 在 .gitignore 里；每次运行独立目录，避免覆盖上一次基线。 */
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return ".maque/profile/run-" + std::to_string(us);
}

maque::profile::Options options_from_env(const std::string& label) {
    maque::profile::Options o;
    o.label = label;
    o.output_dir = env_str("MAQUE_PROFILE_DIR", "");
    o.frequency = (int)env_u64("MAQUE_PROFILE_FREQ", 999);
    o.callgraph = env_str("MAQUE_PROFILE_CALLGRAPH", "fp");
    if (!valid_callgraph(o.callgraph)) o.callgraph = "fp";
    o.exclude_kernel = env_u64("MAQUE_PROFILE_KERNEL", 1) == 0;
    return o;
}

std::string scene_label(const Engine& e) {
    if (!e.scn_vfs_path.empty()) {
        const size_t slash = e.scn_vfs_path.find_last_of('/');
        const std::string base = slash == std::string::npos ? e.scn_vfs_path : e.scn_vfs_path.substr(slash + 1);
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && dot > 0) return base.substr(0, dot);
    }
    return "run";
}

}  // namespace

std::string Engine::profile_context_json() const {
    std::string o = "{";
    o += "\"engine\":\"maque\"";
    o += ",\"scene\":\"" + json_escape(scn_full_path.empty() ? scn_vfs_path : scn_full_path) + "\"";
    o += ",\"scene_vfs\":\"" + json_escape(scn_vfs_path) + "\"";
    o += ",\"frame\":" + std::to_string((unsigned long long)frame);
    o += ",\"window\":" + std::string(window ? "true" : "false");
#ifdef NDEBUG
    o += ",\"build\":\"release\"";
#else
    o += ",\"build\":\"debug\"";
#endif
    o += ",\"present\":\"" + json_escape(env_str("MAQUE_PRESENT", "auto")) + "\"";
    o += ",\"render_scale\":\"" + json_escape(env_str("MAQUE_RENDER_SCALE", "1")) + "\"";
    o += "}";
    return o;
}

bool Engine::profile_start(const maque::profile::Options& in, std::string* error) {
    if (!maque::profile::supported()) {
        const std::string msg = maque::profile::support_reason();
        if (error) *error = msg;
        std::print(stderr, "runtime: [profile] 不可用：{}\n", msg);
        return false;
    }
    maque::profile::Options o = in;
    if (o.output_dir.empty()) {
        if (const char* d = std::getenv("MAQUE_PROFILE_DIR"); d && *d) o.output_dir = d;
        else o.output_dir = default_profile_dir();
    }
    if (o.label.empty()) o.label = scene_label(*this);
    if (o.frequency <= 0) o.frequency = 999;
    if (!valid_callgraph(o.callgraph)) o.callgraph = "fp";
    if (const char* pe = std::getenv("MAQUE_PROFILE_PERF"); pe && *pe) o.perf_exe = pe;
    if (o.context_json.empty()) o.context_json = profile_context_json();
    o.start_frame = frame;

    std::string err;
    if (!maque::profile::start(o, &err)) {
        if (error) *error = err;
        std::print(stderr, "runtime: [profile] perf 启动失败：{}\n", err);
        return false;
    }
    profile_armed = true;
    profile_auto_started = true;
    mq_obs_gauge_set(obs, "profile.recording", 1.0);
    mq_obs_gauge_set(obs, "profile.samples", 0.0);
    mq_obs_gauge_set(obs, "profile.hotspots", 0.0);
    mq_obs_gauge_set(obs, "profile.elapsed_ms", 0.0);
    const auto st = maque::profile::status();
    std::print("runtime: [profile] perf recording → '{}' (pid={}, freq={}, callgraph={})\n",
               st.output_dir, st.pid, o.frequency, o.callgraph);
    std::print("runtime: [profile] 查看/停止：控制面 Perf.profile_status / profile_stop；"
               "F5 开关；产物在 {}\n", st.output_dir);
    return true;
}

bool Engine::profile_stop(bool report, std::string* error) {
    profile_stop_frame = 0;
    std::string err;
    const bool ok = maque::profile::stop(report, &err);
    const auto st = maque::profile::status();
    mq_obs_gauge_set(obs, "profile.recording", 0.0);
    mq_obs_gauge_set(obs, "profile.samples", (double)st.samples);
    mq_obs_gauge_set(obs, "profile.hotspots", (double)maque::profile::hotspots().size());
    mq_obs_gauge_set(obs, "profile.elapsed_ms", st.elapsed_ms);
    if (!ok) {
        if (error) *error = err.empty() ? st.reason : err;
        std::print(stderr, "runtime: [profile] 停止失败：{}\n", err.empty() ? st.reason : err);
        return false;
    }
    if (report) {
        if (st.report_ok) {
            std::print("runtime: [profile] stopped: {} samples, {} hotspots → '{}'\n",
                       (unsigned long long)st.samples, (unsigned long long)maque::profile::hotspots().size(),
                       st.hotspots_path);
        } else {
            const std::string why = err.empty() ? st.reason : err;
            std::print(stderr, "runtime: [profile] 已停止，但报告生成失败：{}\n", why);
            if (error) *error = why;
        }
    } else {
        std::print("runtime: [profile] stopped (report skipped) → '{}'\n", st.data_path);
    }
    return true;
}

void Engine::profile_toggle() {
    if (maque::profile::status().state == maque::profile::State::recording) {
        std::string err;
        profile_stop(true, &err);
        return;
    }
    maque::profile::Options o = options_from_env(scene_label(*this));
    std::string err;
    if (profile_start(o, &err)) {
        const uint64_t frames = env_u64("MAQUE_PROFILE_FRAMES", 0);
        if (frames > 0) profile_stop_frame = frame + frames;
    }
}

void Engine::profile_tick() {
    if (!profile_env_checked) {
        profile_env_checked = true;
        const char* e = std::getenv("MAQUE_PROFILE");
        if (!e || !*e || std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0) return;
        if (!maque::profile::supported()) {
            std::print(stderr, "runtime: [profile] MAQUE_PROFILE 已设置但本构建不可用：{}\n",
                       maque::profile::support_reason());
            return;
        }
        profile_armed = true;
        profile_auto_start_frame = env_u64("MAQUE_PROFILE_START", 0);
    }
    if (!profile_armed) return;

    /* 环境自动启动：只尝试一次（失败后由 F5/控制面重试；不每帧刷错误）。 */
    if (!profile_auto_started && frame >= profile_auto_start_frame) {
        profile_auto_started = true;
        maque::profile::Options o = options_from_env(scene_label(*this));
        std::string err;
        if (profile_start(o, &err)) {
            const uint64_t frames = env_u64("MAQUE_PROFILE_FRAMES", 0);
            if (frames > 0) profile_stop_frame = frame + frames;
            std::print("runtime: [profile] auto profile started (frame {}, {} frames)\n",
                       (unsigned long long)frame, (unsigned long long)frames);
        }
    }
    if (profile_stop_frame > 0 && frame >= profile_stop_frame && maque::profile::recording()) {
        std::string err;
        profile_stop(true, &err);
        std::print("runtime: [profile] auto stop at frame {}\n", (unsigned long long)frame);
    }
}

void Engine::profile_shutdown() {
    if (maque::profile::status().state != maque::profile::State::recording) return;
    std::string err;
    profile_stop(true, &err);
}

}  // namespace maque::rt
