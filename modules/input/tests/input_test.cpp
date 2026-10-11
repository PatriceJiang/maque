// input 单元测试（docs/19/10）：事件流顺序 / 捕获→目标→冒泡 / 指针捕获 / click 合成 /
// 键盘焦点 / 录制→回放一致（输入即数据）。
#include "core/input/mq_input.h"

#include <print>
#include <vector>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

/* 假节点树：0 为根，1/2 为子，路径 1→0。 */
int fake_parent(void*, int e) { return e == 1 || e == 2 ? 0 : -1; }

/* 命中测试：总是命中 1（模拟 UI 顶层控件）。 */
int hit_one(void*, const mq_input_event*, mq_input_target* out) {
    out->entity = 1; out->handle = 1;
    return 1;
}

struct Log { std::vector<std::string> seq; };

int record_handler(void* user, const mq_input_event* ev, mq_input_target* t) {
    auto* l = (Log*)user;
    const char* names[] = { "down", "up", "move", "wheel", "enter", "leave",
                            "click", "dclick", "keydown", "keyup", "text" };
    const char* ph[] = { "cap", "tgt", "bub" };
    l->seq.push_back(std::string(names[ev->type]) + ":" + ph[ev->phase] + ":" + std::to_string(t->entity));
    if (ev->type == MQ_INPUT_CLICK) return 1;    // 消费点击
    return 0;
}

mq_input_event ev(uint32_t type, float x, float y, uint32_t key = 0) {
    mq_input_event e{};
    e.type = type; e.x = x; e.y = y; e.key = key; e.pointer_id = 0;
    e.pointer_type = MQ_PLATFORM_POINTER_MOUSE; e.button = 0;
    return e;
}

} // namespace

int main() {
    // ---------- 1) 顺序 + 路径分发 + 指针捕获 + 状态 ----------
    mq_input* in = mq_input_create();
    CHECK(in != nullptr);
    mq_input_set_hit_test(in, hit_one, nullptr);
    mq_input_set_parent_fn(in, fake_parent, nullptr);
    Log log;
    mq_input_add_handler(in, record_handler, &log);

    mq_input_begin_frame(in, nullptr, 0);
    mq_input_event mv = ev(MQ_INPUT_POINTER_MOVE, 10, 10);
    mq_input_inject(in, &mv);
    mq_input_event dn = ev(MQ_INPUT_POINTER_DOWN, 10, 10);
    mq_input_inject(in, &dn);
    CHECK(mq_input_dispatch(in) == 2);
    // 路径：根(0) 捕获 → 目标(1) 目标阶段 → 根(0) 冒泡
    CHECK(log.seq.size() == 6);
    CHECK(log.seq[0] == "move:cap:0");
    CHECK(log.seq[1] == "move:tgt:1");
    CHECK(log.seq[2] == "move:bub:0");
    CHECK(log.seq[3] == "down:cap:0");
    CHECK(log.seq[4] == "down:tgt:1");
    CHECK(log.seq[5] == "down:bub:0");
    CHECK(mq_input_pointer_down(in, 0) == 1);
    CHECK(mq_input_get_capture(in, 0) == 1);      // 按下即捕获（独立语句：求值顺序）
    mq_input_end_frame(in, 0);

    // 捕获期间 Move 始终发给捕获者，即使命中测试会返回别的目标
    log.seq.clear();
    mq_input_begin_frame(in, nullptr, 1);
    mq_input_event mv2 = ev(MQ_INPUT_POINTER_MOVE, 40, 40);
    mq_input_inject(in, &mv2);
    mq_input_dispatch(in);
    CHECK(log.seq.size() == 3 && log.seq[1] == "move:tgt:1");
    mq_input_end_frame(in, 1);

    // Up → 合成 click（同点）并释放捕获
    log.seq.clear();
    mq_input_begin_frame(in, nullptr, 2);
    mq_input_event up = ev(MQ_INPUT_POINTER_UP, 10, 10);
    mq_input_inject(in, &up);
    mq_input_dispatch(in);
    CHECK(mq_input_pointer_down(in, 0) == 0);
    CHECK(mq_input_get_capture(in, 0) == -1);
    bool saw_click = false;
    for (auto& s : log.seq) if (s == "click:tgt:1") saw_click = true;
    CHECK(saw_click);
    mq_input_end_frame(in, 2);

    // 远距离 Up 不合成 click
    log.seq.clear();
    mq_input_begin_frame(in, nullptr, 3);
    mq_input_event d2 = ev(MQ_INPUT_POINTER_DOWN, 0, 0);
    mq_input_inject(in, &d2);
    mq_input_dispatch(in);
    mq_input_end_frame(in, 3);
    log.seq.clear();
    mq_input_begin_frame(in, nullptr, 4);
    mq_input_event u2 = ev(MQ_INPUT_POINTER_UP, 100, 100);
    mq_input_inject(in, &u2);
    mq_input_dispatch(in);
    for (auto& s : log.seq) CHECK(s != "click:tgt:1");
    mq_input_end_frame(in, 4);

    // ---------- 2) 键盘：焦点节点接收，状态可查 ----------
    mq_input_set_focus(in, 2);
    log.seq.clear();
    mq_input_begin_frame(in, nullptr, 5);
    mq_input_event kd = ev(MQ_INPUT_KEY_DOWN, 0, 0, MQ_KEY_SPACE);
    mq_input_inject(in, &kd);
    mq_input_dispatch(in);
    CHECK(mq_input_key_down(in, MQ_KEY_SPACE) == 1);
    CHECK(log.seq.size() == 3 && log.seq[1] == "keydown:tgt:2");
    mq_input_end_frame(in, 5);

    mq_input_begin_frame(in, nullptr, 6);
    mq_input_event ku = ev(MQ_INPUT_KEY_UP, 0, 0, MQ_KEY_SPACE);
    mq_input_inject(in, &ku);
    mq_input_dispatch(in);
    CHECK(mq_input_key_down(in, MQ_KEY_SPACE) == 0);
    mq_input_end_frame(in, 6);

    mq_input_destroy(in);

    // ---------- 3) 录制 → 回放：同一输入产生同一事件序列（docs/10 §3） ----------
    auto run = [](bool replaying, const void* log, size_t n, std::string* out) {
        mq_input* i2 = mq_input_create();
        mq_input_set_hit_test(i2, hit_one, nullptr);
        mq_input_set_parent_fn(i2, fake_parent, nullptr);
        Log l;
        mq_input_add_handler(i2, record_handler, &l);
        if (replaying) CHECK(mq_input_replay_begin(i2, log, n) == 0);
        else mq_input_record_begin(i2);
        for (uint64_t f = 0; f < 4; ++f) {
            mq_input_begin_frame(i2, nullptr, f);
            if (!replaying) {
                if (f == 1) { mq_input_event d = ev(MQ_INPUT_POINTER_DOWN, 5, 5); mq_input_inject(i2, &d); }
                if (f == 2) { mq_input_event m = ev(MQ_INPUT_POINTER_MOVE, 7, 9); mq_input_inject(i2, &m); }
                if (f == 3) { mq_input_event u = ev(MQ_INPUT_POINTER_UP, 5, 5); mq_input_inject(i2, &u); }
            }
            mq_input_dispatch(i2);
            mq_input_end_frame(i2, f);
        }
        std::string s;
        for (auto& x : l.seq) s += x + "|";
        *out = s;
        size_t bytes = 0;
        const void* bytes_ptr = mq_input_log_bytes(i2, &bytes);
        std::vector<uint8_t> copy;
        if (bytes_ptr && bytes) copy.assign((const uint8_t*)bytes_ptr, (const uint8_t*)bytes_ptr + bytes);
        mq_input_destroy(i2);
        return copy;
    };

    std::string seq_a;
    std::vector<uint8_t> rec = run(false, nullptr, 0, &seq_a);
    CHECK(!rec.empty());
    std::string seq_b;
    run(true, rec.data(), rec.size(), &seq_b);
    CHECK(!seq_a.empty());
    CHECK(seq_a == seq_b);

    // JSON 日志往返
    mq_input* i3 = mq_input_create();
    mq_input_event d = ev(MQ_INPUT_POINTER_DOWN, 3, 4);
    mq_input_record_begin(i3);
    mq_input_begin_frame(i3, nullptr, 0);
    mq_input_inject(i3, &d);
    mq_input_dispatch(i3);
    mq_input_end_frame(i3, 0);
    const char* json = mq_input_log_json(i3);
    CHECK(json && json[0] == '{');
    mq_input* i4 = mq_input_create();
    CHECK(mq_input_log_load_json(i4, json) == 0);
    CHECK(mq_input_replaying(i4) == 1);
    mq_input_destroy(i4);
    mq_input_destroy(i3);

    if (g_failures == 0) { std::print("input_test: all checks passed\n"); return 0; }
    std::print("input_test: {} failure(s)\n", g_failures);
    return 1;
}
