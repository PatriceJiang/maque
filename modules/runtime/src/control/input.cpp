/* 控制面 · 输入/模拟器（docs/19）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

/* Input.inject：AI/CI 注入输入（docs/11/19 §9）——与设备事件同一事件流。 */
extern "C" int mq_control_Input_inject(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    mq_input_event ev{};
    ev.type = (uint32_t)mq_json_num(mq_json_get(params, "type"), (double)MQ_INPUT_POINTER_DOWN);
    ev.pointer_id = (uint32_t)mq_json_num(mq_json_get(params, "pointer_id"), 0);
    ev.pointer_type = (uint32_t)mq_json_num(mq_json_get(params, "pointer_type"), 0);
    ev.button = (uint32_t)mq_json_num(mq_json_get(params, "button"), 0);
    ev.key = (uint32_t)mq_json_num(mq_json_get(params, "key"), 0);
    ev.x = (float)mq_json_num(mq_json_get(params, "x"), 0);
    ev.y = (float)mq_json_num(mq_json_get(params, "y"), 0);
    mq_input_inject(e->input, &ev);
    std::snprintf(out, cap, "{\"ok\":true,\"injected\":\"%s\"}", mq_json_str(mq_json_get(params, "type"), "?"));
    return 0;
}

/* ---- 输入模拟器控制面（docs/19）：场景加载 / 状态 / 事件列表 / 停止 ----
 * 场景来源：path（VFS 路径）或 json（内联字符串）；载入后可 reset 重放、loop 循环。 */
extern "C" int mq_control_Input_sim_load(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string json;
    std::string src = "inline";
    if (const char* p = mq_json_str(mq_json_get(params, "path"), nullptr); p && *p) {
        if (!read_sim_scenario(e->vfs, p, json)) {
            std::snprintf(out, cap, "{\"ok\":false,\"error\":\"scenario not found: %s\"}", p);
            return 0;
        }
        src = p;
    } else if (const char* j = mq_json_str(mq_json_get(params, "json"), nullptr); j && *j) {
        json = j;
    } else {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"need path or json\"}");
        return 0;
    }
    if (!e->sim) e->sim = mq_sim_create();
    char err[256] = { 0 };
    const int rc = mq_sim_load_json(e->sim, json.c_str(), json.size(), err, sizeof err);
    if (rc != 0) {
        std::snprintf(out, cap, "{\"ok\":false,\"rc\":%d,\"error\":\"%s\"}", rc, err);
        return 0;
    }
    if (mq_json_num(mq_json_get(params, "loop"), 0.0) != 0.0) mq_sim_set_loop(e->sim, 1);
    if (mq_json_num(mq_json_get(params, "reset"), 1.0) != 0.0) mq_sim_reset(e->sim);
    e->sim_active = true;
    e->sim_path = src;
    std::snprintf(out, cap,
                  "{\"ok\":true,\"source\":\"%s\",\"events\":%u,\"last_frame\":%llu,\"loop\":%s}",
                  src.c_str(), mq_sim_count(e->sim), (unsigned long long)mq_sim_last_frame(e->sim),
                  mq_json_num(mq_json_get(params, "loop"), 0.0) != 0.0 ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Input_sim_status(void* u, const mq_json* params, char* out, size_t cap) {
    (void)params;
    auto* e = (Engine*)u;
    if (!e->sim || !mq_sim_loaded(e->sim)) {
        std::snprintf(out, cap, "{\"active\":false,\"loaded\":false}");
        return 0;
    }
    std::snprintf(out, cap,
                  "{\"active\":%s,\"loaded\":true,\"source\":\"%s\",\"cursor\":%u,\"total\":%u,"
                  "\"finished\":%s,\"last_frame\":%llu,\"frame\":%llu}",
                  e->sim_active ? "true" : "false", e->sim_path.c_str(), mq_sim_cursor(e->sim),
                  mq_sim_count(e->sim), mq_sim_finished(e->sim) ? "true" : "false",
                  (unsigned long long)mq_sim_last_frame(e->sim), (unsigned long long)e->frame);
    return 0;
}

extern "C" int mq_control_Input_sim_events(void* u, const mq_json* params, char* out, size_t cap) {
    (void)params;
    auto* e = (Engine*)u;
    if (!e->sim || !mq_sim_loaded(e->sim)) {
        std::snprintf(out, cap, "{\"events\":[]}");
        return 0;
    }
    std::string s = "{\"events\":[";
    const uint32_t n = mq_sim_count(e->sim);
    for (uint32_t i = 0; i < n; ++i) {
        mq_sim_event ev{};
        if (mq_sim_event_at(e->sim, i, &ev) != 0) continue;
        if (i) s += ",";
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "{\"frame\":%llu,\"kind\":%u,\"space\":%u,\"x\":%.1f,\"y\":%.1f,"
                      "\"dx\":%.1f,\"dy\":%.1f,\"button\":%u,\"key\":%u,\"text\":\"%s\"}",
                      (unsigned long long)ev.frame, ev.kind, ev.space, ev.x, ev.y, ev.dx, ev.dy,
                      ev.button, ev.key, ev.text ? ev.text : "");
        s += buf;
    }
    s += "]}";
    std::snprintf(out, cap, "%s", s.c_str());
    return 0;
}

extern "C" int mq_control_Input_sim_stop(void* u, const mq_json* params, char* out, size_t cap) {
    (void)params;
    auto* e = (Engine*)u;
    const bool was = e->sim_active;
    e->sim_active = false;
    std::snprintf(out, cap, "{\"ok\":true,\"was_active\":%s}", was ? "true" : "false");
    return 0;
}
