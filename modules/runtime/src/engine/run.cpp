/* 运行时控制（docs/10）：回放准备、检查点、崩溃上下文、终局导出、主循环
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* 载入回放日志（MAQUE_REPLAY）：此后所有步进都只消费记录（设备输入被忽略）。 */
bool Engine::prepare_replay() {
    const char* rp = std::getenv("MAQUE_REPLAY");
    if (!rp || !input) return false;
    std::ifstream f(rp, std::ios::binary);
    if (!f) { std::print("runtime: cannot open replay log '{}'\n", rp); return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty() || mq_input_log_load_json(input, text.c_str()) != 0) {
        std::print("runtime: cannot load replay log '{}'\n", rp);
        return false;
    }
    const char* mf = mq_input_log_get_meta(input, "frames");
    std::print("runtime: replay '{}' ({} frames expected)\n", rp, mf ? mf : "?");
    return true;
}

/* 控制面 Runtime.step：确定性推进 n 帧（AI/CI 驱动同一帧序）。 */
bool Engine::run_frames(uint64_t n) {
    for (uint64_t i = 0; i < n && !quit; ++i) {
#ifdef MQ_RENDERER_STATIC_NULL
        step_frame(false);
#else
        step_frame(true);
#endif
    }
    return !quit;
}

/* 检查点快照（docs/10 §5 分歧定位）：环状保存最近 N 个（默认每 30 帧），
 * 回放失步时可用 Replay.seek 跳回任一检查点做二分/逐步定位。 */
void Engine::checkpoint_push() {
    if (checkpoints.empty()) {
        checkpoints.reserve(cp_count);
        for (uint32_t i = 0; i < cp_count; ++i) checkpoints.push_back(mq_snapshot_create());
    }
    mq_snapshot* s = checkpoints[cp_cursor % checkpoints.size()];
    cp_cursor++;
    if (!world_snapshot(s)) return;
    mq_snapshot_set_frame(s, frame);
    mq_snapshot_set_desc(s, ("checkpoint@" + std::to_string(frame)).c_str());
}

/* 崩溃/异常上下文（docs/09 §5）：把"精确重放所需的一切"落盘为 JSON —— 
 * seed、场景、帧号、最近检查点、输入日志（若在录制）、世界校验和、指标。
 * 触发点：① 脚本异常（可重放定位）；② 信号（abort/段错误，见 install_crash_handler）。 */
/* 把崩溃上下文预渲染进信号安全缓冲并确保报告 fd 已打开（每帧边界调用）。 */
void Engine::refresh_crash_buffer() {
    if (g_crash_fd < 0) {
        const char* out = std::getenv("MAQUE_CRASH_REPORT");
        if (!out || !*out) return;
        g_crash_fd = maque::compat::open_fd(out, maque::compat::kOpenWriteCreate);
        if (g_crash_fd < 0) return;
    }
    uint64_t cp_frame = 0, cp_sum = 0;
    for (auto* s : checkpoints)
        if (s && mq_snapshot_frame(s) > cp_frame) { cp_frame = mq_snapshot_frame(s); cp_sum = mq_snapshot_checksum(s); }
    const int n = std::snprintf(g_crash_buf, sizeof g_crash_buf,
        "{\"reason\":\"fatal signal\",\"scene\":\"%s\",\"frame\":%llu,\"seed\":%u,"
        "\"world_trail\":\"%llu\",\"checkpoint\":{\"frame\":%llu,\"checksum\":\"%llu\"},\n"
        " \"replay\":{\"command\":\"MAQUE_REPLAY=<log> %s\"},\n \"metrics\":%s}\n",
        (scn_full_path.empty() ? scn_vfs_path : scn_full_path).c_str(),
        (unsigned long long)frame, scene ? scene->desc.seed : 0u,
        (unsigned long long)world_trail(),
        (unsigned long long)cp_frame, (unsigned long long)cp_sum,
        (scn_full_path.empty() ? scn_vfs_path : scn_full_path).c_str(),
        mq_obs_dump_json(obs));
    g_crash_len = n > 0 ? (n < (int)sizeof g_crash_buf ? n : (int)sizeof g_crash_buf - 1) : 0;
    if (g_crash_fd >= 0 && g_crash_len > 0) {          // 同步落盘（保证即使不崩也有当前快照）
        const maque::compat::ssize_t ig =
            maque::compat::pwrite_fd(g_crash_fd, g_crash_buf, (size_t)g_crash_len, 0);
        (void)ig;
    }
}

void Engine::write_crash_context(const char* why) {
    const char* out = std::getenv("MAQUE_CRASH_REPORT");
    if (!out || !*out) return;
    std::ofstream f(out, std::ios::binary);
    if (!f) return;
    uint64_t trail = world_trail();
    /* 最近检查点 */
    uint64_t cp_frame = 0, cp_sum = 0;
    for (auto* s : checkpoints)
        if (s && mq_snapshot_frame(s) > cp_frame) { cp_frame = mq_snapshot_frame(s); cp_sum = mq_snapshot_checksum(s); }
    std::string json = "{\n";
    json += "  \"reason\": ";
    write_json_string_pub(json, why ? why : "unknown");
    json += ",\n  \"scene\": ";
    write_json_string_pub(json, scn_full_path.empty() ? scn_vfs_path : scn_full_path);
    json += ",\n  \"frame\": " + std::to_string(frame);
    json += ",\n  \"seed\": " + std::to_string(scene ? scene->desc.seed : 0);
    json += ",\n  \"world_trail\": \"" + std::to_string(trail) + "\"";
    json += ",\n  \"last_checksum\": \"" + std::to_string(last_checksum) + "\"";
    json += ",\n  \"checkpoint\": {\"frame\": " + std::to_string(cp_frame) +
            ", \"checksum\": \"" + std::to_string(cp_sum) + "\"}";
    json += ",\n  \"replay\": {\"command\": \"MAQUE_REPLAY=<log> ";
    json += scn_vfs_path + "\", \"recorded\": ";
    json += (input && mq_input_recording(input)) ? "true" : "false";
    json += "}";
    json += ",\n  \"metrics\": " + std::string(mq_obs_dump_json(obs)) + "\n}\n";
    f << json;
    std::print(stderr, "runtime: crash context → '{}' (frame={} reason={})\n", out, frame, why ? why : "?");
}

/* 导出世界快照（崩溃复现/AI undo 的落点，docs/10 §7）。控制面每次 step 后也会调用。 */
void Engine::export_final_snapshot() {
    const char* sp = std::getenv("MAQUE_SNAPSHOT");
    if (!sp || !snaps) return;
    if (!snap_last) snap_last = mq_snapshot_pool_acquire(snaps);
    if (!world_snapshot(snap_last)) return;
    size_t n = 0;
    const void* b = mq_snapshot_bytes(snap_last, &n);
    std::ofstream f(sp, std::ios::binary);
    if (f && b && n) {
        mq_snapshot_set_frame(snap_last, frame);
        f.write((const char*)b, (std::streamsize)n);
        std::print("runtime: snapshot → '{}' ({} bytes, frame {})\n", sp, n, frame);
    }
}

/* 终局导出：快照 + 录制日志（frames/trail 元数据 → 回放失步检测的基准）。 */
void Engine::finalize_run() {
    if (const char* td = std::getenv("MAQUE_TRAIL_OUT"); td && *td) {   // 失步定位：逐帧校验和（docs/10 §5）
        std::ofstream f(td, std::ios::binary);
        for (auto& [fr, sum] : trail) f << fr << " " << sum << "\n";
    }
    if (snaps) { snap_last = mq_snapshot_pool_acquire(snaps); world_snapshot(snap_last); }
    export_final_snapshot();
    if (input && mq_input_recording(input)) {
        mq_input_log_set_meta(input, "frames", std::to_string(frame).c_str());
        char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)world_trail());
        mq_input_log_set_meta(input, "trail", tb);
        if (const char* rp = std::getenv("MAQUE_RECORD")) {
            std::ofstream f(rp, std::ios::binary);
            const char* json = mq_input_log_json(input);
            if (f && json) f << json;
            std::print("runtime: recorded input log → '{}' ({} frames)\n", rp, frame);
        }
        mq_input_record_end(input);
    }
}

void Engine::main_loop(const std::string& frames_dir, const std::string& name) {
    this->frames_dir = frames_dir;
    this->name = name;
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);
    uint64_t cap = 60 * 60;
    if (const char* mf = std::getenv("MAQUE_MAX_FRAMES")) { long v = std::strtol(mf, nullptr, 10); if (v > 0) cap = (uint64_t)v; }
    if (const char* sf = std::getenv("MAQUE_STOP_FRAME")) { long v = std::strtol(sf, nullptr, 10); if (v > 0) { cap = (uint64_t)v + 1; stop_frame = (uint64_t)v; } }

    /* 回放（docs/10 §3/§5）：输入是数据 → 同一日志应逐帧复现 */
    std::string replay_trail;
    if (prepare_replay()) {
        if (const char* mt = input ? mq_input_log_get_meta(input, "trail") : nullptr) replay_trail = mt;
        /* 日志元数据 frames = **已执行帧数**（finalize_run 写入 loop 计数）→ 回放跑同样多帧；
         * 曾经 +1（误当"末帧索引"）导致回放多跑一帧、末尾校验和必然不等（docs/10 §5）。 */
        if (const char* mf = input ? mq_input_log_get_meta(input, "frames") : nullptr) {
            const uint64_t n = std::strtoull(mf, nullptr, 10);
            if (n > 0) cap = n;
        }
    }
    if (input && std::getenv("MAQUE_RECORD")) mq_input_record_begin(input);

    mq_obs_event(obs, "runtime", 1, "loop_start");
    frame = 0;

    /* 墙钟限速（见 pace_wall_clock 注释）：只在"真窗口 + 非自动化"时启用。
     * 判定为自动化、因此**不限速**（保持"尽快出结果"的既有语义）的情形：
     *   · 无窗口（headless / MAQUE_WINDOW=0x0 / 无显示环境）
     *   · --shot / MAQUE_SHOT（只求一张图，帧数与时间无关）
     *   · 录制/回放（docs/10 §3/§5：要求逐帧复现，不该受墙钟影响）
     * 注意 `MAQUE_MAX_FRAMES`/`MAQUE_STOP_FRAME` **不算**自动化信号 —— 它们只是"跑多少帧"
     * 的上限，与"要不要按真实时间跑"无关（无头跑它们本来就是全速的）。 */
    const bool shot_mode = std::getenv("MAQUE_SHOT") != nullptr;
    const bool recorded = std::getenv("MAQUE_RECORD") != nullptr || !replay_trail.empty();
    pace_wall_clock = window && !shot_mode && !recorded;
    if (const char* pc = std::getenv("MAQUE_PACE")) {          // 显式覆盖：0/off 关闭，1/on 强制开启
        const std::string v(pc);
        if (v == "0" || v == "off") pace_wall_clock = false;
        else if (v == "1" || v == "on") pace_wall_clock = true;
    }
    /* 墙钟限速：**绝对时间基准**（仿真时间 = frame × dt，目标墙钟时刻 = pace_start + 仿真时间）。
     * 每轮按"落后多少"决定补几步（1..8），跑完睡到"下一帧应开始"的时刻：
     *   · deadline 固定推进 → 睡眠过冲下一轮用更短的睡眠吸收，不累积漂移；
     *   · 补步只让**最后一步**成帧（见下）→ 补步不再"越补越慢"；
     *   · 落后超过 8 步（卡顿/断点/挂起）才重锚：丢弃积压，宁可让游戏时间变慢，也不爆追。 */
    auto pace_start = std::chrono::steady_clock::now();
    const auto pace_t0 = pace_start;              // 真实起点（诊断用；pace_start 会因重锚前移）
    double pace_sleep = 0.0;
    double pace_step_cost = 0.0;                  // 上一轮实测的"单步成本"（秒）→ 补步预算
    uint64_t pace_reanchor = 0;
    uint64_t pace_steps = 0;
    /* 逐帧输出（截图/抓帧/逐帧导出）必须**每帧都成帧**，不允许把两步合成一帧 —— 否则
     * `--shot-frame N` / `MAQUE_CAPTURE` 的帧号与画面会对不上。 */
    const bool per_frame_output = !shot_path.empty() || !shot_seq.empty() || !frames_dir.empty() ||
                                  std::getenv("MAQUE_CAPTURE") != nullptr;

    for (; frame < cap && !quit; ) {
        MQ_TRACY_ZONE_N(runtime, "loop");   // Tracy：一轮主循环（含可能的多步补偿 + 墙钟 sleep）
        if (stop_frame && frame > stop_frame) break;
        if (g_quit_requested) {                          // SIGTERM/SIGINT → 优雅退出（正常收尾 + 清理端点）
            quit = true;
            mq_obs_event(obs, "runtime", 1, "terminated");
            break;
        }
        /* 墙钟 → 固定步数（docs/10 D1 的确定性不变：**步长仍恒为 1/60**，变的只是
         * "这一轮屏幕刷新要跑几步"）。两个方向都要处理：
         *   · 机器比实时快 → 只跑 1 步会让动画跟着快（实测 2.7x）→ 用 sleep 压到 1x；
         *   · 机器比实时慢（渲染每帧 ~29ms 实测）→ 只跑 1 步会让动画跟着慢（实测 0.58x）
         *     → 这一轮多补几步，让**游戏时间始终跟墙钟走**。
         * 上限 8 步、超出即重锚（见上）。 */
        int steps_this_frame = 1;
        if (pace_wall_clock && !per_frame_output) {
            constexpr double kDt = 1.0 / 60.0;
            const auto now = std::chrono::steady_clock::now();
            double lag = std::chrono::duration<double>(now - pace_start).count() - (double)frame * kDt;
            const auto drop_backlog = [&](bool stall) {
                /* 丢弃积压 = 把"当前时刻"重新锚成仿真时刻（游戏时间变慢，但不再爆追）。 */
                pace_start += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(lag));
                lag = 0.0;
                if (stall) ++pace_reanchor;
            };
            if (lag > 8.0 * kDt) {
                drop_backlog(true);                  // 卡顿/断点/挂起
            } else if (pace_step_cost > kDt && lag > kDt) {
                /* 上一轮单步成本已超过 dt：这台机器本来就跑不动 1x，补步只会越补越慢 →
                 * 丢掉积压，按"每轮 1 步"的最快速度跑（Tracy 里看速率 <1x 是事实，不是抖动）。 */
                drop_backlog(false);
            }
            steps_this_frame = 1 + (int)std::floor(lag / kDt);
            /* 补步预算：一轮最多多做 ≈1 帧的工作量。上一轮实测单步成本已经 ≈dt 时，多跑一步只会
             * 把这一轮拖得更长（"越补越慢"的自激：Debug+ASan 下重文本场景实测 0.52x / 29 次重锚）。 */
            if (pace_step_cost > 0.0) {
                const int budget = (int)(kDt / pace_step_cost);
                if (steps_this_frame - 1 > budget) steps_this_frame = budget + 1;
            }
            if (steps_this_frame < 1) steps_this_frame = 1;
            if (steps_this_frame > 8) steps_this_frame = 8;
        }
        const auto pace_work0 = std::chrono::steady_clock::now();
        int pace_ran = 0;
        for (int s = 0; s < steps_this_frame && frame < cap && !quit; ++s) {
            if (stop_frame && frame > stop_frame) break;
            /* 补步：只有**最后一步**成帧（渲染 + present + Tracy FrameMark），中间步只推进模拟。
             * 旧实现每步都完整 render+present → 补 2 步 = 渲染两帧，越补越慢；
             * Tracy 里就表现为 22.25/15.32/21.42/11.59ms 这种"长短帧成对"的抖动。 */
            step_frame(s + 1 == steps_this_frame);
            ++pace_steps;
            ++pace_ran;
        }
        if (pace_wall_clock && pace_ran > 0)
            pace_step_cost = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - pace_work0).count() / pace_ran;
        if (pace_wall_clock) {
            /* 睡到"下一帧应开始"的绝对时刻；落后（due 已过）就不睡，由上一段的补步追。 */
            const auto due = pace_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>((double)frame / 60.0));
            const auto now = std::chrono::steady_clock::now();
            if (due > now) {
                const auto d = due - now;
                pace_sleep += std::chrono::duration<double>(d).count();
                std::this_thread::sleep_for(d);
            }
        }
        if (g_signal_hit) {                              // 收到终止信号 → 落盘上下文后退出
            write_crash_context(g_signal_name ? g_signal_name : "signal");
            mq_obs_event(obs, "runtime", 3, g_signal_name ? g_signal_name : "signal");
            break;
        }
    }

    /* 循环是因为"到帧数上限"退出的 → 记下来（退出码按成功算，见 frames_capped 注释）。 */
    if (!quit && (frame >= cap || (stop_frame && frame > stop_frame))) frames_capped = true;

    finalize_run();

    if (const char* td = std::getenv("MAQUE_TRAIL_OUT"); td && *td) {   // 失步定位（docs/10 §5）
        std::ofstream f(td, std::ios::binary);
        for (auto& [fr, sum] : trail) f << fr << " " << sum << "\n";
    }
    // 失步检测：回放必须有记录里的校验和（docs/10 §5）
    if (!replay_trail.empty()) {
        char tb[32]; std::snprintf(tb, sizeof tb, "%llu", (unsigned long long)world_trail());
        if (replay_trail == tb) std::print("runtime: replay matches recorded trail ({})\n", tb);
        else std::print("runtime: DESYNC — replay trail {} != recorded {}\n", tb, replay_trail);
    }
    mq_obs_gauge_set(obs, "frames_total", (double)frame);
    if (const char* pt = std::getenv("MAQUE_TRACE_PACE")) {
        (void)pt;
        /* 墙钟从**真实起点**算（pace_start 会因重锚前移，用它算速率会虚高）。 */
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - pace_t0).count();
        std::print("[pace] frames={} 墙钟={:.3f}s 仿真={:.3f}s 速率={:.2f}x wall_clock={} 睡觉={:.3f}s 重锚定={} 次\n",
                   (long long)frame, wall, (double)frame / 60.0, wall > 0 ? (frame / 60.0) / wall : 0.0,
                   pace_wall_clock ? 1 : 0, pace_sleep, (long long)pace_reanchor);
    }
    mq_obs_event(obs, "runtime", 1, quit ? "quit" : "timeout");
    std::print("runtime: '{}' exited after {} frames (quit={}) world_trail=0x{:016x}\n",
               name, frame, quit ? 1 : 0, world_trail());
}

}  // namespace maque::rt
