#include "core/observability/mq_obs.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <cstdio>
#include <print>

struct mq_obs_ctx {
    std::deque<std::string> events;                       // 环形（保留尾部）
    size_t                 event_cap = 256;
    std::unordered_map<std::string, double> counters;
    std::unordered_map<std::string, double> gauges;
    std::string            dump;
};

static void json_escape(std::string& out, const char* s) {
    for (const char* p = s; p && *p; ++p) {
        switch (*p) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += *p; break;
        }
    }
}

extern "C" {

mq_obs_ctx* mq_obs_create(void) { return new mq_obs_ctx(); }
void mq_obs_destroy(mq_obs_ctx* ctx) { delete ctx; }

void mq_obs_event(mq_obs_ctx* ctx, const char* domain, int level, const char* event) {
    if (!ctx) return;
    std::string line;
    line.reserve(128);
    line += "{\"domain\":\""; json_escape(line, domain ? domain : "");
    line += "\",\"level\":"; line += std::to_string(level);
    line += ",\"event\":\""; json_escape(line, event ? event : "");
    line += "\"}";
    ctx->events.push_back(std::move(line));
    while (ctx->events.size() > ctx->event_cap) ctx->events.pop_front();
    std::print(stdout, "{}\n", ctx->events.back());       // JSON Lines
    std::fflush(stdout);
}

void mq_obs_counter_add(mq_obs_ctx* ctx, const char* name, double delta) {
    if (ctx && name) ctx->counters[name] += delta;
}
void mq_obs_gauge_set(mq_obs_ctx* ctx, const char* name, double value) {
    if (ctx && name) ctx->gauges[name] = value;
}
double mq_obs_gauge_get(mq_obs_ctx* ctx, const char* name) {
    if (!ctx || !name) return 0.0;
    auto it = ctx->gauges.find(name);
    return it == ctx->gauges.end() ? 0.0 : it->second;
}

const char* mq_obs_dump_json(mq_obs_ctx* ctx) {
    if (!ctx) return "{}";
    ctx->dump.clear();
    ctx->dump += "{\"metrics\":{";
    bool first = true;
    for (auto& [k, v] : ctx->counters) {
        if (!first) ctx->dump += ',';
        first = false;
        ctx->dump += '"'; json_escape(ctx->dump, k.c_str()); ctx->dump += "\":";
        ctx->dump += std::to_string(v);
    }
    for (auto& [k, v] : ctx->gauges) {
        if (!first) ctx->dump += ',';
        first = false;
        ctx->dump += '"'; json_escape(ctx->dump, k.c_str()); ctx->dump += "\":";
        ctx->dump += std::to_string(v);
    }
    ctx->dump += "},\"events\":[";
    for (size_t i = 0; i < ctx->events.size(); ++i) {
        if (i) ctx->dump += ',';
        ctx->dump += ctx->events[i];
    }
    ctx->dump += "]}";
    return ctx->dump.c_str();
}

} // extern "C"
