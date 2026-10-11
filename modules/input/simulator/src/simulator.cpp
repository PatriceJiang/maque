/* 输入模拟器：把 .sim.json 场景回放成输入事件流（docs/19）。
 *
 * 场景格式（JSON）：
 * {
 *   "name": "ball_clicks",          // 可选：场景名（日志/自省）
 *   "loop": false,                  // 可选：播完后回绕（演示用）
 *   "space": "window",              // 可选：默认坐标空间 window（窗口像素）| world（世界坐标）
 *   "events": [
 *     { "frame": 10, "type": "pointer_down", "x": 100, "y": 150 },
 *     { "delay": 5,  "type": "pointer_move", "x": 120, "y": 160 },   // delay = 相对上一条 +5 帧
 *     { "frame": 20, "type": "click", "x": 400, "y": 300 },          // click → down+up（同帧有序）
 *     { "frame": 30, "type": "wheel", "dy": -1.0, "x": 400, "y": 300 },
 *     { "frame": 40, "type": "key_down", "key": 32 },
 *     { "frame": 42, "type": "text", "text": "hello" }
 *   ]
 * }
 *
 * 确定性（docs/10）：事件只由**帧号**决定；按帧稳定排序（同帧按声明顺序）；
 * 查询 `mq_sim_due(sim, frame)` 取出**到期或已过期**（frame <= 当前帧）的事件并前进游标，
 * 不依赖墙钟或随机数；loop 时帧号按场景长度取模循环。 */
#include "sim/mq_sim.h"

#include "abi/persist.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Ev {
    mq_sim_event e{};
    std::string  text;      /* MQ_SIM_TEXT 的文本存储 */
};

/* 事件类型：click 是语法糖（展开为 down+up），因此单独一档 */
enum class Type { Move, Down, Up, Click, Wheel, KeyDown, KeyUp, Text };

bool type_of(const std::string& s, Type* out) {
    if (s == "pointer_move" || s == "move")          { *out = Type::Move;    return true; }
    if (s == "pointer_down" || s == "down")          { *out = Type::Down;    return true; }
    if (s == "pointer_up"   || s == "up")            { *out = Type::Up;      return true; }
    if (s == "click" || s == "tap")                  { *out = Type::Click;   return true; }
    if (s == "wheel" || s == "pointer_wheel")        { *out = Type::Wheel;   return true; }
    if (s == "key_down" || s == "keydown")           { *out = Type::KeyDown; return true; }
    if (s == "key_up"   || s == "keyup")             { *out = Type::KeyUp;   return true; }
    if (s == "text" || s == "char")                  { *out = Type::Text;    return true; }
    return false;
}

uint32_t kind_of(Type t) {
    switch (t) {
        case Type::Move:    return MQ_SIM_POINTER_MOVE;
        case Type::Down:
        case Type::Click:   return MQ_SIM_POINTER_DOWN;
        case Type::Up:      return MQ_SIM_POINTER_UP;
        case Type::Wheel:   return MQ_SIM_POINTER_WHEEL;
        case Type::KeyDown: return MQ_SIM_KEY_DOWN;
        case Type::KeyUp:   return MQ_SIM_KEY_UP;
        case Type::Text:    return MQ_SIM_TEXT;
    }
    return MQ_SIM_POINTER_MOVE;
}

uint32_t space_of(const std::string& s, bool* ok) {
    *ok = true;
    if (s == "window" || s == "screen") return MQ_SIM_SPACE_WINDOW;
    if (s == "world") return MQ_SIM_SPACE_WORLD;
    *ok = false;
    return 0;
}

void set_err(char* err, size_t cap, const std::string& msg) {
    if (!err || cap == 0) return;
    std::snprintf(err, cap, "%s", msg.c_str());
}

} // namespace

struct mq_sim {
    std::vector<Ev> events;         /* 按帧稳定排序 */
    uint32_t cursor = 0;
    bool loop = false;
    bool loaded = false;
    uint32_t last_frame = 0;
    uint64_t phase = 0;             /* loop 时的轮次（帧号 / 场景长度） */
    std::string name;
};

extern "C" mq_sim* mq_sim_create(void) { return new (std::nothrow) mq_sim(); }

extern "C" void mq_sim_destroy(mq_sim* sim) { delete sim; }

extern "C" void mq_sim_reset(mq_sim* sim) { if (sim) { sim->cursor = 0; sim->phase = 0; } }

extern "C" void mq_sim_set_loop(mq_sim* sim, int loop) { if (sim) sim->loop = loop != 0; }

extern "C" int mq_sim_loop(mq_sim* sim) { return sim && sim->loop ? 1 : 0; }

extern "C" int mq_sim_loaded(mq_sim* sim) { return sim && sim->loaded ? 1 : 0; }

extern "C" uint32_t mq_sim_count(mq_sim* sim) { return sim ? (uint32_t)sim->events.size() : 0; }

extern "C" uint32_t mq_sim_cursor(mq_sim* sim) { return sim ? sim->cursor : 0; }

extern "C" uint64_t mq_sim_last_frame(mq_sim* sim) { return sim ? sim->last_frame : 0; }

extern "C" int mq_sim_finished(mq_sim* sim) {
    if (!sim || !sim->loaded) return 1;
    return (!sim->loop && sim->cursor >= sim->events.size()) ? 1 : 0;
}

extern "C" int mq_sim_event_at(mq_sim* sim, uint32_t index, mq_sim_event* out) {
    if (!sim || !out || index >= sim->events.size()) return 1;
    *out = sim->events[index].e;
    if (sim->events[index].e.kind == MQ_SIM_TEXT) out->text = sim->events[index].text.c_str();
    return 0;
}

extern "C" int mq_sim_load_json(mq_sim* sim, const char* json, size_t len,
                                char* err, size_t err_cap) {
    if (!sim || !json) { set_err(err, err_cap, "null sim/json"); return 1; }
    mq_json* root = mq_json_parse(json, len ? len : std::strlen(json));
    if (!root) { set_err(err, err_cap, "json parse error"); return 2; }

    std::vector<Ev> parsed;
    uint32_t default_space = MQ_SIM_SPACE_WINDOW;
    bool loop = false;
    std::string name;

    if (const char* n = mq_json_str(mq_json_get(root, "name"), nullptr)) name = n;
    if (const char* sp = mq_json_str(mq_json_get(root, "space"), nullptr)) {
        bool ok = false;
        const uint32_t v = space_of(sp, &ok);
        if (!ok) {
            const std::string msg = std::string("unknown space: ") + sp;
            mq_json_free(root);
            set_err(err, err_cap, msg);
            return 3;
        }
        default_space = v;
    }
    loop = mq_json_num(mq_json_get(root, "loop"), 0.0) != 0.0;

    const mq_json* events = mq_json_get(root, "events");
    const uint32_t n = mq_json_len(events);
    if (n == 0) {
        mq_json_free(root);
        set_err(err, err_cap, "no events");
        return 4;
    }

    uint64_t prev_frame = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const mq_json* e = mq_json_at(events, i);
        const char* ts = mq_json_str(mq_json_get(e, "type"), nullptr);
        if (!ts) { set_err(err, err_cap, "event missing type"); mq_json_free(root); return 5; }
        Type type{};
        if (!type_of(ts, &type)) {
            const std::string msg = std::string("unknown type: ") + ts;   // ts 指向 JSON 树 → 先拷贝
            mq_json_free(root);
            set_err(err, err_cap, msg);
            return 6;
        }

        /* frame（绝对）优先；否则 delay（相对上一条事件） */
        const mq_json* jf = mq_json_get(e, "frame");
        const uint64_t frame = jf ? (uint64_t)mq_json_num(jf, 0.0)
                                  : prev_frame + (uint64_t)mq_json_num(mq_json_get(e, "delay"), 0.0);

        uint32_t space = default_space;
        if (const char* sp = mq_json_str(mq_json_get(e, "space"), nullptr)) {
            bool sok = false;
            const uint32_t v = space_of(sp, &sok);
            if (!sok) {
                const std::string msg = std::string("unknown space: ") + sp;
                mq_json_free(root);
                set_err(err, err_cap, msg);
                return 3;
            }
            space = v;
        }

        auto fill = [&](uint32_t kind, uint64_t f) {
            Ev ev;
            ev.e.frame = f;
            ev.e.kind = kind;
            ev.e.space = space;
            ev.e.x = (float)mq_json_num(mq_json_get(e, "x"), 0.0);
            ev.e.y = (float)mq_json_num(mq_json_get(e, "y"), 0.0);
            ev.e.button = (uint32_t)mq_json_num(mq_json_get(e, "button"), 0.0);
            ev.e.key = (uint32_t)mq_json_num(mq_json_get(e, "key"), 0.0);
            ev.e.dx = (float)mq_json_num(mq_json_get(e, "dx"),
                                         mq_json_num(mq_json_get(e, "scroll_x"), 0.0));
            ev.e.dy = (float)mq_json_num(mq_json_get(e, "dy"),
                                         mq_json_num(mq_json_get(e, "scroll_y"), 0.0));
            if (kind == MQ_SIM_TEXT) ev.text = mq_json_str(mq_json_get(e, "text"), "");
            ev.e.text = nullptr;
            parsed.push_back(std::move(ev));
        };

        if (type == Type::Click) {
            fill(MQ_SIM_POINTER_DOWN, frame);      /* click = down + up（同帧、有序） */
            fill(MQ_SIM_POINTER_UP, frame);
        } else {
            fill(kind_of(type), frame);
        }
        /* 注意：text 指针**必须等容器定稿后再解析** —— 解析期间 push_back 会使
         * 指向 vector 元素内部 string 的指针悬空（ASan 抓到过 heap-use-after-free）。 */
        prev_frame = frame;
    }

    /* 按帧**稳定**排序（同帧保持声明顺序 → 确定） */
    std::stable_sort(parsed.begin(), parsed.end(),
                     [](const Ev& a, const Ev& b) { return a.e.frame < b.e.frame; });
    /* 定稿后再绑定 text 指针（此后不再增删元素 → 指针稳定） */
    for (Ev& ev : parsed)
        if (ev.e.kind == MQ_SIM_TEXT) ev.e.text = ev.text.c_str();

    sim->events = std::move(parsed);
    sim->cursor = 0;
    sim->phase = 0;
    sim->loop = loop;
    sim->loaded = true;
    sim->name = name;
    sim->last_frame = sim->events.empty() ? 0 : (uint32_t)sim->events.back().e.frame;
    mq_json_free(root);
    return 0;
}

extern "C" int mq_sim_due(mq_sim* sim, uint64_t frame, mq_sim_event* out,
                          uint32_t cap, uint32_t* out_count) {
    if (!sim) return 1;
    if (out_count) *out_count = 0;
    if (!sim->loaded || sim->events.empty()) return 0;
    uint32_t count = 0;
    /* 语义：取出**到期或已过期**（frame <= 当前帧）的全部事件 —— 宿主按块步进（Runtime.step N）
     * 也不会漏事件；同帧顺序保持声明顺序（稳定）。
     * loop：帧号按场景长度取模（有效帧 = frame % (last_frame+1)），跨轮次时游标归零。 */
    uint64_t f = frame;
    if (sim->loop) {
        const uint64_t len = (uint64_t)sim->last_frame + 1;
        if (len > 0) {
            const uint64_t phase = frame / len;
            if (phase != sim->phase) { sim->cursor = 0; sim->phase = phase; }
            f = frame % len;
        }
    }
    while (sim->cursor < sim->events.size() && sim->events[sim->cursor].e.frame <= f) {
        const Ev& ev = sim->events[sim->cursor];
        if (out && count < cap) {
            out[count] = ev.e;
            if (ev.e.kind == MQ_SIM_TEXT) out[count].text = ev.text.c_str();
        }
        ++count;
        ++sim->cursor;
    }
    if (out_count) *out_count = count;
    return (out && count > cap) ? 2 : 0;
}
