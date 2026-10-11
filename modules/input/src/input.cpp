/* 输入系统实现（docs/19）：确定性事件流 + 录制/回放 + 捕获/目标/冒泡分发。
 * 时间只来自帧号（模拟时间），绝不读墙钟（docs/10 D3）。 */
#include "core/input/mq_input.h"

#include "core/persist/mq_persist.h"     // mq_json（日志 JSON 形态）

#include <print>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>

namespace {

/* 日志条目：自描述 POD（固定布局，跨平台稳定）。 */
struct LogEntry {
    uint64_t frame;
    uint32_t type;         /* mq_input_event_type */
    uint32_t pointer_id;
    uint32_t pointer_type;
    uint32_t button;
    uint32_t modifiers;
    uint32_t key;
    float    x, y, dx, dy;
    float    scroll_x, scroll_y;
    float    pressure;
    char     text[16];
};

struct PendingClick {
    float x = 0, y = 0;
    uint32_t button = 0;
    uint64_t frame = 0;
    int      target = -1;
};

} // namespace

struct mq_input {
    uint64_t frame_index = 0;

    /* 本帧事件（按到达顺序：device → replay → inject） */
    std::vector<mq_input_event> events;
    std::vector<mq_input_event> pending_inject;   // 注入：帧开始前入队

    /* 状态 */
    std::unordered_map<uint32_t, std::pair<float, float>> pos;
    std::unordered_set<uint32_t> pointers_down;
    std::unordered_set<uint32_t> keys_down;
    std::unordered_map<uint32_t, int> capture;    // pointer_id → entity
    int  focus = -1;
    int  quit = 0;
    int  hover = -1;

    /* 回调 */
    mq_input_hit_test  hit = nullptr;      void* hit_user = nullptr;
    mq_input_parent_fn parent = nullptr;   void* parent_user = nullptr;
    struct Handler { mq_input_handler fn; void* user; };
    std::vector<Handler> handlers;

    /* 合成 click */
    std::unordered_map<uint32_t, PendingClick> pending_click;

    /* 日志元数据（键值对；随日志保存） */
    std::vector<std::pair<std::string, std::string>> meta;

    /* 录制/回放 */
    bool recording = false;
    bool replaying = false;
    std::vector<LogEntry> log;        // 录制产物
    std::vector<LogEntry> replay_src; // 回放源
    size_t replay_cursor = 0;
    std::string json_cache;
};

namespace {

void push_event(mq_input* in, const mq_input_event& ev) { in->events.push_back(ev); }

/* 平台指针事件 → 语义事件（1:1，类型映射稳定）。 */
bool from_platform(mq_input* in, const mq_platform_pointer_event& pe) {
    mq_input_event ev{};
    ev.pointer_id = pe.pointer_id;
    ev.pointer_type = pe.pointer_type;
    ev.button = pe.button;
    ev.modifiers = pe.modifiers;
    ev.x = pe.x; ev.y = pe.y; ev.dx = pe.dx; ev.dy = pe.dy;
    ev.scroll_x = pe.scroll_x; ev.scroll_y = pe.scroll_y;
    ev.pressure = pe.pressure;
    ev.target = -1;
    switch (pe.type) {
        case MQ_PLATFORM_POINTER_DOWN:   ev.type = MQ_INPUT_POINTER_DOWN; break;
        case MQ_PLATFORM_POINTER_UP:     ev.type = MQ_INPUT_POINTER_UP; break;
        case MQ_PLATFORM_POINTER_MOVE:   ev.type = MQ_INPUT_POINTER_MOVE; break;
        case MQ_PLATFORM_POINTER_WHEEL:  ev.type = MQ_INPUT_POINTER_WHEEL; break;
        default: return false;   // CANCEL：释放捕获但不分发
    }
    push_event(in, ev);
    return true;
}

bool from_platform_key(mq_input* in, const mq_platform_key_event& ke, bool down) {
    mq_input_event ev{};
    ev.pointer_id = 0; ev.target = -1;
    ev.key = ke.key; ev.modifiers = ke.modifiers;
    if (ke.key == MQ_KEY_TEXT && down) {
        ev.type = MQ_INPUT_TEXT;
        std::memcpy(ev.text, ke.text, sizeof(ev.text));
        if (ev.text[0] == '\0') return false;
    } else {
        ev.type = down ? MQ_INPUT_KEY_DOWN : MQ_INPUT_KEY_UP;
    }
    push_event(in, ev);
    return true;
}

void log_push(mq_input* in, const mq_input_event& ev, uint64_t frame) {
    LogEntry e{};
    e.frame = frame;
    e.type = ev.type; e.pointer_id = ev.pointer_id; e.pointer_type = ev.pointer_type;
    e.button = ev.button; e.modifiers = ev.modifiers; e.key = ev.key;
    e.x = ev.x; e.y = ev.y; e.dx = ev.dx; e.dy = ev.dy;
    e.scroll_x = ev.scroll_x; e.scroll_y = ev.scroll_y; e.pressure = ev.pressure;
    std::memcpy(e.text, ev.text, sizeof(e.text));
    in->log.push_back(e);
}

mq_input_event from_log(const LogEntry& e) {
    mq_input_event ev{};
    ev.type = e.type; ev.pointer_id = e.pointer_id; ev.pointer_type = e.pointer_type;
    ev.button = e.button; ev.modifiers = e.modifiers; ev.key = e.key;
    ev.x = e.x; ev.y = e.y; ev.dx = e.dx; ev.dy = e.dy;
    ev.scroll_x = e.scroll_x; ev.scroll_y = e.scroll_y; ev.pressure = e.pressure;
    std::memcpy(ev.text, e.text, sizeof(ev.text));
    ev.target = -1;
    return ev;
}

const char* ev_type_name(uint32_t t) {
    static const char* names[] = { "pointer_down", "pointer_up", "pointer_move", "pointer_wheel",
                                   "pointer_enter", "pointer_leave", "click", "double_click",
                                   "key_down", "key_up", "text" };
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "unknown";
}

} // namespace

extern "C" {

mq_input* mq_input_create(void) { return new mq_input(); }
void      mq_input_destroy(mq_input* in) { delete in; }

void mq_input_set_hit_test(mq_input* in, mq_input_hit_test fn, void* user) {
    if (!in) return;
    in->hit = fn; in->hit_user = user;
}

void mq_input_set_parent_fn(mq_input* in, mq_input_parent_fn fn, void* user) {
    if (!in) return;
    in->parent = fn; in->parent_user = user;
}

void mq_input_add_handler(mq_input* in, mq_input_handler fn, void* user) {
    if (in && fn) in->handlers.push_back({ fn, user });
}

void mq_input_begin_frame(mq_input* in, const mq_input_frame* frame, uint64_t frame_index) {
    if (!in) return;
    in->frame_index = frame_index;
    in->events.clear();

    if (in->replaying) {
        // 回放：只喂记录里属于本帧的事件（与录制时的帧号对齐）
        while (in->replay_cursor < in->replay_src.size() &&
               in->replay_src[in->replay_cursor].frame <= frame_index) {
            push_event(in, from_log(in->replay_src[in->replay_cursor]));
            ++in->replay_cursor;
        }
        return;
    }

    if (frame) {
        in->quit = in->quit || frame->quit;
        for (uint32_t i = 0; i < frame->pointer_count; ++i) from_platform(in, frame->pointers[i]);
        for (uint32_t i = 0; i < frame->key_count; ++i)
            from_platform_key(in, frame->keys[i], frame->keys[i].down != 0);
    }
    // 注入（AI/CI）：与设备事件同流
    for (const auto& ev : in->pending_inject) push_event(in, ev);
    in->pending_inject.clear();
    if (in->recording) for (const auto& ev : in->events) log_push(in, ev, frame_index);
}

/* 单条事件：命中测试 → 捕获/目标/冒泡分发 → 状态更新（docs/19 §4/§5）。 */
static void dispatch_one(mq_input* in, mq_input_event ev) {
    mq_input_target t{};
    t.entity = -1; t.handle = (uint32_t)-1;

    int captured = -1;
    auto cit = in->capture.find(ev.pointer_id);
    if (cit != in->capture.end()) captured = cit->second;
    const bool pointerish = ev.type == MQ_INPUT_POINTER_DOWN || ev.type == MQ_INPUT_POINTER_UP ||
                            ev.type == MQ_INPUT_POINTER_MOVE || ev.type == MQ_INPUT_POINTER_WHEEL;
    if (pointerish && captured >= 0 &&
        (ev.type == MQ_INPUT_POINTER_MOVE || ev.type == MQ_INPUT_POINTER_UP)) {
        t.entity = captured;                     // 指针捕获：始终发给捕获者（docs/19 §6）
    } else if (pointerish && in->hit) {
        in->hit(in->hit_user, &ev, &t);          // 命中测试（引擎提供，稳定排序）
    } else if (!pointerish && in->focus >= 0) {
        t.entity = in->focus;                    // 键盘/文本 → 焦点节点
    }
    ev.target = t.entity;

    auto invoke = [&](uint32_t phase, int entity, const mq_input_event* src) -> bool {
        mq_input_target lt{};
        lt.entity = entity;
        lt.handle = entity >= 0 ? (uint32_t)entity : (uint32_t)-1;
        mq_input_event local = src ? *src : ev;   // 合成事件（如 click）走同一路径
        local.phase = phase;
        bool stop = false;
        for (const auto& h : in->handlers) {
            if (h.fn(h.user, &local, &lt) != 0) stop = true;
            if (lt.stopped) stop = true;
        }
        if (entity == t.entity) { t.consumed = lt.consumed; t.stopped = lt.stopped; }
        return stop;
    };
    const mq_input_event* self = nullptr;

    if (t.entity >= 0 && in->parent) {
        std::vector<int> path;                   // 目标 → 根
        int cur = t.entity;
        for (int guard = 0; cur >= 0 && guard < 256; ++guard) {
            path.push_back(cur);
            cur = in->parent(in->parent_user, cur);
        }
        std::reverse(path.begin(), path.end());  // 根 → 目标
        bool stop = false;
        for (size_t pi = 0; pi + 1 < path.size() && !stop; ++pi)
            stop = invoke(MQ_INPUT_PHASE_CAPTURE, path[pi], self);
        if (!stop) stop = invoke(MQ_INPUT_PHASE_TARGET, t.entity, self);
        for (size_t pi = path.size(); pi-- > 1 && !stop;)
            stop = invoke(MQ_INPUT_PHASE_BUBBLE, path[pi - 1], self);
    } else if (t.entity >= 0) {
        invoke(MQ_INPUT_PHASE_TARGET, t.entity, self);
    } else {
        invoke(MQ_INPUT_PHASE_TARGET, -1, self);   // 未命中 → 全局处理器
    }

    switch (ev.type) {
        case MQ_INPUT_POINTER_DOWN:
            in->pointers_down.insert(ev.pointer_id);
            if (t.entity >= 0) in->capture[ev.pointer_id] = t.entity;   // 按下即捕获（拖拽）
            in->pending_click[ev.pointer_id] = PendingClick{ ev.x, ev.y, ev.button, in->frame_index, t.entity };
            break;
        case MQ_INPUT_POINTER_UP: {
            in->pointers_down.erase(ev.pointer_id);
            auto pc = in->pending_click.find(ev.pointer_id);
            if (pc != in->pending_click.end()) {
                const float ddx = ev.x - pc->second.x, ddy = ev.y - pc->second.y;
                if (ddx * ddx + ddy * ddy <= 64.0F && pc->second.target >= 0) {   // 8px 内视为点击
                    mq_input_event click = ev;
                    click.type = MQ_INPUT_CLICK;
                    click.target = pc->second.target;
                    invoke(MQ_INPUT_PHASE_TARGET, pc->second.target, &click);
                }
                in->pending_click.erase(pc);
            }
            in->capture.erase(ev.pointer_id);
            break;
        }
        case MQ_INPUT_POINTER_MOVE:
            in->pos[ev.pointer_id] = { ev.x, ev.y };
            break;
        case MQ_INPUT_KEY_DOWN: in->keys_down.insert(ev.key); break;
        case MQ_INPUT_KEY_UP:   in->keys_down.erase(ev.key); break;
        default: break;
    }
}

uint32_t mq_input_dispatch(mq_input* in) {
    if (!in) return 0;
    // 帧内注入（AI/CI 在 begin_frame 之后到达）也并入本帧，顺序 = 到达顺序
    if (!in->pending_inject.empty()) {
        for (const auto& ev : in->pending_inject) {
            push_event(in, ev);
            if (in->recording) log_push(in, ev, in->frame_index);
        }
        in->pending_inject.clear();
    }
    const size_t n = in->events.size();
    for (size_t i = 0; i < n; ++i) dispatch_one(in, in->events[i]);
    return (uint32_t)n;
}


void mq_input_end_frame(mq_input* in, uint64_t frame_index) {
    if (!in) return;
    (void)frame_index;
    in->events.clear();
}

int  mq_input_quit(mq_input* in) { return in ? in->quit : 1; }

void mq_input_pointer_pos(mq_input* in, uint32_t pointer_id, float* x, float* y) {
    if (x) *x = 0; if (y) *y = 0;
    if (!in) return;
    auto it = in->pos.find(pointer_id);
    if (it != in->pos.end()) { if (x) *x = it->second.first; if (y) *y = it->second.second; }
}

int mq_input_pointer_down(mq_input* in, uint32_t pointer_id) {
    return in && in->pointers_down.count(pointer_id) ? 1 : 0;
}
int mq_input_key_down(mq_input* in, uint32_t key) {
    return in && in->keys_down.count(key) ? 1 : 0;
}
int  mq_input_focus(mq_input* in) { return in ? in->focus : -1; }
void mq_input_set_focus(mq_input* in, int entity) { if (in) in->focus = entity; }
void mq_input_set_capture(mq_input* in, uint32_t pointer_id, int entity) {
    if (in) in->capture[pointer_id] = entity;
}
int  mq_input_get_capture(mq_input* in, uint32_t pointer_id) {
    if (!in) return -1;
    auto it = in->capture.find(pointer_id);
    return it == in->capture.end() ? -1 : it->second;
}
void mq_input_release_capture(mq_input* in, uint32_t pointer_id) {
    if (in) in->capture.erase(pointer_id);
}
int mq_input_frame_index(mq_input* in) { return in ? (int)in->frame_index : -1; }

void mq_input_record_begin(mq_input* in) {
    if (!in) return;
    in->log.clear(); in->recording = true; in->replaying = false;
}
void mq_input_record_end(mq_input* in) { if (in) in->recording = false; }
int  mq_input_recording(mq_input* in) { return in && in->recording; }

int mq_input_replay_begin(mq_input* in, const void* log, size_t size) {
    if (!in || !log) return 1;
    if (size % sizeof(LogEntry) != 0) return 2;
    in->replay_src.assign((const LogEntry*)log, (const LogEntry*)log + size / sizeof(LogEntry));
    in->replay_cursor = 0;
    in->replaying = true; in->recording = false;
    return 0;
}
void mq_input_replay_end(mq_input* in) {
    if (!in) return;
    in->replaying = false; in->replay_src.clear(); in->replay_cursor = 0;
}
int mq_input_replaying(mq_input* in) { return in && in->replaying; }

void mq_input_inject(mq_input* in, const mq_input_event* ev) {
    if (!in || !ev) return;
    mq_input_event copy = *ev;
    copy.phase = MQ_INPUT_PHASE_TARGET;
    copy.target = -1;
    in->pending_inject.push_back(copy);
}

const void* mq_input_log_bytes(mq_input* in, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!in) return nullptr;
    const std::vector<LogEntry>& src = in->replaying ? in->replay_src : in->log;
    if (out_size) *out_size = src.size() * sizeof(LogEntry);
    return src.empty() ? nullptr : (const void*)src.data();
}

const char* mq_input_log_json(mq_input* in) {
    if (!in) return "[]";
    const std::vector<LogEntry>& src = in->replaying ? in->replay_src : in->log;
    std::string o = "{\"version\":" + std::to_string(MQ_INPUT_LOG_VERSION) + ",\"meta\":{";
    for (size_t i = 0; i < in->meta.size(); ++i) {
        if (i) o += ",";
        o += "\"" + in->meta[i].first + "\":\"" + in->meta[i].second + "\"";
    }
    o += "},\"events\":[";
    for (size_t i = 0; i < src.size(); ++i) {
        const LogEntry& e = src[i];
        if (i) o += ",";
        o += std::format(
            "{{\"f\":{},\"t\":\"{}\",\"pid\":{},\"ptype\":{},\"btn\":{},\"mod\":{},\"key\":{},"
            "\"x\":{:.4f},\"y\":{:.4f},\"dx\":{:.4f},\"dy\":{:.4f},"
            "\"sx\":{:.4f},\"sy\":{:.4f},\"pr\":{:.4f}}}",
            (unsigned long long)e.frame, ev_type_name(e.type), e.pointer_id, e.pointer_type,
            e.button, e.modifiers, e.key,
            (double)e.x, (double)e.y, (double)e.dx, (double)e.dy,
            (double)e.scroll_x, (double)e.scroll_y, (double)e.pressure);
    }
    o += "]}";
    in->json_cache = std::move(o);
    return in->json_cache.c_str();
}

int mq_input_log_load_json(mq_input* in, const char* json) {
    if (!in || !json) return 1;
    mq_json* dom = mq_json_parse(json, std::strlen(json));
    if (!dom) return 2;
    in->meta.clear();
    if (const mq_json* mj = mq_json_get(dom, "meta"))
        mq_json_object_each(mj, [](void* u, const char* k, const char* v) {
            static_cast<mq_input*>(u)->meta.emplace_back(k ? k : "", v ? v : "");
            return 0;
        }, in);
    const mq_json* evs = mq_json_get(dom, "events");
    const uint32_t n = mq_json_len(evs);
    in->replay_src.clear();
    in->replay_src.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const mq_json* j = mq_json_at(evs, i);
        LogEntry e{};
        e.frame = (uint64_t)mq_json_num(mq_json_get(j, "f"), 0);
        e.pointer_id = (uint32_t)mq_json_num(mq_json_get(j, "pid"), 0);
        e.pointer_type = (uint32_t)mq_json_num(mq_json_get(j, "ptype"), 0);
        e.button = (uint32_t)mq_json_num(mq_json_get(j, "btn"), 0);
        e.modifiers = (uint32_t)mq_json_num(mq_json_get(j, "mod"), 0);
        e.key = (uint32_t)mq_json_num(mq_json_get(j, "key"), 0);
        e.x = (float)mq_json_num(mq_json_get(j, "x"), 0);
        e.y = (float)mq_json_num(mq_json_get(j, "y"), 0);
        e.dx = (float)mq_json_num(mq_json_get(j, "dx"), 0);
        e.dy = (float)mq_json_num(mq_json_get(j, "dy"), 0);
        e.scroll_x = (float)mq_json_num(mq_json_get(j, "sx"), 0);
        e.scroll_y = (float)mq_json_num(mq_json_get(j, "sy"), 0);
        e.pressure = (float)mq_json_num(mq_json_get(j, "pr"), 0);
        const char* tn = mq_json_str(mq_json_get(j, "t"), "");
        e.type = MQ_INPUT_POINTER_DOWN;
        for (uint32_t k = 0; k <= MQ_INPUT_TEXT; ++k)
            if (std::strcmp(tn, ev_type_name(k)) == 0) { e.type = k; break; }
        in->replay_src.push_back(e);
    }
    mq_json_free(dom);
    in->replay_cursor = 0;
    in->replaying = true;
    in->recording = false;
    return 0;
}

void mq_input_log_set_meta(mq_input* in, const char* key, const char* value) {
    if (!in || !key) return;
    for (auto& kv : in->meta)
        if (kv.first == key) { kv.second = value ? value : ""; return; }
    in->meta.emplace_back(key, value ? value : "");
}

const char* mq_input_log_get_meta(mq_input* in, const char* key) {
    if (!in || !key) return nullptr;
    for (auto& kv : in->meta) if (kv.first == key) return kv.second.c_str();
    return nullptr;
}

} // extern "C"
