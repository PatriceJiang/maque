/* 控制面 · 自省/驱动（Perf/Control/Types/Runtime/Replay/Director/Reload；docs/11）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

/* ---- 控制平面方法（引擎能力，docs/11） ---- */
extern "C" int mq_control_Perf_get(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    /* 查询时刷新一次内存域（采样型域是"读当前真相"，delta 型域直接读账本）→ 控制面
     * 永远拿到最新值，不受 MAQUE_TRACY_MEM_INTERVAL 的采样节流影响。 */
    if (e) {
        e->memory_sample();
        /* 进程常驻内存（WorkingSet/RSS）：覆盖"没有进任何域"的堆/映射增长。
         * 只在查询时取一次，避免与 Tracy 的 1Hz CPU 采样争用 ps_prev 时间戳。 */
        if (e->platform) {
            double cpu_percent = 0.0;
            uint64_t rss = 0;
            if (mq_platform_process_stats(e->platform, &cpu_percent, &rss) == 0)
                mq_obs_gauge_set(e->obs, "mem.rss", (double)rss);
        }
    }
    std::snprintf(out, cap, "%s", mq_obs_dump_json(e ? e->obs : nullptr));
    return 0;
}

/* Control.info：端点/权限/计数（inspector 状态栏；plans/17 §3） */
extern "C" int mq_control_Control_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const char* url = eng->ctl_server ? mq_control_server_url(eng->ctl_server) : "";
    (void)std::snprintf(out, cap,
        "{\"endpoint\":\"%s\",\"read_only\":%s,\"clients\":%u,\"rejected\":%u,"
        "\"methods\":%u,\"frame\":%llu}",
        url, eng->inspect_read_only ? "true" : "false",
        mq_control_server_clients(eng->ctl_server), mq_control_server_rejected(eng->ctl_server),
        mq_control_method_count(eng->control), (unsigned long long)eng->frame);
    return 0;                                   /* 约定：内容写入 out，返回 0（见其它 handler） */
}

extern "C" int mq_control_Types_describe(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "[";
    const uint32_t n = mq_reflect_count(e->reflect);
    char item[512];
    for (uint32_t i = 0; i < n; ++i) {
        const mq_type_desc* d = mq_reflect_type_at(e->reflect, i);
        if (!d) continue;
        std::snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"id\":%u,\"fields\":%u,\"role\":%u}",
                      i ? "," : "", d->name, d->type_id, d->field_count, d->role);
        o += item;
    }
    o += "]";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Control.mcp：MCP 语义（initialize/tools/list/tools/call）经控制平面进出。
 * 请求体放 params.request（字符串）；也可直接传 params.method/params.name。 */
/* Reload.script / Reload.scene：手动触发帧边界重载；dry_run=1 只报告不执行（docs/11 §4）。 */
extern "C" int mq_control_Reload_script(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    if (dry) {
        void* d = nullptr; size_t n = 0;
        uint64_t h = 0;
        if (!e->script_path.empty() && mq_vfs_read(e->vfs, ("/" + e->script_path).c_str(), &d, &n) == MQ_VFS_OK) {
            h = mq_vfs_hash(d, n); mq_vfs_free(d);
        }
        std::snprintf(out, cap, "{\"would_reload\":%s,\"script\":\"%s\"}",
                      h != e->script_hash ? "true" : "false", e->script_path.c_str());
        return 0;
    }
    std::string err;
    const bool ok = e->reload_script(&err);
    std::snprintf(out, cap, "{\"reloaded\":%s,\"reloads\":%u,\"error\":\"%s\"}",
                  ok ? "true" : "false", e->reloads, err.c_str());
    return 0;
}

extern "C" int mq_control_Reload_scene(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    if (dry) { std::snprintf(out, cap, "{\"would_reload\":true,\"scene\":\"%s\"}", e->scn_vfs_path.c_str()); return 0; }
    const bool ok = e->reload_scene();
    std::snprintf(out, cap, "{\"reloaded\":%s,\"reloads\":%u,\"entities\":%d}",
                  ok ? "true" : "false", e->reloads, e->scene ? e->scene->count() : 0);
    return 0;
}

extern "C" int mq_control_Reload_files(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "{\"watching\":" + std::to_string(e->watcher ? mq_watcher_count(e->watcher) : 0) + ",\"files\":[";
    if (e->watcher)
        for (uint32_t i = 0; i < mq_watcher_count(e->watcher); ++i) {
            const char* p = mq_watcher_path_at(e->watcher, i);
            o += std::string(i ? ",\"" : "\"") + (p ? p : "") + "\"";
        }
    o += "],\"reloads\":" + std::to_string(e->reloads) + "}";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Control.mcp：MCP 语义（initialize/tools/list/tools/call）。
 * 两种入参形态：
 *   ① params.request = "<完整 MCP 请求 JSON 文本>"（原样走 MCP 包装）
 *   ② params.method  = "tools/call" + params.name/arguments（直接转发到同名方法，
 *      避免手工拼 JSON —— 结构化入参优先，docs/11 §4）。 */
extern "C" int mq_control_Control_mcp(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (const char* raw = mq_json_str(mq_json_get(params, "request"), nullptr)) {
        std::snprintf(out, cap, "%s", mq_control_dispatch_mcp(e->control, raw));
        return 0;
    }
    const char* method = mq_json_str(mq_json_get(params, "method"), nullptr);
    if (!method) { std::snprintf(out, cap, "{\"error\":\"missing request/method\"}"); return 0; }
    if (std::strcmp(method, "tools/call") == 0) {
        const char* name = mq_json_str(mq_json_get(params, "name"), nullptr);
        const mq_json* args = mq_json_get(params, "arguments");
        if (!name) {
            std::snprintf(out, cap, "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"missing tool name\"}]}");
            return 0;
        }
        /* 直接调用已注册的 JSON-RPC 方法（与 MCP tools/call 相同的语义） */
        std::string inner = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + std::string(name) + "\"";
        if (args) {
            std::string argtxt = "{";
            struct Ctx { std::string* out; bool first; };
            Ctx cx{ &argtxt, true };
            mq_json_object_each(args, [](void* ud, const char* k, const char* v) {
                auto* c = (Ctx*)ud;
                if (!c->first) *c->out += ",";
                c->first = false;
                const bool numeric = v && *v && ((v[0] >= '0' && v[0] <= '9') ||
                                                 ((v[0] == '-' || v[0] == '+') && v[1] >= '0' && v[1] <= '9'));
                *c->out += std::string("\"") + k + "\":" +
                           (numeric ? std::string(v) : ("\"" + std::string(v ? v : "") + "\""));
                return 0;
            }, &cx);
            argtxt += "}";
            inner += ",\"params\":" + argtxt;
        }
        inner += "}";
        const char* resp = mq_control_dispatch(e->control, inner.c_str());
        std::string text = resp ? resp : "null";
        std::string escaped;
        for (char c : text) { if (c == '"' || c == '\\') escaped += '\\'; escaped += c; }
        std::snprintf(out, cap, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", escaped.c_str());
        return 0;
    }
    /* initialize / tools/list / 其它：经 MCP 包装 */
    std::string req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + std::string(method) + "\"";
    if (const mq_json* p = mq_json_get(params, "params")) {
        if (const char* nm = mq_json_str(mq_json_get(p, "name"), nullptr)) {
            req += std::string(",\"params\":{\"name\":\"") + nm + "\"}";
        }
    }
    req += "}";
    /* MCP 包装返回 {"jsonrpc":..,"result":X}；此处抽出 X（外层由控制平面再包一次） */
    const char* resp = mq_control_dispatch_mcp(e->control, req.c_str());
    const char* res = resp ? std::strstr(resp, "\"result\":") : nullptr;
    std::snprintf(out, cap, "%s", res ? res + 9 : (resp ? resp : "{}"));
    if (res) {   // 去掉末尾多余的 '}'（外层控制平面会补）
        size_t n = std::strlen(out);
        if (n > 0 && out[n - 1] == '}') out[n - 1] = '\0';
    }
    return 0;
}

/* Replay.checkpoint：列出检查点（帧号/校验和）——分歧定位的锚点。 */
extern "C" int mq_control_Replay_checkpoints(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "{\"interval\":" + std::to_string(e->cp_interval) + ",\"active\":";
    o += e->cp_active ? "true" : "false";
    o += ",\"checkpoints\":[";
    bool first = true;
    for (auto* s : e->checkpoints) {
        if (!s || mq_snapshot_frame(s) == 0) continue;
        if (!first) o += ",";
        first = false;
        o += "{\"frame\":" + std::to_string(mq_snapshot_frame(s)) +
             ",\"checksum\":\"" + std::to_string(mq_snapshot_checksum(s)) + "\"}";
    }
    o += "]}";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Replay.seek：跳到指定帧号的检查点（回滚 + 重建世界）。 */
extern "C" int mq_control_Replay_seek(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const uint64_t want = (uint64_t)mq_json_num(mq_json_get(params, "frame"), 0);
    mq_snapshot* best = nullptr;
    for (auto* s : e->checkpoints) {
        if (!s) continue;
        const uint64_t f = mq_snapshot_frame(s);
        if (f && f <= want && (!best || f > mq_snapshot_frame(best))) best = s;
    }
    if (!best) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no checkpoint at or before frame %llu\",\"interval\":%u}",
                      (unsigned long long)want, e->cp_interval);
        return 0;
    }
    e->world_restore(best);
    std::snprintf(out, cap, "{\"ok\":true,\"frame\":%llu,\"checksum\":\"%llu\"}",
                  (unsigned long long)mq_snapshot_frame(best), (unsigned long long)e->world_checksum());
    return 0;
}

/* Runtime.frame：当前模拟帧号与状态摘要（脚本/CI 读模拟时间；回滚后自动一致）。 */
extern "C" int mq_control_Director_pause(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    e->director_paused = true;
    std::snprintf(out, cap, "{\"ok\":true,\"paused\":true,\"frame\":%llu}", (unsigned long long)e->frame);
    return 0;
}

extern "C" int mq_control_Director_resume(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    e->director_paused = false;
    std::snprintf(out, cap, "{\"ok\":true,\"paused\":false,\"frame\":%llu}", (unsigned long long)e->frame);
    return 0;
}

extern "C" int mq_control_Director_status(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap, "{\"paused\":%s,\"frame\":%llu,\"script_frames\":%llu}",
                  e->director_paused ? "true" : "false",
                  (unsigned long long)e->frame, (unsigned long long)e->script_frames);
    return 0;
}

extern "C" int mq_control_Runtime_frame(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::snprintf(out, cap, "{\"frame\":%llu,\"entities\":%d,\"systems.particles\":%zu,\"quit\":%s}",
                  (unsigned long long)e->frame, e->scene ? e->scene->count() : 0,
                  e->systems.particles.size(), e->quit ? "true" : "false");
    return 0;
}

/* Runtime.step：确定性推进 n 帧（AI/CI 逐帧驱动，docs/11 §5）。 */
extern "C" int mq_control_Runtime_step(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const double n = mq_json_num(mq_json_get(params, "frames"), 1);
    const uint64_t steps = n > 0 ? (uint64_t)n : 1;
    e->run_frames(steps);
    std::snprintf(out, cap, "{\"frame\":%llu,\"checksum\":\"%llu\",\"entities\":%d,\"quit\":%s}",
                  (unsigned long long)e->frame, (unsigned long long)e->last_checksum,
                  e->scene ? e->scene->count() : 0, e->quit ? "true" : "false");
    return 0;
}

/* Replay.log：导出输入日志（录制/回放是数据，docs/10 §3）。 */
extern "C" int mq_control_Replay_log(void* u, const mq_json*, char* out, size_t cap) {
    const char* json = mq_input_log_json(((Engine*)u)->input);
    std::snprintf(out, cap, "%s", json ? json : "{}");
    return 0;
}
